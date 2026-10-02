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
            renderer.setSource(xyz, localSource(xyz, server, false));
            draw(renderer, canvas);
            QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty(); }, 5000));
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
        }
        {
            TileRenderer cached;
            cached.setSource(xyz, localSource(xyz, server, true));
            QSignalSpy changed(&cached, &TileRenderer::changed);
            draw(cached, canvas);
            QVERIFY(changed.wait(5000));
            QCOMPARE(server.requests.size(), 1);
        }
        {
            TileRenderer strict;
            strict.setSource(xyz, localSource(xyz, server, false));
            draw(strict, canvas);
            QVERIFY(QTest::qWaitFor([&] { return !strict.status().isEmpty(); }, 5000));
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
        QVERIFY(changed.wait(5000));
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
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty(); }, 5000));
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
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty(); }, 16000));
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
        QVERIFY(QTest::qWaitFor([&] { return !renderer.status().isEmpty(); }, 5000));
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
