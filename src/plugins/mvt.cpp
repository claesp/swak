#include "core/interfaces.h"
#include "core/maps.h"
#include "core/vector_tiles.h"
#include <protozero/pbf_reader.hpp>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace swak {
namespace {
using Reader = protozero::pbf_reader;
using Wire = protozero::pbf_wire_type;
struct TileLimit : std::runtime_error { TileLimit() : std::runtime_error("Vector tile resource limit") {} };
void bounded(bool valid) { if (!valid) throw TileLimit{}; }
void require(bool valid) { if (!valid) throw std::runtime_error("Invalid vector tile"); }
void wire(const Reader &reader, Wire expected) { require(reader.has_wire_type(expected)); }
quint32 unsigned32(Reader &reader)
{
    wire(reader, Wire::varint);
    const auto value = reader.get_uint64();
    require(value <= std::numeric_limits<quint32>::max());
    return static_cast<quint32>(value);
}
QString string(protozero::data_view value, size_t limit = 2048)
{
    bounded(value.size() <= limit);
    auto result = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    for (auto &character : result) if (!character.isPrint()) character = QLatin1Char(' ');
    return result;
}
QJsonValue property(Reader reader)
{
    QJsonValue result;
    bool found = false;
    while (reader.next()) {
        if (reader.tag() < 1 || reader.tag() > 7) { reader.skip(); continue; }
        require(!found);
        found = true;
        switch (reader.tag()) {
        case 1: wire(reader, Wire::length_delimited); result = string(reader.get_view()); break;
        case 2: { wire(reader, Wire::fixed32); const auto number = reader.get_float(); require(std::isfinite(number)); result = number; break; }
        case 3: { wire(reader, Wire::fixed64); const auto number = reader.get_double(); require(std::isfinite(number)); result = number; break; }
        case 4: wire(reader, Wire::varint); result = QString::number(reader.get_int64()); break;
        case 5: wire(reader, Wire::varint); result = QString::number(reader.get_uint64()); break;
        case 6: wire(reader, Wire::varint); result = QString::number(reader.get_sint64()); break;
        case 7: wire(reader, Wire::varint); result = reader.get_bool(); break;
        default: break;
        }
    }
    require(found);
    return result;
}
QList<quint32> packed(Reader &reader, int limit)
{
    QList<quint32> result;
    if (reader.has_wire_type(Wire::varint)) { result.append(unsigned32(reader)); return result; }
    wire(reader, Wire::length_delimited);
    for (const auto value : reader.get_packed_uint64()) { bounded(result.size() < limit); require(value <= std::numeric_limits<quint32>::max()); result.append(static_cast<quint32>(value)); }
    return result;
}
void geometry(VectorFeature &feature, const QList<quint32> &commands, quint32 extent, int &vertices, const std::atomic_bool &cancelled)
{
    qint64 x = 0, y = 0;
    int segment = 0;
    bool closed = true;
    const auto delta = [](quint32 value) -> qint64 { return (value & 1U) ? -static_cast<qint64>(value >> 1U) - 1 : value >> 1U; };
    for (qsizetype offset = 0; offset < commands.size();) {
        require(!cancelled.load(std::memory_order_relaxed));
        const quint32 command = commands.at(offset++);
        const quint32 operation = command & 7U;
        const quint32 count = command >> 3U;
        require(count > 0); bounded(count <= 500000);
        if (operation == 7) {
            require(feature.kind == VectorFeature::Kind::Polygon && count == 1 && segment >= 3 && !closed);
            feature.path.closeSubpath(); closed = true; segment = 0; continue;
        }
        require(operation == 1 || operation == 2);
        require(count <= static_cast<quint32>((commands.size() - offset) / 2));
        if (feature.kind == VectorFeature::Kind::Point) require(operation == 1);
        else if (operation == 1) {
            require(count == 1 && (feature.kind == VectorFeature::Kind::Polygon ? closed : segment == 0 || segment >= 2));
            segment = 0;
        } else require(segment > 0 && !closed);
        for (quint32 i = 0; i < count; ++i) {
            bounded(++vertices <= 500000);
            x += delta(commands.at(offset++)); y += delta(commands.at(offset++));
            require(std::abs(x) <= static_cast<qint64>(extent) * 16 && std::abs(y) <= static_cast<qint64>(extent) * 16);
            const QPointF point(static_cast<double>(x) / extent, static_cast<double>(y) / extent);
            if (feature.kind == VectorFeature::Kind::Point) feature.points.append(point);
            else if (operation == 1) feature.path.moveTo(point);
            else feature.path.lineTo(point);
            ++segment; closed = false;
        }
    }
    require(feature.kind == VectorFeature::Kind::Point ? !feature.points.isEmpty() :
        feature.kind == VectorFeature::Kind::Line ? segment >= 2 : closed && !feature.path.isEmpty());
    feature.path.setFillRule(Qt::OddEvenFill);
}
QColor color(const VectorFeature &feature)
{
    const auto &layer = feature.layer;
    if (layer.startsWith(QLatin1String("water"))) return QColor("#92bfd0");
    if (layer == QLatin1String("landcover") || layer == QLatin1String("park")) return QColor("#bdcfa7");
    if (layer == QLatin1String("landuse")) return QColor("#d8d9bc");
    if (layer == QLatin1String("building")) return QColor("#c6bdb1");
    if (layer == QLatin1String("boundary")) return QColor("#a089a5");
    if (layer.startsWith(QLatin1String("transportation"))) {
        const auto road = feature.properties.value(QStringLiteral("class")).toString();
        if (road == QLatin1String("ferry")) return QColor("#7296aa");
        return road == QLatin1String("motorway") || road == QLatin1String("trunk") ? QColor("#e6b36b") : QColor("#ffffff");
    }
    if (layer.startsWith(QLatin1String("aero"))) return QColor("#b5bac0");
    return feature.kind == VectorFeature::Kind::Point ? QColor("#785644") : QColor("#a2ada5");
}
int order(const VectorFeature &feature)
{
    static const QStringList layers{QStringLiteral("landcover"), QStringLiteral("landuse"), QStringLiteral("park"),
        QStringLiteral("water"), QStringLiteral("waterway"), QStringLiteral("building"), QStringLiteral("aeroway"),
        QStringLiteral("transportation"), QStringLiteral("boundary")};
    const auto index = layers.indexOf(feature.layer);
    return index >= 0 ? static_cast<int>(index) : feature.kind == VectorFeature::Kind::Point ? 20 : 10;
}
VectorTile decode(const QByteArray &bytes, const std::atomic_bool &cancelled)
{
    VectorTile result;
    bounded(bytes.size() <= VectorDownloadLimit);
    Reader tile(bytes.constData(), static_cast<size_t>(bytes.size()));
    int vertices = 0;
    qsizetype retained = 0;
    while (tile.next()) {
        require(!cancelled.load(std::memory_order_relaxed));
        if (tile.tag() != 3) { tile.skip(); continue; }
        wire(tile, Wire::length_delimited);
        Reader layer(tile.get_view());
        QString name;
        quint32 extent = 4096, version = 0;
        QList<protozero::data_view> features;
        QStringList keys;
        QList<QJsonValue> values;
        bounded(result.layers.size() < 64);
        while (layer.next()) {
            require(!cancelled.load(std::memory_order_relaxed));
            switch (layer.tag()) {
            case 1: wire(layer, Wire::length_delimited); require(name.isEmpty()); name = string(layer.get_view(), 100); break;
            case 2: wire(layer, Wire::length_delimited); bounded(features.size() + result.features.size() < 50000); features.append(layer.get_view()); break;
            case 3: wire(layer, Wire::length_delimited); bounded(keys.size() < 65536); keys.append(string(layer.get_view(), 100)); break;
            case 4: wire(layer, Wire::length_delimited); bounded(values.size() < 65536); values.append(property(Reader(layer.get_view()))); break;
            case 5: wire(layer, Wire::varint); extent = unsigned32(layer); break;
            case 15: wire(layer, Wire::varint); version = unsigned32(layer); break;
            default: layer.skip(); break;
            }
        }
        require((version == 1 || version == 2) && extent > 0 && extent <= 65536 && !name.isEmpty() && !result.layers.contains(name));
        result.layers.append(name);
        for (const auto &view : features) {
            require(!cancelled.load(std::memory_order_relaxed));
            Reader input(view);
            VectorFeature feature;
            feature.layer = name;
            QList<quint32> tags, commands;
            quint32 kind = 0;
            while (input.next()) {
                switch (input.tag()) {
                case 1: wire(input, Wire::varint); feature.id = QString::number(input.get_uint64()); break;
                case 2: tags.append(packed(input, 512)); bounded(tags.size() <= 512); break;
                case 3: wire(input, Wire::varint); kind = unsigned32(input); break;
                case 4: commands.append(packed(input, 1500000)); bounded(commands.size() <= 1500000); break;
                default: input.skip(); break;
                }
            }
            require(kind >= 1 && kind <= 3 && tags.size() % 2 == 0);
            feature.kind = static_cast<VectorFeature::Kind>(kind);
            for (qsizetype i = 0; i < tags.size(); i += 2) {
                require(tags.at(i) < static_cast<quint32>(keys.size()) && tags.at(i + 1) < static_cast<quint32>(values.size()));
                const auto &key = keys.at(tags.at(i)); const auto &value = values.at(tags.at(i + 1));
                retained += 128 + key.size() * 2 + value.toString().size() * 2;
                bounded(retained <= 16 * 1024 * 1024);
                feature.properties.insert(key, value);
            }
            geometry(feature, commands, extent, vertices, cancelled);
            bounded(retained + vertices * 64 + (result.features.size() + 1) * 256 <= 64 * 1024 * 1024);
            feature.color = color(feature);
            feature.labelOnly = name.endsWith(QLatin1String("_name")) || name.endsWith(QLatin1String("_label"))
                || name == QLatin1String("place") || name == QLatin1String("housenumber");
            result.features.append(std::move(feature));
        }
    }
    std::stable_sort(result.features.begin(), result.features.end(), [](const auto &a, const auto &b) { return order(a) < order(b); });
    result.costKiB = static_cast<int>((retained + vertices * 64 + result.features.size() * 256) / 1024 + 1);
    return result;
}
}
class MvtProvider final : public QObject, public Plugin, public MapProvider, public VectorTileProvider {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "mvt/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider swak::VectorTileProvider)
public:
    QString mapId() const override { return QStringLiteral("org.swak.mvt"); }
    QString mapName() const override { return tr("MVT vector tiles (OpenMapTiles)"); }
    QStringList layerNames() const override
    {
        return {QStringLiteral("aerodrome_label"), QStringLiteral("aeroway"), QStringLiteral("boundary"), QStringLiteral("building"),
            QStringLiteral("housenumber"), QStringLiteral("landcover"), QStringLiteral("landuse"), QStringLiteral("mountain_peak"),
            QStringLiteral("park"), QStringLiteral("place"), QStringLiteral("poi"), QStringLiteral("transportation"),
            QStringLiteral("transportation_name"), QStringLiteral("water"), QStringLiteral("water_name"), QStringLiteral("waterway")};
    }
    QJsonObject defaults() const override
    {
        auto result = mapDefaults(tr("OpenFreeMap"), QStringLiteral("https://tiles.openfreemap.org/planet/latest/{z}/{x}/{y}.pbf"),
            QStringLiteral("OpenFreeMap © OpenMapTiles Data from OpenStreetMap | https://openfreemap.org | https://openmaptiles.org | https://www.openstreetmap.org/copyright"), 14);
        result.insert(QStringLiteral("labels"), true); result.insert(QStringLiteral("labelSize"), 12);
        result.insert(QStringLiteral("lineWidth"), 1.5); result.insert(QStringLiteral("pointRadius"), 3);
        result.insert(QStringLiteral("language"), QString{}); result.insert(QStringLiteral("visibleLayers"), QJsonObject{});
        QJsonObject visible; for (const auto &layer : layerNames()) visible.insert(layer, true);
        result.insert(QStringLiteral("visibleLayers"), visible);
        return result;
    }
    QList<SettingField> fields() const override
    {
        auto result = commonMapFields();
        result.append(booleanSetting(QStringLiteral("labels"), tr("Show map labels")));
        result.append(numberSetting(QStringLiteral("labelSize"), tr("Label size (pixels)"), 8, 24));
        result.append(numberSetting(QStringLiteral("lineWidth"), tr("Line width (pixels)"), 0.5, 6, 1));
        result.append(numberSetting(QStringLiteral("pointRadius"), tr("Point radius (pixels)"), 1, 10, 1));
        result.append(textSetting(QStringLiteral("language"), tr("Label language code (empty uses local names)"), 16));
        for (const auto &layer : layerNames()) result.append(booleanSetting(QStringLiteral("visibleLayers/") + layer, tr("Show layer: %1").arg(layer)));
        return result;
    }
    QString validate(const QJsonObject &source) const override
    {
        auto error = validateMapSource(source); if (!error.isEmpty()) return error;
        const auto endpoint = source.value(QStringLiteral("endpoint")).toString();
        for (const auto *part : {"{z}", "{x}", "{y}"}) if (!endpoint.contains(QLatin1String(part))) return tr("The MVT URL needs {z}, {x}, and {y}.");
        for (const auto &field : fields()) if (field.kind == SettingField::Kind::Number && source.contains(field.path)) {
            const double value = source.value(field.path).toDouble(-1);
            if (!std::isfinite(value) || value < field.minimum || value > field.maximum || (field.decimals == 0 && std::floor(value) != value))
                return tr("A vector rendering value is outside its limits.");
        }
        static const QRegularExpression language(QStringLiteral("\\A[a-zA-Z0-9_-]{0,16}\\z"));
        if (!language.match(source.value(QStringLiteral("language")).toString()).hasMatch()) return tr("Use a short language code, such as en or sv.");
        const auto visible = source.value(QStringLiteral("visibleLayers")).toObject();
        if (visible.size() > 64) return tr("Use at most 64 layer controls.");
        for (auto it = visible.begin(); it != visible.end(); ++it)
            if (it.key().isEmpty() || it.key().size() > 100 || !it.value().isBool()) return tr("A layer control is invalid.");
        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        if (!validTile(z, x, y) || !validate(source).isEmpty()) return {};
        return QUrl(source.value(QStringLiteral("endpoint")).toString().replace(QStringLiteral("{z}"), QString::number(z))
            .replace(QStringLiteral("{x}"), QString::number(x)).replace(QStringLiteral("{y}"), QString::number(y)));
    }
    VectorTile decodeTile(const QByteArray &bytes, const std::atomic_bool &cancelled) const override
    {
        try { return decode(bytes, cancelled); }
        catch (const TileLimit &) { VectorTile result; result.resourceLimit = true; result.error = tr("Vector tile exceeds the resource limit."); return result; }
        catch (const std::exception &) { VectorTile result; result.error = (cancelled.load() ? tr("Vector tile decoding cancelled.") : tr("Vector tile is invalid.")); return result; }
    }
};
}
#include "mvt.moc"
