#include "core/maps.h"
#include "core/tiles.h"
#include "tile_server.h"
#include <QPluginLoader>
#include <QPainter>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <limits>
#include <QSslConfiguration>
#include <QJsonDocument>
using namespace swak;
class MapTest final : public QObject {
    Q_OBJECT
    MapProvider *format(const QString &id)
    {
        for (auto *object : QPluginLoader::staticInstances())
            if (auto *provider = qobject_cast<MapProvider *>(object); provider && provider->mapId() == id) return provider;
        return nullptr;
    }
    QJsonObject localSource(MapProvider *provider, const TileServer &server, bool ignore)
    {
        auto source = provider->defaults();
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), ignore);
        return source;
    }
    void draw(TileRenderer &renderer, QImage &canvas)
    {
        QPainter painter(&canvas);
        renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 1, false);
    }
private slots:
    void mapCredentials()
    {
        auto *xyz = format(QStringLiteral("org.swak.xyz")); QVERIFY(xyz);
        auto source = xyz->defaults();
        source.insert(QStringLiteral("authMode"), QStringLiteral("basic"));
        source.insert(QStringLiteral("authUser"), QStringLiteral("test-user"));
        QNetworkRequest request(xyz->tileUrl(source, 0, 0, 0));
        QVERIFY(authorizeMapRequest(request, source, "password").isEmpty());
        QCOMPARE(request.rawHeader("Authorization"), QByteArray("Basic dGVzdC11c2VyOnBhc3N3b3Jk"));
        QCOMPARE(request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool(), false);
        source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        QVERIFY(authorizeMapRequest(request, source, "token").isEmpty()); QCOMPARE(request.rawHeader("Authorization"), QByteArray("Bearer token"));
        source.insert(QStringLiteral("authMode"), QStringLiteral("header")); source.insert(QStringLiteral("authParameter"), QStringLiteral("X-API-Key"));
        QVERIFY(authorizeMapRequest(request, source, "key").isEmpty()); QCOMPARE(request.rawHeader("X-API-Key"), QByteArray("key"));
        source.insert(QStringLiteral("authParameter"), QStringLiteral("Cookie")); QVERIFY(!validateMapAuthentication(source).isEmpty());
        source.insert(QStringLiteral("authMode"), QStringLiteral("query")); source.insert(QStringLiteral("authParameter"), QStringLiteral("apikey"));
        QVERIFY(authorizeMapRequest(request, source, "a&b=+%# /?").isEmpty());
        QCOMPARE(QUrlQuery(request.url()).queryItemValue(QStringLiteral("apikey"), QUrl::FullyDecoded), QStringLiteral("a&b=+%# /?"));
        QCOMPARE(QUrlQuery(request.url()).queryItems().size(), 1);
        QVERIFY(!authorizeMapRequest(request, source, "bad\r\nHeader: value").isEmpty());
        QVERIFY(!authorizeMapRequest(request, source, QByteArray(4097, 'x')).isEmpty());
        request.setUrl(QUrl(QStringLiteral("https://other.test/"))); QVERIFY(!authorizeMapRequest(request, source, "token").isEmpty());
        request.setUrl(QUrl(QStringLiteral("http://tile.openstreetmap.org/"))); QVERIFY(!authorizeMapRequest(request, source, "token").isEmpty());
        source.insert(QStringLiteral("ignoreTls"), true); QVERIFY(!xyz->validate(source).isEmpty()); source.insert(QStringLiteral("ignoreTls"), false);
        source.insert(QStringLiteral("authEnvironment"), QStringLiteral("HOME")); QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("authEnvironment"), QStringLiteral("SWAK_MAP_TEST_SECRET"));
        qputenv("SWAK_MAP_TEST_SECRET", "environment-key");
        source.insert(QStringLiteral("format"), xyz->mapId());
        MapSources sources(QPluginLoader::staticInstances()); QVERIFY(sources.put(QStringLiteral("auth-test"), source).isEmpty());
        QCOMPARE(sources.credential(QStringLiteral("auth-test")), QByteArray("environment-key"));
        QVERIFY(sources.setCredential(QStringLiteral("auth-test"), "session-key").isEmpty());
        QCOMPARE(sources.credential(QStringLiteral("auth-test")), QByteArray("session-key"));
        const auto saved = sources.savePreferences(); const auto json = QJsonDocument(saved).toJson();
        QVERIFY(!json.contains("session-key")); QVERIFY(!json.contains("environment-key"));
        MapSources restored(QPluginLoader::staticInstances()); restored.restorePreferences(saved);
        QCOMPARE(restored.credential(QStringLiteral("auth-test")), QByteArray("environment-key"));
        qunsetenv("SWAK_MAP_TEST_SECRET"); QVERIFY(restored.credential(QStringLiteral("auth-test")).isEmpty());
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://example.test/{z}/{x}/{y}.png"));
        QVERIFY(sources.put(QStringLiteral("auth-test"), source).isEmpty()); QVERIFY(sources.credential(QStringLiteral("auth-test")).isEmpty());
        QVERIFY(sources.setCredential(QStringLiteral("auth-test"), "new").isEmpty()); sources.remove(QStringLiteral("auth-test")); QVERIFY(sources.credential(QStringLiteral("auth-test")).isEmpty());
        for (const auto &bad : {QJsonValue(QStringLiteral("user\nInjected")), QJsonValue(QString(257, 'x')), QJsonValue(42)}) {
            auto invalid = source; invalid.insert(QStringLiteral("authUser"), bad); QVERIFY(!xyz->validate(invalid).isEmpty());
        }
        source.insert(QStringLiteral("authMode"), true); QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("authMode"), QStringLiteral("unsupported")); QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("authMode"), QStringLiteral("basic")); source.insert(QStringLiteral("authUser"), QStringLiteral("a:b")); QVERIFY(!xyz->validate(source).isEmpty());
    }
    void authenticatedTiles()
    {
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto previous = QSslConfiguration::defaultConfiguration(); auto trusted = previous;
        auto authorities = trusted.caCertificates(); authorities.append(server.certificate); trusted.setCaCertificates(authorities); QSslConfiguration::setDefaultConfiguration(trusted);
        struct RestoreTls { QSslConfiguration value; ~RestoreTls() { QSslConfiguration::setDefaultConfiguration(value); } } restore{previous};
        auto *xyz = format(QStringLiteral("org.swak.xyz")); auto source = localSource(xyz, server, false);
        source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        TileRenderer renderer; QImage canvas(256, 256, QImage::Format_ARGB32);
        renderer.setSource(xyz, source); draw(renderer, canvas); QCOMPARE(renderer.pendingCount(), 0); QVERIFY(!renderer.status().isEmpty());
        renderer.setSource(xyz, source, "first-token"); draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return renderer.coverage().rendered == 1; }, 5000));
        QCOMPARE(renderer.diskCacheBytes(), qint64(-1)); QVERIFY(server.requests.first().toLower().contains("authorization: bearer first-token"));
        renderer.setSource(xyz, source, "second-token"); draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return renderer.coverage().rendered == 1; }, 5000));
        QVERIFY(server.requests.last().toLower().contains("authorization: bearer second-token")); QCOMPARE(server.requests.size(), 2);
        server.headers = "HTTP/1.1 302 Found\r\nLocation: https://example.test/redirect\r\n";
        renderer.setSource(xyz, source, "third-token"); draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty(); }, 5000)); QCOMPARE(renderer.coverage().rendered, 0);
        server.headers = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=map\r\n";
        renderer.setSource(xyz, source, "fourth-token"); draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty(); }, 5000)); QVERIFY(!renderer.status().contains(QStringLiteral("fourth-token")));
        renderer.setSource(nullptr, {}); QCOMPARE(renderer.pendingCount(), 0);
    }
    void templateAddressingAndPresets()
    {
        auto *xyz = format(QStringLiteral("org.swak.xyz")); QVERIFY(xyz);
        auto source = xyz->defaults(); source.insert(QStringLiteral("invertY"), true);
        QCOMPARE(xyz->tileUrl(source, 2, 1, 3).path(), QStringLiteral("/2/1/0.png"));
        source.insert(QStringLiteral("invertY"), QStringLiteral("true"));
        QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("invertY"), false);
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://example.test/{q}.png"));
        QVERIFY(xyz->validate(source).isEmpty());
        QCOMPARE(xyz->tileUrl(source, 3, 3, 5).path(), QStringLiteral("/213.png"));
        QCOMPARE(xyz->tileUrl(source, 0, 0, 0).path(), QStringLiteral("/.png"));
        QCOMPARE(xyz->tileUrl(source, 19, 524287, 524287).path(), QStringLiteral("/") + QString(19, '3') + QStringLiteral(".png"));
        QVERIFY(xyz->tileUrl(source, 20, 0, 0).isEmpty());
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://example.test/{q}/{x}.png"));
        QVERIFY(!xyz->validate(source).isEmpty());
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://example.test/{$q}.png"));
        QVERIFY(!xyz->validate(source).isEmpty());
        MapSources sources(QPluginLoader::staticInstances());
        const auto id = QStringLiteral("org.swak.xyz.usgs-topo");
        QVERIFY(sources.sources().contains(id)); QVERIFY(!sources.networkEnabled());
        for (const auto &presetId : {QStringLiteral("usgs-imagery"), QStringLiteral("usgs-imagery-topo"), QStringLiteral("usgs-relief"), QStringLiteral("thunderforest-outdoors"), QStringLiteral("thunderforest-cycle")}) {
            const auto item = sources.sources().value(QStringLiteral("org.swak.xyz.") + presetId).toObject();
            QVERIFY(!item.isEmpty()); QVERIFY(xyz->validate(item).isEmpty());
            QVERIFY(xyz->tileUrl(item, item.value(QStringLiteral("minZoom")).toInt(), 0, 0).isValid());
        }
        QCOMPARE(sources.sources().value(QStringLiteral("org.swak.xyz.usgs-imagery-topo")).toObject().value(QStringLiteral("minZoom")).toInt(), 1);

        auto preset = sources.sources().value(id).toObject();
        QCOMPARE(preset.value(QStringLiteral("maxZoom")).toInt(), 8);
        QCOMPARE(xyz->tileUrl(preset, 3, 2, 4).path(), QStringLiteral("/arcgis/rest/services/USGSTopo/MapServer/tile/3/4/2"));
        sources.remove(id); const auto saved = sources.savePreferences();
        MapSources restored(QPluginLoader::staticInstances()); restored.restorePreferences(saved);
        QVERIFY(!restored.sources().contains(id));
        auto old = saved; old.remove(QStringLiteral("knownPresets")); restored.restorePreferences(old);
        QVERIFY(restored.sources().contains(id));
        MapSources absent({}); absent.restorePreferences(restored.savePreferences());
        QVERIFY(absent.sources().contains(id)); QVERIFY(absent.activeSource().isEmpty());
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
        const auto edge = tileFetchArea(visibleTiles({QSizeF(256, 256), QPointF(0.125, 0.125), 2}, 0, 19), 1);
        QCOMPARE(edge.size(), 4);
        const auto large = visibleTiles({QSizeF(4096, 4096), QPointF(0.5, 0.5), 10}, 0, 19);
        const auto bounded = tileFetchArea(large, 2);
        QVERIFY(bounded.size() <= large.size() + 64);
        QSet<QString> keys;
        for (const auto &tile : bounded) { QVERIFY(!keys.contains(tile.key)); keys.insert(tile.key); }
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost));
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer; renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        const auto drawView = [&](const Viewport &position) { QPainter painter(&canvas); renderer.paint(painter, position, 1, false); };
        drawView(view);
        QVERIFY(QTest::qWaitFor([&] { return server.requests.size() == 9 && renderer.pendingCount() == 0; }, 5000));
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
        { QPainter painter(&canvas); renderer.paint(painter, view, 1, false); }
        QCOMPARE(renderer.pendingCount(), 0);
        for (int y : {0, 256}) for (int x : {0, 256}) {
            QCOMPARE(canvas.pixelColor(x + 20, y + 20), QColor(MapBackgroundRgb));
            int textPixels = 0;
            QRect textBounds;
            for (int py = y + 50; py < y + 206; ++py)
                for (int px = x + 50; px < x + 206; ++px) {
                    const auto color = canvas.pixelColor(px, py);
                    if (color != QColor(MapBackgroundRgb)) {
                        ++textPixels;
                        textBounds = textBounds.united(QRect(px, py, 1, 1));
                        QVERIFY(color.red() < 100);
                        QVERIFY(color.green() < 110);
                    }
                }
            QVERIFY(textPixels > 20);
            QVERIFY(textBounds.height() > 30);
            QVERIFY(std::abs(textBounds.width() - textBounds.height()) < 12);
        }
        { QPainter painter(&canvas); renderer.paint(painter, view, 1, false, 0); }
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 512; ++x)
            QCOMPARE(canvas.pixelColor(x, y), QColor(MapBackgroundRgb));
    }
    void formats()
    {
        MapSources sources(QPluginLoader::staticInstances());
        QCOMPARE(sources.providers().size(), 4);
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
            auto invalidFormat = source;
            invalidFormat.insert(QStringLiteral("imageFormat"), QStringLiteral("unknown"));
            if (provider->mapId() == QStringLiteral("org.swak.wms") || provider->mapId() == QStringLiteral("org.swak.wmts")) QVERIFY(!provider->validate(invalidFormat).isEmpty());
            auto bad = source;
            bad.insert(QStringLiteral("endpoint"), QStringLiteral("http://example.com"));
            QVERIFY(!provider->validate(bad).isEmpty());
            bad = source; bad.insert(QStringLiteral("minZoom"), 19); bad.insert(QStringLiteral("maxZoom"), 1);
            QVERIFY(!provider->validate(bad).isEmpty());
        }
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        QCOMPARE(xyz->tileUrl(xyz->defaults(), 2, 1, 3).toString(), QStringLiteral("https://tile.openstreetmap.org/2/1/3.png"));
        auto *wmts = format(QStringLiteral("org.swak.wmts"));
        auto invalidWmts = wmts->defaults();
        invalidWmts.insert(QStringLiteral("layer"), QString{});
        QVERIFY(!wmts->validate(invalidWmts).isEmpty());
        invalidWmts = wmts->defaults();
        invalidWmts.insert(QStringLiteral("style"), QString(101, 'x'));
        QVERIFY(!wmts->validate(invalidWmts).isEmpty());
        QUrlQuery tile(wmts->tileUrl(wmts->defaults(), 2, 1, 3));
        QCOMPARE(tile.queryItemValue(QStringLiteral("TILEROW")), QStringLiteral("3"));
        QCOMPARE(tile.queryItemValue(QStringLiteral("TILECOL")), QStringLiteral("1"));
        auto *wms = format(QStringLiteral("org.swak.wms"));
        QUrlQuery image(wms->tileUrl(wms->defaults(), 1, 1, 0));
        QCOMPARE(image.queryItemValue(QStringLiteral("CRS")), QStringLiteral("EPSG:3857"));
        const auto bbox = image.queryItemValue(QStringLiteral("BBOX")).split(',');
        QCOMPARE(bbox.size(), 4);
        QCOMPARE(bbox.at(0).toDouble(), 0.0);
        QCOMPARE(bbox.at(1).toDouble(), 0.0);
        QVERIFY(bbox.at(2).toDouble() > 20037508);
    }
    void portableProviders()
    {
        MapSources sources(QPluginLoader::staticInstances());
        const auto id = QStringLiteral("custom.xyz");
        QVERIFY(sources.put(id, sources.sources().value(QStringLiteral("org.swak.xyz")).toObject()).isEmpty());
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
        entries.insert(QStringLiteral("custom"), entries.value(QStringLiteral("org.swak.xyz")));
        auto invalid = entries.value(QStringLiteral("org.swak.wms")).toObject();
        invalid.insert(QStringLiteral("endpoint"), QStringLiteral("invalid"));
        entries.insert(QStringLiteral("org.swak.wms"), invalid);
        saved.insert(QStringLiteral("sources"), entries);
        maps.restorePreferences(saved);
        QCOMPARE(maps.savePreferences(), saved);
        maps.select(QStringLiteral("org.swak.wms"));
        QVERIFY(maps.activeSource().isEmpty());
        QVERIFY(maps.put(QStringLiteral("custom"), entries.value(QStringLiteral("custom")).toObject()).isEmpty());
        for (const auto &id : {QStringLiteral("org.swak.wms"), QStringLiteral("org.swak.wmts")}) {
            auto *provider = format(id);
            auto values = provider->defaults();
            values.insert(QStringLiteral("layer"), QStringLiteral("a+b"));
            values.insert(QStringLiteral("time"), QStringLiteral("12:00:00+02:00"));
            const auto url = provider->tileUrl(values, 0, 0, 0);
            QVERIFY(url.toEncoded().contains("a%2Bb"));
            QVERIFY(!url.toEncoded().contains('+'));
        }
    }
    void failedTilesDoNotStarveNeighbors()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.failFirst = 4;
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(1024, 1024, QImage::Format_RGB32);
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(1024, 1024), QPointF(0.5, 0.5), 2}, 1, false); }
        QVERIFY(QTest::qWaitFor([&] { return server.requests.size() == 16; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return renderer.pendingCount() == 0; }));
        QVERIFY(!renderer.status().isEmpty());
        QSet<QByteArray> paths;
        for (const auto &request : server.requests) paths.insert(request.split(' ').at(1));
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
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(1024, 1024, QImage::Format_RGB32);
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(1024, 1024), QPointF(0.5, 0.5), 2}, 1, false); }
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty() && server.requests.size() == 4; }));
        QCOMPARE(renderer.pendingCount(), 3);
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(2, 2), 0}, 1, false); }
        QVERIFY(QTest::qWaitFor([&] { return renderer.pendingCount() == 0; }, 2000));
        QCOMPARE(server.requests.size(), 4);
    }
    void projectionAndDecoding()
    {
        auto tiles = visibleTiles(Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 0, 19);
        QCOMPARE(tiles.size(), 1);
        QCOMPARE(tiles.first().rectangle, QRectF(0, 0, 256, 256));
        QVERIFY(visibleTiles(Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 3, 19).isEmpty());
        QVERIFY(visibleTiles(Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), std::numeric_limits<double>::infinity()}, 0, 19).isEmpty());
        QVERIFY(visibleTiles(Viewport{QSizeF(16000, 16000), QPointF(0.5, 0.5), 19}, 0, 19).size() <= 256);
        TileServer fixture;
        const auto image = decodeMapTile(fixture.body);
        QCOMPARE(image.size(), QSize(256, 256));
        QCOMPARE(image.pixelColor(0, 0), QColor("#eb426a"));
        QVERIFY(decodeMapTile("<xml>not an image</xml>").isNull());
        QVERIFY(decodeMapTile(QByteArray(2 * 1024 * 1024 + 1, 'x')).isNull());
        QImage huge(1024, 1024, QImage::Format_RGB32);
        QByteArray bytes;
        QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); huge.save(&buffer, "PNG");
        QVERIFY(decodeMapTile(bytes).isNull());
    }
    void httpsAndCache()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        QImage canvas(256, 256, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        {
            TileRenderer renderer;
            renderer.setDiskLimit(16);
            QCOMPARE(renderer.diskCacheBytes(), -1);
            renderer.setSource(xyz, localSource(xyz, server, false));
            draw(renderer, canvas);
            QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty() && !renderer.status().startsWith(QStringLiteral("Map coverage:")); }, 5000));
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
            QVERIFY(QTest::qWaitFor([&] { return renderer.diskCacheBytes() > 0; }, 3000));
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
            QVERIFY(QTest::qWaitFor([&] { return !strict.status().isEmpty() && !strict.status().startsWith(QStringLiteral("Map coverage:")); }, 5000));
            QCOMPARE(server.requests.size(), 1);
        }
    }
    void cacheLockRetry()
    {
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
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
        server.headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: max-age=1\r\nETag: \"fixture\"\r\n";
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        QSignalSpy changed(&renderer, &TileRenderer::changed);
        draw(renderer, canvas);
        QVERIFY(changed.wait(5000));
        const auto expired = QDateTime::currentDateTimeUtc().addSecs(2);
        QVERIFY(QTest::qWaitFor([&] { return QDateTime::currentDateTimeUtc() >= expired; }, 3000));
        canvas.fill(Qt::black);
        draw(renderer, canvas);
        QCOMPARE(canvas.pixelColor(128, 128), QColor("#eb426a"));
        QVERIFY(QTest::qWaitFor([&] { return server.requests.size() == 2; }, 5000));
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(server.requests.last().toLower().contains("if-none-match: \"fixture\""));
    }
    void noStoreAndOpacity()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: no-store\r\n";
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(0.5, 0.5), 0}, 0, false); }
        QCOMPARE(renderer.pendingCount(), 0);
        QSignalSpy changed(&renderer, &TileRenderer::changed);
        draw(renderer, canvas);
        QVERIFY(changed.wait(5000));
        draw(renderer, canvas);
        QVERIFY(!QTest::qWaitFor([&] { return server.requests.size() > 1; }, 300));
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(2, 2), 0}, 1, false); }
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
        QTest::newRow("invalid-image") << QByteArray("HTTP/1.1 200 OK\r\n") << QByteArray("<ServiceException>error</ServiceException>");
        QTest::newRow("oversized-length") << QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 3000000\r\n") << QByteArray("bad");
        QTest::newRow("oversized-body") << QByteArray("HTTP/1.1 200 OK\r\n") << QByteArray(2 * 1024 * 1024 + 1, 'x');
        QTest::newRow("rate-limit") << QByteArray("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 60\r\n") << QByteArray("busy");
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
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(256, 256, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty() && !renderer.status().startsWith(QStringLiteral("Map coverage:")); }, 5000));
        QCOMPARE(renderer.pendingCount(), 0);
        QCOMPARE(canvas.pixelColor(20, 20), QColor(MapBackgroundRgb));
        draw(renderer, canvas);
        QVERIFY(!QTest::qWaitFor([&] { return server.requests.size() > 1; }, 300));
    }
    void concurrencyAndTimeout()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.hold = true;
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        QImage canvas(1024, 1024, QImage::Format_RGB32);
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(1024, 1024), QPointF(0.5, 0.5), 3}, 1, false); }
        QVERIFY(QTest::qWaitFor([&] { return renderer.pendingCount() == 4; }, 5000));
        { QPainter painter(&canvas); renderer.paint(painter, Viewport{QSizeF(256, 256), QPointF(2, 2), 0}, 1, false); }
        QVERIFY(QTest::qWaitFor([&] { return renderer.pendingCount() == 0; }, 5000));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty() && !renderer.status().startsWith(QStringLiteral("Map coverage:")); }, 16000));
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
        auto *xyz = format(QStringLiteral("org.swak.xyz"));
        server.headers = "HTTP/1.1 302 Found\r\nLocation: https://example.org/\r\n";
        QImage canvas(256, 256, QImage::Format_RGB32);
        TileRenderer renderer;
        renderer.setSource(xyz, localSource(xyz, server, true));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty() && !renderer.status().startsWith(QStringLiteral("Map coverage:")); }, 5000));
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(renderer.pendingCount(), 0);
        renderer.setSource(nullptr, {});
        QVERIFY(renderer.status().isEmpty());
        server.hold = true;
        renderer.setSource(xyz, localSource(xyz, server, true));
        draw(renderer, canvas);
        QVERIFY(QTest::qWaitFor([&] { return renderer.pendingCount() > 0; }, 5000));
        renderer.stop();
        QCOMPARE(renderer.pendingCount(), 0);
    }
};
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    app.setApplicationName(QStringLiteral("swak-map-tests"));
    MapTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_maps.moc"
