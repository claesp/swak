#include "core/interfaces.h"
#include "core/maps.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

#include <algorithm>

namespace swak {
namespace {
// Azure Maps Render, the successor of Bing Maps. Tiles and attribution share this host.
const QString Host = QStringLiteral("https://atlas.microsoft.com");
const QString Endpoint = Host + QStringLiteral("/map/tile");
const QString ApiVersion = QStringLiteral("2024-04-01");
const QStringList Tilesets{QStringLiteral("microsoft.base.road"),
                           QStringLiteral("microsoft.base.darkgrey"),
                           QStringLiteral("microsoft.imagery")};
QString coordinate(double value)
{
    return QString::number(value, 'f', 6);
}
} // namespace
class AzureMapsProvider final : public QObject,
                                public Plugin,
                                public MapProvider,
                                public MapServiceProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "azure_maps/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider swak::MapServiceProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("org.swak.azure.tiles");
    }
    QString mapName() const override
    {
        return tr("Azure Maps tiles");
    }
    QJsonObject defaults() const override
    {
        return source(tr("Azure Maps Road (API key)"), QStringLiteral("microsoft.base.road"));
    }
    QJsonObject additionalSources() const override
    {
        auto imagery =
            source(tr("Azure Maps Imagery (API key)"), QStringLiteral("microsoft.imagery"));
        // Azure Maps imagery starts at zoom level 1.
        imagery.insert(QStringLiteral("minZoom"), 1);
        return {{QStringLiteral("org.swak.azure.darkgrey"),
                 source(tr("Azure Maps Dark Grey (API key)"),
                        QStringLiteral("microsoft.base.darkgrey"))},
                {QStringLiteral("org.swak.azure.imagery"), imagery}};
    }
    QList<SettingField> fields() const override
    {
        return {
            textSetting(QStringLiteral("name"), tr("Name"), 100),
            choiceSetting(QStringLiteral("tilesetId"),
                          tr("Map style"),
                          {tr("Road"), tr("Dark grey"), tr("Imagery")},
                          QJsonArray::fromStringList(Tilesets)),
            textSetting(QStringLiteral("language"), tr("Language (IETF tag, such as en-US)"), 35),
            numberSetting(QStringLiteral("minZoom"), tr("Minimum tile zoom"), 0, 19),
            numberSetting(QStringLiteral("maxZoom"), tr("Maximum tile zoom"), 0, 19),
            textSetting(QStringLiteral("authEnvironment"),
                        tr("API key environment variable (SWAK_MAP_...)"),
                        128)};
    }
    QString validate(const QJsonObject &source) const override
    {
        const auto error =
            validateServiceSource(source, Endpoint, QStringLiteral("subscription-key"));
        if (!error.isEmpty()) {
            return error;
        }
        const auto tileset = source.value(QStringLiteral("tilesetId")).toString();
        if (!Tilesets.contains(tileset)) {
            return tr("Select an Azure Maps style.");
        }
        if (tileset == u"microsoft.imagery" &&
            source.value(QStringLiteral("minZoom")).toInt() < 1) {
            return tr("Azure Maps imagery starts at zoom level 1.");
        }
        if (!validLanguageTag(source.value(QStringLiteral("language")).toString())) {
            return tr("Enter an IETF language tag, such as en-US, or leave it empty.");
        }
        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int zoom, int x, int y) const override
    {
        if (!validTile(zoom, x, y) || !validate(source).isEmpty()) {
            return {};
        }
        QUrl url(Endpoint);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("api-version"), ApiVersion);
        query.addQueryItem(QStringLiteral("tilesetId"),
                           source.value(QStringLiteral("tilesetId")).toString());
        query.addQueryItem(QStringLiteral("zoom"), QString::number(zoom));
        query.addQueryItem(QStringLiteral("x"), QString::number(x));
        query.addQueryItem(QStringLiteral("y"), QString::number(y));
        query.addQueryItem(QStringLiteral("tileSize"), QStringLiteral("256"));
        const auto language = source.value(QStringLiteral("language")).toString();
        if (!language.isEmpty()) {
            query.addQueryItem(QStringLiteral("language"), language);
        }
        url.setQuery(query);
        return url;
    }
    QUrl attributionUrl(const QJsonObject &source,
                        const QString &,
                        int zoom,
                        const MapBounds &bounds) const override
    {
        if (zoom < 0 || zoom > 22 || !validate(source).isEmpty()) {
            return {};
        }
        QUrl url(Host + QStringLiteral("/map/attribution"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("api-version"), ApiVersion);
        query.addQueryItem(QStringLiteral("tilesetId"),
                           source.value(QStringLiteral("tilesetId")).toString());
        query.addQueryItem(QStringLiteral("zoom"), QString::number(zoom));
        // Azure Maps expects the southwest corner, then the northeast corner.
        query.addQueryItem(QStringLiteral("bounds"),
                           QStringList{coordinate(bounds.west),
                                       coordinate(bounds.south),
                                       coordinate(bounds.east),
                                       coordinate(bounds.north)}
                               .join(QLatin1Char(',')));
        url.setQuery(query);
        return url;
    }
    QString readAttribution(const QByteArray &reply) const override
    {
        QStringList credits;
        const auto copyrights =
            QJsonDocument::fromJson(reply).object().value(QStringLiteral("copyrights")).toArray();
        for (qsizetype i = 0; i < std::min<qsizetype>(copyrights.size(), 16); ++i) {
            const auto text = copyrights.at(i).toString().left(1024);
            if (!text.isEmpty()) {
                credits.append(text);
            }
        }
        // Each entry can hold simple HTML. The host converts the result to plain text.
        return credits.join(QStringLiteral(", "));
    }

private:
    QJsonObject source(const QString &name, const QString &tileset) const
    {
        auto result = mapDefaults(name, Endpoint, QStringLiteral("Microsoft Azure Maps"), 19);
        result.insert(QStringLiteral("tilesetId"), tileset);
        result.insert(QStringLiteral("language"), QString{});
        result.insert(QStringLiteral("authMode"), QStringLiteral("query"));
        result.insert(QStringLiteral("authParameter"), QStringLiteral("subscription-key"));
        result.insert(QStringLiteral("authEnvironment"), QStringLiteral("SWAK_MAP_AZURE_KEY"));
        return result;
    }
};
} // namespace swak
#include "azure_maps.moc"
