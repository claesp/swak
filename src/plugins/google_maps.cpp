#include "core/interfaces.h"
#include "core/maps.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace swak {
namespace {
// The Google Map Tiles API. Tiles, sessions, and viewport attribution share this host.
const QString Host = QStringLiteral("https://tile.googleapis.com");
const QString Endpoint = Host + QStringLiteral("/v1/2dtiles");
const QStringList MapTypes{
    QStringLiteral("roadmap"), QStringLiteral("satellite"), QStringLiteral("terrain")};
QString encoded(const QString &value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}
QString coordinate(double value)
{
    return QString::number(value, 'f', 6);
}
} // namespace
class GoogleMapsProvider final : public QObject,
                                 public Plugin,
                                 public MapProvider,
                                 public MapServiceProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "google_maps/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider swak::MapServiceProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("org.swak.google.tiles");
    }
    QString mapName() const override
    {
        return tr("Google Map Tiles");
    }
    QJsonObject defaults() const override
    {
        return source(tr("Google Maps Roadmap (API key)"), QStringLiteral("roadmap"));
    }
    QJsonObject additionalSources() const override
    {
        return {{QStringLiteral("org.swak.google.satellite"),
                 source(tr("Google Maps Satellite (API key)"), QStringLiteral("satellite"))},
                {QStringLiteral("org.swak.google.terrain"),
                 source(tr("Google Maps Terrain (API key)"), QStringLiteral("terrain"))}};
    }
    QList<SettingField> fields() const override
    {
        return {
            textSetting(QStringLiteral("name"), tr("Name"), 100),
            choiceSetting(QStringLiteral("mapType"),
                          tr("Map type"),
                          {tr("Roadmap"), tr("Satellite"), tr("Terrain")},
                          QJsonArray::fromStringList(MapTypes)),
            textSetting(QStringLiteral("language"), tr("Language (IETF tag, such as en-US)"), 35),
            textSetting(QStringLiteral("region"), tr("Region (two letters, such as US)"), 2),
            numberSetting(QStringLiteral("minZoom"), tr("Minimum tile zoom"), 0, 19),
            numberSetting(QStringLiteral("maxZoom"), tr("Maximum tile zoom"), 0, 19),
            textSetting(QStringLiteral("authEnvironment"),
                        tr("API key environment variable (SWAK_MAP_...)"),
                        128)};
    }
    QString validate(const QJsonObject &source) const override
    {
        const auto error = validateServiceSource(source, Endpoint, QStringLiteral("key"));
        if (!error.isEmpty()) {
            return error;
        }
        if (!MapTypes.contains(source.value(QStringLiteral("mapType")).toString())) {
            return tr("Select a Google map type.");
        }
        if (!validLanguageTag(source.value(QStringLiteral("language")).toString())) {
            return tr("Enter an IETF language tag, such as en-US, or leave it empty.");
        }
        static const QRegularExpression regionPattern(QStringLiteral("\\A([A-Z]{2})?\\z"));
        if (!regionPattern.match(source.value(QStringLiteral("region")).toString()).hasMatch()) {
            return tr("Enter a region as two capital letters, such as US, or leave it empty.");
        }
        return {};
    }
    QUrl tileUrl(const QJsonObject &, int, int, int) const override
    {
        // Every tile request needs a session. See sessionTileUrl.
        return {};
    }
    QUrl sessionUrl(const QJsonObject &source) const override
    {
        return validate(source).isEmpty() ? QUrl(Host + QStringLiteral("/v1/createSession"))
                                          : QUrl{};
    }
    QByteArray sessionBody(const QJsonObject &source) const override
    {
        const auto type = source.value(QStringLiteral("mapType")).toString();
        const auto language = source.value(QStringLiteral("language")).toString();
        const auto region = source.value(QStringLiteral("region")).toString();
        QJsonObject body{
            {QStringLiteral("mapType"), type},
            {QStringLiteral("language"), language.isEmpty() ? QStringLiteral("en-US") : language},
            {QStringLiteral("region"), region.isEmpty() ? QStringLiteral("US") : region}};
        // Google requires the roadmap layer with terrain.
        if (type == u"terrain") {
            body.insert(QStringLiteral("layerTypes"), QJsonArray{QStringLiteral("layerRoadmap")});
        }
        return QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    MapSession readSession(const QByteArray &reply) const override
    {
        const auto object = QJsonDocument::fromJson(reply).object();
        const auto token = object.value(QStringLiteral("session")).toString();
        static const QRegularExpression tokenPattern(
            QStringLiteral("\\A[A-Za-z0-9_.~+/=-]{1,1024}\\z"));
        if (!tokenPattern.match(token).hasMatch()) {
            return {};
        }
        // Google sends the expiry as a string of seconds since the epoch.
        const auto expiry = object.value(QStringLiteral("expiry"));
        qint64 seconds = -1;
        if (expiry.isString()) {
            bool number = false;
            seconds = expiry.toString().toLongLong(&number);
            seconds = number ? seconds : -1;
        } else if (expiry.isDouble() && std::isfinite(expiry.toDouble())) {
            seconds = static_cast<qint64>(std::min(expiry.toDouble(), 1e12));
        }
        // Reject a missing expiry and one after the year 3000.
        if (seconds <= 0 || seconds > 32503680000) {
            return {};
        }
        return {token, QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC)};
    }
    QUrl sessionTileUrl(
        const QJsonObject &source, const QString &session, int zoom, int x, int y) const override
    {
        if (session.isEmpty() || !validTile(zoom, x, y) || !validate(source).isEmpty()) {
            return {};
        }
        QUrl url(Endpoint + QStringLiteral("/%1/%2/%3").arg(zoom).arg(x).arg(y));
        url.setQuery(QStringLiteral("session=") + encoded(session), QUrl::StrictMode);
        return url;
    }
    QUrl attributionUrl(const QJsonObject &source,
                        const QString &session,
                        int zoom,
                        const MapBounds &bounds) const override
    {
        if (session.isEmpty() || zoom < 0 || zoom > 22 || !validate(source).isEmpty()) {
            return {};
        }
        // Google accepts open latitude and longitude ranges.
        const auto longitude = [](double value) {
            return std::clamp(value, -179.999999, 179.999999);
        };
        QUrl url(Host + QStringLiteral("/tile/v1/viewport"));
        url.setQuery(QStringLiteral("session=%1&zoom=%2&north=%3&south=%4&east=%5&west=%6")
                         .arg(encoded(session),
                              QString::number(zoom),
                              coordinate(std::clamp(bounds.north, -89.999999, 89.999999)),
                              coordinate(std::clamp(bounds.south, -89.999999, 89.999999)),
                              coordinate(longitude(bounds.east)),
                              coordinate(longitude(bounds.west))),
                     QUrl::StrictMode);
        return url;
    }
    QString readAttribution(const QByteArray &reply) const override
    {
        return QJsonDocument::fromJson(reply)
            .object()
            .value(QStringLiteral("copyright"))
            .toString()
            .left(1024);
    }

private:
    QJsonObject source(const QString &name, const QString &type) const
    {
        // The Google Maps name is the required product attribution. Live data credits follow.
        auto result = mapDefaults(name, Endpoint, QStringLiteral("Google Maps"), 19);
        result.insert(QStringLiteral("mapType"), type);
        result.insert(QStringLiteral("language"), QString{});
        result.insert(QStringLiteral("region"), QString{});
        result.insert(QStringLiteral("authMode"), QStringLiteral("query"));
        result.insert(QStringLiteral("authParameter"), QStringLiteral("key"));
        result.insert(QStringLiteral("authEnvironment"), QStringLiteral("SWAK_MAP_GOOGLE_KEY"));
        return result;
    }
};
} // namespace swak
#include "google_maps.moc"
