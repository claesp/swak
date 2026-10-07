#include "core/data.h"
#include "core/gdal_support.h"
#include "core/interfaces.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal_priv.h>
#include <memory>
#include <numeric>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>
#include <optional>
#include <utility>

namespace swak {
namespace {
constexpr int MaxShapes = 10000;
constexpr int MaxFeatures = 100000;
constexpr int MaxCoordinates = 100000;
// KML folders become layers. NOAA NDBC groups about 110 station owners into folders.
// The feature, shape, and coordinate limits still bound the total work.
constexpr int MaxLayers = 1024;
constexpr int MaxGeometryDepth = 8;
constexpr int MaxProperties = 256;
constexpr int MaxArchiveEntries = 256;
constexpr qint64 MaxArchiveExpansion = 32 * 1024 * 1024;
constexpr qsizetype MaxMetadata = 8 * 1024 * 1024;

// File name suffixes that name one dataset inside an archive.
const QStringList DatasetSuffixes{QStringLiteral("kml"),
                                  QStringLiteral("shp"),
                                  QStringLiteral("geojson"),
                                  QStringLiteral("json"),
                                  QStringLiteral("gpx"),
                                  QStringLiteral("gml"),
                                  QStringLiteral("gpkg"),
                                  QStringLiteral("fgb"),
                                  QStringLiteral("csv"),
                                  QStringLiteral("tab"),
                                  QStringLiteral("osm"),
                                  QStringLiteral("pbf"),
                                  QStringLiteral("dxf"),
                                  QStringLiteral("sqlite"),
                                  QStringLiteral("000")};
// LIBKML adds these style fields to every layer. They are not feature properties.
const QStringList KmlStyleFields{QStringLiteral("id"),
                                 QStringLiteral("timestamp"),
                                 QStringLiteral("begin"),
                                 QStringLiteral("end"),
                                 QStringLiteral("altitudeMode"),
                                 QStringLiteral("tessellate"),
                                 QStringLiteral("extrude"),
                                 QStringLiteral("visibility"),
                                 QStringLiteral("drawOrder"),
                                 QStringLiteral("icon")};

QString suffixFor(const QByteArray &bytes)
{
    const auto head = bytes.left(512).trimmed();
    if (head.startsWith("SQLite format 3")) {
        return QStringLiteral("gpkg");
    }
    if (head.startsWith("fgb")) {
        return QStringLiteral("fgb");
    }
    if (head.startsWith('{') || head.startsWith('[')) {
        return QStringLiteral("json");
    }
    if (head.contains("<kml")) {
        return QStringLiteral("kml");
    }
    if (head.contains("<gpx")) {
        return QStringLiteral("gpx");
    }
    return head.startsWith('<') ? QStringLiteral("xml") : QStringLiteral("dat");
}
bool xmlSuffix(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == u"kml" || suffix == u"gpx" || suffix == u"gml" || suffix == u"xml" ||
           suffix == u"georss";
}

class Reader
{
public:
    Reader(DataDocument &result, const std::atomic_bool &cancelled)
        : result_(result), cancelled_(cancelled)
    {
        target_.SetWellKnownGeogCS("WGS84");
        target_.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }
    void read(GDALDataset &dataset)
    {
        const bool kml = QString::fromLatin1(dataset.GetDriverName()) == u"LIBKML";
        if (dataset.GetLayerCount() > MaxLayers) {
            fail(QObject::tr("The data has more than %1 layers.").arg(MaxLayers));
            return;
        }
        for (auto *layer : dataset.GetLayers()) {
            if (!readLayer(*layer, kml)) {
                return;
            }
        }
    }

private:
    bool fail(const QString &message)
    {
        if (result_.error.isEmpty()) {
            result_.error = message;
        }
        return false;
    }
    bool readLayer(OGRLayer &layer, bool kml)
    {
        std::unique_ptr<OGRCoordinateTransformation> transform;
        if (const auto *source = layer.GetSpatialRef(); source && !source->IsSame(&target_)) {
            transform.reset(OGRCreateCoordinateTransformation(source, &target_));
            if (!transform) {
                return fail(gdalError(QObject::tr("Cannot convert the layer coordinates.")));
            }
        }
        const auto *definition = layer.GetLayerDefn();
        const int visibility = kml ? definition->GetFieldIndex("visibility") : -1;
        layer.ResetReading();
        while (OGRFeatureUniquePtr feature{layer.GetNextFeature()}) {
            if (cancelled_) {
                return fail(QObject::tr("Data parsing cancelled."));
            }
            if (++features_ > MaxFeatures) {
                return fail(QObject::tr("The data has more than %1 features.").arg(MaxFeatures));
            }
            if (visibility >= 0 && feature->IsFieldSetAndNotNull(visibility) &&
                feature->GetFieldAsInteger(visibility) == 0) {
                continue;
            }
            const auto *geometry = feature->GetGeometryRef();
            if (!geometry || geometry->IsEmpty()) {
                continue;
            }
            DataShape prototype;
            if (!readFields(*feature, kml, prototype)) {
                return false;
            }
            std::unique_ptr<OGRGeometry> converted(
                geometry->hasCurveGeometry() ? geometry->getLinearGeometry() : geometry->clone());
            if (!converted) {
                return fail(QObject::tr("Cannot convert a curved geometry."));
            }
            if (transform && converted->transform(transform.get()) != OGRERR_NONE) {
                return fail(gdalError(QObject::tr("Cannot convert the feature coordinates.")));
            }
            if (!appendGeometry(*converted, prototype, 0)) {
                return false;
            }
        }
        return true;
    }
    bool readFields(const OGRFeature &feature, bool kml, DataShape &shape)
    {
        const auto *definition = feature.GetDefnRef();
        for (int i = 0; i < definition->GetFieldCount(); ++i) {
            if (!feature.IsFieldSetAndNotNull(i)) {
                continue;
            }
            const auto key = QString::fromUtf8(definition->GetFieldDefn(i)->GetNameRef());
            const auto value = QString::fromUtf8(feature.GetFieldAsString(i)).left(4096);
            if (kml && KmlStyleFields.contains(key)) {
                continue;
            }
            if (key.compare(u"name", Qt::CaseInsensitive) == 0 && shape.name.isEmpty()) {
                shape.name = value.trimmed().left(128);
            } else if (key.compare(u"description", Qt::CaseInsensitive) == 0 &&
                       shape.description.isEmpty()) {
                shape.description = value;
            } else if (!value.trimmed().isEmpty()) {
                if (shape.properties.size() >= MaxProperties || key.size() > 128) {
                    return fail(QObject::tr("Feature properties exceed their limit."));
                }
                shape.properties.insert(key, value.trimmed());
            }
        }
        return true;
    }
    std::optional<QPolygonF> points(const OGRSimpleCurve &curve, DataShape *point)
    {
        QPolygonF result;
        for (int i = 0; i < curve.getNumPoints(); ++i) {
            if (cancelled_) {
                fail(QObject::tr("Data parsing cancelled."));
                return std::nullopt;
            }
            if (++coordinates_ > MaxCoordinates) {
                fail(QObject::tr("The data has more than %1 coordinates.").arg(MaxCoordinates));
                return std::nullopt;
            }
            const double lon = curve.getX(i);
            const double lat = curve.getY(i);
            if (!std::isfinite(lon) || !std::isfinite(lat) || std::abs(lon) > 180 ||
                std::abs(lat) > 90) {
                fail(QObject::tr("The data contains an invalid coordinate."));
                return std::nullopt;
            }
            if (point) {
                point->properties.insert(QStringLiteral("longitude"), lon);
                point->properties.insert(QStringLiteral("latitude"), lat);
                if (curve.Is3D() && std::isfinite(curve.getZ(i))) {
                    point->properties.insert(QStringLiteral("altitude"), curve.getZ(i));
                }
            }
            QPointF projected = project(lat, lon);
            if (!result.isEmpty()) {
                projected.rx() += std::round(result.last().x() - projected.x());
            }
            if (std::abs(projected.x()) > 2) {
                fail(QObject::tr("A geometry wraps around the world too many times."));
                return std::nullopt;
            }
            result.append(projected);
        }
        return result;
    }
    bool appendShape(DataShape shape)
    {
        if (result_.shapes.size() >= MaxShapes) {
            return fail(QObject::tr("The data has more than %1 shapes.").arg(MaxShapes));
        }
        metadata_ += (shape.name.size() + shape.description.size()) * 2;
        for (auto it = shape.properties.begin(); it != shape.properties.end(); ++it) {
            metadata_ += (it.key().size() + it.value().toString().size()) * 2 + 64;
        }
        if (metadata_ > MaxMetadata) {
            return fail(QObject::tr("Feature metadata exceeds its limit."));
        }
        result_.shapes.append(std::move(shape));
        return true;
    }
    bool appendGeometry(const OGRGeometry &geometry, const DataShape &prototype, int depth)
    {
        if (depth > MaxGeometryDepth) {
            return fail(QObject::tr("Nested geometry exceeds its limit."));
        }
        switch (wkbFlatten(geometry.getGeometryType())) {
        case wkbPoint: {
            const auto *point = geometry.toPoint();
            OGRLineString line;
            line.addPoint(point);
            DataShape shape = prototype;
            shape.kind = DataShape::Kind::Point;
            const auto projected = points(line, &shape);
            if (!projected || projected->size() != 1) {
                return false;
            }
            shape.point = projected->first();
            return appendShape(std::move(shape));
        }
        case wkbLineString:
        case wkbLinearRing: {
            const auto projected = points(*geometry.toLineString(), nullptr);
            if (!projected) {
                return false;
            }
            if (projected->size() < 2) {
                return true;
            }
            DataShape shape = prototype;
            shape.kind = DataShape::Kind::Line;
            shape.path.moveTo(projected->first());
            for (qsizetype i = 1; i < projected->size(); ++i) {
                shape.path.lineTo(projected->at(i));
            }
            shape.point = projected->first();
            return appendShape(std::move(shape));
        }
        case wkbPolygon: {
            DataShape shape = prototype;
            shape.kind = DataShape::Kind::Polygon;
            shape.path.setFillRule(Qt::OddEvenFill);
            for (const auto *ring : *geometry.toPolygon()) {
                auto projected = points(*ring, nullptr);
                if (!projected) {
                    return false;
                }
                if (projected->size() < 3) {
                    continue;
                }
                if (!shape.path.isEmpty()) {
                    // Keep holes on the same world copy as the outer ring.
                    const double offset =
                        std::round(shape.path.boundingRect().center().x() - projected->first().x());
                    projected->translate(offset, 0);
                }
                shape.path.addPolygon(*projected);
                shape.path.closeSubpath();
            }
            if (shape.path.isEmpty()) {
                return true;
            }
            shape.point = shape.path.elementAt(0);
            return appendShape(std::move(shape));
        }
        case wkbMultiPoint:
        case wkbMultiLineString:
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            const auto &parts = *geometry.toGeometryCollection();
            return std::all_of(parts.begin(), parts.end(), [&](const OGRGeometry *part) {
                return appendGeometry(*part, prototype, depth + 1);
            });
        }
        default:
            // Other types, such as triangulated surfaces, have no map form here.
            return true;
        }
    }

