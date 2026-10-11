#include "core/gdal_support.h"
#include "core/maps.h"
#include "core/tiles.h"
#include "image_fault.h"
#include "tile_server.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtConcurrentRun>

#include <array>
#include <cpl_conv.h>
#include <gdal_priv.h>
#include <limits>
#include <ogr_spatialref.h>
#include <vector>
using namespace swak;
// Builds HTTPS tile URLs from a template. The renderer network path still serves URL
// providers, such as MVT, so these tests keep a small provider of that kind.
class UrlTemplateProvider final : public QObject, public MapProvider
{
    Q_OBJECT
    Q_INTERFACES(swak::MapExtension swak::MapProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("test.url");
    }
    QString mapName() const override
    {
        return QStringLiteral("URL template fixture");
    }
    QJsonObject defaults() const override
    {
        return mapDefaults(QStringLiteral("Fixture"),
                           QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
                           QStringLiteral("Fixture"),
                           19);
    }
    QList<SettingField> fields() const override
    {
        return commonMapFields();
    }
    QString validate(const QJsonObject &source) const override
    {
        return validateMapSource(source);
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
};
QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
// A session service on the local test server. It follows the Google flow: a POST starts a
// session, tiles carry the session, and a viewport request reports attribution.
class SessionProvider final : public QObject, public MapProvider, public MapServiceProvider
{
    Q_OBJECT
    Q_INTERFACES(swak::MapExtension swak::MapProvider swak::MapServiceProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("test.session");
    }
    QString mapName() const override
    {
        return QStringLiteral("Session fixture");
    }
    QJsonObject defaults() const override
    {
        return mapDefaults(QStringLiteral("Session"),
                           QStringLiteral("https://localhost/tiles"),
                           QStringLiteral("Session fixture"),
                           19);
    }
    QList<SettingField> fields() const override
    {
        return commonMapFields();
    }
    QString validate(const QJsonObject &source) const override
    {
        return validateMapSource(source);
    }
    QUrl tileUrl(const QJsonObject &, int, int, int) const override
    {
        return {};
    }
    QUrl sessionUrl(const QJsonObject &source) const override
    {
        return QUrl(origin(source) + QStringLiteral("/session"));
    }
    QByteArray sessionBody(const QJsonObject &) const override
    {
        return json({{QStringLiteral("mapType"), QStringLiteral("roadmap")}});
    }
    MapSession readSession(const QByteArray &reply) const override
    {
        const auto object = QJsonDocument::fromJson(reply).object();
        return {
            object.value(QStringLiteral("session")).toString(),
            QDateTime::fromSecsSinceEpoch(
                object.value(QStringLiteral("expiry")).toString().toLongLong(), QTimeZone::UTC)};
    }
    QUrl sessionTileUrl(
        const QJsonObject &source, const QString &session, int z, int x, int y) const override
    {
        return QUrl(origin(source) +
                    QStringLiteral("/tiles/%1/%2/%3?session=%4").arg(z).arg(x).arg(y).arg(session));
    }
    QUrl attributionUrl(const QJsonObject &source,
                        const QString &session,
                        int zoom,
                        const MapBounds &bounds) const override
    {
        if (session.isEmpty()) {
            return {};
        }
        lastBounds = bounds;
        return QUrl(origin(source) +
                    QStringLiteral("/viewport?session=%1&zoom=%2").arg(session).arg(zoom));
    }
    QString readAttribution(const QByteArray &reply) const override
    {
        return QJsonDocument::fromJson(reply)
            .object()
            .value(QStringLiteral("copyright"))
            .toString();
    }
    mutable MapBounds lastBounds;

private:
    static QString origin(const QJsonObject &source)
    {
        return QUrl(source.value(QStringLiteral("endpoint")).toString())
            .adjusted(QUrl::RemovePath | QUrl::RemoveQuery)
            .toString();
    }
};
class MapTest final : public QObject
{
    Q_OBJECT
    UrlTemplateProvider urlTiles_;
    MapProvider *format(const QString &id)
    {
        for (auto *object : QPluginLoader::staticInstances()) {
            if (auto *provider = qobject_cast<MapProvider *>(object);
                provider && provider->mapId() == id) {
                return provider;
            }
        }
        return nullptr;
    }
    QJsonObject localSource(MapProvider *provider, const TileServer &server, bool ignore)
    {
        auto source = provider->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), ignore);
        return source;
    }
    // GDAL waits for HTTP replies. Render on a worker so the test server can answer.
    RasterTile renderOnWorker(const RasterRequest &request, bool cancel = false)
    {
        const auto *raster =
            dynamic_cast<RasterTileProvider *>(format(QStringLiteral("org.swak.gdal.raster")));
        auto future = QtConcurrent::run([raster, request, cancel] {
            std::atomic_bool cancelled = cancel;
            return raster->renderTile(request, cancelled);
        });
        if (!QTest::qWaitFor(
                [&] {
                    return future.isFinished();
                },
                30000)) {
            future.waitForFinished();
        }
        return future.result();
    }
    QJsonObject datasetSource(const QString &path)
    {
        auto source = format(QStringLiteral("org.swak.gdal.raster"))->defaults();
        source.insert(QStringLiteral("service"), QStringLiteral("dataset"));
        source.insert(QStringLiteral("endpoint"), path);
        return source;
    }
    void draw(TileRenderer &renderer, QImage &canvas)
    {
        QPainter painter(&canvas);
        renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 1, false);
    }
    // Trusts the test server certificate until this object ends.
    struct TrustServer
    {
        QSslConfiguration previous = QSslConfiguration::defaultConfiguration();
        explicit TrustServer(const TileServer &server)
        {
            auto trusted = previous;
            auto authorities = trusted.caCertificates();
            authorities.append(server.certificate);
            trusted.setCaCertificates(authorities);
            QSslConfiguration::setDefaultConfiguration(trusted);
        }
        ~TrustServer()
        {
            QSslConfiguration::setDefaultConfiguration(previous);
        }
        TrustServer(const TrustServer &) = delete;
        TrustServer &operator=(const TrustServer &) = delete;
    };
