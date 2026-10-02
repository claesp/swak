#include "core/maps.h"
namespace swak {
class XyzProvider final : public QObject, public MapProvider {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.MapProvider/1.0")
    Q_INTERFACES(swak::MapExtension swak::MapProvider)
public:
    QString mapId() const override { return QStringLiteral("org.swak.xyz"); }
    QString mapName() const override { return tr("XYZ image tiles"); }
    QJsonObject defaults() const override
    { return mapDefaults(tr("OpenStreetMap"), QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
        QStringLiteral("© OpenStreetMap contributors | https://www.openstreetmap.org/copyright"), 19); }
    QList<SettingField> fields() const override { return commonMapFields(); }
    QString validate(const QJsonObject &source) const override
    {
        const auto error = validateMapSource(source);
        if (!error.isEmpty()) return error;
        const auto url = source.value(QStringLiteral("endpoint")).toString();
        for (const auto *part : {"{z}", "{x}", "{y}"}) if (!url.contains(QLatin1String(part))) return tr("The XYZ URL needs {z}, {x}, and {y}.");
        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        if (!validTile(z, x, y) || !validate(source).isEmpty()) return {};
        return QUrl(source.value(QStringLiteral("endpoint")).toString().replace(QStringLiteral("{z}"), QString::number(z))
            .replace(QStringLiteral("{x}"), QString::number(x)).replace(QStringLiteral("{y}"), QString::number(y)));
    }
};
}
#include "xyz.moc"
