#include "core/data.h"
#include "core/gdal_support.h"
#include "tile_server.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cmath>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <optional>
#include <stdexcept>
using namespace swak;
Q_IMPORT_PLUGIN(GdalVectorProvider)
namespace {
const QByteArray point =
    "<kml "
    "xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Placemark><name>Station</"
    "name><Point><coordinates>0,0,0</coordinates></Point></Placemark></Document></kml>";
QByteArray kmz(const QByteArray &xml, const char *name = "doc.kml", bool duplicate = false)
{
    QByteArray buffer(1024 * 1024, '\0');
    size_t written = 0;
    auto *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    archive_write_open_memory(writer, buffer.data(), static_cast<size_t>(buffer.size()), &written);
    for (int i = 0; i < (duplicate ? 2 : 1); ++i) {
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, i ? "other.kml" : name);
        archive_entry_set_size(entry, xml.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_write_header(writer, entry);
        archive_write_data(writer, xml.constData(), static_cast<size_t>(xml.size()));
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    buffer.resize(static_cast<qsizetype>(written));
    return buffer;
}
// Writes a zip archive with the given entries.
QByteArray zipEntries(const QList<std::pair<QByteArray, QByteArray>> &entries)
{
    QByteArray buffer(4 * 1024 * 1024, '\0');
    size_t written = 0;
    auto *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    archive_write_open_memory(writer, buffer.data(), static_cast<size_t>(buffer.size()), &written);
    for (const auto &[name, bytes] : entries) {
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.constData());
        archive_entry_set_size(entry, bytes.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_write_header(writer, entry);
        archive_write_data(writer, bytes.constData(), static_cast<size_t>(bytes.size()));
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    buffer.resize(static_cast<qsizetype>(written));
    return buffer;
}
DataProvider *format()
{
    for (auto *plugin : QPluginLoader::staticInstances()) {
        if (auto *provider = qobject_cast<DataProvider *>(plugin)) {
            return provider;
        }
    }
    return nullptr;
}
QJsonObject source(const TileServer &server)
{
    auto value = dataDefaults();
    value.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
    value.insert(QStringLiteral("name"), QStringLiteral("Test feed"));
    value.insert(QStringLiteral("attribution"), QStringLiteral("Test fixture"));
    value.insert(QStringLiteral("endpoint"),
                 QStringLiteral("https://127.0.0.1:%1/feed.kml").arg(server.serverPort()));
    value.insert(QStringLiteral("enabled"), true);
    value.insert(QStringLiteral("ignoreTls"), true);
    return value;
}
bool writeShapefile(const QString &path)
{
    initializeGdal();
    auto *driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!driver) {
        return false;
    }
    GdalDataset dataset(driver->Create(path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!dataset) {
        return false;
    }
    OGRSpatialReference mercator;
    mercator.importFromEPSG(3857);
    auto *layer = dataset->CreateLayer("sites", &mercator, wkbPoint, nullptr);
    OGRFieldDefn name("name", OFTString);
    OGRFieldDefn kind("kind", OFTString);
    if (!layer || layer->CreateField(&name) != OGRERR_NONE ||
        layer->CreateField(&kind) != OGRERR_NONE) {
        return false;
    }
    OGRFeature feature(layer->GetLayerDefn());
    feature.SetField("name", "Depot");
    feature.SetField("kind", "supply");
    // Ten degrees east on the equator in Web Mercator meters.
    OGRPoint location(1113194.9079327357, 0);
    feature.SetGeometry(&location);
    return layer->CreateFeature(&feature) == OGRERR_NONE;
}
bool zipDirectory(const QString &path, QByteArray &output)
{
    QByteArray buffer(4 * 1024 * 1024, '\0');
    size_t written = 0;
    auto *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    archive_write_open_memory(writer, buffer.data(), static_cast<size_t>(buffer.size()), &written);
    for (const auto &name : QDir(path).entryList(QDir::Files)) {
        QFile file(QDir(path).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        const auto bytes = file.readAll();
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, bytes.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0600);
        archive_write_header(writer, entry);
        archive_write_data(writer, bytes.constData(), static_cast<size_t>(bytes.size()));
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    buffer.resize(static_cast<qsizetype>(written));
    output = buffer;
    return written > 0;
}
} // namespace
class FaultyDataProvider final : public QObject, public DataProvider
{
    Q_OBJECT
    Q_INTERFACES(swak::DataProvider)
public:
    QString dataId() const override
    {
        return QStringLiteral("test.faulty");
    }
    QString dataName() const override
    {
        return QStringLiteral("Fault fixture");
    }
    QList<QJsonObject> presets() const override
    {
        return {};
    }
    DataDocument decode(const QByteArray &, const std::atomic_bool &) const override
    {
        throw std::runtime_error("test failure");
    }
};
class SlowDataProvider final : public QObject, public DataProvider
{
    Q_OBJECT
    Q_INTERFACES(swak::DataProvider)
public:
    mutable std::atomic_int started = 0;
    QString dataId() const override
    {
        return QStringLiteral("test.slow");
    }
    QString dataName() const override
    {
        return QStringLiteral("Cancellation fixture");
    }
    QList<QJsonObject> presets() const override
    {
        return {};
    }
    DataDocument decode(const QByteArray &, const std::atomic_bool &cancelled) const override
    {
        ++started;
        for (int i = 0; i < 5000 && !cancelled; ++i) {
            QThread::msleep(1);
        }
        return {};
    }
};
class DataTest final : public QObject
{
    Q_OBJECT
private slots:
    void manyKmlFolders()
    {
        // NOAA NDBC groups its stations in about 110 owner folders. Each folder is a layer.
        const auto document = [](int folders) {
            QByteArray kml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                             "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>";
            for (int i = 0; i < folders; ++i) {
                kml += "<Folder><name>Owner " + QByteArray::number(i) +
                       "</name><Placemark><name>Station " + QByteArray::number(i) +
                       "</name><description><![CDATA[<b>Wave height:</b> 1.3 ft<br/>]]>"
                       "</description><Point><coordinates>" +
                       QByteArray::number(-150 + i * 0.1) +
                       ",59</coordinates></Point>"
                       "</Placemark></Folder>";
            }
            return kml + "</Document></kml>";
        };
        std::atomic_bool cancelled = false;
        const auto decoded = format()->decode(document(120), cancelled);
        QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error));
        QCOMPARE(decoded.shapes.size(), 120);
        QVERIFY(decoded.shapes.first().description.contains(QStringLiteral("<b>")));
        const auto excessive = format()->decode(document(1025), cancelled);
        QVERIFY(excessive.error.contains(QStringLiteral("layers")));
        QVERIFY(excessive.shapes.isEmpty());
    }
    void parser()
    {
        QVERIFY(format());
        std::atomic_bool cancelled = false;
        const auto decoded = format()->decode(point, cancelled);
        QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error));
        QCOMPARE(decoded.shapes.size(), 1);
        QCOMPARE(decoded.shapes.first().name, QStringLiteral("Station"));
        QCOMPARE(decoded.shapes.first().point, QPointF(0.5, 0.5));
        QCOMPARE(format()->decode(kmz(point), cancelled).shapes.size(), 1);
        QCOMPARE(format()
                     ->decode(QByteArray(point).replace("www.opengis.net", "earth.google.com"),
                              cancelled)
                     .shapes.size(),
                 1);
        for (const auto &bad : {QByteArray{},
                                QByteArray("no xml"),
                                point.left(50),
                                QByteArray("PKbad"),
                                kmz(point, "../doc.kml"),
                                kmz(point, "doc.kml", true),
                                kmz(point, "image.png"),
                                QByteArray("<!DOCTYPE kml [<!ENTITY x 'bad'>]>") + point,
                                kmz(QByteArray("<!DOCTYPE kml [<!ENTITY x 'bad'>]>") + point),
                                QByteArray(point).replace("0,0,0", "200,0,0")}) {
            QVERIFY2(!format()->decode(bad, cancelled).error.isEmpty(), bad.left(80));
        }
        // GDAL reads these as empty geometry. The feature has no map shape.
        for (const auto &empty :
             {QByteArray(point).replace("0,0,0", "NaN,0,0"),
              QByteArray(point).replace("<coordinates>0,0,0</coordinates>", "")}) {
            const auto result = format()->decode(empty, cancelled);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QVERIFY(result.shapes.isEmpty());
        }
        // GDAL accepts an unknown namespace and uses the first point of a point list.
        for (const auto &loose :
             {QByteArray(point).replace("0,0,0", "0,0,0 1,1,0"),
              QByteArray(point).replace("http://www.opengis.net/kml/2.2", "bad")}) {
            QCOMPARE(format()->decode(loose, cancelled).shapes.size(), 1);
        }
        cancelled = true;
        QVERIFY(!format()->decode(point, cancelled).error.isEmpty());
    }
    void xmlPrologCheck()
    {
        // A DTD can follow comments and processing instructions of any length in the prolog.
        const QByteArray dtd = "<!DOCTYPE kml [<!ENTITY x 'bad'>]>";
        const QByteArray longComment = "<!--" + QByteArray(5000, 'c') + "-->";
        QVERIFY(declaresXmlDocumentType(dtd + point));
        QVERIFY(declaresXmlDocumentType("<?xml version=\"1.0\"?>" + longComment + dtd + point));
        QVERIFY(declaresXmlDocumentType("<?xml version=\"1.0\"?>\n<?style a?>\n" + dtd));
        QVERIFY(declaresXmlDocumentType("<!--" + QByteArray(XmlPrologLimit, 'c') + "-->" + point));
        QVERIFY(declaresXmlDocumentType(QByteArray("\xFF\xFE<\0k\0", 6)));
        QVERIFY(declaresXmlDocumentType(QByteArray("<\0k\0m\0l\0", 8)));
        // A root element ends the prolog. A DTD text later in the body is only text.
        QVERIFY(!declaresXmlDocumentType("<?xml version=\"1.0\"?>" + longComment + point));
        QVERIFY(!declaresXmlDocumentType(point + "<!-- <!DOCTYPE x> -->"));
        QVERIFY(!declaresXmlDocumentType("\xEF\xBB\xBF" + point));
        // Other formats pass: JSON, and binary files such as a shapefile header.
        QVERIFY(!declaresXmlDocumentType("{\"type\":\"FeatureCollection\"}"));
        QVERIFY(!declaresXmlDocumentType(QByteArray("\0\0\x27\x0a\0\0", 6)));
        QVERIFY(!declaresXmlDocumentType(QByteArray("SQLite format 3\0", 16)));
        std::atomic_bool cancelled = false;
        QCOMPARE(format()
                     ->decode("<?xml version=\"1.0\"?>" + longComment + dtd + point, cancelled)
                     .error,
                 QStringLiteral("XML declarations and external entities are not allowed."));
    }
    void documentUrlsStayOffline()
    {
        // GML schema locations and GeoJSON CRS links name URLs. GDAL must not fetch them.
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        const auto base =
            QByteArray("http://127.0.0.1:") + QByteArray::number(listener.serverPort());
        const QByteArray gml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<wfs:FeatureCollection xmlns:wfs=\"http://www.opengis.net/wfs\" "
            "xmlns:gml=\"http://www.opengis.net/gml\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
            "xmlns:t=\"http://example.com/t\" xsi:schemaLocation=\"http://example.com/t " +
            base +
            "/wfs?SERVICE=WFS&amp;VERSION=1.1.0&amp;REQUEST=DescribeFeatureType&amp;"
            "TYPENAME=t:site\"><gml:featureMember><t:site><t:name>A</t:name><t:geom>"
            "<gml:Point><gml:pos>59 18</gml:pos></gml:Point></t:geom></t:site>"
            "</gml:featureMember></wfs:FeatureCollection>";
        const QByteArray geojson =
            "{\"type\":\"FeatureCollection\",\"crs\":{\"type\":\"link\",\"properties\":"
            "{\"href\":\"" +
            base +
            "/crs\",\"type\":\"proj4\"}},\"features\":[{\"type\":\"Feature\","
            "\"properties\":{\"name\":\"A\"},\"geometry\":{\"type\":\"Point\","
            "\"coordinates\":[18,59]}}]}";
        std::atomic_bool cancelled = false;
        for (const auto &document : {gml, geojson}) {
            static_cast<void>(format()->decode(document, cancelled));
            QVERIFY2(!listener.waitForNewConnection(100), document.left(60));
        }
    }
    void metadataAndSelection()
    {
        std::atomic_bool cancelled = false;
        const auto xml =
            QByteArray(point).replace("<name>Station</name>",
                                      "<name>Station</name><description>&lt;b&gt;Read "
                                      "only&lt;/b&gt;</description><ExtendedData><Data "
                                      "name=\"status\"><value>Ready</value></Data></ExtendedData>");
        const auto parsed = format()->decode(xml, cancelled);
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        QCOMPARE(parsed.shapes.first().description, QStringLiteral("<b>Read only</b>"));
        QCOMPARE(parsed.shapes.first().properties.value(QStringLiteral("status")).toString(),
                 QStringLiteral("Ready"));
        QVERIFY(!format()
                     ->decode(QByteArray(xml).replace("name=\"status\"",
                                                      "name=\"" + QByteArray(129, 'a') + "\""),
                              cancelled)
                     .error.isEmpty());
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = xml;
        DataSources feeds(QPluginLoader::staticInstances());
        QVERIFY(feeds.put(QStringLiteral("test"), source(server)).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.shapeCount(QStringLiteral("test")) == 1;
            },
            5000));
        Viewport view{QSizeF(1024, 512), QPointF(0.5, 0.5), 0};
        const auto itemAt = [&](const Viewport &viewport,
                                QPointF position) -> std::optional<MapItem> {
            const auto items = feeds.itemsAt(viewport, position);
            return items.isEmpty() ? std::nullopt : std::optional<MapItem>(items.first());
        };
        const auto hit = itemAt(view, QPointF(512, 256));
        QVERIFY(hit);
        QCOMPARE(hit->label, QStringLiteral("Station"));
        QVERIFY(!itemAt(view, QPointF(256, 256)));
        QImage canvas(1024, 512, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        {
            QPainter painter(&canvas);
            feeds.paint(painter, view);
        }
        QCOMPARE(canvas.pixelColor(256, 256), QColor(Qt::black));
        QVERIFY(canvas.pixelColor(512, 256) != QColor(Qt::black));
        view.zoom = 3;
        view.center = QPointF(0.55, 0.5);
        QVERIFY(itemAt(view, QPointF(512 - 0.05 * view.scale(), 256)));
        QVERIFY(!itemAt(view, QPointF(512, 256)));
        MapSelection selection;
        selection.select(*hit);
        canvas.fill(Qt::black);
        {
            QPainter painter(&canvas);
            selection.paint(painter, Viewport{QSizeF(1024, 512), QPointF(0.5, 0.5), 0});
        }
        QCOMPARE(canvas.pixelColor(512 + qRound(hit->radius + 2), 256), QColor(Qt::white));
        for (const int percent : {50, 75, 100, 125, 150, 200}) {
            const Viewport scaled{QSizeF(1024, 512), QPointF(.5, .5), 0, percent};
            canvas.fill(Qt::black);
            {
                QPainter painter(&canvas);
                feeds.paint(painter, scaled);
            }
            const double radius = hit->radius * percent / 100.0;
            QVERIFY(canvas.pixelColor(512, 256 + qFloor(radius - 1)) != QColor(Qt::black));
            QVERIFY(itemAt(scaled, QPointF(512, 256 + radius)));
            QVERIFY(!itemAt(scaled, QPointF(512, 256 + std::max(8.0, radius + 3) + 2)));
        }
        MapItem route;
        route.kind = MapItem::Kind::Line;
        route.color = QColor(20, 40, 60);
        route.path.moveTo(0.4, 0.5);
        route.path.lineTo(0.6, 0.5);
        selection.select(route);
        canvas.fill(Qt::black);
        {
            QPainter painter(&canvas);
            selection.paint(painter, Viewport{QSizeF(1024, 512), QPointF(0.5, 0.5), 0});
        }
        QCOMPARE(canvas.pixelColor(512, 256), QColor(235, 215, 195));
        selection.clear();
        QVERIFY(!selection.item());
        view = Viewport{QSizeF(1024, 512), QPointF(0.5, 0.5), 0};
        server.body =
            QByteArray(point).replace("Point", "LineString").replace("0,0,0", "-20,0 20,0");
        feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto item = itemAt(view, QPointF(512, 256));
                return item && item->kind == MapItem::Kind::Line;
            },
            5000));
        QVERIFY(!itemAt(view, QPointF(512, 270)));
        server.body = QByteArray(point).replace(
            "<Point><coordinates>0,0,0</coordinates></Point>",
            "<Polygon><outerBoundaryIs><LinearRing><coordinates>-20,-20 20,-20 20,20 -20,20 "
            "-20,-20</coordinates></LinearRing></outerBoundaryIs></Polygon>");
        feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto item = itemAt(view, QPointF(512, 256));
                return item && item->kind == MapItem::Kind::Polygon;
            },
            5000));
        selection.select(*itemAt(view, QPointF(512, 256)));
        canvas.fill(Qt::black);
        {
            QPainter painter(&canvas);
            selection.paint(painter, view);
        }
        QVERIFY(canvas.pixelColor(512, 256).red() > 0);
        QVERIFY(!itemAt(view, QPointF(512, 350)));
        QVERIFY(feeds.put(QStringLiteral("second"), source(server)).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.shapeCount(QStringLiteral("second")) == 1;
            },
            5000));
        QCOMPARE(feeds.itemsAt(view, QPointF(512, 256)).size(), 2);
        QVERIFY(feeds.itemsAt(view, QPointF(512, 350)).isEmpty());
    }
    void geometry()
    {
        const QByteArray body = "<kml "
                                "xmlns=\"http://www.opengis.net/kml/"
                                "2.2\"><Document><Placemark><name>Shapes</name><MultiGeometry>"
                                "<LineString><coordinates>179,0 -179,0</coordinates></LineString>"
                                "<Polygon><outerBoundaryIs><LinearRing><coordinates>-10,-10 10,-10 "
                                "10,10 -10,10 -10,-10</coordinates></LinearRing></outerBoundaryIs>"
                                "<innerBoundaryIs><LinearRing><coordinates>-2,-2 -2,2 2,2 2,-2 "
                                "-2,-2</coordinates></LinearRing></innerBoundaryIs></Polygon>"
                                "</MultiGeometry></Placemark><Folder><visibility>0</"
                                "visibility><Placemark><Point><coordinates>1,1</coordinates></"
                                "Point></Placemark></Folder>"
                                "<NetworkLink><Link><href>file:///etc/passwd</href></Link></"
                                "NetworkLink><GroundOverlay/></Document></kml>";
        std::atomic_bool cancelled = false;
        const auto decoded = format()->decode(body, cancelled);
        QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error));
        // GDAL does not report folder visibility, so the hidden folder point is a third shape.
        QCOMPARE(decoded.shapes.size(), 3);
        QVERIFY(decoded.shapes[0].path.boundingRect().width() < 0.01);
        QVERIFY(decoded.shapes[1].path.contains(project(5, 5)));
        QVERIFY(!decoded.shapes[1].path.contains(project(0, 0)));
        QCOMPARE(decoded.ignoredLinks, 1);
        QCOMPARE(decoded.ignoredOverlays, 1);
        auto deep = QByteArray("<kml xmlns=\"http://www.opengis.net/kml/2.2\">");
        for (int i = 0; i < 40; ++i) {
            deep += "<Folder>";
        }
        QVERIFY(!format()->decode(deep, cancelled).error.isEmpty());
        QVERIFY(
            !format()->decode(QByteArray(DataDownloadLimit + 1, 'x'), cancelled).error.isEmpty());
        // GDAL rejects the oversized number. The feature then has no map shape.
        QVERIFY(format()
                    ->decode(QByteArray(point).replace("0,0,0", QByteArray(200, '1')), cancelled)
                    .shapes.isEmpty());
    }
    void vectorLimits()
    {
        std::atomic_bool cancelled = false;
        const auto collection = [](const QByteArray &features) {
            return R"({"type":"FeatureCollection","features":[)" + features + "]}";
        };
        const auto feature = [](const QByteArray &geometry) {
            return R"({"type":"Feature","properties":{},"geometry":)" + geometry + "}";
        };
        const auto error = [&](const QByteArray &bytes) {
            return format()->decode(bytes, cancelled).error;
        };
        // Degenerate rings and lines have no map shape.
        const auto degenerate = format()->decode(
            collection(feature(R"({"type":"Polygon","coordinates":[[[0,0],[1,0]]]})") + "," +
                       feature(R"({"type":"LineString","coordinates":[[0,0]]})")),
            cancelled);
        QVERIFY2(degenerate.error.isEmpty(), qPrintable(degenerate.error));
        QVERIFY(degenerate.shapes.isEmpty());
        // A line may cross the antimeridian, but not go around the world more than once.
        QCOMPARE(error(collection(
                     feature(R"({"type":"LineString","coordinates":[[0,0],[120,0],[-120,0],[0,0],)"
                             R"([120,0],[-120,0],[0,0],[120,0],[-120,0],[0,0]]})"))),
                 QStringLiteral("A geometry wraps around the world too many times."));
        // Collections can nest to a limited depth.
        QByteArray nested = R"({"type":"Point","coordinates":[1,1]})";
        for (int depth = 0; depth < 10; ++depth) {
            nested = R"({"type":"GeometryCollection","geometries":[)" + nested + "]}";
        }
        QCOMPARE(error(collection(feature(nested))),
                 QStringLiteral("Nested geometry exceeds its limit."));
        // The shape and coordinate limits stop large documents.
        QByteArrayList points;
        for (int index = 0; index <= 10000; ++index) {
            points.append(feature(R"({"type":"Point","coordinates":[1,1]})"));
        }
        QCOMPARE(error(collection(points.join(','))),
                 QStringLiteral("The data has more than 10000 shapes."));
        QByteArrayList coordinates;
        for (int index = 0; index <= 100000; ++index) {
            coordinates.append(QByteArray::number(index % 100) + ",1");
        }
        QCOMPARE(
            error(QByteArray(point).replace("<Point><coordinates>0,0,0</coordinates></Point>",
                                            "<LineString><coordinates>" + coordinates.join(' ') +
                                                "</coordinates></LineString>")),
            QStringLiteral("The data has more than 100000 coordinates."));
        // A cancelled decoder stops at the next feature.
        std::atomic_bool stop = true;
        QCOMPARE(format()
                     ->decode(collection(feature(R"({"type":"Point","coordinates":[1,1]})")), stop)
                     .error,
                 QStringLiteral("Data parsing cancelled."));
        // Archives must hold safe entries, within the entry limit.
        QCOMPARE(error(zipEntries({})), QStringLiteral("Invalid or empty archive."));
        QList<std::pair<QByteArray, QByteArray>> many;
        for (int index = 0; index <= 256; ++index) {
            many.append({"part" + QByteArray::number(index) + ".txt", "x"});
        }
        QCOMPARE(error(zipEntries(many)), QStringLiteral("The archive has more than 256 entries."));
        // GDAL leaves out entries with "..", and changes backslashes to slashes. The archive
        // then has no dataset. The name rules of the decoder stay as a second defense.
        QVERIFY(!error(zipEntries({{"../doc.kml", point}})).isEmpty());
        // Local files: archives, XML document types, unreadable files, and KML skip counts.
        QTemporaryDir directory;
        const auto write = [&](const QString &name, const QByteArray &bytes) {
            const auto path = directory.filePath(name);
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() ? path
                                                                                        : QString{};
        };
        const auto local =
            format()->decodeFile(write(QStringLiteral("doc.kmz"), kmz(point)), cancelled);
        QVERIFY2(local.error.isEmpty(), qPrintable(local.error));
        QCOMPARE(local.shapes.size(), 1);
        QCOMPARE(
            format()->decodeFile(write(QStringLiteral("bad.kmz"), zipEntries({})), cancelled).error,
            QStringLiteral("Invalid or empty archive."));
        QCOMPARE(format()
                     ->decodeFile(write(QStringLiteral("doctype.kml"),
                                        "<?xml version=\"1.0\"?><!DOCTYPE kml []>" + point),
                                  cancelled)
                     .error,
                 QStringLiteral("XML declarations and external entities are not allowed."));
        const auto locked = write(QStringLiteral("locked.kml"), point);
        QVERIFY(QFile::setPermissions(locked, {}));
        QCOMPARE(format()->decodeFile(locked, cancelled).error,
                 QStringLiteral("Cannot read the local data file."));
        const auto links = format()->decodeFile(
            write(QStringLiteral("links.kml"),
                  QByteArray(point).replace(
                      "</Document>",
                      "<NetworkLink><Link><href>x.kml</href></Link></NetworkLink></Document>")),
            cancelled);
        QVERIFY2(links.error.isEmpty(), qPrintable(links.error));
        QCOMPARE(links.ignoredLinks, 1);
    }
    void catalog()
    {
        DataSources feeds(QPluginLoader::staticInstances());
        QCOMPARE(feeds.sources().size(), 2);
        QVERIFY(!feeds.settingsFields().isEmpty());
        for (const auto &entry : feeds.sources()) {
            QVERIFY(!entry.toObject().value(QStringLiteral("enabled")).toBool());
        }
        TileServer server;
        auto values = source(server);
        values.insert(QStringLiteral("enabled"), false);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        auto bad = values;
        bad.insert(QStringLiteral("interval"), 0);
        QVERIFY(!feeds.put(QStringLiteral("test"), bad).isEmpty());
        for (const auto &endpoint : {QStringLiteral("feed.kml"),
                                     QStringLiteral("file://host/feed.kml"),
                                     QStringLiteral("/tmp/../etc/passwd"),
                                     QStringLiteral("ftp://example.com/feed.kml"),
                                     QStringLiteral("http://example.com/feed.kml")}) {
            bad = values;
            bad.insert(QStringLiteral("endpoint"), endpoint);
            QVERIFY2(!feeds.put(QStringLiteral("test"), bad).isEmpty(), qPrintable(endpoint));
        }
        for (const auto &endpoint :
             {QStringLiteral("/tmp/feed.kml"), QStringLiteral("file:///tmp/feed.kml")}) {
            auto local = values;
            local.insert(QStringLiteral("endpoint"), endpoint);
            QVERIFY2(feeds.put(QStringLiteral("test"), local).isEmpty(), qPrintable(endpoint));
        }
        bad = values;
        bad.insert(QStringLiteral("color"), QStringLiteral("red"));
        QVERIFY(!feeds.put(QStringLiteral("test"), bad).isEmpty());
        QVERIFY(!feeds.put(QStringLiteral("bad/id"), values).isEmpty());
        const auto saved = feeds.savePreferences();
        DataSources absent({});
        absent.restorePreferences(saved);
        QCOMPARE(absent.savePreferences(), saved);
        QVERIFY(absent.status(QStringLiteral("test")).contains(QStringLiteral("unavailable")));
        feeds.remove(QStringLiteral("test"));
        QVERIFY(!feeds.sources().contains(QStringLiteral("test")));
    }
    void refreshAndFailures()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = point;
        DataSources feeds(QPluginLoader::staticInstances());
        auto values = source(server);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.shapeCount(QStringLiteral("test")) == 1;
            },
            5000));
        auto *timer = feeds.findChild<QTimer *>(QStringLiteral("dataRefresh-test"));
        QVERIFY(timer);
        QCOMPARE(timer->interval(), 300000);
        QVERIFY(timer->isActive());
        QImage canvas(512, 512, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        {
            QPainter painter(&canvas);
            feeds.paint(painter, Viewport{QSizeF(512, 512), QPointF(0.5, 0.5), 2});
        }
        QVERIFY(canvas.pixelColor(256, 256) != QColor(Qt::black));
        server.body = "malformed";
        const auto previous = server.requests.size();
        QMetaObject::invokeMethod(timer, "timeout");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return server.requests.size() > previous &&
                       feeds.status(QStringLiteral("test")).contains(QStringLiteral("Malformed"));
            },
            5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 1);
        QVERIFY(timer->isActive());
        server.body = kmz(QByteArray(point).replace("Station", "Updated"));
        feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.status(QStringLiteral("test")).contains(QStringLiteral("Updated"));
            },
            5000));
        server.headers = "HTTP/1.1 302 Found\r\nLocation: https://example.invalid/\r\n";
        feeds.refresh(QStringLiteral("test"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.status(QStringLiteral("test"))
                    .contains(QStringLiteral("download failed"));
            },
            5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 1);
        values.insert(QStringLiteral("enabled"), false);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
        QVERIFY(!feeds.findChild<QTimer *>(QStringLiteral("dataRefresh-test")));
    }
    void cancelBusyParser()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = point;
        SlowDataProvider plugin;
        DataSources feeds({&plugin});
        auto values = source(server);
        values.insert(QStringLiteral("format"), plugin.dataId());
        for (int i = 0; i < 3; ++i) {
            QVERIFY(feeds.put(QStringLiteral("test%1").arg(i), values).isEmpty());
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return plugin.started == 2;
            },
            3000));
        QCOMPARE(server.requests.size(), 2);
        feeds.remove(QStringLiteral("test0"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return plugin.started == 3;
            },
            3000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test0")), 0);
    }
    void decoderFailure()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = point;
        FaultyDataProvider plugin;
        DataSources feeds({&plugin});
        auto values = source(server);
        values.insert(QStringLiteral("format"), plugin.dataId());
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.status(QStringLiteral("test"))
                    .contains(QStringLiteral("decoder failed"));
            },
            5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
    }
    void repeatedRefresh()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = kmz(point);
        for (int cycle = 0; cycle < 12; ++cycle) {
            DataSources feeds(QPluginLoader::staticInstances());
            const auto values = source(server);
            QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return feeds.shapeCount(QStringLiteral("test")) == 1;
                },
                5000));
            feeds.refresh(QStringLiteral("test"));
            feeds.remove(QStringLiteral("test"));
            QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return feeds.shapeCount(QStringLiteral("test")) == 1;
                },
                5000));
        }
        std::atomic_bool cancelled = false;
        quint32 seed = 12345;
        for (int i = 0; i < 500; ++i) {
            auto bytes = point;
            seed = seed * 1664525U + 1013904223U;
            bytes[static_cast<qsizetype>(seed % static_cast<quint32>(bytes.size()))] =
                static_cast<char>(seed >> 24);
            const auto result = format()->decode(bytes, cancelled);
            QVERIFY(result.shapes.size() <= 1);
        }
    }
    void otherFormats()
    {
        std::atomic_bool cancelled = false;
        const QByteArray gpx =
            "<gpx version=\"1.1\" creator=\"test\" xmlns=\"http://www.topografix.com/GPX/1/1\">"
            "<trk><name>Patrol</name><trkseg><trkpt lat=\"1\" lon=\"1\"/><trkpt lat=\"2\" "
            "lon=\"2\"/></trkseg></trk></gpx>";
        const auto track = format()->decode(gpx, cancelled);
        QVERIFY2(track.error.isEmpty(), qPrintable(track.error));
        QVERIFY(std::any_of(track.shapes.cbegin(), track.shapes.cend(), [](const auto &shape) {
            return shape.kind == DataShape::Kind::Line && shape.name == QStringLiteral("Patrol");
        }));
        const QByteArray json =
            R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":)"
            R"({"name":"Gate","kind":"entry"},"geometry":{"type":"Polygon","coordinates":)"
            R"([[[0,0],[1,0],[1,1],[0,1],[0,0]]]}}]})";
        const auto polygon = format()->decode(json, cancelled);
        QVERIFY2(polygon.error.isEmpty(), qPrintable(polygon.error));
        QCOMPARE(polygon.shapes.size(), 1);
        QCOMPARE(polygon.shapes.first().kind, DataShape::Kind::Polygon);
        QCOMPARE(polygon.shapes.first().name, QStringLiteral("Gate"));
        QCOMPARE(polygon.shapes.first().properties.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("entry"));
        QVERIFY(polygon.shapes.first().path.contains(project(0.5, 0.5)));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto shapefile = directory.filePath(QStringLiteral("sites.shp"));
        QVERIFY(writeShapefile(shapefile));
        const auto sites = format()->decodeFile(shapefile, cancelled);
        QVERIFY2(sites.error.isEmpty(), qPrintable(sites.error));
        QCOMPARE(sites.shapes.size(), 1);
        QCOMPARE(sites.shapes.first().name, QStringLiteral("Depot"));
        QCOMPARE(sites.shapes.first().properties.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("supply"));
        // The shapefile uses Web Mercator. The decoder converts it to longitude and latitude.
        QVERIFY(
            std::abs(sites.shapes.first().properties.value(QStringLiteral("longitude")).toDouble() -
                     10) < 1e-6);
        QByteArray archive;
        QVERIFY(zipDirectory(directory.path(), archive));
        const auto zipped = format()->decode(archive, cancelled);
        QVERIFY2(zipped.error.isEmpty(), qPrintable(zipped.error));
        QCOMPARE(zipped.shapes.size(), 1);
        QVERIFY(!format()
                     ->decodeFile(directory.filePath(QStringLiteral("absent.shp")), cancelled)
                     .error.isEmpty());
        QVERIFY(!format()->decodeFile(directory.path(), cancelled).error.isEmpty());

        const auto geojson = directory.filePath(QStringLiteral("gate.geojson"));
        QFile file(geojson);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(json), json.size());
        file.close();
        DataSources feeds(QPluginLoader::staticInstances());
        auto values = dataDefaults();
        values.insert(QStringLiteral("format"), QStringLiteral("org.swak.gdal.vector"));
        values.insert(QStringLiteral("name"), QStringLiteral("Local gate"));
        values.insert(QStringLiteral("attribution"), QStringLiteral("Test fixture"));
        values.insert(QStringLiteral("endpoint"), geojson);
        values.insert(QStringLiteral("enabled"), true);
        QVERIFY(feeds.put(QStringLiteral("local"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.shapeCount(QStringLiteral("local")) == 1;
            },
            5000));
        values.insert(QStringLiteral("endpoint"), directory.filePath(QStringLiteral("gone.json")));
        QVERIFY(feeds.put(QStringLiteral("local"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.status(QStringLiteral("local")).contains(QStringLiteral("not exist"));
            },
            5000));
    }
    void tlsAndCancellation()
    {
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = point;
        DataSources feeds(QPluginLoader::staticInstances());
        auto values = source(server);
        values.insert(QStringLiteral("ignoreTls"), false);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.status(QStringLiteral("test")).contains(QStringLiteral("failed"));
            },
            5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
        server.hold = true;
        values.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(feeds.put(QStringLiteral("test"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !server.requests.isEmpty();
            },
            5000));
        feeds.remove(QStringLiteral("test"));
        QCOMPARE(feeds.shapeCount(QStringLiteral("test")), 0);
        server.hold = false;
        for (int i = 0; i < 4; ++i) {
            QVERIFY(feeds.put(QStringLiteral("feed%1").arg(i), values).isEmpty());
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.shapeCount(QStringLiteral("feed3")) == 1;
            },
            5000));
        server.body = QByteArray(DataDownloadLimit + 1, 'x');
        feeds.refresh(QStringLiteral("feed0"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds.status(QStringLiteral("feed0")).contains(QStringLiteral("failed"));
            },
            5000));
        QCOMPARE(feeds.shapeCount(QStringLiteral("feed0")), 1);
    }
};
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    // Close cached GDAL resources after the tests and before the leak check.
    swak::GdalShutdownGuard gdal;
    if (argc == 3 && QByteArray(argv[1]) == "--sample") {
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (!file.open(QIODevice::ReadOnly)) {
            return 1;
        }
        std::atomic_bool cancelled = false;
        const auto document = format()->decode(file.read(DataDownloadLimit + 1), cancelled);
        qInfo() << "shapes=" << document.shapes.size() << "links=" << document.ignoredLinks
                << "error=" << document.error;
        return document.error.isEmpty() ? 0 : 1;
    }
    DataTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_data.moc"