private slots:
    void googleMapTiles()
    {
        auto *google = format(QStringLiteral("org.swak.google.tiles"));
        QVERIFY(google);
        auto *service = dynamic_cast<MapServiceProvider *>(google);
        QVERIFY(service);
        MapSources sources(QPluginLoader::staticInstances());
        for (const auto &[id, type] :
             {std::pair{QStringLiteral("org.swak.google.tiles"), QStringLiteral("roadmap")},
              std::pair{QStringLiteral("org.swak.google.satellite"), QStringLiteral("satellite")},
              std::pair{QStringLiteral("org.swak.google.terrain"), QStringLiteral("terrain")}}) {
            const auto preset = sources.sources().value(id).toObject();
            QCOMPARE(preset.value(QStringLiteral("mapType")).toString(), type);
            QVERIFY(google->validate(preset).isEmpty());
            QCOMPARE(preset.value(QStringLiteral("authEnvironment")).toString(),
                     QStringLiteral("SWAK_MAP_GOOGLE_KEY"));
        }
        auto source = google->defaults();
        QVERIFY(google->validate(source).isEmpty());
        QVERIFY(google->tileUrl(source, 1, 0, 0).isEmpty());
        QCOMPARE(service->sessionUrl(source),
                 QUrl(QStringLiteral("https://tile.googleapis.com/v1/createSession")));
        auto body = QJsonDocument::fromJson(service->sessionBody(source)).object();
        QCOMPARE(body.value(QStringLiteral("mapType")).toString(), QStringLiteral("roadmap"));
        QCOMPARE(body.value(QStringLiteral("language")).toString(), QStringLiteral("en-US"));
        QCOMPARE(body.value(QStringLiteral("region")).toString(), QStringLiteral("US"));
        QVERIFY(!body.contains(QStringLiteral("layerTypes")));
        auto terrain = source;
        terrain.insert(QStringLiteral("mapType"), QStringLiteral("terrain"));
        terrain.insert(QStringLiteral("language"), QStringLiteral("sv-SE"));
        terrain.insert(QStringLiteral("region"), QStringLiteral("SE"));
        body = QJsonDocument::fromJson(service->sessionBody(terrain)).object();
        QCOMPARE(body.value(QStringLiteral("layerTypes")).toArray(),
                 QJsonArray{QStringLiteral("layerRoadmap")});
        QCOMPARE(body.value(QStringLiteral("language")).toString(), QStringLiteral("sv-SE"));
        // Sessions need a usable token and an expiry in seconds since the epoch.
        auto session =
            service->readSession(json({{QStringLiteral("session"), QStringLiteral("a+b/c=")},
                                       {QStringLiteral("expiry"), QStringLiteral("1900000000")}}));
        QCOMPARE(session.token, QStringLiteral("a+b/c="));
        QCOMPARE(session.expires.toSecsSinceEpoch(), 1900000000);
        QCOMPARE(service
                     ->readSession(json({{QStringLiteral("session"), QStringLiteral("t")},
                                         {QStringLiteral("expiry"), 1900000000}}))
                     .token,
                 QStringLiteral("t"));
        for (const QByteArray &bad :
             {json({{QStringLiteral("expiry"), QStringLiteral("1900000000")}}),
              json({{QStringLiteral("session"), QStringLiteral("a b")},
                    {QStringLiteral("expiry"), QStringLiteral("1900000000")}}),
              json({{QStringLiteral("session"), QStringLiteral("t")},
                    {QStringLiteral("expiry"), QStringLiteral("soon")}}),
              json({{QStringLiteral("session"), QStringLiteral("t")},
                    {QStringLiteral("expiry"), QStringLiteral("-5")}}),
              json({{QStringLiteral("session"), QStringLiteral("t")}}),
              QByteArray("not json")}) {
            QVERIFY2(service->readSession(bad).token.isEmpty(), bad.constData());
        }
        // The token is percent-encoded. The host adds the key parameter.
        const auto tile = service->sessionTileUrl(source, QStringLiteral("a+b/c="), 3, 2, 1);
        QCOMPARE(
            tile.toString(QUrl::FullyEncoded),
            QStringLiteral("https://tile.googleapis.com/v1/2dtiles/3/2/1?session=a%2Bb%2Fc%3D"));
        QVERIFY(service->sessionTileUrl(source, {}, 3, 2, 1).isEmpty());
        QVERIFY(service->sessionTileUrl(source, QStringLiteral("t"), 3, 8, 1).isEmpty());
        const auto viewport =
            service->attributionUrl(source, QStringLiteral("t"), 5, MapBounds{-180, -10, 20, 95});
        QCOMPARE(viewport.path(), QStringLiteral("/tile/v1/viewport"));
        const QUrlQuery query(viewport);
        QCOMPARE(query.queryItemValue(QStringLiteral("zoom")), QStringLiteral("5"));
        QCOMPARE(query.queryItemValue(QStringLiteral("west")), QStringLiteral("-179.999999"));
        QCOMPARE(query.queryItemValue(QStringLiteral("north")), QStringLiteral("89.999999"));
        QCOMPARE(query.queryItemValue(QStringLiteral("east")), QStringLiteral("20.000000"));
        QVERIFY(service->attributionUrl(source, {}, 5, {}).isEmpty());
        QCOMPARE(service->readAttribution(json(
                     {{QStringLiteral("copyright"), QStringLiteral("Map data ©2026 Google")}})),
                 QStringLiteral("Map data ©2026 Google"));
        // The fixed service rejects another server, another key parameter, and bad options.
        for (const auto &[key, value] :
             {std::pair{"endpoint", QJsonValue(QStringLiteral("https://example.test/v1/2dtiles"))},
              std::pair{"authParameter", QJsonValue(QStringLiteral("apikey"))},
              std::pair{"authMode", QJsonValue(QStringLiteral("bearer"))},
              std::pair{"ignoreTls", QJsonValue(true)},
              std::pair{"mapType", QJsonValue(QStringLiteral("hybrid"))},
              std::pair{"language", QJsonValue(QStringLiteral("e"))},
              std::pair{"region", QJsonValue(QStringLiteral("us"))}}) {
            auto bad = source;
            bad.insert(QLatin1String(key), value);
            QVERIFY2(!google->validate(bad).isEmpty(), key);
            QVERIFY(service->sessionUrl(bad).isEmpty());
        }
    }
    void azureMapTiles()
    {
        auto *azure = format(QStringLiteral("org.swak.azure.tiles"));
        QVERIFY(azure);
        auto *service = dynamic_cast<MapServiceProvider *>(azure);
        QVERIFY(service);
        MapSources sources(QPluginLoader::staticInstances());
        for (const auto &id : {QStringLiteral("org.swak.azure.tiles"),
                               QStringLiteral("org.swak.azure.darkgrey"),
                               QStringLiteral("org.swak.azure.imagery")}) {
            QVERIFY2(azure->validate(sources.sources().value(id).toObject()).isEmpty(),
                     qPrintable(id));
        }
        auto source = azure->defaults();
        source.insert(QStringLiteral("language"), QStringLiteral("de-DE"));
        QVERIFY(service->sessionUrl(source).isEmpty());
        const auto tile = azure->tileUrl(source, 4, 3, 2);
        QCOMPARE(tile.host(), QStringLiteral("atlas.microsoft.com"));
        QCOMPARE(tile.path(), QStringLiteral("/map/tile"));
        const QUrlQuery query(tile);
        QCOMPARE(query.queryItemValue(QStringLiteral("api-version")), QStringLiteral("2024-04-01"));
        QCOMPARE(query.queryItemValue(QStringLiteral("tilesetId")),
                 QStringLiteral("microsoft.base.road"));
        QCOMPARE(query.queryItemValue(QStringLiteral("zoom")), QStringLiteral("4"));
        QCOMPARE(query.queryItemValue(QStringLiteral("x")), QStringLiteral("3"));
        QCOMPARE(query.queryItemValue(QStringLiteral("y")), QStringLiteral("2"));
        QCOMPARE(query.queryItemValue(QStringLiteral("language")), QStringLiteral("de-DE"));
        QVERIFY(azure->tileUrl(source, 2, 4, 0).isEmpty());
        // The key travels as subscription-key on the same host.
        QNetworkRequest request(tile);
        QVERIFY(authorizeMapRequest(request, source, "azure-secret").isEmpty());
        QCOMPARE(QUrlQuery(request.url()).queryItemValue(QStringLiteral("subscription-key")),
                 QStringLiteral("azure-secret"));
        const auto attribution =
            service->attributionUrl(source, {}, 6, MapBounds{-122.4, 47.5, -122.2, 47.7});
        QCOMPARE(attribution.path(), QStringLiteral("/map/attribution"));
        QCOMPARE(QUrlQuery(attribution).queryItemValue(QStringLiteral("bounds")),
                 QStringLiteral("-122.400000,47.500000,-122.200000,47.700000"));
        QCOMPARE(service->readAttribution(
                     "{\"copyrights\":[\"<a data-tileset=\\\"x\\\">&copy;2026 TomTom</a>\","
                     "\"Earthstar\"]}"),
                 QStringLiteral("<a data-tileset=\"x\">&copy;2026 TomTom</a>, Earthstar"));
        QVERIFY(service->readAttribution("[]").isEmpty());
        auto imagery = sources.sources().value(QStringLiteral("org.swak.azure.imagery")).toObject();
        imagery.insert(QStringLiteral("minZoom"), 0);
        QVERIFY(!azure->validate(imagery).isEmpty());
        for (const auto &[key, value] :
             {std::pair{"endpoint", QJsonValue(QStringLiteral("https://example.test/map/tile"))},
              std::pair{"authParameter", QJsonValue(QStringLiteral("key"))},
              std::pair{"tilesetId", QJsonValue(QStringLiteral("microsoft.base"))},
              std::pair{"language", QJsonValue(QStringLiteral("123"))}}) {
            auto bad = source;
            bad.insert(QLatin1String(key), value);
            QVERIFY2(!azure->validate(bad).isEmpty(), key);
            QVERIFY(azure->tileUrl(bad, 1, 0, 0).isEmpty());
            QVERIFY(service->attributionUrl(bad, {}, 1, {}).isEmpty());
        }
    }
    void sessionTilesAndAttribution()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const TrustServer trust(server);
        const auto expiry = QByteArray::number(QDateTime::currentSecsSinceEpoch() + 86400 * 14);
        QByteArray sessionStatus = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
        QByteArray viewportStatus = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
        QByteArray sessionReply = json({{QStringLiteral("session"), QStringLiteral("token-1")},
                                        {QStringLiteral("expiry"), QString::fromLatin1(expiry)}});
        server.respond = [&](const QByteArray &head) -> std::pair<QByteArray, QByteArray> {
            const auto line = head.left(head.indexOf("\r\n"));
            if (line.startsWith("POST /session")) {
                return {sessionStatus, sessionReply};
            }
            if (line.startsWith("GET /viewport")) {
                return {viewportStatus,
                        json({{QStringLiteral("copyright"),
                               QStringLiteral("<b>Map data</b> &copy; Test\u0007")}})};
            }
            return {server.headers, server.body};
        };
        SessionProvider provider;
        auto source = provider.defaults();
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://localhost:%1/tiles").arg(server.serverPort()));
        source.insert(QStringLiteral("authMode"), QStringLiteral("query"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("key"));
        TileRenderer renderer;
        QSignalSpy changed(&renderer, &TileRenderer::changed);
        renderer.setSource(&provider, source, "secret-key");
        QImage canvas(256, 256, QImage::Format_ARGB32);
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.coverage().rendered == 1;
            },
            5000));
        // The session starts first. The tile carries the session and the key.
        QVERIFY(server.requests.first().startsWith("POST /session?key=secret-key "));
        QVERIFY(server.requests.first().toLower().contains("content-type: application/json"));
        const auto tileRequest = std::find_if(
            server.requests.cbegin(), server.requests.cend(), [](const QByteArray &request) {
                return request.startsWith("GET /tiles/0/0/0?");
            });
        QVERIFY(tileRequest != server.requests.cend());
        QVERIFY(tileRequest->contains("session=token-1"));
        QVERIFY(tileRequest->contains("key=secret-key"));
        // The view attribution arrives after the view rests and becomes plain text.
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.attribution().isEmpty();
            },
            5000));
        QCOMPARE(renderer.attribution(), QStringLiteral("Map data © Test"));
        QCOMPARE(provider.lastBounds.west, -180.0);
        QCOMPARE(provider.lastBounds.east, 180.0);
        QVERIFY(provider.lastBounds.north <= 85 && provider.lastBounds.south >= -85);
        const auto viewportRequests = [&server] {
            return std::count_if(
                server.requests.cbegin(), server.requests.cend(), [](const QByteArray &request) {
                    return request.startsWith("GET /viewport");
                });
        };
        // The same view does not ask again.
        const auto asked = viewportRequests();
        draw(renderer, canvas);
        QTest::qWait(900);
        QCOMPARE(viewportRequests(), asked);
        // A narrower view reports its own bounds. A failed request keeps the last attribution.
        viewportStatus = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n";
        const auto zoomIn = [&] {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(0.55, 0.3), 3}, 1, false);
        };
        zoomIn();
        QVERIFY(QTest::qWaitFor(
            [&] {
                zoomIn();
                return viewportRequests() > asked;
            },
            5000));
        QTest::qWait(300);
        QCOMPARE(renderer.attribution(), QStringLiteral("Map data © Test"));
        QVERIFY(provider.lastBounds.west > -180 && provider.lastBounds.east < 180);
        QVERIFY(provider.lastBounds.west < provider.lastBounds.east);
        // A new source clears the attribution. A rejected session stops tiles.
        sessionStatus = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n";
        server.requests.clear();
        renderer.setSource(&provider, source, "other-key");
        QVERIFY(renderer.attribution().isEmpty());
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty();
            },
            5000));
        QCOMPARE(renderer.coverage().rendered, 0);
        QVERIFY(std::none_of(
            server.requests.cbegin(), server.requests.cend(), [](const QByteArray &request) {
                return request.startsWith("GET /tiles");
            }));
        QVERIFY(!renderer.status().contains(QStringLiteral("other-key")));
        // A session that ends within ten minutes is not usable.
        sessionStatus = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
        sessionReply = json(
            {{QStringLiteral("session"), QStringLiteral("short")},
             {QStringLiteral("expiry"), QString::number(QDateTime::currentSecsSinceEpoch() + 60)}});
        server.requests.clear();
        renderer.setSource(&provider, source, "third-key");
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty();
            },
            5000));
        QCOMPARE(renderer.coverage().rendered, 0);
        // Missing credentials stop the session before any request.
        server.requests.clear();
        renderer.setSource(&provider, source);
        draw(renderer, canvas);
        QTest::qWait(200);
        QVERIFY(server.requests.isEmpty());
        QVERIFY(!renderer.status().isEmpty());
        renderer.setSource(nullptr, {});
        QVERIFY(changed.count() > 0);
    }
    void mapCredentials()
    {
        auto *xyz = &urlTiles_;
        QVERIFY(xyz);
        auto source = xyz->defaults();
        source.insert(QStringLiteral("authMode"), QStringLiteral("basic"));
        source.insert(QStringLiteral("authUser"), QStringLiteral("test-user"));
        QNetworkRequest request(xyz->tileUrl(source, 0, 0, 0));
        QVERIFY(authorizeMapRequest(request, source, "password").isEmpty());
        QCOMPARE(request.rawHeader("Authorization"), QByteArray("Basic dGVzdC11c2VyOnBhc3N3b3Jk"));
        QCOMPARE(request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool(), false);
        source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        QVERIFY(authorizeMapRequest(request, source, "token").isEmpty());
        QCOMPARE(request.rawHeader("Authorization"), QByteArray("Bearer token"));
        source.insert(QStringLiteral("authMode"), QStringLiteral("header"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("X-API-Key"));
        QVERIFY(authorizeMapRequest(request, source, "key").isEmpty());
        QCOMPARE(request.rawHeader("X-API-Key"), QByteArray("key"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("Cookie"));
        QVERIFY(!validateMapAuthentication(source).isEmpty());
        source.insert(QStringLiteral("authMode"), QStringLiteral("query"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("apikey"));
        QVERIFY(authorizeMapRequest(request, source, "a&b=+%# /?").isEmpty());
        QCOMPARE(
            QUrlQuery(request.url()).queryItemValue(QStringLiteral("apikey"), QUrl::FullyDecoded),
            QStringLiteral("a&b=+%# /?"));
        QCOMPARE(QUrlQuery(request.url()).queryItems().size(), 1);
        QVERIFY(!authorizeMapRequest(request, source, "bad\r\nHeader: value").isEmpty());
        QVERIFY(!authorizeMapRequest(request, source, QByteArray(4097, 'x')).isEmpty());
        request.setUrl(QUrl(QStringLiteral("https://other.test/")));
        QVERIFY(!authorizeMapRequest(request, source, "token").isEmpty());
        request.setUrl(QUrl(QStringLiteral("http://tile.openstreetmap.org/")));
        QVERIFY(!authorizeMapRequest(request, source, "token").isEmpty());
        source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("ignoreTls"), false);
        source.insert(QStringLiteral("authEnvironment"), QStringLiteral("HOME"));
        QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("authEnvironment"), QStringLiteral("SWAK_MAP_TEST_SECRET"));
        qputenv("SWAK_MAP_TEST_SECRET", "environment-key");
        source.insert(QStringLiteral("format"), xyz->mapId());
        MapSources sources(QPluginLoader::staticInstances());
        QVERIFY(sources.put(QStringLiteral("auth-test"), source).isEmpty());
        QCOMPARE(sources.credential(QStringLiteral("auth-test")), QByteArray("environment-key"));
        QVERIFY(sources.setCredential(QStringLiteral("auth-test"), "session-key").isEmpty());
        QCOMPARE(sources.credential(QStringLiteral("auth-test")), QByteArray("session-key"));
        const auto saved = sources.savePreferences();
        const auto json = QJsonDocument(saved).toJson();
        QVERIFY(!json.contains("session-key"));
        QVERIFY(!json.contains("environment-key"));
        MapSources restored(QPluginLoader::staticInstances());
        restored.restorePreferences(saved);
        QCOMPARE(restored.credential(QStringLiteral("auth-test")), QByteArray("environment-key"));
        qunsetenv("SWAK_MAP_TEST_SECRET");
        QVERIFY(restored.credential(QStringLiteral("auth-test")).isEmpty());
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://example.test/{z}/{x}/{y}.png"));
        QVERIFY(sources.put(QStringLiteral("auth-test"), source).isEmpty());
        QVERIFY(sources.credential(QStringLiteral("auth-test")).isEmpty());
        QVERIFY(sources.setCredential(QStringLiteral("auth-test"), "new").isEmpty());
        sources.remove(QStringLiteral("auth-test"));
        QVERIFY(sources.credential(QStringLiteral("auth-test")).isEmpty());
        for (const auto &bad : {QJsonValue(QStringLiteral("user\nInjected")),
                                QJsonValue(QString(257, 'x')),
                                QJsonValue(42)}) {
            auto invalid = source;
            invalid.insert(QStringLiteral("authUser"), bad);
            QVERIFY(!xyz->validate(invalid).isEmpty());
        }
        source.insert(QStringLiteral("authMode"), true);
        QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("authMode"), QStringLiteral("unsupported"));
        QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("authMode"), QStringLiteral("basic"));
        source.insert(QStringLiteral("authUser"), QStringLiteral("a:b"));
        QVERIFY(!xyz->validate(source).isEmpty());
    }
    void authenticatedTiles()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto previous = QSslConfiguration::defaultConfiguration();
        auto trusted = previous;
        auto authorities = trusted.caCertificates();
        authorities.append(server.certificate);
        trusted.setCaCertificates(authorities);
        QSslConfiguration::setDefaultConfiguration(trusted);
        struct RestoreTls
        {
            QSslConfiguration value;
            ~RestoreTls()
            {
                QSslConfiguration::setDefaultConfiguration(value);
            }
        } restore{previous};
        auto *xyz = &urlTiles_;
        auto source = localSource(xyz, server, false);
        source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        TileRenderer renderer;
        QImage canvas(256, 256, QImage::Format_ARGB32);
        renderer.setSource(xyz, source);
        draw(renderer, canvas);
        QCOMPARE(renderer.pendingCount(), 0);
        QVERIFY(!renderer.status().isEmpty());
        renderer.setSource(xyz, source, "first-token");
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.coverage().rendered == 1;
            },
            5000));
        QCOMPARE(renderer.diskCacheBytes(), qint64(-1));
        QVERIFY(server.requests.first().toLower().contains("authorization: bearer first-token"));
        renderer.setSource(xyz, source, "second-token");
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.coverage().rendered == 1;
            },
            5000));
        QVERIFY(server.requests.last().toLower().contains("authorization: bearer second-token"));
        QCOMPARE(server.requests.size(), 2);
        server.headers = "HTTP/1.1 302 Found\r\nLocation: https://example.test/redirect\r\n";
        renderer.setSource(xyz, source, "third-token");
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty();
            },
            5000));
        QCOMPARE(renderer.coverage().rendered, 0);
        server.headers = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=map\r\n";
        renderer.setSource(xyz, source, "fourth-token");
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty();
            },
            5000));
        QVERIFY(!renderer.status().contains(QStringLiteral("fourth-token")));
        renderer.setSource(nullptr, {});
        QCOMPARE(renderer.pendingCount(), 0);
    }
    void templateAddressingAndPresets()
    {
        auto *raster = format(QStringLiteral("org.swak.gdal.raster"));
        QVERIFY(raster);
        auto source = raster->defaults();
        QCOMPARE(source.value(QStringLiteral("service")).toString(), QStringLiteral("xyz"));
        source.insert(QStringLiteral("invertY"), QStringLiteral("true"));
        QVERIFY(!raster->validate(source).isEmpty());
        source.insert(QStringLiteral("invertY"), false);
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://example.test/{q}.png"));
        QVERIFY(raster->validate(source).isEmpty());
        for (const auto &endpoint : {QStringLiteral("https://example.test/{q}/{x}.png"),
                                     QStringLiteral("https://example.test/{$q}.png"),
                                     QStringLiteral("https://example.test/${z}/{x}/{y}.png"),
                                     QStringLiteral("https://example.test/{z}/{x}.png"),
                                     QStringLiteral("http://example.test/{z}/{x}/{y}.png")}) {
            source.insert(QStringLiteral("endpoint"), endpoint);
            QVERIFY2(!raster->validate(source).isEmpty(), qPrintable(endpoint));
        }
        MapSources sources(QPluginLoader::staticInstances());
        const auto id = QStringLiteral("org.swak.xyz.usgs-topo");
        QVERIFY(sources.sources().contains(id));
        QVERIFY(!sources.networkEnabled());
        for (const auto &presetId : {QStringLiteral("org.swak.xyz.usgs-imagery"),
                                     QStringLiteral("org.swak.xyz.usgs-imagery-topo"),
                                     QStringLiteral("org.swak.xyz.usgs-relief"),
                                     QStringLiteral("org.swak.xyz.thunderforest-outdoors"),
                                     QStringLiteral("org.swak.xyz.thunderforest-cycle"),
                                     QStringLiteral("org.swak.wms"),
                                     QStringLiteral("org.swak.wmts")}) {
            const auto item = sources.sources().value(presetId).toObject();
            QVERIFY2(!item.isEmpty(), qPrintable(presetId));
            QCOMPARE(item.value(QStringLiteral("format")).toString(), raster->mapId());
            QVERIFY2(raster->validate(item).isEmpty(), qPrintable(raster->validate(item)));
        }
        QCOMPARE(sources.sources()
                     .value(QStringLiteral("org.swak.xyz.usgs-imagery-topo"))
                     .toObject()
                     .value(QStringLiteral("minZoom"))
                     .toInt(),
                 1);
        QCOMPARE(sources.sources().value(id).toObject().value(QStringLiteral("maxZoom")).toInt(),
                 8);
        sources.remove(id);
        const auto saved = sources.savePreferences();
        MapSources restored(QPluginLoader::staticInstances());
        restored.restorePreferences(saved);
        QVERIFY(!restored.sources().contains(id));
        auto old = saved;
        old.remove(QStringLiteral("knownPresets"));
        restored.restorePreferences(old);
        QVERIFY(restored.sources().contains(id));
        MapSources absent({});
        absent.restorePreferences(restored.savePreferences());
        QVERIFY(absent.sources().contains(id));
        QVERIFY(absent.activeSource().isEmpty());
    }
    void legacyProfiles()
    {
        // A profile from before GDAL names the removed XYZ, WMS, and WMTS plugins.
        QJsonObject xyz{{QStringLiteral("format"), QStringLiteral("org.swak.xyz")},
                        {QStringLiteral("name"), QStringLiteral("OpenStreetMap")},
                        {QStringLiteral("endpoint"),
                         QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png")},
                        {QStringLiteral("attribution"), QStringLiteral("OSM")},
                        {QStringLiteral("minZoom"), 0},
                        {QStringLiteral("maxZoom"), 19},
                        {QStringLiteral("ignoreTls"), false},
                        {QStringLiteral("invertY"), false}};
        auto wms = xyz;
        wms.insert(QStringLiteral("format"), QStringLiteral("org.swak.wms"));
        wms.insert(QStringLiteral("endpoint"), QStringLiteral("https://example.test/wms"));
        wms.insert(QStringLiteral("layer"), QStringLiteral("base"));
        wms.insert(QStringLiteral("style"), QString{});
        wms.insert(QStringLiteral("imageFormat"), QStringLiteral("image/png"));
        const QJsonObject legacy{
            {QStringLiteral("knownProviders"),
             QJsonArray{QStringLiteral("org.swak.xyz"),
                        QStringLiteral("org.swak.wmts"),
                        QStringLiteral("org.swak.wms"),
                        QStringLiteral("org.swak.mvt")}},
            {QStringLiteral("knownPresets"), QJsonArray{QStringLiteral("org.swak.xyz.usgs-topo")}},
            {QStringLiteral("network"), true},
            {QStringLiteral("active"), QStringLiteral("org.swak.xyz")},
            {QStringLiteral("sources"),
             QJsonObject{{QStringLiteral("org.swak.xyz"), xyz},
                         {QStringLiteral("custom-wms"), wms}}}};
        MapSources sources(QPluginLoader::staticInstances());
        sources.restorePreferences(legacy);
        const auto entries = sources.sources();
        // The old entries migrate. The old defaults and the replacement default do not return.
        QVERIFY(!entries.contains(QStringLiteral("org.swak.gdal.raster")));
        QVERIFY(!entries.contains(QStringLiteral("org.swak.wms")));
        QVERIFY(!entries.contains(QStringLiteral("org.swak.wmts")));
        QVERIFY(!entries.contains(QStringLiteral("org.swak.xyz.usgs-topo")));
        const auto migrated = entries.value(QStringLiteral("org.swak.xyz")).toObject();
        QCOMPARE(migrated.value(QStringLiteral("format")).toString(),
                 QStringLiteral("org.swak.gdal.raster"));
        QCOMPARE(migrated.value(QStringLiteral("service")).toString(), QStringLiteral("xyz"));
        QCOMPARE(entries.value(QStringLiteral("custom-wms"))
                     .toObject()
                     .value(QStringLiteral("service"))
                     .toString(),
                 QStringLiteral("wms"));
        QVERIFY(!sources.activeSource().isEmpty());
        const auto known =
            sources.savePreferences().value(QStringLiteral("knownProviders")).toArray();
        QVERIFY(known.contains(QStringLiteral("org.swak.gdal.raster")));
    }
    void gdalRasterFiles()
    {
        initializeGdal();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto *driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        QVERIFY(driver);
        OGRSpatialReference geographic;
        geographic.SetWellKnownGeogCS("WGS84");
        const auto create = [&](const QString &name,
                                int bands,
                                GDALDataType type,
                                const std::array<double, 6> &transform) {
            GdalDataset dataset(driver->Create(
                directory.filePath(name).toUtf8().constData(), 64, 64, bands, type, nullptr));
            if (dataset) {
                auto values = transform;
                dataset->SetGeoTransform(values.data());
                dataset->SetSpatialRef(&geographic);
            }
            return dataset;
        };
        const std::array<double, 6> world{-180, 360.0 / 64, 0, 85, 0, -170.0 / 64};
        {
            auto rgb = create(QStringLiteral("rgb.tif"), 3, GDT_Byte, world);
            QVERIFY(rgb);
            const std::array<int, 3> color{200, 30, 40};
            for (int band = 0; band < 3; ++band) {
                QCOMPARE(rgb->GetRasterBand(band + 1)->Fill(color[size_t(band)]), CE_None);
            }
            auto small = create(
                QStringLiteral("small.tif"), 3, GDT_Byte, {10, 10.0 / 64, 0, 20, 0, -10.0 / 64});
            QVERIFY(small);
            for (int band = 1; band <= 3; ++band) {
                QCOMPARE(small->GetRasterBand(band)->Fill(250), CE_None);
            }
            auto palette = create(QStringLiteral("palette.tif"), 1, GDT_Byte, world);
            QVERIFY(palette);
            GDALColorTable table;
            const GDALColorEntry green{20, 180, 60, 255};
            table.SetColorEntry(1, &green);
            QCOMPARE(palette->GetRasterBand(1)->SetColorTable(&table), CE_None);
            QCOMPARE(palette->GetRasterBand(1)->Fill(1), CE_None);
            auto elevation = create(QStringLiteral("elevation.tif"), 1, GDT_Int16, world);
            QVERIFY(elevation);
            std::vector<qint16> heights(64 * 64);
            for (int i = 0; i < 64 * 64; ++i) {
                heights[size_t(i)] = static_cast<qint16>((i % 64) * 100);
            }
            QCOMPARE(elevation->GetRasterBand(1)->RasterIO(
                         GF_Write, 0, 0, 64, 64, heights.data(), 64, 64, GDT_Int16, 0, 0, nullptr),
                     CE_None);
        }
        const auto tile = [&](const QString &name, int z = 0, int x = 0, int y = 0) {
            return renderOnWorker(
                RasterRequest{datasetSource(directory.filePath(name)), {}, {}, 0, z, x, y});
        };
        const auto rgb = tile(QStringLiteral("rgb.tif"));
        QVERIFY2(rgb.error.isEmpty(), qPrintable(rgb.error));
        QCOMPARE(rgb.image.size(), QSize(256, 256));
        QCOMPARE(rgb.image.pixelColor(128, 128), QColor(200, 30, 40));
        const auto small = tile(QStringLiteral("small.tif"), 4, 8, 7);
        QVERIFY2(small.error.isEmpty(), qPrintable(small.error));
        const auto inside = QPointF((15 + 180.0) / 360 * 16 - 8, project(15, 15).y() * 16 - 7);
        QCOMPARE(small.image.pixelColor(QPoint(int(inside.x() * 256), int(inside.y() * 256))),
                 QColor(250, 250, 250));
        // Pixels outside the dataset are transparent.
        QCOMPARE(small.image.pixelColor(2, 2).alpha(), 0);
        const auto palette = tile(QStringLiteral("palette.tif"));
        QVERIFY2(palette.error.isEmpty(), qPrintable(palette.error));
        QCOMPARE(palette.image.pixelColor(128, 128), QColor(20, 180, 60));
        const auto elevation = tile(QStringLiteral("elevation.tif"));
        QVERIFY2(elevation.error.isEmpty(), qPrintable(elevation.error));
        const auto west = elevation.image.pixelColor(20, 128);
        const auto east = elevation.image.pixelColor(236, 128);
        QCOMPARE(west.red(), west.blue());
        QVERIFY(west.red() < east.red());
        const auto missing = tile(QStringLiteral("absent.tif"));
        QVERIFY(!missing.error.isEmpty());
        QVERIFY(missing.providerWide);
        QVERIFY(!renderOnWorker(
                     RasterRequest{datasetSource(directory.filePath(QStringLiteral("rgb.tif"))),
                                   {},
                                   {},
                                   0,
                                   0,
                                   0,
                                   0},
                     true)
                     .error.isEmpty());
        QVERIFY(!tile(QStringLiteral("rgb.tif"), 20).error.isEmpty());
        TileRenderer renderer;
        auto *raster = format(QStringLiteral("org.swak.gdal.raster"));
        renderer.setSource(raster, datasetSource(directory.filePath(QStringLiteral("rgb.tif"))));
        QImage canvas(256, 256, QImage::Format_ARGB32);
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.coverage().rendered == 1;
            },
            10000));
        draw(renderer, canvas);
        QCOMPARE(canvas.pixelColor(128, 128), QColor(200, 30, 40));
        // A tile whose image cannot be made fails. A thrown allocation failure stops only its
        // worker. Afterwards the same dataset renders again.
        {
            const ImageFaultGuard fault(ImageFault::NullImage);
            QCOMPARE(tile(QStringLiteral("rgb.tif"), 1, 0, 0).error,
                     QStringLiteral("Map tile image allocation failed."));
        }
        for (const auto &[fault, text] :
             {std::pair{ImageFault::NullImage, QStringLiteral("Map tile image allocation failed.")},
              std::pair{ImageFault::Throw, QStringLiteral("Map tile rendering failed.")}}) {
            const ImageFaultGuard guard(fault);
            TileRenderer failing;
            failing.setSource(raster, datasetSource(directory.filePath(QStringLiteral("rgb.tif"))));
            QVERIFY2(QTest::qWaitFor(
                         [&] {
                             draw(failing, canvas);
                             return failing.status().contains(text);
                         },
                         10000),
                     qPrintable(failing.status()));
            QCOMPARE(failing.coverage().rendered, 0);
        }
        const auto again = tile(QStringLiteral("rgb.tif"), 1, 1, 0);
        QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
        QCOMPARE(again.image.pixelColor(128, 128), QColor(200, 30, 40));
        renderer.setSource(raster, datasetSource(directory.filePath(QStringLiteral("absent.tif"))));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty();
            },
            10000));
        QCOMPARE(renderer.coverage().rendered, 0);
    }
    void gdalRasterServices()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto *raster = format(QStringLiteral("org.swak.gdal.raster"));
        auto source = raster->defaults();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        source.insert(QStringLiteral("maxZoom"), 3);
        const auto path = [&] {
            const auto request = server.requests.last();
            return request.mid(4, request.indexOf(" HTTP/") - 4);
        };
        auto tile = renderOnWorker(RasterRequest{source, {}, {}, 0, 2, 1, 3});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QCOMPARE(path(), QByteArray("/2/1/3.png"));
        QCOMPARE(tile.image.pixelColor(128, 128), QColor("#eb426a"));
        QVERIFY(server.requests.last().toLower().contains("user-agent: swak/"));
        source.insert(QStringLiteral("invertY"), true);
        tile = renderOnWorker(RasterRequest{source, {}, {}, 0, 2, 1, 3});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QCOMPARE(path(), QByteArray("/2/1/0.png"));
        source.insert(QStringLiteral("invertY"), false);
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://localhost:%1/{q}.png").arg(server.serverPort()));
        tile = renderOnWorker(RasterRequest{source, {}, {}, 0, 3, 3, 5});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QCOMPARE(path(), QByteArray("/213.png"));
        // Without the override, GDAL rejects the test certificate.
        source.insert(QStringLiteral("ignoreTls"), false);
        tile = renderOnWorker(RasterRequest{source, {}, {}, 0, 3, 3, 5});
        QVERIFY(!tile.error.isEmpty());
        // Trust the test certificate and send credentials.
        CPLSetConfigOption("CURL_CA_BUNDLE",
                           QFINDTESTDATA("fixtures/map-test-cert.pem").toUtf8().constData());
        struct RestoreBundle
        {
            ~RestoreBundle()
            {
                CPLSetConfigOption("CURL_CA_BUNDLE", nullptr);
            }
        } restore;
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        QVERIFY(raster->validate(source).isEmpty());
        tile = renderOnWorker(RasterRequest{source, "map-token", {}, 0, 1, 0, 0});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QVERIFY(server.requests.last().toLower().contains("authorization: bearer map-token"));
        source.insert(QStringLiteral("authMode"), QStringLiteral("basic"));
        source.insert(QStringLiteral("authUser"), QStringLiteral("test-user"));
        tile = renderOnWorker(RasterRequest{source, "password", {}, 0, 2, 2, 2});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QVERIFY(server.requests.last().contains("Authorization: Basic dGVzdC11c2VyOnBhc3N3b3Jk"));
        source.insert(QStringLiteral("authMode"), QStringLiteral("header"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("X-API-Key"));
        tile = renderOnWorker(RasterRequest{source, "map-key", {}, 0, 2, 3, 2});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QVERIFY(server.requests.last().toLower().contains("x-api-key: map-key"));
        // A zoom above the provider range and an invalid provider fail without a request.
        const auto requests = server.requests.size();
        tile = renderOnWorker(RasterRequest{source, "map-key", {}, 0, 4, 0, 0});
        QCOMPARE(tile.error, QStringLiteral("The map tile is beyond the provider zoom range."));
        auto invalid = source;
        invalid.insert(QStringLiteral("service"), QStringLiteral("wmts"));
        invalid.insert(QStringLiteral("endpoint"),
                       QStringLiteral("https://localhost:%1/wmts").arg(server.serverPort()));
        invalid.insert(QStringLiteral("layer"), QStringLiteral("roads"));
        invalid.insert(QStringLiteral("imageFormat"), QStringLiteral("image/png"));
        invalid.insert(QStringLiteral("matrixSet"), QString());
        QCOMPARE(raster->validate(invalid),
                 QStringLiteral("Enter a bounded layer and matrix set."));
        tile = renderOnWorker(RasterRequest{invalid, "map-key", {}, 0, 1, 0, 0});
        QCOMPARE(tile.error, QStringLiteral("The map provider is invalid."));
        QVERIFY(tile.providerWide);
        QCOMPARE(server.requests.size(), requests);
        source.insert(QStringLiteral("authMode"), QStringLiteral("query"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("apikey"));
        tile = renderOnWorker(RasterRequest{source, "a&b", {}, 0, 1, 1, 0});
        QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
        QCOMPARE(path(), QByteArray("/1/1/0.png?apikey=a%26b"));
        QCOMPARE(redactQueryValues(QStringLiteral("GET https://x.test/t.png?apikey=secret&b=2")),
                 QStringLiteral("GET https://x.test/t.png?apikey=***&b=***"));
        server.headers = "HTTP/1.1 401 Unauthorized\r\n";
        tile = renderOnWorker(RasterRequest{source, "secret-key", {}, 0, 1, 1, 1});
        QVERIFY(!tile.error.isEmpty());
        QVERIFY(!tile.error.contains(QStringLiteral("secret-key")));
    }
    void nearbyTiles()
    {
        const Viewport view{QSizeF(256, 256), QPointF(0.375, 0.375), 2};
        const auto visible = visibleTiles(view, 0, 19);
        QCOMPARE(visible.size(), 1);
        const auto area = tileFetchArea(visible, 1);
        QCOMPARE(area.size(), 9);
        QCOMPARE(area.first().key, visible.first().key);
        QCOMPARE(tileFetchArea(visible, 0).size(), 1);
        QCOMPARE(tileFetchArea(visible, 2).size(), 16);
        const auto edge =
            tileFetchArea(visibleTiles({QSizeF(256, 256), QPointF(0.125, 0.125), 2}, 0, 19), 1);
        QCOMPARE(edge.size(), 4);
        const auto large = visibleTiles({QSizeF(4096, 4096), QPointF(0.5, 0.5), 10}, 0, 19);
        const auto bounded = tileFetchArea(large, 2);
        QVERIFY(bounded.size() <= large.size() + 64);
        QSet<QString> keys;
        for (const auto &tile : bounded) {
            QVERIFY(!keys.contains(tile.key));
            keys.insert(tile.key);
        }
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        const auto drawView = [&](const Viewport &position) {
            QPainter painter(&canvas);
            renderer.paint(painter, position, 1, false);
        };
        drawView(view);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return server.requests.size() == 9 && renderer.pendingCount() == 0;
            },
            5000));
        QVERIFY(server.requests.first().contains("/2/1/1.png"));
        renderer.setPrefetchMargin(0);
        drawView({QSizeF(256, 256), QPointF(0.625, 0.375), 2});
        QCOMPARE(canvas.pixelColor(128, 128), QColor("#eb426a"));
        QCOMPARE(server.requests.size(), 9);
        renderer.stop();
        QCOMPARE(renderer.pendingCount(), 0);
    }
    void missingTiles()
    {
        TileRenderer renderer;
        QImage canvas(512, 512, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        const Viewport view{QSizeF(512, 512), QPointF(0.5, 0.5), 1};
        {
            QPainter painter(&canvas);
            renderer.paint(painter, view, 1, false);
        }
        QCOMPARE(renderer.pendingCount(), 0);
        for (int y : {0, 256}) {
            for (int x : {0, 256}) {
                QCOMPARE(canvas.pixelColor(x + 20, y + 20), QColor(MapBackgroundRgb));
                int textPixels = 0;
                QRect textBounds;
                for (int py = y + 50; py < y + 206; ++py) {
                    for (int px = x + 50; px < x + 206; ++px) {
                        const auto color = canvas.pixelColor(px, py);
                        if (color != QColor(MapBackgroundRgb)) {
                            ++textPixels;
                            textBounds = textBounds.united(QRect(px, py, 1, 1));
                            QVERIFY(color.red() < 100);
                            QVERIFY(color.green() < 110);
                        }
                    }
                }
                QVERIFY(textPixels > 20);
                QVERIFY(textBounds.height() > 30);
                QVERIFY(std::abs(textBounds.width() - textBounds.height()) < 12);
            }
        }
        {
            QPainter painter(&canvas);
            renderer.paint(painter, view, 1, false, 0);
        }
        for (int y = 0; y < 512; ++y) {
            for (int x = 0; x < 512; ++x) {
                QCOMPARE(canvas.pixelColor(x, y), QColor(MapBackgroundRgb));
            }
        }
    }
    void formats()
    {
        MapSources sources(QPluginLoader::staticInstances());
        QCOMPARE(sources.providers().size(), 4);
        // The first provider choice in Settings clears the selection.
        const auto fields = sources.settingsFields();
        const auto active = std::find_if(fields.cbegin(), fields.cend(), [](const auto &field) {
            return field.path == u"active";
        });
        QVERIFY(active != fields.cend());
        QCOMPARE(active->choices.value(0), QStringLiteral("Disable"));
        QCOMPARE(active->choiceValues.at(0).toString(), QString{});
        QVERIFY(!sources.networkEnabled());
        QVERIFY(sources.activeSource().isEmpty());
        for (auto *provider : sources.providers()) {
            const auto source = provider->defaults();
            QVERIFY2(provider->validate(source).isEmpty(), qPrintable(provider->validate(source)));
            QVERIFY(!source.value(QStringLiteral("ignoreTls")).toBool());
            QVERIFY(provider->tileUrl(source, -1, 0, 0).isEmpty());
            QVERIFY(provider->tileUrl(source, 1, 2, 0).isEmpty());
            QVERIFY(provider->tileUrl(source, 20, 0, 0).isEmpty());
            QVERIFY(!provider->mapName().isEmpty());
            auto bad = source;
            bad.insert(QStringLiteral("endpoint"), QStringLiteral("http://example.com"));
            QVERIFY(!provider->validate(bad).isEmpty());
            bad = source;
            bad.insert(QStringLiteral("minZoom"), 19);
            bad.insert(QStringLiteral("maxZoom"), 1);
            QVERIFY(!provider->validate(bad).isEmpty());
        }
        auto *raster = format(QStringLiteral("org.swak.gdal.raster"));
        // The raster provider renders tiles itself and returns no URL.
        QVERIFY(raster->tileUrl(raster->defaults(), 2, 1, 3).isEmpty());
        QVERIFY(dynamic_cast<RasterTileProvider *>(raster));
        for (const auto &id : {QStringLiteral("org.swak.wms"), QStringLiteral("org.swak.wmts")}) {
            const auto service = sources.sources().value(id).toObject();
            auto invalid = service;
            invalid.insert(QStringLiteral("imageFormat"), QStringLiteral("unknown"));
            QVERIFY(!raster->validate(invalid).isEmpty());
            invalid = service;
            invalid.insert(QStringLiteral("layer"), QString{});
            QVERIFY(!raster->validate(invalid).isEmpty());
            invalid = service;
            invalid.insert(QStringLiteral("style"), QString(101, 'x'));
            QVERIFY(!raster->validate(invalid).isEmpty());
            invalid = service;
            invalid.insert(QStringLiteral("endpoint"),
                           service.value(QStringLiteral("endpoint")).toString() +
                               QStringLiteral("?SERVICE=WMS"));
            QVERIFY(!raster->validate(invalid).isEmpty());
        }
        auto dataset = raster->defaults();
        dataset.insert(QStringLiteral("service"), QStringLiteral("dataset"));
        for (const auto &endpoint : {QStringLiteral("/data/map.tif"),
                                     QStringLiteral("/vsicurl/https://example.test/map.tif"),
                                     QStringLiteral("WMTS:https://example.test/caps.xml,layer=a"),
                                     QStringLiteral("WMS:https://example.test/wms")}) {
            dataset.insert(QStringLiteral("endpoint"), endpoint);
            QVERIFY2(raster->validate(dataset).isEmpty(), qPrintable(endpoint));
        }
        for (const auto &endpoint : {QStringLiteral("relative/map.tif"),
                                     QStringLiteral("/data/../etc/map.tif"),
                                     QStringLiteral("/vsicurl/http://example.test/map.tif"),
                                     QStringLiteral("/vsizip//data/map.zip"),
                                     QStringLiteral("<GDAL_WMS></GDAL_WMS>"),
                                     QStringLiteral("PG:dbname=maps")}) {
            dataset.insert(QStringLiteral("endpoint"), endpoint);
            QVERIFY2(!raster->validate(dataset).isEmpty(), qPrintable(endpoint));
        }
        dataset.insert(QStringLiteral("endpoint"), QStringLiteral("/data/map.tif"));
        dataset.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        QVERIFY(!raster->validate(dataset).isEmpty());
        dataset.insert(QStringLiteral("authMode"), QStringLiteral("none"));
        dataset.insert(QStringLiteral("service"), QStringLiteral("unknown"));
        QVERIFY(!raster->validate(dataset).isEmpty());
    }
    void portableProviders()
    {
        MapSources sources(QPluginLoader::staticInstances());
        const auto id = QStringLiteral("custom.xyz");
        QVERIFY(
            sources
                .put(id, sources.sources().value(QStringLiteral("org.swak.gdal.raster")).toObject())
                .isEmpty());
        sources.select(id);
        QVERIFY(!sources.activeSource().isEmpty());
        const auto saved = sources.savePreferences();
        MapSources absent({});
        absent.restorePreferences(saved);
        QVERIFY(absent.activeSource().isEmpty());
        QCOMPARE(absent.savePreferences(), saved);
        MapSources restored(QPluginLoader::staticInstances());
        restored.restorePreferences(absent.savePreferences());
        QCOMPARE(restored.activeId(), id);
        QVERIFY(!restored.activeSource().isEmpty());
        restored.remove(id);
        QVERIFY(restored.activeSource().isEmpty());
        auto bad = saved;
        auto entries = bad.value(QStringLiteral("sources")).toObject();
        entries.insert(QStringLiteral("bad/path"), QJsonObject{});
        bad.insert(QStringLiteral("sources"), entries);
        const auto before = restored.savePreferences();
        restored.restorePreferences(bad);
        QCOMPARE(restored.savePreferences(), before);
    }
    void invalidSourceRecoveryAndQueryEncoding()
    {
        MapSources maps(QPluginLoader::staticInstances());
        auto saved = maps.savePreferences();
        auto entries = maps.sources();
        entries.insert(QStringLiteral("custom"),
                       entries.value(QStringLiteral("org.swak.gdal.raster")));
        auto invalid = entries.value(QStringLiteral("org.swak.wms")).toObject();
        invalid.insert(QStringLiteral("endpoint"), QStringLiteral("invalid"));
        entries.insert(QStringLiteral("org.swak.wms"), invalid);
        saved.insert(QStringLiteral("sources"), entries);
        maps.restorePreferences(saved);
        QCOMPARE(maps.savePreferences(), saved);
        maps.select(QStringLiteral("org.swak.wms"));
        QVERIFY(maps.activeSource().isEmpty());
        QVERIFY(
            maps.put(QStringLiteral("custom"), entries.value(QStringLiteral("custom")).toObject())
                .isEmpty());
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto *raster = format(QStringLiteral("org.swak.gdal.raster"));
        for (const auto &id : {QStringLiteral("org.swak.wms"), QStringLiteral("org.swak.wmts")}) {
            auto values =
                MapSources(QPluginLoader::staticInstances()).sources().value(id).toObject();
            values.insert(QStringLiteral("endpoint"),
                          QStringLiteral("https://localhost:%1/service").arg(server.serverPort()));
            values.insert(QStringLiteral("ignoreTls"), true);
            values.insert(QStringLiteral("layer"), QStringLiteral("a+b"));
            values.insert(QStringLiteral("time"), QStringLiteral("12:00:00+02:00"));
            values.insert(QStringLiteral("maxZoom"), 2);
            const auto before = server.requests.size();
            std::atomic_bool cancelled = false;
            Q_UNUSED(cancelled)
            Q_UNUSED(raster)
            const auto tile = renderOnWorker(RasterRequest{values, {}, {}, 0, 1, 1, 0});
            QVERIFY2(tile.error.isEmpty(), qPrintable(tile.error));
            QVERIFY(server.requests.size() > before);
            const auto request = server.requests.last().left(server.requests.last().indexOf('\r'));
            QVERIFY2(request.contains("a%2Bb"), request.constData());
            QVERIFY2(!request.contains('+'), request.constData());
        }
    }
    void failedTilesDoNotStarveNeighbors()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.failFirst = 4;
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(1024, 1024, QImage::Format_RGB32);
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(1024, 1024), QPointF(0.5, 0.5), 2}, 1, false);
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return server.requests.size() == 16;
            },
            5000));
        QVERIFY(QTest::qWaitFor([&] {
            return renderer.pendingCount() == 0;
        }));
        QVERIFY(!renderer.status().isEmpty());
        QSet<QByteArray> paths;
        for (const auto &request : server.requests) {
            paths.insert(request.split(' ').at(1));
        }
        QCOMPARE(paths.size(), 16);
    }
    void cancelDuringProviderBackoff()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.failFirst = 1;
        server.failureHeaders = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 60\r\n";
        server.holdRequests = {2, 3, 4};
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(1024, 1024, QImage::Format_RGB32);
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(1024, 1024), QPointF(0.5, 0.5), 2}, 1, false);
        }
        QVERIFY(QTest::qWaitFor([&] {
            return !renderer.status().isEmpty() && server.requests.size() == 4;
        }));
        QCOMPARE(renderer.pendingCount(), 3);
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(2, 2), 0}, 1, false);
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.pendingCount() == 0;
            },
            2000));
        QCOMPARE(server.requests.size(), 4);
    }
    void projectionAndDecoding()
    {
        auto tiles = visibleTiles(Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 0, 19);
        QCOMPARE(tiles.size(), 1);
        QCOMPARE(tiles.first().rectangle, QRectF(0, 0, 256, 256));
        QVERIFY(visibleTiles(Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 3, 19).isEmpty());
        QVERIFY(visibleTiles(Viewport{QSizeF(256, 256),
                                      QPointF(0.5, 0.5),
                                      std::numeric_limits<double>::infinity()},
                             0,
                             19)
                    .isEmpty());
        QVERIFY(visibleTiles(Viewport{QSizeF(16000, 16000), QPointF(0.5, 0.5), 19}, 0, 19).size() <=
                256);
        TileServer fixture;
        const auto image = decodeMapTile(fixture.body);
        QCOMPARE(image.size(), QSize(256, 256));
        QCOMPARE(image.pixelColor(0, 0), QColor("#eb426a"));
        QVERIFY(decodeMapTile("<xml>not an image</xml>").isNull());
        QVERIFY(decodeMapTile(QByteArray(2 * 1024 * 1024 + 1, 'x')).isNull());
        QImage huge(1024, 1024, QImage::Format_RGB32);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        huge.save(&buffer, "PNG");
        QVERIFY(decodeMapTile(bytes).isNull());
    }
    void httpsAndCache()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto *xyz = &urlTiles_;
        QImage canvas(256, 256, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        {
            TileRenderer renderer;
            renderer.setDiskLimit(16);
            QCOMPARE(renderer.diskCacheBytes(), -1);
            renderer.setSource(xyz, localSource(xyz, server, false));
            draw(renderer, canvas);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return !renderer.status().isEmpty() &&
                           !renderer.status().startsWith(QStringLiteral("Map coverage:"));
                },
                5000));
            QVERIFY(server.requests.isEmpty());
            renderer.setSource(xyz, localSource(xyz, server, true));
            QSignalSpy changed(&renderer, &TileRenderer::changed);
            draw(renderer, canvas);
            QVERIFY(changed.wait(5000));
            QVERIFY2(renderer.status().isEmpty(), qPrintable(renderer.status()));
            draw(renderer, canvas);
            QCOMPARE(canvas.pixelColor(128, 128), QColor("#eb426a"));
            QCOMPARE(server.requests.size(), 1);
            QVERIFY(server.requests.first().toLower().contains("user-agent: swak/"));
            QVERIFY(!server.requests.first().contains("no-cache"));
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return renderer.diskCacheBytes() > 0;
                },
                3000));
        }
        {
            TileRenderer cached;
            cached.setSource(xyz, localSource(xyz, server, true));
            QVERIFY(cached.diskCacheBytes() > 0);
            cached.setDiskLimit(32);
            QSignalSpy changed(&cached, &TileRenderer::changed);
            draw(cached, canvas);
            QVERIFY(changed.wait(5000));
            QCOMPARE(server.requests.size(), 1);
        }
        {
            TileRenderer strict;
            strict.setSource(xyz, localSource(xyz, server, false));
            draw(strict, canvas);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return !strict.status().isEmpty() &&
                           !strict.status().startsWith(QStringLiteral("Map coverage:"));
                },
                5000));
            QCOMPARE(server.requests.size(), 1);
        }
    }
    void cacheLockRetry()
    {
        auto *xyz = &urlTiles_;
        TileRenderer first;
        TileRenderer second;
        first.setSource(xyz, xyz->defaults());
        QVERIFY(first.status().isEmpty());
        second.setSource(xyz, xyz->defaults());
        QVERIFY(!second.status().isEmpty());
        first.setSource(nullptr, {});
        second.setSource(xyz, xyz->defaults());
        QVERIFY(second.status().isEmpty());
    }
    void expiredTile()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: "
                         "max-age=1\r\nETag: \"fixture\"\r\n";
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        QSignalSpy changed(&renderer, &TileRenderer::changed);
        draw(renderer, canvas);
        QVERIFY(changed.wait(5000));
        const auto expired = QDateTime::currentDateTimeUtc().addSecs(2);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return QDateTime::currentDateTimeUtc() >= expired;
            },
            3000));
        canvas.fill(Qt::black);
        draw(renderer, canvas);
        QCOMPARE(canvas.pixelColor(128, 128), QColor("#eb426a"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return server.requests.size() == 2;
            },
            5000));
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(server.requests.last().toLower().contains("if-none-match: \"fixture\""));
    }
    void noStoreAndOpacity()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.headers =
            "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: no-store\r\n";
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 0, false);
        }
        QCOMPARE(renderer.pendingCount(), 0);
        QSignalSpy changed(&renderer, &TileRenderer::changed);
        draw(renderer, canvas);
        QVERIFY(changed.wait(5000));
        draw(renderer, canvas);
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return server.requests.size() > 1;
            },
            300));
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(2, 2), 0}, 1, false);
        }
        draw(renderer, canvas);
        QVERIFY(changed.wait(5000));
        QCOMPARE(server.requests.size(), 2);
        renderer.stop();
        draw(renderer, canvas);
        QVERIFY(changed.wait(5000));
        QCOMPARE(server.requests.size(), 3);
    }
    void rejectedResponses_data()
    {
        QTest::addColumn<QByteArray>("headers");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("invalid-image") << QByteArray("HTTP/1.1 200 OK\r\n")
                                       << QByteArray("<ServiceException>error</ServiceException>");
        QTest::newRow("oversized-length")
            << QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 3000000\r\n") << QByteArray("bad");
        QTest::newRow("oversized-body")
            << QByteArray("HTTP/1.1 200 OK\r\n") << QByteArray(2 * 1024 * 1024 + 1, 'x');
        QTest::newRow("rate-limit")
            << QByteArray("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 60\r\n")
            << QByteArray("busy");
    }
    void rejectedResponses()
    {
        QFETCH(QByteArray, headers);
        QFETCH(QByteArray, body);
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.headers = headers;
        server.body = body;
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty() &&
                       !renderer.status().startsWith(QStringLiteral("Map coverage:"));
            },
            5000));
        QCOMPARE(renderer.pendingCount(), 0);
        QCOMPARE(canvas.pixelColor(20, 20), QColor(MapBackgroundRgb));
        draw(renderer, canvas);
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return server.requests.size() > 1;
            },
            300));
    }
    void concurrencyAndTimeout()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.hold = true;
        auto *xyz = &urlTiles_;
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(1024, 1024, QImage::Format_RGB32);
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(1024, 1024), QPointF(0.5, 0.5), 3}, 1, false);
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.pendingCount() == 4;
            },
            5000));
        {
            QPainter painter(&canvas);
            renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(2, 2), 0}, 1, false);
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.pendingCount() == 0;
            },
            5000));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty() &&
                       !renderer.status().startsWith(QStringLiteral("Map coverage:"));
            },
            16000));
        QVERIFY(renderer.pendingCount() <= 4);
        renderer.stop();
        QCOMPARE(renderer.pendingCount(), 0);
        for (int i = 0; i < 20; ++i) {
            renderer.setSource(nullptr, {});
            renderer.setSource(xyz, localSource(xyz, server, true));
        }
        renderer.stop();
    }
    void failureAndCancellation()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto *xyz = &urlTiles_;
        server.headers = "HTTP/1.1 302 Found\r\nLocation: https://example.org/\r\n";
        QImage canvas(256, 256, QImage::Format_RGB32);
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !renderer.status().isEmpty() &&
                       !renderer.status().startsWith(QStringLiteral("Map coverage:"));
            },
            5000));
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(renderer.pendingCount(), 0);
        renderer.setSource(nullptr, {});
        QVERIFY(renderer.status().isEmpty());
        server.hold = true;
        renderer.setSource(xyz, localSource(xyz, server, true));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return renderer.pendingCount() > 0;
            },
            5000));
        renderer.stop();
        QCOMPARE(renderer.pendingCount(), 0);
    }
};
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // Close cached GDAL resources after the tests and before the leak check.
    swak::GdalShutdownGuard gdal;
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    app.setApplicationName(QStringLiteral("swak-map-tests"));
    MapTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_maps.moc"
