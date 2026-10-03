#include "core/data.h"
#include "tile_server.h"
#include <QGuiApplication>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QPainter>
#include <QTest>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <stdexcept>
using namespace swak;
Q_IMPORT_PLUGIN(KmlPlugin)
namespace {
const QByteArray point = "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark><name>Station</name><Point><coordinates>0,0,0</coordinates></Point></Placemark></Document></kml>";
QByteArray kmz(const QByteArray &xml, const char *name = "doc.kml", bool duplicate = false)
{
    QByteArray buffer(1024 * 1024, '\0'); size_t written = 0;
    auto *writer = archive_write_new(); archive_write_set_format_zip(writer);
    archive_write_open_memory(writer, buffer.data(), static_cast<size_t>(buffer.size()), &written);
    for (int i = 0; i < (duplicate ? 2 : 1); ++i) {
        auto *entry = archive_entry_new(); archive_entry_set_pathname(entry, i ? "other.kml" : name);
        archive_entry_set_size(entry, xml.size()); archive_entry_set_filetype(entry, AE_IFREG); archive_entry_set_perm(entry, 0600);
        archive_write_header(writer, entry); archive_write_data(writer, xml.constData(), static_cast<size_t>(xml.size())); archive_entry_free(entry);
    }
    archive_write_close(writer); archive_write_free(writer); buffer.resize(static_cast<qsizetype>(written)); return buffer;
}
DataProvider *format()
{
    for (auto *plugin : QPluginLoader::staticInstances()) if (auto *provider = qobject_cast<DataProvider *>(plugin)) return provider;
    return nullptr;
}
QJsonObject source(const TileServer &server)
{
    auto value = dataDefaults(); value.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
    value.insert(QStringLiteral("name"), QStringLiteral("Test feed")); value.insert(QStringLiteral("attribution"), QStringLiteral("Test fixture"));
    value.insert(QStringLiteral("endpoint"), QStringLiteral("https://127.0.0.1:%1/feed.kml").arg(server.serverPort()));
    value.insert(QStringLiteral("enabled"), true); value.insert(QStringLiteral("ignoreTls"), true); return value;
}
}
class FaultyDataProvider final : public QObject, public DataProvider {
    Q_OBJECT
    Q_INTERFACES(swak::DataProvider)
public:
    QString dataId() const override { return QStringLiteral("test.faulty"); }
    QString dataName() const override { return QStringLiteral("Fault fixture"); }
    QList<QJsonObject> presets() const override { return {}; }
    DataDocument decode(const QByteArray &, const std::atomic_bool &) const override { throw std::runtime_error("test failure"); }
};
class SlowDataProvider final : public QObject, public DataProvider {
    Q_OBJECT
    Q_INTERFACES(swak::DataProvider)
public:
    mutable std::atomic_int started = 0;
    QString dataId() const override { return QStringLiteral("test.slow"); }
    QString dataName() const override { return QStringLiteral("Cancellation fixture"); }
    QList<QJsonObject> presets() const override { return {}; }
    DataDocument decode(const QByteArray &, const std::atomic_bool &cancelled) const override
    {
        ++started;
        for (int i = 0; i < 5000 && !cancelled; ++i) QThread::msleep(1);
        return {};
    }
};
class DataTest final : public QObject {
    Q_OBJECT
private slots:
    void parser()
    {
        QVERIFY(format()); std::atomic_bool cancelled = false;
        const auto decoded = format()->decode(point, cancelled);
        QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error)); QCOMPARE(decoded.shapes.size(), 1);
        QCOMPARE(decoded.shapes.first().name, QStringLiteral("Station")); QCOMPARE(decoded.shapes.first().point, QPointF(0.5, 0.5));
        QCOMPARE(format()->decode(kmz(point), cancelled).shapes.size(), 1);
        QCOMPARE(format()->decode(QByteArray(point).replace("www.opengis.net", "earth.google.com"), cancelled).shapes.size(), 1);
        for (const auto &bad : {QByteArray{}, QByteArray("no xml"), point.left(50), QByteArray("PKbad"),
            QByteArray(point).replace("0,0,0", "NaN,0,0"), kmz(point, "../doc.kml"), kmz(point, "doc.kml", true), kmz(point, "image.png"),
            QByteArray("<!DOCTYPE kml [<!ENTITY x 'bad'>]>") + point,
            QByteArray(point).replace("<coordinates>0,0,0</coordinates>", ""), QByteArray(point).replace("0,0,0", "200,0,0"),
            QByteArray(point).replace("0,0,0", "0,0,0 1,1,0"), QByteArray(point).replace("http://www.opengis.net/kml/2.2", "bad")})
            QVERIFY(!format()->decode(bad, cancelled).error.isEmpty());
        cancelled = true; QVERIFY(!format()->decode(point, cancelled).error.isEmpty());
    }
    void metadataAndSelection()
    {
        std::atomic_bool cancelled = false;
        const auto xml = QByteArray(point).replace("<name>Station</name>", "<name>Station</name><description>&lt;b&gt;Read only&lt;/b&gt;</description><ExtendedData><Data name=\"status\"><value>Ready</value></Data></ExtendedData>");
        const auto parsed = format()->decode(xml, cancelled);
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        QCOMPARE(parsed.shapes.first().description, QStringLiteral("<b>Read only</b>"));
        QCOMPARE(parsed.shapes.first().properties.value(QStringLiteral("status")).toString(), QStringLiteral("Ready"));
        QVERIFY(!format()->decode(QByteArray(xml).replace("name=\"status\"", "name=\"" + QByteArray(129, 'a') + "\""), cancelled).error.isEmpty());
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost)); server.body = xml;
        DataSources feeds(QPluginLoader::staticInstances()); QVERIFY(feeds.put(QStringLiteral("test"), source(server)).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds.shapeCount(QStringLiteral("test")) == 1; }, 5000));
        Viewport view{QSizeF(1024, 512), QPointF(0.5, 0.5), 0};
        const auto itemAt = [&](const Viewport &viewport, QPointF position) -> std::optional<MapItem> {
            const auto items = feeds.itemsAt(viewport, position);
            return items.isEmpty() ? std::nullopt : std::optional<MapItem>(items.first());
        };
        const auto hit = itemAt(view, QPointF(512, 256)); QVERIFY(hit);
        QCOMPARE(hit->label, QStringLiteral("Station"));
        QVERIFY(!itemAt(view, QPointF(256, 256)));
        QImage canvas(1024, 512, QImage::Format_RGB32); canvas.fill(Qt::black);
        { QPainter painter(&canvas); feeds.paint(painter, view); }
        QCOMPARE(canvas.pixelColor(256, 256), QColor(Qt::black));
        QVERIFY(canvas.pixelColor(512, 256) != QColor(Qt::black));
        view.zoom = 3; view.center = QPointF(0.55, 0.5);
        QVERIFY(itemAt(view, QPointF(512 - 0.05 * view.scale(), 256)));
        QVERIFY(!itemAt(view, QPointF(512, 256)));
        MapSelection selection; selection.select(*hit);
        canvas.fill(Qt::black);
        { QPainter painter(&canvas); selection.paint(painter, Viewport{QSizeF(1024, 512), QPointF(0.5, 0.5), 0}); }
        QCOMPARE(canvas.pixelColor(512 + qRound(hit->radius + 2), 256), QColor(Qt::white));
        MapItem route; route.kind = MapItem::Kind::Line; route.color = QColor(20, 40, 60);
        route.path.moveTo(0.4, 0.5); route.path.lineTo(0.6, 0.5); selection.select(route);
        canvas.fill(Qt::black);
        { QPainter painter(&canvas); selection.paint(painter, Viewport{QSizeF(1024, 512), QPointF(0.5, 0.5), 0}); }
        QCOMPARE(canvas.pixelColor(512, 256), QColor(235, 215, 195));
        selection.clear(); QVERIFY(!selection.item());
        view = Viewport{QSizeF(1024, 512), QPointF(0.5, 0.5), 0};
        server.body = QByteArray(point).replace("Point", "LineString").replace("0,0,0", "-20,0 20,0");
        feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor([&] { const auto item = itemAt(view, QPointF(512, 256)); return item && item->kind == MapItem::Kind::Line; }, 5000));
        QVERIFY(!itemAt(view, QPointF(512, 270)));
        server.body = QByteArray(point).replace("<Point><coordinates>0,0,0</coordinates></Point>",
            "<Polygon><outerBoundaryIs><LinearRing><coordinates>-20,-20 20,-20 20,20 -20,20 -20,-20</coordinates></LinearRing></outerBoundaryIs></Polygon>");
        feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor([&] { const auto item = itemAt(view, QPointF(512, 256)); return item && item->kind == MapItem::Kind::Polygon; }, 5000));
        selection.select(*itemAt(view, QPointF(512, 256)));
        canvas.fill(Qt::black);
        { QPainter painter(&canvas); selection.paint(painter, view); }
        QVERIFY(canvas.pixelColor(512, 256).red() > 0);
        QVERIFY(!itemAt(view, QPointF(512, 350)));
        QVERIFY(feeds.put(QStringLiteral("second"), source(server)).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds.shapeCount(QStringLiteral("second")) == 1; }, 5000));
        QCOMPARE(feeds.itemsAt(view, QPointF(512, 256)).size(), 2);
        QVERIFY(feeds.itemsAt(view, QPointF(512, 350)).isEmpty());
    }
    void geometry()
    {
        const QByteArray body = "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark><name>Shapes</name><MultiGeometry>"
            "<LineString><coordinates>179,0 -179,0</coordinates></LineString>"
            "<Polygon><outerBoundaryIs><LinearRing><coordinates>-10,-10 10,-10 10,10 -10,10 -10,-10</coordinates></LinearRing></outerBoundaryIs>"
            "<innerBoundaryIs><LinearRing><coordinates>-2,-2 -2,2 2,2 2,-2 -2,-2</coordinates></LinearRing></innerBoundaryIs></Polygon>"
            "</MultiGeometry></Placemark><Folder><visibility>0</visibility><Placemark><Point><coordinates>1,1</coordinates></Point></Placemark></Folder>"
            "<NetworkLink><Link><href>file:///etc/passwd</href></Link></NetworkLink><GroundOverlay/></Document></kml>";
        std::atomic_bool cancelled = false; const auto decoded = format()->decode(body, cancelled);
        QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error)); QCOMPARE(decoded.shapes.size(), 2);
        QVERIFY(decoded.shapes[0].path.boundingRect().width() < 0.01);
        QVERIFY(decoded.shapes[1].path.contains(project(5, 5))); QVERIFY(!decoded.shapes[1].path.contains(project(0, 0)));
        QCOMPARE(decoded.ignoredLinks, 1); QCOMPARE(decoded.ignoredOverlays, 1);
        auto deep = QByteArray("<kml xmlns=\"http://www.opengis.net/kml/2.2\">");
        for (int i = 0; i < 40; ++i) deep += "<Folder>";
        QVERIFY(!format()->decode(deep, cancelled).error.isEmpty());
        QVERIFY(!format()->decode(QByteArray(DataDownloadLimit + 1, 'x'), cancelled).error.isEmpty());
        QVERIFY(!format()->decode(QByteArray(point).replace("0,0,0", QByteArray(200, '1')), cancelled).error.isEmpty());
    }
    void catalog()
    {
        DataSources feeds(QPluginLoader::staticInstances()); QCOMPARE(feeds.sources().size(), 2);
        QVERIFY(!feeds.settingsFields().isEmpty());
        for (const auto &entry : feeds.sources()) QVERIFY(!entry.toObject().value(QStringLiteral("enabled")).toBool());
        TileServer server; auto values = source(server); values.insert(QStringLiteral("enabled"), false);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        auto bad = values; bad.insert(QStringLiteral("interval"), 0); QVERIFY(!feeds.put(QStringLiteral("test"), bad).isEmpty());
        bad = values; bad.insert(QStringLiteral("endpoint"), QStringLiteral("file:///tmp/feed")); QVERIFY(!feeds.put(QStringLiteral("test"), bad).isEmpty());
        bad = values; bad.insert(QStringLiteral("color"), QStringLiteral("red")); QVERIFY(!feeds.put(QStringLiteral("test"), bad).isEmpty());
        QVERIFY(!feeds.put(QStringLiteral("bad/id"), values).isEmpty());
        const auto saved = feeds.savePreferences(); DataSources absent({}); absent.restorePreferences(saved); QCOMPARE(absent.savePreferences(), saved);
        QVERIFY(absent.status(QStringLiteral("test")).contains(QStringLiteral("unavailable")));
        feeds.remove(QStringLiteral("test")); QVERIFY(!feeds.sources().contains(QStringLiteral("test")));
    }
    void refreshAndFailures()
    {
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost)); server.body = point;
        DataSources feeds(QPluginLoader::staticInstances()); auto values = source(server);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds.shapeCount(QStringLiteral("test")) == 1; }, 5000));
        auto *timer = feeds.findChild<QTimer *>(QStringLiteral("dataRefresh-test")); QVERIFY(timer); QCOMPARE(timer->interval(), 300000); QVERIFY(timer->isActive());
        QImage canvas(512, 512, QImage::Format_RGB32); canvas.fill(Qt::black);
        { QPainter painter(&canvas); feeds.paint(painter, Viewport{QSizeF(512, 512), QPointF(0.5, 0.5), 2}); }
        QVERIFY(canvas.pixelColor(256, 256) != QColor(Qt::black));
        server.body = "malformed"; const auto previous = server.requests.size(); QMetaObject::invokeMethod(timer, "timeout");
        QVERIFY(QTest::qWaitFor([&] { return server.requests.size() > previous && feeds.status(QStringLiteral("test")).contains(QStringLiteral("Malformed")); }, 5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 1); QVERIFY(timer->isActive());
        server.body = kmz(QByteArray(point).replace("Station", "Updated")); feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor([&] { return feeds.status(QStringLiteral("test")).contains(QStringLiteral("Updated")); }, 5000));
        server.headers = "HTTP/1.1 302 Found\r\nLocation: https://example.invalid/\r\n"; feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor([&] { return feeds.status(QStringLiteral("test")).contains(QStringLiteral("download failed")); }, 5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 1);
        values.insert(QStringLiteral("enabled"), false); QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty()); QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
        QVERIFY(!feeds.findChild<QTimer *>(QStringLiteral("dataRefresh-test")));
    }
    void cancelBusyParser()
    {
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost)); server.body = point;
        SlowDataProvider plugin; DataSources feeds({&plugin}); auto values = source(server); values.insert(QStringLiteral("format"), plugin.dataId());
        for (int i = 0; i < 3; ++i) QVERIFY(feeds.put(QStringLiteral("test%1").arg(i), values).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return plugin.started == 2; }, 3000));
        QCOMPARE(server.requests.size(), 2);
        feeds.remove(QStringLiteral("test0"));
        QVERIFY(QTest::qWaitFor([&] { return plugin.started == 3; }, 3000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test0")), 0);
    }
    void decoderFailure()
    {
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost)); server.body = point;
        FaultyDataProvider plugin; DataSources feeds({&plugin}); auto values = source(server);
        values.insert(QStringLiteral("format"), plugin.dataId()); QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds.status(QStringLiteral("test")).contains(QStringLiteral("decoder failed")); }, 5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
    }
    void repeatedRefresh()
    {
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost)); server.body = kmz(point);
        for (int cycle = 0; cycle < 12; ++cycle) {
            DataSources feeds(QPluginLoader::staticInstances()); const auto values = source(server);
            QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
            QVERIFY(QTest::qWaitFor([&] { return feeds.shapeCount(QStringLiteral("test")) == 1; }, 5000));
            feeds.refresh(QStringLiteral("test")); feeds.remove(QStringLiteral("test"));
            QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
            QVERIFY(QTest::qWaitFor([&] { return feeds.shapeCount(QStringLiteral("test")) == 1; }, 5000));
        }
        std::atomic_bool cancelled = false;
        quint32 seed = 12345;
        for (int i = 0; i < 500; ++i) {
            auto bytes = point; seed = seed * 1664525U + 1013904223U;
            bytes[static_cast<qsizetype>(seed % static_cast<quint32>(bytes.size()))] = static_cast<char>(seed >> 24);
            const auto result = format()->decode(bytes, cancelled);
            QVERIFY(result.shapes.size() <= 1);
        }
    }
    void tlsAndCancellation()
    {
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost)); server.body = point;
        DataSources feeds(QPluginLoader::staticInstances()); auto values = source(server); values.insert(QStringLiteral("ignoreTls"), false);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds.status(QStringLiteral("test")).contains(QStringLiteral("failed")); }, 5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
        server.hold = true; values.insert(QStringLiteral("ignoreTls"), true); QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return !server.requests.isEmpty(); }, 5000));
        feeds.remove(QStringLiteral("test")); QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
        server.hold = false;
        for (int i = 0; i < 4; ++i) QVERIFY(feeds.put(QStringLiteral("feed%1").arg(i), values).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds.shapeCount(QStringLiteral("feed3")) == 1; }, 5000));
        server.body = QByteArray(DataDownloadLimit + 1, 'x'); feeds.refresh(QStringLiteral("feed0"));
        QVERIFY(QTest::qWaitFor([&] { return feeds.status(QStringLiteral("feed0")).contains(QStringLiteral("failed")); }, 5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("feed0")), 1);
    }
};
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--sample") {
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (!file.open(QIODevice::ReadOnly)) return 1;
        std::atomic_bool cancelled = false;
        const auto document = format()->decode(file.read(DataDownloadLimit + 1), cancelled);
        qInfo() << "shapes=" << document.shapes.size() << "links=" << document.ignoredLinks << "error=" << document.error;
        return document.error.isEmpty() ? 0 : 1;
    }
    DataTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_data.moc"