    DataDocument &result_;
    const std::atomic_bool &cancelled_;
    OGRSpatialReference target_;
    int features_ = 0;
    int coordinates_ = 0;
    qsizetype metadata_ = 0;
};

// Finds the one dataset file in an archive and checks every entry against the limits.
QString archiveDataset(const QByteArray &archive, QString &error)
{
    const QByteArray root = QByteArrayLiteral("/vsizip/") + archive;
    const CPLStringList entries(VSIReadDirRecursive(root.constData()));
    if (entries.Count() == 0) {
        error = QObject::tr("Invalid or empty archive.");
        return {};
    }
    if (entries.Count() > MaxArchiveEntries) {
        error = QObject::tr("The archive has more than %1 entries.").arg(MaxArchiveEntries);
        return {};
    }
    QString dataset;
    qint64 expanded = 0;
    for (int i = 0; i < entries.Count(); ++i) {
        const auto name = QString::fromUtf8(entries[i]);
        if (name.isEmpty() || name.size() > 1024 || name.startsWith('/') || name.contains('\\') ||
            name.split('/').contains(QStringLiteral(".."))) {
            error = QObject::tr("Unsafe archive entry.");
            return {};
        }
        VSIStatBufL status{};
        const auto path = root + '/' + name.toUtf8();
        if (VSIStatL(path.constData(), &status) != 0) {
            error = QObject::tr("Invalid archive entry.");
            return {};
        }
        if (VSI_ISDIR(status.st_mode)) {
            continue;
        }
        expanded += status.st_size;
        if (status.st_size < 0 || status.st_size > DataXmlLimit || expanded > MaxArchiveExpansion) {
            error = QObject::tr("The archive expands beyond its limit.");
            return {};
        }
        if (DatasetSuffixes.contains(QFileInfo(name).suffix().toLower())) {
            if (!dataset.isEmpty()) {
                error = QObject::tr("Use an archive with one dataset.");
                return {};
            }
            dataset = QString::fromUtf8(path);
        }
    }
    if (dataset.isEmpty()) {
        error = QObject::tr("The archive contains no supported dataset.");
    }
    return dataset;
}
DataDocument readDataset(const QString &path, const std::atomic_bool &cancelled)
{
    DataDocument result;
    if (cancelled) {
        result.error = QObject::tr("Data parsing cancelled.");
        return result;
    }
    initializeGdal();
    CPLErrorReset();
    GdalDataset dataset(GDALDataset::FromHandle(GDALOpenEx(path.toUtf8().constData(),
                                                           GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                           gdalVectorDrivers().data(),
                                                           nullptr,
                                                           nullptr)));
    if (!dataset) {
        result.error = gdalError(QObject::tr("Malformed or unsupported data."));
        return result;
    }
    Reader(result, cancelled).read(*dataset);
    if (!result.error.isEmpty()) {
        result.shapes.clear();
    }
    return result;
}
int countTags(const QByteArray &bytes, const QList<QByteArray> &tags)
{
    return std::accumulate(tags.begin(), tags.end(), 0, [&bytes](int total, const auto &tag) {
        return total + static_cast<int>(bytes.count(tag));
    });
}
// GDAL skips KML network links and overlays without a report. Count them for the status.
void countKmlSkips(const QByteArray &xml, DataDocument &result)
{
    if (!xml.contains("<kml")) {
        return;
    }
    result.ignoredLinks =
        countTags(xml, {QByteArrayLiteral("<NetworkLink>"), QByteArrayLiteral("<NetworkLink ")});
    result.ignoredOverlays = countTags(xml,
                                       {QByteArrayLiteral("<GroundOverlay"),
                                        QByteArrayLiteral("<ScreenOverlay"),
                                        QByteArrayLiteral("<PhotoOverlay"),
                                        QByteArrayLiteral("<Model"),
                                        QByteArrayLiteral("<gx:Track")});
}
} // namespace
class GdalVectorProvider final : public QObject, public Plugin, public DataProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "gdal_vector/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::DataProvider)
public:
    QString dataId() const override
    {
        return QStringLiteral("org.swak.gdal.vector");
    }
    QStringList legacyIds() const override
    {
        return {QStringLiteral("org.swak.kml")};
    }
    QString dataName() const override
    {
        return tr("GDAL vector data");
    }
    QList<QJsonObject> presets() const override
    {
        QList<QJsonObject> result;
        for (const auto &entry :
             {QStringList{
                  tr("USGS earthquakes, M2.5+, past day"),
                  QStringLiteral(
                      "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day_age.kml"),
                  QStringLiteral("U.S. Geological Survey | "
                                 "https://www.usgs.gov/information-policies-and-instructions/"
                                 "copyrights-and-credits")},
              QStringList{
                  tr("NOAA NDBC marine observations"),
                  QStringLiteral("https://www.ndbc.noaa.gov/kml/marineobs_as_kml.php"),
                  QStringLiteral(
                      "NOAA / NDBC and station operators | https://www.weather.gov/disclaimer")}}) {
            auto source = dataDefaults();
            source.insert(QStringLiteral("name"), entry[0]);
            source.insert(QStringLiteral("endpoint"), entry[1]);
            source.insert(QStringLiteral("attribution"), entry[2]);
            source.insert(QStringLiteral("format"), dataId());
            result.append(source);
        }
        return result;
    }
    DataDocument decode(const QByteArray &bytes, const std::atomic_bool &cancelled) const override
    {
        DataDocument result;
        if (bytes.isEmpty() || bytes.size() > DataDownloadLimit) {
            result.error = tr("Empty or oversized data response.");
            return result;
        }
        initializeGdal();
        const bool archive = bytes.startsWith("PK");
        if (!archive && declaresXmlDocumentType(bytes)) {
            result.error = tr("XML declarations and external entities are not allowed.");
            return result;
        }
        const GdalMemoryFile file(bytes, archive ? QStringLiteral("zip") : suffixFor(bytes));
        if (!file.isValid()) {
            result.error = tr("Cannot hold the data in memory.");
            return result;
        }
        QString path = QString::fromUtf8(file.path());
        if (archive) {
            path = archiveDataset(file.path(), result.error);
            if (!result.error.isEmpty() || !checkArchivedXml(path, result)) {
                return result;
            }
        }
        result = readDataset(path, cancelled);
        if (result.error.isEmpty() && !archive) {
            countKmlSkips(bytes, result);
        }
        return result;
    }
    DataDocument decodeFile(const QString &path, const std::atomic_bool &cancelled) const override
    {
        DataDocument result;
        const QFileInfo info(path);
        const bool directory =
            info.isDir() && info.suffix().compare(u"gdb", Qt::CaseInsensitive) == 0;
        if (!info.exists() || (!info.isFile() && !directory)) {
            result.error = tr("The local data file does not exist or is not a regular file.");
            return result;
        }
        initializeGdal();
        const auto suffix = info.suffix().toLower();
        if (suffix == u"zip" || suffix == u"kmz") {
            const auto dataset = archiveDataset(path.toUtf8(), result.error);
            if (!result.error.isEmpty() || !checkArchivedXml(dataset, result)) {
                return result;
            }
            return readDataset(dataset, cancelled);
        }
        if (info.isFile() && xmlSuffix(path)) {
            QFile input(path);
            if (!input.open(QIODevice::ReadOnly)) {
                result.error = tr("Cannot read the local data file.");
                return result;
            }
            if (declaresXmlDocumentType(input.read(4096))) {
                result.error = tr("XML declarations and external entities are not allowed.");
                return result;
            }
        }
        result = readDataset(path, cancelled);
        if (result.error.isEmpty() && suffix == u"kml" && info.size() <= DataXmlLimit) {
            QFile input(path);
            if (input.open(QIODevice::ReadOnly)) {
                countKmlSkips(input.read(DataXmlLimit), result);
            }
        }
        return result;
    }

private:
    static bool checkArchivedXml(const QString &path, DataDocument &result)
    {
        if (!xmlSuffix(path)) {
            return true;
        }
        VSILFILE *file = VSIFOpenL(path.toUtf8().constData(), "rb");
        if (!file) {
            result.error = tr("Cannot read the archived dataset.");
            return false;
        }
        QByteArray head(4096, '\0');
        const auto amount = VSIFReadL(head.data(), 1, static_cast<size_t>(head.size()), file);
        VSIFCloseL(file);
        head.resize(static_cast<qsizetype>(amount));
        if (declaresXmlDocumentType(head)) {
            result.error = tr("XML declarations and external entities are not allowed.");
            return false;
        }
        return true;
    }
};
} // namespace swak
#include "gdal_vector.moc"
