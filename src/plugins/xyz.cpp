#include "core/interfaces.h"
#include "core/maps.h"
namespace swak {
class XyzProvider final : public QObject, public Plugin, public MapProvider {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "xyz/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider)
public:
    QString mapId() const override { return QStringLiteral("org.swak.xyz"); }
    QString mapName() const override { return tr("XYZ image tiles"); }
    QJsonObject defaults() const override
    { auto source = mapDefaults(tr("OpenStreetMap"), QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
        QStringLiteral("© OpenStreetMap contributors | https://www.openstreetmap.org/copyright"), 19); source.insert(QStringLiteral("invertY"), false); return source; }
    QJsonObject additionalSources() const override
    {
        return {{QStringLiteral("org.swak.xyz.usgs-topo"), mapDefaults(tr("USGS Topo"),
            QStringLiteral("https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}"),
            QStringLiteral("USGS The National Map and contributors | https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer"), 8)}};
    }
    QList<SettingField> fields() const override {
        auto fields = commonMapFields();
        fields.append(booleanSetting(QStringLiteral("invertY"), tr("Invert tile rows (TMS)")));
        return fields;
    }
    QString validate(const QJsonObject &source) const override
    {
        const auto error = validateMapSource(source);
        if (!error.isEmpty()) return error;
        if (source.contains(QStringLiteral("invertY")) && !source.value(QStringLiteral("invertY")).isBool())
            return tr("The inverted row value must be true or false.");
        auto url = source.value(QStringLiteral("endpoint")).toString();
        const QStringList parts = url.contains(QStringLiteral("{q}")) ? QStringList{QStringLiteral("{q}")} :
            QStringList{QStringLiteral("{z}"), QStringLiteral("{x}"), QStringLiteral("{y}")};
        for (const auto &part : parts) {
            if (!url.contains(part)) return tr("The tile URL needs %1.").arg(parts.join(QStringLiteral(", ")));
            url.replace(part, QString{});
        }
        if (url.contains('{') || url.contains('}')) return tr("The tile URL contains an unsupported placeholder.");

        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        if (!validTile(z, x, y) || !validate(source).isEmpty()) return {};
        auto url = source.value(QStringLiteral("endpoint")).toString();
        if (source.value(QStringLiteral("invertY")).toBool(false)) y = (1 << z) - 1 - y;
        if (url.contains(QStringLiteral("{q}"))) {
            QString key; key.reserve(z);
            for (int bit = z - 1; bit >= 0; --bit)
                key.append(QChar(u'0' + ((x >> bit) & 1) + 2 * ((y >> bit) & 1)));
            return QUrl(url.replace(QStringLiteral("{q}"), key));
        }
        return QUrl(url.replace(QStringLiteral("{z}"), QString::number(z))
            .replace(QStringLiteral("{x}"), QString::number(x)).replace(QStringLiteral("{y}"), QString::number(y)));
    }
};
}
#include "xyz.moc"
