#include "core/gdal_support.h"
#include "core/interfaces.h"
#include "core/maps.h"
#include "core/vector_tiles.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <exception>
#include <gdal_priv.h>
#include <ogr_geometry.h>
#include <ogrsf_frmts.h>
#include <stdexcept>
#include <utility>

namespace swak {
namespace {
struct TileLimit : std::runtime_error
{
    TileLimit() : std::runtime_error("Vector tile resource limit") {}
};
void bounded(bool valid)
{
    if (!valid) {
        throw TileLimit{};
    }
}
void require(bool valid)
{
    if (!valid) {
        throw std::runtime_error("Invalid vector tile");
    }
}
// GDAL places the tile at 0/0/0 in Web Mercator meters. These values map it back to the
// tile-local unit square with y down. GDAL applies the layer extent itself.
constexpr double MercatorHalfWorld = 20037508.342789244;
QPointF tilePoint(const OGRPoint &point)
{
    const double x = (point.getX() + MercatorHalfWorld) / (2 * MercatorHalfWorld);
    const double y = (MercatorHalfWorld - point.getY()) / (2 * MercatorHalfWorld);
    require(std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 16 && std::abs(y) <= 16);
    return {x, y};
}
QString printable(const char *text, qsizetype limit)
{
    auto result = QString::fromUtf8(text ? text : "");
    bounded(result.size() <= limit);
    for (auto &character : result) {
        if (!character.isPrint()) {
            character = QLatin1Char(' ');
        }
    }
    return result;
}
QJsonValue fieldValue(const OGRFeature &feature, int index)
{
    const auto *definition = feature.GetFieldDefnRef(index);
    switch (definition->GetType()) {
    case OFTReal: {
        const double number = feature.GetFieldAsDouble(index);
        require(std::isfinite(number));
        return number;
    }
    case OFTInteger:
        if (definition->GetSubType() == OFSTBoolean) {
            return feature.GetFieldAsInteger(index) != 0;
        }
        return QString::number(feature.GetFieldAsInteger(index));
    case OFTInteger64:
        return QString::number(feature.GetFieldAsInteger64(index));
    default:
        return printable(feature.GetFieldAsString(index), 2048);
    }
}
void appendLine(QPainterPath &path,
                const OGRSimpleCurve &curve,
                int &vertices,
                const std::atomic_bool &cancelled)
{
    for (int i = 0; i < curve.getNumPoints(); ++i) {
        require(!cancelled.load(std::memory_order_relaxed));
        bounded(++vertices <= 500000);
        OGRPoint point;
        curve.getPoint(i, &point);
        const auto local = tilePoint(point);
        if (i == 0) {
            path.moveTo(local);
        } else {
            path.lineTo(local);
        }
    }
}
void appendGeometry(VectorFeature &feature,
                    const OGRGeometry &geometry,
                    int &vertices,
                    const std::atomic_bool &cancelled,
                    int depth = 0)
{
    require(depth <= 2);
    switch (wkbFlatten(geometry.getGeometryType())) {
    case wkbPoint:
        require(feature.kind == VectorFeature::Kind::Point);
        bounded(++vertices <= 500000);
        feature.points.append(tilePoint(*geometry.toPoint()));
        break;
    case wkbLineString:
        require(feature.kind == VectorFeature::Kind::Line);
        appendLine(feature.path, *geometry.toLineString(), vertices, cancelled);
        break;
    case wkbPolygon:
        require(feature.kind == VectorFeature::Kind::Polygon);
        for (const auto *ring : *geometry.toPolygon()) {
            require(ring->getNumPoints() >= 4);
            appendLine(feature.path, *ring, vertices, cancelled);
            feature.path.closeSubpath();
        }
        break;
    case wkbMultiPoint:
    case wkbMultiLineString:
    case wkbMultiPolygon:
        for (const auto *part : *geometry.toGeometryCollection()) {
            appendGeometry(feature, *part, vertices, cancelled, depth + 1);
        }
        break;
    default:
        require(false);
    }
}
VectorFeature::Kind featureKind(const OGRGeometry &geometry)
{
    switch (wkbFlatten(geometry.getGeometryType())) {
    case wkbPoint:
    case wkbMultiPoint:
        return VectorFeature::Kind::Point;
    case wkbLineString:
    case wkbMultiLineString:
        return VectorFeature::Kind::Line;
    case wkbPolygon:
    case wkbMultiPolygon:
        return VectorFeature::Kind::Polygon;
    default:
        throw std::runtime_error("Invalid vector tile");
    }
}
QColor color(const VectorFeature &feature)
{
    const auto &layer = feature.layer;
    if (layer.startsWith(QLatin1String("water"))) {
        return QColor("#92bfd0");
    }
    if (layer == QLatin1String("landcover") || layer == QLatin1String("park")) {
        return QColor("#bdcfa7");
    }
    if (layer == QLatin1String("landuse")) {
        return QColor("#d8d9bc");
    }
    if (layer == QLatin1String("building")) {
        return QColor("#c6bdb1");
    }
    if (layer == QLatin1String("boundary")) {
        return QColor("#a089a5");
    }
    if (layer.startsWith(QLatin1String("transportation"))) {
        const auto road = feature.properties.value(QStringLiteral("class")).toString();
        if (road == QLatin1String("ferry")) {
            return QColor("#7296aa");
        }
        return road == QLatin1String("motorway") || road == QLatin1String("trunk")
                   ? QColor("#e6b36b")
                   : QColor("#ffffff");
    }
    if (layer.startsWith(QLatin1String("aero"))) {
        return QColor("#b5bac0");
    }
    return feature.kind == VectorFeature::Kind::Point ? QColor("#785644") : QColor("#a2ada5");
}
int order(const VectorFeature &feature)
{
    static const QStringList layers{QStringLiteral("landcover"),
                                    QStringLiteral("landuse"),
                                    QStringLiteral("park"),
                                    QStringLiteral("water"),
                                    QStringLiteral("waterway"),
                                    QStringLiteral("building"),
                                    QStringLiteral("aeroway"),
                                    QStringLiteral("transportation"),
                                    QStringLiteral("boundary")};
    const auto index = layers.indexOf(feature.layer);
    return index >= 0                                   ? static_cast<int>(index)
           : feature.kind == VectorFeature::Kind::Point ? 20
                                                        : 10;
}
VectorTile decode(const QByteArray &bytes, const std::atomic_bool &cancelled)
{
    VectorTile result;
    bounded(bytes.size() <= VectorDownloadLimit);
    require(!bytes.isEmpty());
    initializeGdal();
    const GdalMemoryFile file(bytes, QStringLiteral("pbf"));
    require(file.isValid());
    static const char *const drivers[] = {"MVT", nullptr};
    // Keep the tile buffer. The renderer clips each tile itself.
    static const char *const options[] = {"X=0", "Y=0", "Z=0", "CLIP=NO", nullptr};
    CPLErrorReset();
    GdalDataset dataset(GDALDataset::FromHandle(GDALOpenEx(
        file.path().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, drivers, options, nullptr)));
    require(dataset != nullptr);
    int vertices = 0;
    qsizetype retained = 0;
    for (auto *layer : dataset->GetLayers()) {
        require(!cancelled.load(std::memory_order_relaxed));
        bounded(result.layers.size() < 64);
        const auto name = printable(layer->GetName(), 100);
        require(!name.isEmpty() && !result.layers.contains(name));
        result.layers.append(name);
        const auto *definition = layer->GetLayerDefn();
        const int identifier = definition->GetFieldIndex("mvt_id");
        layer->ResetReading();
        while (OGRFeatureUniquePtr input{layer->GetNextFeature()}) {
            require(!cancelled.load(std::memory_order_relaxed));
            bounded(result.features.size() < 50000);
            const auto *geometry = input->GetGeometryRef();
            if (!geometry || geometry->IsEmpty()) {
                continue;
            }
            VectorFeature feature;
            feature.layer = name;
            feature.kind = featureKind(*geometry);
            for (int i = 0; i < definition->GetFieldCount(); ++i) {
                if (!input->IsFieldSetAndNotNull(i)) {
                    continue;
                }
                if (i == identifier) {
                    feature.id = QString::number(input->GetFieldAsInteger64(i));
                    continue;
                }
                const auto key = printable(definition->GetFieldDefn(i)->GetNameRef(), 100);
                const auto value = fieldValue(*input, i);
                retained += 128 + key.size() * 2 + value.toString().size() * 2;
                bounded(retained <= 16 * 1024 * 1024);
                feature.properties.insert(key, value);
            }
            appendGeometry(feature, *geometry, vertices, cancelled);
            feature.path.setFillRule(Qt::OddEvenFill);
            bounded(retained + vertices * 64 + (result.features.size() + 1) * 256 <=
                    64 * 1024 * 1024);
            feature.color = color(feature);
            feature.labelOnly =
                name.endsWith(QLatin1String("_name")) || name.endsWith(QLatin1String("_label")) ||
                name == QLatin1String("place") || name == QLatin1String("housenumber");
            result.features.append(std::move(feature));
        }
    }
    std::stable_sort(
        result.features.begin(), result.features.end(), [](const auto &a, const auto &b) {
            return order(a) < order(b);
        });
    result.costKiB =
        static_cast<int>((retained + vertices * 64 + result.features.size() * 256) / 1024 + 1);
    return result;
}
} // namespace
class MvtProvider final : public QObject,
                          public Plugin,
                          public MapProvider,
                          public VectorTileProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "mvt/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider swak::VectorTileProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("org.swak.mvt");
    }
    QString mapName() const override
    {
        return tr("MVT vector tiles (OpenMapTiles)");
    }
    QStringList layerNames() const override
    {
        return {QStringLiteral("aerodrome_label"),
                QStringLiteral("aeroway"),
                QStringLiteral("boundary"),
                QStringLiteral("building"),
                QStringLiteral("housenumber"),
                QStringLiteral("landcover"),
                QStringLiteral("landuse"),
                QStringLiteral("mountain_peak"),
                QStringLiteral("park"),
                QStringLiteral("place"),
                QStringLiteral("poi"),
                QStringLiteral("transportation"),
                QStringLiteral("transportation_name"),
                QStringLiteral("water"),
                QStringLiteral("water_name"),
                QStringLiteral("waterway")};
    }
    QJsonObject defaults() const override
    {
        auto result = mapDefaults(
            tr("OpenFreeMap"),
            QStringLiteral("https://tiles.openfreemap.org/planet/latest/{z}/{x}/{y}.pbf"),
            QStringLiteral(
                "OpenFreeMap © OpenMapTiles Data from OpenStreetMap | https://openfreemap.org | "
                "https://openmaptiles.org | https://www.openstreetmap.org/copyright"),
            14);
        result.insert(QStringLiteral("labels"), true);
        result.insert(QStringLiteral("labelSize"), 12);
        result.insert(QStringLiteral("lineWidth"), 1.5);
        result.insert(QStringLiteral("pointRadius"), 3);
        result.insert(QStringLiteral("language"), QString{});
        result.insert(QStringLiteral("visibleLayers"), QJsonObject{});
        QJsonObject visible;
        for (const auto &layer : layerNames()) {
            visible.insert(layer, true);
        }
        result.insert(QStringLiteral("visibleLayers"), visible);
        return result;
    }
    QList<SettingField> fields() const override
    {
        auto result = commonMapFields();
        result.append(booleanSetting(QStringLiteral("labels"), tr("Show map labels")));
        result.append(numberSetting(QStringLiteral("labelSize"), tr("Label size (pixels)"), 8, 24));
        result.append(
            numberSetting(QStringLiteral("lineWidth"), tr("Line width (pixels)"), 0.5, 6, 1));
        result.append(
            numberSetting(QStringLiteral("pointRadius"), tr("Point radius (pixels)"), 1, 10, 1));
        result.append(textSetting(
            QStringLiteral("language"), tr("Label language code (empty uses local names)"), 16));
        for (const auto &layer : layerNames()) {
            result.append(booleanSetting(QStringLiteral("visibleLayers/") + layer,
                                         tr("Show layer: %1").arg(layer)));
        }
        return result;
    }
    QString validate(const QJsonObject &source) const override
    {
        auto error = validateMapSource(source);
        if (!error.isEmpty()) {
            return error;
        }
        const auto endpoint = source.value(QStringLiteral("endpoint")).toString();
        for (const auto *part : {"{z}", "{x}", "{y}"}) {
            if (!endpoint.contains(QLatin1String(part))) {
                return tr("The MVT URL needs {z}, {x}, and {y}.");
            }
        }
        for (const auto &field : fields()) {
            if (field.kind == SettingField::Kind::Number && source.contains(field.path)) {
                const double value = source.value(field.path).toDouble(-1);
                if (!std::isfinite(value) || value < field.minimum || value > field.maximum ||
                    (field.decimals == 0 && std::floor(value) != value)) {
                    return tr("A vector rendering value is outside its limits.");
                }
            }
        }
        static const QRegularExpression language(QStringLiteral("\\A[a-zA-Z0-9_-]{0,16}\\z"));
        if (!language.match(source.value(QStringLiteral("language")).toString()).hasMatch()) {
            return tr("Use a short language code, such as en or sv.");
        }
        const auto visible = source.value(QStringLiteral("visibleLayers")).toObject();
        if (visible.size() > 64) {
            return tr("Use at most 64 layer controls.");
        }
        for (auto it = visible.begin(); it != visible.end(); ++it) {
            if (it.key().isEmpty() || it.key().size() > 100 || !it.value().isBool()) {
                return tr("A layer control is invalid.");
            }
        }
        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        if (!validTile(z, x, y) || !validate(source).isEmpty()) {
            return {};
        }
        return QUrl(source.value(QStringLiteral("endpoint"))
                        .toString()
                        .replace(QStringLiteral("{z}"), QString::number(z))
                        .replace(QStringLiteral("{x}"), QString::number(x))
                        .replace(QStringLiteral("{y}"), QString::number(y)));
    }
    VectorTile decodeTile(const QByteArray &bytes, const std::atomic_bool &cancelled) const override
    {
        try {
            return decode(bytes, cancelled);
        } catch (const TileLimit &) {
            VectorTile result;
            result.resourceLimit = true;
            result.error = tr("Vector tile exceeds the resource limit.");
            return result;
        } catch (const std::exception &) {
            VectorTile result;
            result.error = (cancelled.load() ? tr("Vector tile decoding cancelled.")
                                             : tr("Vector tile is invalid."));
            return result;
        }
    }
};
} // namespace swak
#include "mvt.moc"
