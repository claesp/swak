#include "core/data.h"
#include "core/interfaces.h"
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cmath>

namespace swak {
namespace {
QByteArray unpack(const QByteArray &bytes, QString &error, const std::atomic_bool &cancelled)
{
    if (!bytes.startsWith("PK")) return bytes;
    std::unique_ptr<archive, decltype(&archive_read_free)> input(archive_read_new(), archive_read_free);
    if (!input) { error = QObject::tr("Cannot open the KMZ archive."); return {}; }
    archive_read_support_format_zip_seekable(input.get());
    if (archive_read_open_memory(input.get(), bytes.constData(), static_cast<size_t>(bytes.size())) != ARCHIVE_OK) {
        error = QObject::tr("Invalid KMZ archive."); return {};
    }
    QByteArray xml;
    archive_entry *entry = nullptr;
    int count = 0;
    qint64 expanded = 0;
    int result = ARCHIVE_OK;
    while ((result = archive_read_next_header(input.get(), &entry)) == ARCHIVE_OK) {
        if (cancelled || ++count > 256) { error = QObject::tr("KMZ processing stopped or exceeded its entry limit."); return {}; }
        const auto *path = archive_entry_pathname_utf8(entry);
        const QString name = path ? QString::fromUtf8(path) : QString{};
        const auto size = archive_entry_size(entry);
        if (name.isEmpty() || name.size() > 1024 || name.startsWith('/') || name.contains('\\')
            || name.split('/').contains(QStringLiteral("..")) || archive_entry_is_encrypted(entry)
            || archive_entry_symlink(entry) || archive_entry_hardlink(entry) || size < 0 || size > DataXmlLimit
            || (archive_entry_filetype(entry) != AE_IFREG && archive_entry_filetype(entry) != AE_IFDIR)) {
            error = QObject::tr("Unsafe or oversized KMZ entry."); return {};
        }
        expanded += size;
        if (expanded > 32 * 1024 * 1024) { error = QObject::tr("KMZ expanded size exceeds its limit."); return {}; }
        if (!name.endsWith(QStringLiteral(".kml"), Qt::CaseInsensitive)) {
            if (archive_read_data_skip(input.get()) != ARCHIVE_OK) { error = QObject::tr("Invalid KMZ entry."); return {}; }
            continue;
        }
        if (!xml.isEmpty()) { error = QObject::tr("Use a KMZ archive with one KML document."); return {}; }
        std::array<char, 32768> buffer{};
        la_ssize_t amount = 0;
        while ((amount = archive_read_data(input.get(), buffer.data(), buffer.size())) > 0) {
            if (cancelled || xml.size() + amount > DataXmlLimit) { error = QObject::tr("KML data exceeds its limit or was cancelled."); return {}; }
            xml.append(buffer.data(), static_cast<qsizetype>(amount));
        }
        if (amount < 0) { error = QObject::tr("KMZ decompression failed."); return {}; }
    }
    if (result != ARCHIVE_EOF || xml.isEmpty()) error = QObject::tr("KMZ has no valid KML document.");
    return xml;
}
struct Frame { QString tag; QString text; bool visible = true; QString property; };
DataDocument parse(const QByteArray &bytes, const std::atomic_bool &cancelled)
{
    DataDocument result;
    if (bytes.isEmpty() || bytes.size() > DataDownloadLimit) { result.error = QObject::tr("Empty or oversized data response."); return result; }
    const auto xml = unpack(bytes, result.error, cancelled);
    if (!result.error.isEmpty()) return result;
    QXmlStreamReader reader(xml);
    QList<Frame> stack;
    QList<DataShape> feature;
    QString name;
    QString description;
    QJsonObject properties;
    qsizetype retainedMetadata = 0;
    DataShape shape;
    int geometryDepth = 0;
    bool geometryCoordinates = false;
    int featureDepth = 0;
    int coordinates = 0;
    int elements = 0;
    int features = 0;
    bool root = false;
    auto fail = [&](const QString &message) { result.error = message; };
    while (!reader.atEnd() && result.error.isEmpty()) {
        reader.readNext();
        if (cancelled) { fail(QObject::tr("Data parsing cancelled.")); break; }
        if (reader.isDTD() || reader.isEntityReference() || reader.isProcessingInstruction()) {
            fail(QObject::tr("KML declarations and external entities are not allowed.")); break;
        }
        if (reader.isStartElement()) {
            auto tag = reader.name().toString();
            const auto namespaceUri = reader.namespaceUri();
            if (root && namespaceUri != u"http://www.opengis.net/kml/2.2" && namespaceUri != u"http://earth.google.com/kml/2.1"
                && namespaceUri != u"http://earth.google.com/kml/2.0" && namespaceUri != u"http://earth.google.com/kml/2.2") {
                if (tag == QStringLiteral("Track")) ++result.ignoredOverlays;
                tag.prepend(QStringLiteral("foreign:"));
            }
            if (++elements > 200000 || stack.size() >= 32 || reader.attributes().size() > 32) {
                fail(QObject::tr("KML structure exceeds its limit.")); break;
            }
            if (!root) {
                root = true;
                const auto ns = reader.namespaceUri();
                if (tag != QStringLiteral("kml") || (ns != u"http://www.opengis.net/kml/2.2"
                    && ns != u"http://earth.google.com/kml/2.1" && ns != u"http://earth.google.com/kml/2.0" && ns != u"http://earth.google.com/kml/2.2")) {
                    fail(QObject::tr("Use a KML document with a supported namespace.")); break;
                }
            }
            const auto propertyName = reader.attributes().value(QStringLiteral("name")).toString();
            if (propertyName.size() > 128) { fail(QObject::tr("Data property name is too long.")); break; }
            stack.append({tag, {}, true, propertyName});
            if (tag == QStringLiteral("NetworkLink")) ++result.ignoredLinks;
            if (tag.endsWith(QStringLiteral("Overlay")) || tag == QStringLiteral("Model") || tag == QStringLiteral("Track")) ++result.ignoredOverlays;
            if (tag == QStringLiteral("Placemark")) {
                if (featureDepth || ++features > 10000) { fail(QObject::tr("KML placemarks exceed their limit.")); break; }
                featureDepth = static_cast<int>(stack.size()); feature.clear(); name.clear(); description.clear(); properties = {};
                for (const auto &attribute : reader.attributes()) {
                    if (properties.size() >= 256 || attribute.name().size() > 128 || attribute.value().size() > 4096) { fail(QObject::tr("KML property exceeds its limit.")); break; }
                    properties.insert(attribute.name().toString(), attribute.value().toString());
                }
            }
            if (featureDepth && (tag == QStringLiteral("Point") || tag == QStringLiteral("LineString") || tag == QStringLiteral("Polygon"))) {
                if (geometryDepth) { fail(QObject::tr("Nested KML geometry is invalid.")); break; }
                geometryDepth = static_cast<int>(stack.size()); shape = {}; geometryCoordinates = false;
                shape.kind = tag == QStringLiteral("Point") ? DataShape::Kind::Point : tag == QStringLiteral("LineString") ? DataShape::Kind::Line : DataShape::Kind::Polygon;
                shape.path.setFillRule(Qt::OddEvenFill);
            }
        } else if (reader.isCharacters() && !stack.isEmpty()) {
            auto &frame = stack.last();
            if (featureDepth || frame.tag == QStringLiteral("visibility")) {
                const auto limit = frame.tag == QStringLiteral("coordinates") ? DataXmlLimit : 4096;
                if (frame.text.size() + reader.text().size() > limit) { fail(QObject::tr("KML text exceeds its limit.")); break; }
                frame.text += reader.text();
            }
        } else if (reader.isEndElement() && !stack.isEmpty()) {
            const auto &frame = stack.last();
            if (frame.tag == QStringLiteral("visibility") && frame.text.trimmed() == QStringLiteral("0") && stack.size() > 1) stack[stack.size() - 2].visible = false;
            if (featureDepth && stack.size() == featureDepth + 1 && frame.tag == QStringLiteral("name")) name = frame.text.trimmed().left(128);
            if (featureDepth && stack.size() == featureDepth + 1 && frame.tag == QStringLiteral("description")) description = frame.text;
            if (featureDepth && frame.tag != QStringLiteral("coordinates") && !frame.text.trimmed().isEmpty()
                && frame.tag != QStringLiteral("name") && frame.tag != QStringLiteral("description")) {
                QString key = frame.property.isEmpty() ? frame.tag : frame.property;
                if (frame.tag == QStringLiteral("value") && stack.size() > 1 && !stack[stack.size() - 2].property.isEmpty()) key = stack[stack.size() - 2].property;
                if (properties.size() >= 256 || key.size() > 128) { fail(QObject::tr("KML properties exceed their limit.")); break; }
                properties.insert(key, frame.text.trimmed());
            }
            if (geometryDepth && frame.tag == QStringLiteral("coordinates")) {
                QStringView remaining(frame.text);
                QPolygonF points;
                while (!remaining.isEmpty()) {
                    if (cancelled) { fail(QObject::tr("Data parsing cancelled.")); break; }
                    if (remaining.front().isSpace()) { remaining = remaining.sliced(1); continue; }
                    qsizetype end = 0;
                    while (end < remaining.size() && !remaining[end].isSpace() && end <= 192) ++end;
                    if (end > 192 || ++coordinates > 100000) { fail(QObject::tr("KML coordinates exceed their limit.")); break; }
                    const auto parts = remaining.first(end).toString().split(',');
                    remaining = remaining.sliced(end);
                    bool lonOk = false, latOk = false, altOk = true;
                    const double lon = parts.value(0).toDouble(&lonOk), lat = parts.value(1).toDouble(&latOk);
                    const double altitude = parts.size() == 3 ? parts[2].toDouble(&altOk) : 0;
                    if (parts.size() < 2 || parts.size() > 3 || !lonOk || !latOk || !altOk || !std::isfinite(lon)
                        || !std::isfinite(lat) || !std::isfinite(altitude) || std::abs(lon) > 180 || std::abs(lat) > 90) {
                        fail(QObject::tr("Invalid KML coordinate.")); break;
                    }
                    if (shape.kind == DataShape::Kind::Point) {
                        shape.properties.insert(QStringLiteral("longitude"), lon); shape.properties.insert(QStringLiteral("latitude"), lat);
                        if (parts.size() == 3) shape.properties.insert(QStringLiteral("altitude"), altitude);
                    }
                    QPointF point = project(lat, lon);
                    if (!points.isEmpty()) point.rx() += std::round(points.last().x() - point.x());
                    if (std::abs(point.x()) > 2) { fail(QObject::tr("KML geometry wraps too many times.")); break; }
                    points.append(point);
                }
                if (!result.error.isEmpty()) break;
                if (shape.kind != DataShape::Kind::Polygon && geometryCoordinates) { fail(QObject::tr("Duplicate KML coordinates.")); break; }
                if (shape.kind == DataShape::Kind::Polygon && !shape.path.isEmpty() && !points.isEmpty()) {
                    const double offset = std::round(shape.path.boundingRect().center().x() - points.first().x());
                    for (auto &point : points) point.rx() += offset;
                }
                geometryCoordinates = true;
                if (shape.kind == DataShape::Kind::Point) {
                    if (points.size() != 1) { fail(QObject::tr("A KML point needs one coordinate.")); break; }
                    shape.point = points.first();
                } else {
                    if (points.size() < (shape.kind == DataShape::Kind::Polygon ? 4 : 2)) { fail(QObject::tr("Incomplete KML line or ring.")); break; }
                    shape.path.moveTo(points.first());
                    for (qsizetype i = 1; i < points.size(); ++i) shape.path.lineTo(points[i]);
                    if (shape.kind == DataShape::Kind::Polygon) {
                        if (points.first() != points.last()) { fail(QObject::tr("A KML polygon ring must be closed.")); break; }
                        shape.path.closeSubpath();
                    }
                    shape.point = points.first();
                }
            }
            if (geometryDepth == stack.size()) {
                if (!geometryCoordinates) { fail(QObject::tr("KML geometry has no coordinates.")); break; }
                feature.append(shape); geometryDepth = 0;
            }
            if (featureDepth == stack.size()) {
                bool visible = true;
                for (const auto &parent : stack) visible = visible && parent.visible;
                if (result.shapes.size() + feature.size() > 10000) { fail(QObject::tr("KML geometry exceeds its limit.")); break; }
                if (visible) for (auto &item : feature) {
                    item.name = name; item.description = description;
                    for (auto value = properties.begin(); value != properties.end(); ++value) item.properties.insert(value.key(), value.value());
                    retainedMetadata += (name.size() + description.size()) * 2;
                    for (auto value = item.properties.begin(); value != item.properties.end(); ++value)
                        retainedMetadata += (value.key().size() + value.value().toString().size()) * 2 + 64;
                    if (retainedMetadata > 8 * 1024 * 1024) { fail(QObject::tr("KML metadata exceeds its limit.")); break; }
                    result.shapes.append(std::move(item));
                }
                featureDepth = 0;
            }
            stack.removeLast();
        }
    }
    if (reader.hasError() || !root) result.error = QObject::tr("Malformed KML document.");
    if (!result.error.isEmpty()) result.shapes.clear();
    return result;
}
}
class KmlPlugin final : public QObject, public Plugin, public DataProvider {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "kml/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::DataProvider)
public:
    QString dataId() const override { return QStringLiteral("org.swak.kml"); }
    QString dataName() const override { return tr("KML and KMZ data"); }
    QList<QJsonObject> presets() const override
    {
        QList<QJsonObject> result;
        for (const auto &entry : {QStringList{tr("USGS earthquakes, M2.5+, past day"), QStringLiteral("https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day_age.kml"), QStringLiteral("U.S. Geological Survey | https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits")},
             QStringList{tr("NOAA NDBC marine observations"), QStringLiteral("https://www.ndbc.noaa.gov/kml/marineobs_as_kml.php"), QStringLiteral("NOAA / NDBC and station operators | https://www.weather.gov/disclaimer")}}) {
            auto source = dataDefaults();
            source.insert(QStringLiteral("name"), entry[0]); source.insert(QStringLiteral("endpoint"), entry[1]);
            source.insert(QStringLiteral("attribution"), entry[2]); source.insert(QStringLiteral("format"), dataId());
            result.append(source);
        }
        return result;
    }
    DataDocument decode(const QByteArray &bytes, const std::atomic_bool &cancelled) const override { return parse(bytes, cancelled); }
};
}
#include "kml.moc"
