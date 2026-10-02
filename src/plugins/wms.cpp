#include "core/interfaces.h"
#include "core/maps.h"
#include <QUrlQuery>
namespace swak {
class WmsProvider final : public QObject, public Plugin, public MapProvider {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "wms/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider)
public:
    QString mapId() const override { return QStringLiteral("org.swak.wms"); }
    QString mapName() const override { return tr("WMS 1.3 images"); }
    QJsonObject defaults() const override
    {
        auto source = mapDefaults(tr("terrestris OpenStreetMap"), QStringLiteral("https://ows.terrestris.de/osm/service"),
            QStringLiteral("© OpenStreetMap contributors | terrestris | https://www.openstreetmap.org/copyright"), 18);
        source.insert(QStringLiteral("layer"), QStringLiteral("OSM-WMS"));
        source.insert(QStringLiteral("style"), QString{});
        source.insert(QStringLiteral("imageFormat"), QStringLiteral("image/png"));
        return source;
    }
    QList<SettingField> fields() const override
    {
        auto result = commonMapFields();
        result.append({textSetting(QStringLiteral("layer"), tr("Layer names (comma separated)"), 200),
            textSetting(QStringLiteral("style"), tr("Style names (comma separated)"), 100),
            choiceSetting(QStringLiteral("imageFormat"), tr("Image format"), {tr("JPEG"), tr("PNG")}, {QStringLiteral("image/jpeg"), QStringLiteral("image/png")})});
        return result;
    }
    QString validate(const QJsonObject &source) const override
    {
        const auto error = validateMapSource(source);
        if (!error.isEmpty()) return error;
        const auto layer = source.value(QStringLiteral("layer")).toString();
        if (layer.isEmpty() || layer.size() > 200 || source.value(QStringLiteral("style")).toString().size() > 100) return tr("Enter bounded layer and style names.");
        const auto format = source.value(QStringLiteral("imageFormat")).toString();
        if (format != QStringLiteral("image/png") && format != QStringLiteral("image/jpeg")) return tr("Select PNG or JPEG.");
        if (QUrl(source.value(QStringLiteral("endpoint")).toString()).hasQuery()) return tr("Enter the service URL without query parameters.");
        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        if (!validTile(z, x, y) || !validate(source).isEmpty()) return {};
        constexpr double edge = 20037508.342789244;
        const double span = 2 * edge / (1 << z);
        const auto bbox = QStringLiteral("%1,%2,%3,%4").arg(-edge + x * span, 0, 'f', 8)
            .arg(edge - (y + 1) * span, 0, 'f', 8).arg(-edge + (x + 1) * span, 0, 'f', 8).arg(edge - y * span, 0, 'f', 8);
        QUrl url(source.value(QStringLiteral("endpoint")).toString());
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMS"));
        query.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("1.3.0"));
        query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetMap"));
        query.addQueryItem(QStringLiteral("CRS"), QStringLiteral("EPSG:3857"));
        query.addQueryItem(QStringLiteral("BBOX"), bbox);
        query.addQueryItem(QStringLiteral("WIDTH"), QStringLiteral("256"));
        query.addQueryItem(QStringLiteral("HEIGHT"), QStringLiteral("256"));
        query.addQueryItem(QStringLiteral("LAYERS"), source.value(QStringLiteral("layer")).toString());
        query.addQueryItem(QStringLiteral("STYLES"), source.value(QStringLiteral("style")).toString());
        query.addQueryItem(QStringLiteral("FORMAT"), source.value(QStringLiteral("imageFormat")).toString());
        query.addQueryItem(QStringLiteral("TRANSPARENT"), QStringLiteral("TRUE"));
        url.setQuery(query);
        return url;
    }
};
}
#include "wms.moc"
