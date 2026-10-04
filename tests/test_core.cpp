#include "core/servers.h"
#include "core/map_tools.h"
#include <QMenu>
#include "core/marker_symbols.h"
#include "core/marker_collections.h"
#include <QJsonDocument>
#include <archive.h>
#include <archive_entry.h>
#include "core/plugin_catalog.h"
#include <QJsonArray>
#include "core/activity.h"
#include "core/maps.h"
#include "core/interfaces.h"
#include "core/user_objects.h"
#include "core/polygon.h"
#include "core/map_grid.h"
#include <QPainter>
#include "core/projection.h"
#include "core/store.h"
#include "core/files.h"
#include <QTemporaryDir>
#include <QFile>
#include <sys/stat.h>
#include <QAbstractItemModelTester>
#include <QLineEdit>
#include <QPluginLoader>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QUdpSocket>
#include <cmath>
#include <limits>
#include <memory>

using namespace swak;
class CoreTest final : public QObject {
    Q_OBJECT
private:
    Codec *codec_ = nullptr;
    SourcePlugin *source_ = nullptr;
    Event sample() const
    {
        Event event;
        event.uid = QStringLiteral("test-1");
        event.callsign = QStringLiteral("Ångström <test> & sensor");
        event.type = QStringLiteral("a-f-G");
        event.how = QStringLiteral("h-e");
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addSecs(300);
        event.latitude = 59.3293;
        event.longitude = 18.0686;
        return event;
    }
private slots:
    void traceLimits()
    {
        Store store; ActivityModel activity; MapSources maps(QPluginLoader::staticInstances()); MapTools tools; UserObjects objects;
        PluginServices services{store, activity, maps}; services.mapTools = &tools; services.objects = &objects;
        QMenu main;
        for (auto *plugin : builtInPlugins()) if (auto *map = qobject_cast<MapExtension *>(plugin); map && map->mapId() == QStringLiteral("org.swak.trace"))
            qobject_cast<MenuPlugin *>(plugin)->populateMenu(services, main);
        auto *tool = dynamic_cast<MapTool *>(main.findChild<QObject *>(QStringLiteral("traceTool"))); QVERIFY(tool);
        auto *settings = dynamic_cast<SettingsParticipant *>(tool); QVERIFY(settings);
        for (int i = 0; i < 129; ++i) {
            MapItem item; item.owner = QStringLiteral("org.swak.markers"); item.id = QString::number(i);
            QMenu menu; tools.populateContextMenu(item, menu); auto *action = menu.findChild<QAction *>(QStringLiteral("traceMarker")); QVERIFY(action);
            QCOMPARE(action->isEnabled(), i < 128); if (action->isEnabled()) action->trigger();
        }
        auto values = settings->savePreferences(); QCOMPARE(values.value(QStringLiteral("targets")).toArray().size(), 128);
        values.insert(QStringLiteral("positions"), 1000); values.insert(QStringLiteral("opacity"), -1);
        settings->restorePreferences(values); QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), 10);
        QVERIFY(settings->savePreferences().value(QStringLiteral("opacity")).toDouble() > 0);
        main.findChild<QAction *>(QStringLiteral("clearTraces"))->trigger(); QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 0);
        settings->restorePreferences(values); QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 128);
        MapItem area; area.kind = MapItem::Kind::Polygon; area.owner = QStringLiteral("org.swak.markers"); QMenu excluded;
        tools.populateContextMenu(area, excluded); QVERIFY(excluded.isEmpty());
        main.findChild<QAction *>(QStringLiteral("clearTraces"))->trigger();
        UserObject marker; marker.id = QStringLiteral("local-trace"); marker.type = QStringLiteral("circle"); marker.title = QStringLiteral("Moving local marker");
        marker.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}; QVERIFY(objects.put(marker).isEmpty());
        MapItem point; point.owner = QStringLiteral("org.swak.markers"); point.id = marker.id;
        QMenu local; tools.populateContextMenu(point, local); local.findChild<QAction *>(QStringLiteral("traceMarker"))->trigger();
        for (int i = 1; i <= 12; ++i) { marker.geometry.insert(QStringLiteral("x"), 0.5 + i * 0.00003); QVERIFY(objects.put(marker).isEmpty()); }
        QImage image(1000, 100, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        const Viewport view{QSizeF(1000, 100), QPointF(0.5, 0.5), 12};
        { QPainter painter(&image); tools.paint(painter, view); }
        QVERIFY(image.pixelColor(850, 50).alpha() > 0); QVERIFY(image.pixelColor(850, 50).red() > image.pixelColor(850, 50).blue());
        QCOMPARE(image.pixelColor(510, 50).alpha(), 0);
        objects.remove(marker.id); image.fill(Qt::transparent); { QPainter painter(&image); tools.paint(painter, view); }
        QCOMPARE(image.pixelColor(850, 50).alpha(), 0);
        marker.id = QStringLiteral("date-line"); marker.geometry.insert(QStringLiteral("x"), 0.99999); QVERIFY(objects.put(marker).isEmpty()); point.id = marker.id;
        QMenu crossing; tools.populateContextMenu(point, crossing); crossing.findChild<QAction *>(QStringLiteral("traceMarker"))->trigger();
        marker.geometry.insert(QStringLiteral("x"), 0.00001); QVERIFY(objects.put(marker).isEmpty());
        image.fill(Qt::transparent); { QPainter painter(&image); tools.paint(painter, Viewport{QSizeF(1000, 100), QPointF(0, 0.5), 12}); }
        QVERIFY(image.pixelColor(500, 50).alpha() > 0); QCOMPARE(image.pixelColor(850, 50).alpha(), 0);
    }
    void markerCollections()
    {
        MarkerCollections collections(QPluginLoader::staticInstances());
        QCOMPARE(collections.collections().size(), 2);
        QVERIFY(MarkerCollections::validate(coreMarkerCollection()).isEmpty());
        QVERIFY(MarkerCollections::validate(militaryMarkerCollection()).isEmpty());
        QVERIFY(collections.symbol({}, {}));
        for (const auto &symbol : markerSymbols()) {
            const auto found = collections.symbol({}, symbol.code); QVERIFY(found);
            QCOMPARE(found->frame, markerFrame(symbol.code, {}, 1)); QCOMPARE(found->glyph, markerGlyph(symbol.code));
        }
        QVERIFY(!collections.remove(CoreMarkerCollection).isEmpty());
        QVERIFY(!collections.put(coreMarkerCollection()).isEmpty());
        QVERIFY(collections.setEnabled(MilitaryMarkerCollection, false).isEmpty());
        QVERIFY(collections.symbol(MilitaryMarkerCollection, QStringLiteral("a-f-G-U")));
        QVERIFY(!collections.setEnabled(QStringLiteral("missing"), true).isEmpty());
        auto custom = coreMarkerCollection(); custom.insert(QStringLiteral("id"), QStringLiteral("test.collection")); custom.insert(QStringLiteral("name"), QStringLiteral("Custom"));
        QVERIFY(collections.put(custom).isEmpty());
        auto saved = collections.savePreferences();
        MarkerCollections absent({}); absent.restorePreferences(saved);
        QCOMPARE(absent.collections(), collections.collections()); QVERIFY(absent.symbol(CoreMarkerCollection, {}));
        auto future = custom; future.insert(QStringLiteral("schemaVersion"), 2);
        auto entries = saved.value(QStringLiteral("collections")).toObject(); entries.insert(QStringLiteral("test.collection"), future);
        saved.insert(QStringLiteral("collections"), entries); absent.restorePreferences(saved);
        QCOMPARE(absent.collections().value(QStringLiteral("test.collection")).toObject(), future);
        QVERIFY(!absent.symbol(QStringLiteral("test.collection"), {}));
        QVERIFY(!absent.setEnabled(QStringLiteral("test.collection"), true).isEmpty());
        QVERIFY(collections.remove(QStringLiteral("test.collection")).isEmpty());
        for (int i = 0; i < 14; ++i) { custom.insert(QStringLiteral("id"), QStringLiteral("custom.%1").arg(i)); QVERIFY(collections.put(custom).isEmpty()); }
        custom.insert(QStringLiteral("id"), QStringLiteral("overflow")); QVERIFY(!collections.put(custom).isEmpty());
        const auto before = collections.savePreferences(); entries.insert(QStringLiteral("invalid/key"), custom);
        collections.restorePreferences({{QStringLiteral("collections"), entries}}); QCOMPARE(collections.savePreferences(), before);
        UserObject record{QStringLiteral("test"), QStringLiteral("circle"), QStringLiteral("Custom marker"), {}, {},
            {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        record.collection = QStringLiteral("test.collection"); QVERIFY(UserObjects::validate(record).isEmpty());
        QCOMPARE(UserObject::fromJson(record.json()).collection, record.collection);
        record.collection = QString(65, 'x'); QVERIFY(!UserObjects::validate(record).isEmpty());
    }
    void markerCollectionVectors()
    {
        const auto original = coreMarkerCollection();
        for (const auto &path : QList<QJsonArray>{QJsonArray{}, QJsonArray{QJsonArray{}}, QJsonArray{QJsonArray{QStringLiteral("Z")}},
                 QJsonArray{QJsonArray{QStringLiteral("L"), 0, 0}}, QJsonArray{QJsonArray{QStringLiteral("M"), 5, 0}},
                 QJsonArray{QJsonArray{QStringLiteral("M"), 0, 0}, QJsonArray{QStringLiteral("C"), 1}},
                 QJsonArray{QJsonArray{QStringLiteral("M"), 0, QJsonValue{}}}}) {
            auto value = original; auto symbols = value.value(QStringLiteral("symbols")).toArray(); auto symbol = symbols[0].toObject();
            symbol.insert(QStringLiteral("frame"), path); symbols[0] = symbol; value.insert(QStringLiteral("symbols"), symbols);
            QVERIFY(!MarkerCollections::validate(value).isEmpty());
        }
        auto empty = original; empty.insert(QStringLiteral("symbols"), QJsonArray{}); QVERIFY(!MarkerCollections::validate(empty).isEmpty());
        auto oversized = original; oversized.insert(QStringLiteral("extra"), QString(256 * 1024, 'x')); QVERIFY(!MarkerCollections::validate(oversized).isEmpty());
        auto value = original; auto symbols = value.value(QStringLiteral("symbols")).toArray(); symbols.append(symbols[0]); value.insert(QStringLiteral("symbols"), symbols);
        QVERIFY(!MarkerCollections::validate(value).isEmpty());
        value = original; value.insert(QStringLiteral("name"), QStringLiteral("bad\nname")); QVERIFY(!MarkerCollections::validate(value).isEmpty());
        value = original; value.insert(QStringLiteral("enabled"), QStringLiteral("yes")); QVERIFY(!MarkerCollections::validate(value).isEmpty());
        value = original; value.insert(QStringLiteral("schemaVersion"), 2); QVERIFY(!MarkerCollections::validate(value).isEmpty());
    }
    void markerCollectionArchives()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        MarkerCollections source(QPluginLoader::staticInstances());
        auto custom = militaryMarkerCollection(); custom.insert(QStringLiteral("id"), QStringLiteral("archive.test")); QVERIFY(source.put(custom).isEmpty());
        for (const auto &suffix : {QStringLiteral(".zip"), QStringLiteral(".tar.gz")}) {
            const auto path = directory.filePath(QStringLiteral("markers") + suffix);
            QVERIFY(source.exportArchive(QStringLiteral("archive.test"), path).isEmpty());
            MarkerCollections imported({}); QVERIFY(imported.importArchive(path).isEmpty()); QCOMPARE(imported.collections().value(QStringLiteral("archive.test")).toObject(), custom);
            QVERIFY(!imported.importArchive(path).isEmpty()); QCOMPARE(imported.collections().size(), 1);
            QVERIFY(imported.remove(QStringLiteral("archive.test")).isEmpty()); QVERIFY(imported.importArchive(path).isEmpty());
        }
        QVERIFY(!source.exportArchive(QStringLiteral("archive.test"), QString(QChar::Null)).isEmpty());
        QVERIFY(!source.exportArchive(QStringLiteral("missing"), directory.filePath(QStringLiteral("missing.zip"))).isEmpty());
        QVERIFY(!source.exportArchive(QStringLiteral("archive.test"), directory.path()).isEmpty());
        QVERIFY(!source.importArchive(directory.path()).isEmpty());
        const auto zip = [&](const QString &member, const QByteArray &contents, bool extra, bool link) {
            QByteArray data(1024 * 1024, '\0'); size_t used = 0;
            std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
            archive_write_set_format_zip(writer.get()); archive_write_open_memory(writer.get(), data.data(), static_cast<size_t>(data.size()), &used);
            std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
            archive_entry_set_pathname(entry.get(), member.toUtf8().constData()); archive_entry_set_filetype(entry.get(), link ? AE_IFLNK : AE_IFREG);
            if (link) archive_entry_set_symlink(entry.get(), "outside");
            archive_entry_set_perm(entry.get(), 0600); archive_entry_set_size(entry.get(), link ? 0 : contents.size());
            archive_write_header(writer.get(), entry.get()); if (!link) archive_write_data(writer.get(), contents.constData(), static_cast<size_t>(contents.size()));
            if (extra) { archive_entry_set_pathname(entry.get(), "extra.txt"); archive_entry_set_size(entry.get(), 0); archive_write_header(writer.get(), entry.get()); }
            archive_write_close(writer.get()); data.resize(static_cast<qsizetype>(used)); return data;
        };
        const auto json = QJsonDocument(custom).toJson();
        const auto valid = zip(QStringLiteral("collection.json"), json, false, false);
        const QList<QByteArray> invalid{QByteArrayLiteral("not an archive"), valid.left(valid.size() / 2),
            zip(QStringLiteral("../collection.json"), json, false, false), zip(QStringLiteral("/collection.json"), json, false, false),
            zip(QStringLiteral("collection.json"), json, true, false), zip(QStringLiteral("collection.json"), json, false, true),
            zip(QStringLiteral("collection.json"), QByteArray(256 * 1024 + 1, ' '), false, false),
            zip(QStringLiteral("collection.json"), QByteArrayLiteral("[]"), false, false),
            zip(QStringLiteral("collection.json"), QByteArrayLiteral("{"), false, false)};
        MarkerCollections target({});
        for (const auto &bytes : invalid) {
            const auto path = directory.filePath(QStringLiteral("invalid.zip")); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
            QVERIFY2(!target.importArchive(path).isEmpty(), qPrintable(QString::number(bytes.size()))); QVERIFY(target.collections().isEmpty());
        }
        const auto path = directory.filePath(QStringLiteral("recovery.zip")); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(valid), valid.size()); file.close();
        QVERIFY(target.importArchive(path).isEmpty()); QVERIFY(target.symbol(QStringLiteral("archive.test"), QStringLiteral("a-h-G-U-C-I")));
    }
    void vectorMarkerSymbols()
    {
        QCOMPARE(markerSymbols().size(), 16);
        QImage sheet(320, 100, QImage::Format_ARGB32_Premultiplied); sheet.fill(Qt::transparent);
        QPainter painter(&sheet); painter.setRenderHint(QPainter::Antialiasing);
        int column = 0;
        for (const auto &symbol : markerSymbols()) {
            QVERIFY(validMarkerSymbol(symbol.code)); QCOMPARE(markerSymbolForCot(symbol.code), symbol.code);
            const auto path = markerFrame(symbol.code, QPointF{}, 7);
            QVERIFY(path.boundingRect().width() <= 14.01); QVERIFY(path.boundingRect().height() <= 14.01);
            QVERIFY(path.contains(QPointF{}));
            paintMarkerSymbol(painter, symbol.code, QPointF(10 + 20 * column++, 20), 7, symbol.color);
            UserObject item{QStringLiteral("test"), QStringLiteral("circle"), QStringLiteral("Example"), {}, {},
                {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
            item.symbol = symbol.code;
            QVERIFY(UserObjects::validate(item).isEmpty());
            QCOMPARE(UserObject::fromJson(item.json()).symbol, symbol.code);
            UserObjects objects; QVERIFY(objects.put(item).isEmpty()); objects.remove(item.id); objects.undoStack().undo();
            QCOMPARE(objects.find(item.id)->symbol, symbol.code);
        }
        QVERIFY(markerSymbolForCot(QStringLiteral("a-f-A")).isEmpty());
        QVERIFY(markerSymbolForCot(QStringLiteral("a-f-G-U-C-I-custom")).isEmpty());
        QCOMPARE(markerFrame(QStringLiteral("future"), {}, 7), markerFrame({}, {}, 7));
        paintMarkerSymbol(painter, QStringLiteral("future"), QPointF(10, 50), 7, Qt::yellow, true);
        painter.end(); QVERIFY(sheet.save(QStringLiteral("marker-symbols.png")));
        UserObject item{QStringLiteral("future"), QStringLiteral("circle"), QStringLiteral("Future"), {}, {},
            {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        item.symbol = QStringLiteral("future"); QVERIFY(UserObjects::validate(item).isEmpty());
        item.symbol = QString(65, 'a'); QVERIFY(!UserObjects::validate(item).isEmpty());
        auto malformed = item.json(); malformed.insert(QStringLiteral("symbol"), 42);
        QVERIFY(!UserObjects::validate(UserObject::fromJson(malformed)).isEmpty());
    }

    void cotGroundUnitSymbols()
    {
        MapLayer *tracks = nullptr;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            auto *layer = qobject_cast<MapLayer *>(plugin);
            if (layer && layer->order() == 100) tracks = layer;
        }
        QVERIFY(tracks);
        for (const auto &symbol : markerSymbols()) {
            Store store; auto event = sample(); event.type = symbol.code;
            event.latitude = 0; event.longitude = 0; QVERIFY(store.accept(event).isEmpty());
            for (const bool selected : {false, true}) {
                if (selected) store.select(event.uid);
                QImage actual(64, 64, QImage::Format_ARGB32_Premultiplied); actual.fill(Qt::transparent);
                { QPainter painter(&actual); tracks->paint(painter, {QSizeF(64, 64), QPointF(0.5, 0.5), 3}, store); }
                QImage expected(64, 64, QImage::Format_ARGB32_Premultiplied); expected.fill(Qt::transparent);
                { QPainter painter(&expected); paintMarkerSymbol(painter, symbol.code, QPointF(32, 32), selected ? 8 : 6, symbol.color, selected); }
                QCOMPARE(actual.copy(22, 22, 20, 20), expected.copy(22, 22, 20, 20));
            }
        }
    }

    void markerColorStorage()
    {
        UserObject item{QStringLiteral("color"), QStringLiteral("circle"), QStringLiteral("Marker"), {}, {},
            {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        QCOMPARE(UserObject::fromJson(item.json()).color, QStringLiteral("#ffc857"));
        auto legacy = item.json(); legacy.remove(QStringLiteral("color"));
        QCOMPARE(UserObject::fromJson(legacy).color, QStringLiteral("#ffc857"));
        item.color = QStringLiteral("#1e88e5"); QVERIFY(UserObjects::validate(item).isEmpty());
        QCOMPARE(UserObject::fromJson(item.json()).color, item.color);
        item.color = QStringLiteral("not-a-color"); QVERIFY(!UserObjects::validate(item).isEmpty());
    }
    void coordinateGrid()
    {
        QImage image(900, 600, QImage::Format_ARGB32_Premultiplied);
        for (double zoom : {2.0, 8.0, 19.0}) for (double latitude : {-80.0, 0.0, 80.0}) {
            image.fill(Qt::transparent); QPainter painter(&image);
            Viewport view; view.size = image.size(); view.zoom = zoom; view.center = project(latitude, 18);
            paintMapGrid(painter, view, 0.2, 140); painter.end();
            int top = 0, bottom = 0, left = 0, right = 0;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixelColor(x, y); if (!pixel.alpha()) continue;
                QCOMPARE(pixel.red(), 0); QCOMPARE(pixel.green(), 0); QCOMPARE(pixel.blue(), 0);
                if (y < 30) ++top;
                if (y >= image.height()-30) ++bottom;
                if (x < 90) ++left;
                if (x >= image.width()-90) ++right;
            }
            QVERIFY(top > 60); QVERIFY(bottom > 60); QVERIFY(left > 60); QVERIFY(right > 60);
        }
        image.fill(Qt::transparent); QPainter painter(&image); Viewport invalid; invalid.size = image.size(); invalid.zoom = std::numeric_limits<double>::quiet_NaN();
        paintMapGrid(painter, invalid, 0.2, 140); painter.end(); QCOMPARE(image.pixelColor(450, 300).alpha(), 0);
    }
    void polygons()
    {
        const QPolygonF square{project(0, 0), project(0, 1), project(1, 1), project(1, 0)};
        QVERIFY(validatePolygon(square).isEmpty());
        QCOMPARE(polygonVertices(polygonGeometry(square)), square);
        const double expected = 12363718145.18;
        QVERIFY(std::abs(polygonArea(square) / expected - 1) < 0.00001);
        auto reverse = square; std::reverse(reverse.begin(), reverse.end());
        QVERIFY(std::abs(polygonArea(reverse) - polygonArea(square)) < 0.1);
        QVERIFY(square.containsPoint(polygonLabelPoint(square), Qt::OddEvenFill));
        const QPolygonF concave{{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9}, {0.7, 0.9}, {0.7, 0.3}, {0.3, 0.3}, {0.3, 0.9}, {0.1, 0.9}};
        QVERIFY(validatePolygon(concave).isEmpty()); QVERIFY(concave.containsPoint(polygonLabelPoint(concave), Qt::OddEvenFill));
        QVERIFY(!validatePolygon({}).isEmpty()); QCOMPARE(polygonArea({}), 0); QCOMPARE(polygonLabelPoint({}), QPointF{});
        QVERIFY(!validatePolygon(QPolygonF{{0, 0}, {1e-13, 0}, {0, 1e-13}}).isEmpty());
        QVERIFY(!validatePolygon(QPolygonF{{0, 0}, {1, 1}, {0, 1}, {1, 0}}).isEmpty());
        QVERIFY(!validatePolygon(QPolygonF{{0, 0}, {0.5, 0}, {1, 0}}).isEmpty());
        auto bad = square; bad.append(square.first()); QVERIFY(!validatePolygon(bad).isEmpty());
        bad = square; bad[0] = {-1, 0}; QVERIFY(!validatePolygon(bad).isEmpty());
        bad[0] = {std::numeric_limits<double>::infinity(), 0}; QVERIFY(!validatePolygon(bad).isEmpty());
        bad = QPolygonF{{0, 0}, {1, 0}, {0.5, 0}, {0.5, 1}}; QVERIFY(!validatePolygon(bad).isEmpty());
        QPolygonF limit;
        for (int i = 0; i < MaxPolygonVertices; ++i) { const double angle = i * 6.283185307179586 / MaxPolygonVertices; limit.append({0.5 + 0.1 * std::cos(angle), 0.5 + 0.1 * std::sin(angle)}); }
        QVERIFY(validatePolygon(limit).isEmpty());
        limit.append({0.5, 0.5}); QVERIFY(!validatePolygon(limit).isEmpty()); QVERIFY(polygonVertices(polygonGeometry(limit)).isEmpty());
        QVERIFY(polygonVertices({{QStringLiteral("vertices"), QJsonArray{QJsonArray{QStringLiteral("bad"), 0}}}}).isEmpty());
        QCOMPARE(formatArea(999, QLocale(QLocale::Swedish, QLocale::Sweden)), QString::fromUtf8("999,00 m²"));
        QCOMPARE(formatArea(1000, QLocale(QLocale::Swedish, QLocale::Sweden)), QString::fromUtf8("0,0010 km²"));
        QVERIFY(formatArea(10, QLocale(QLocale::English, QLocale::UnitedStates)).endsWith(QStringLiteral("ft²")));
        QVERIFY(formatArea(10000, QLocale(QLocale::English, QLocale::UnitedStates)).endsWith(QStringLiteral("acres")));
        QVERIFY(formatArea(10000000, QLocale(QLocale::English, QLocale::UnitedStates)).endsWith(QStringLiteral("mi²")));
        QVERIFY(formatArea(-1).isEmpty()); QVERIFY(formatArea(std::numeric_limits<double>::infinity()).isEmpty());
        UserObjects objects; UserObject item; item.id = QStringLiteral("area"); item.type = QStringLiteral("polygon"); item.title = QStringLiteral("Area"); item.geometry = polygonGeometry(square);
        QVERIFY(objects.put(item).isEmpty()); const auto saved = objects.savePreferences();
        objects.remove(item.id); objects.undoStack().undo(); QCOMPARE(objects.find(item.id)->geometry, item.geometry);
        UserObjects restored; restored.restorePreferences(saved); QCOMPARE(restored.find(item.id)->json(), item.json());
        item.properties.insert(QStringLiteral("Area"), QStringLiteral("fake")); QVERIFY(!objects.put(item).isEmpty());
        item.properties = {}; item.geometry = polygonGeometry(bad); QVERIFY(!objects.put(item).isEmpty());
    }

    void userObjectsAndUndo()
    {
        UserObjects objects;
        UserObject item{QStringLiteral("test"), QStringLiteral("circle"), QStringLiteral("Marker"), QStringLiteral("Description"),
            {{QStringLiteral("unit"), QStringLiteral("Alpha")}}, {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        QVERIFY(objects.put(item).isEmpty()); objects.remove(item.id); QVERIFY(objects.items().isEmpty());
        QVERIFY(!objects.put(item).isEmpty());
        objects.undoStack().undo(); QCOMPARE(objects.find(item.id)->json(), item.json());
        objects.undoStack().redo(); QVERIFY(objects.items().isEmpty()); objects.undoStack().undo();
        auto invalid = item; invalid.geometry.insert(QStringLiteral("x"), 2); QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item; invalid.type = QStringLiteral("other"); QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item; invalid.title = QString(129, 'x'); QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item; invalid.properties.insert(QStringLiteral("Unit"), QString(257, 'x')); QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item; invalid.properties.insert(QStringLiteral("ID"), QStringLiteral("reserved")); QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item; QJsonObject nested{{QStringLiteral("leaf"), true}};
        for (int i = 0; i < 10; ++i) nested = {{QStringLiteral("child"), nested}};
        invalid.geometry = nested; QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item; invalid.geometry.insert(QStringLiteral("path"), QJsonArray{1, 2, 3}); QVERIFY(objects.put(invalid).isEmpty()); QVERIFY(objects.put(item).isEmpty());
        const auto saved = objects.savePreferences(); UserObjects restored; restored.restorePreferences(saved); QCOMPARE(restored.find(item.id)->json(), item.json());
        for (int i = 0; i < 300; ++i) { auto next = item; next.id = QString::number(i); objects.put(next); }
        QVERIFY(objects.items().size() <= 256);
        objects.remove(item.id); auto next = item; next.id = QStringLiteral("overflow"); QVERIFY(!objects.put(next).isEmpty());
        objects.undoStack().undo(); QVERIFY(objects.find(item.id));
        for (const auto &id : objects.items().keys()) objects.remove(id);
        QVERIFY(objects.undoStack().count() <= 100); objects.restorePreferences(saved); QVERIFY(!objects.undoStack().canUndo());
    }

    void pluginManifests()
    {
        QCOMPARE(builtInPlugins().size(), 17);
        for (const auto &entry : QPluginLoader::staticPlugins()) {
            if (entry.metaData().value(QStringLiteral("IID")).toString() != QStringLiteral("org.swak.Plugin/2.0")) continue;
            const auto manifest = entry.metaData().value(QStringLiteral("MetaData")).toObject();
            QVERIFY2(validatePluginManifest(manifest, entry.instance()).isEmpty(), qPrintable(validatePluginManifest(manifest, entry.instance())));
            for (const auto &key : {QStringLiteral("schemaVersion"), QStringLiteral("apiVersion"), QStringLiteral("id"),
                QStringLiteral("version"), QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("entryPoint"),
                QStringLiteral("linkage"), QStringLiteral("platforms"), QStringLiteral("qtMajor"), QStringLiteral("qtMinimum"),
                QStringLiteral("interfaces"), QStringLiteral("requires"), QStringLiteral("permissions"), QStringLiteral("settingsSchemaVersion")}) {
                auto invalid = manifest;
                invalid.remove(key);
                QVERIFY2(!validatePluginManifest(invalid, entry.instance()).isEmpty(), qPrintable(key));
            }
            for (const auto &key : {QStringLiteral("id"), QStringLiteral("version"), QStringLiteral("qtMinimum")}) {
                auto invalid = manifest;
                invalid.insert(key, manifest.value(key).toString() + '\n');
                QVERIFY(!validatePluginManifest(invalid, entry.instance()).isEmpty());
            }
            auto future = manifest;
            future.insert(QStringLiteral("qtMinimum"), QStringLiteral("99.0.0"));
            QVERIFY(!validatePluginManifest(future, entry.instance()).isEmpty());
            future = manifest;
            future.insert(QStringLiteral("permissions"), QJsonArray{QStringLiteral("unknown")});
            QVERIFY(!validatePluginManifest(future, entry.instance()).isEmpty());
        }
        QVERIFY(!validatePluginManifest({}, nullptr).isEmpty());
    }

    void initTestCase()
    {
        int layers = 0;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (auto *codec = qobject_cast<Codec *>(plugin)) codec_ = codec;
            if (auto *source = qobject_cast<SourcePlugin *>(plugin)) source_ = source;
            if (qobject_cast<MapLayer *>(plugin)) ++layers;
        }
        QVERIFY(codec_);
        QVERIFY(source_);
        QCOMPARE(layers, 2);
    }
    void roundTrip()
    {
        const auto event = sample();
        const auto bytes = codec_->encode(event);
        QVERIFY(bytes.contains("&lt;test&gt; &amp;"));
        const auto decoded = codec_->decode(bytes);
        QVERIFY2(decoded.event.has_value(), qPrintable(decoded.error));
        QCOMPARE(decoded.event->callsign, event.callsign);
        QCOMPARE(decoded.event->time, event.time);
        QCOMPARE(decoded.event->latitude, event.latitude);
        QCOMPARE(decoded.event->longitude, event.longitude);
        QCOMPARE(codec_->encode(*decoded.event), bytes);
    }
    void malformed_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const auto valid = codec_->encode(sample());
        QTest::newRow("empty") << QByteArray{};
        QTest::newRow("size") << QByteArray(MaxMessageBytes + 1, 'x');
        QTest::newRow("doctype") << QByteArray(valid).replace("?><event", "?><!DOCTYPE event [<!ENTITY e 'bad'>]><event");
        QTest::newRow("entity") << QByteArray(valid).replace("test-1", "&missing;");
        QTest::newRow("duplicate") << valid + valid;
        QTest::newRow("truncated") << valid.left(valid.size() - 20);
        QTest::newRow("version") << QByteArray(valid).replace("version=\"2.0\"", "version=\"3.0\"");
        QTest::newRow("nan") << QByteArray(valid).replace("59.32930000", "nan");
        QTest::newRow("range") << QByteArray(valid).replace("59.32930000", "90.00000001");
        QTest::newRow("numeric-overflow") << QByteArray(valid).replace("59.32930000", QByteArray(350, '9'));
        QTest::newRow("exponent") << QByteArray(valid).replace("59.32930000", "1e1");
        QTest::newRow("negative-error") << QByteArray(valid).replace("ce=\"9999999.000\"", "ce=\"-1\"");
        QTest::newRow("duplicate-point") << QByteArray(valid).replace("<detail>", "<point lat=\"0\" lon=\"0\" hae=\"0\" ce=\"1\" le=\"1\"/><detail>");
        QTest::newRow("namespace") << QByteArray(valid).replace("<event ", "<event xmlns=\"urn:wrong\" ");
        QTest::newRow("depth") << QByteArray(valid).replace("<detail>", "<detail>" + QByteArray("<a>").repeated(20)).replace("</detail>", QByteArray("</a>").repeated(20) + "</detail>");
        QTest::newRow("elements") << QByteArray(valid).replace("<detail>", "<detail>" + QByteArray("<a/>").repeated(260));
        QTest::newRow("uid-limit") << QByteArray(valid).replace("test-1", QByteArray(129, 'x'));
        QTest::newRow("control") << QByteArray(valid).replace("test-1", "test&#10;1");
        QTest::newRow("text") << QByteArray(valid).replace("</event>", "text</event>");
        QTest::newRow("point-child") << QByteArray(valid).replace("<detail>", "<wrong/><detail>");
        QTest::newRow("invalid-how") << QByteArray(valid).replace("h-e", "invalid");
        QTest::newRow("invalid-type") << QByteArray(valid).replace("a-f-G", "a--G");
        QTest::newRow("missing-zone") << QByteArray(valid).replace("Z\"", "\"");
    }
    void malformed()
    {
        QFETCH(QByteArray, bytes);
        const auto result = codec_->decode(bytes);
        QVERIFY(!result.event);
        QVERIFY(!result.error.isEmpty());
    }
    void boundedCorpus()
    {
        const auto valid = codec_->encode(sample());
        for (qsizetype length = 0; length < valid.size() - 2; ++length) QVERIFY(!codec_->decode(valid.left(length)).event);
        for (qsizetype index = 0; index < valid.size(); index += 3) {
            auto mutation = valid;
            mutation[index] = '\0';
            QVERIFY(!codec_->decode(mutation).event);
        }
    }
    void detailExtension()
    {
        auto bytes = codec_->encode(sample());
        bytes.replace("<detail>", "<detail><extension><nested>opaque text</nested></extension>");
        QVERIFY(codec_->decode(bytes).event);
    }
    void invalidGeneratedValues()
    {
        auto event = sample();
        event.latitude = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(codec_->encode(event).isEmpty());
        event = sample();
        event.stale = event.start;
        QVERIFY(codec_->encode(event).isEmpty());
    }
    void storeLifecycle()
    {
        Store store;
        QAbstractItemModelTester tester(&store, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto event = sample();
        const auto now = event.time;
        QVERIFY(store.accept(event, now).isEmpty());
        store.select(event.uid);
        QCOMPARE(store.selected(), event.uid);
        event.latitude = 0;
        event.time = now.addSecs(1);
        QVERIFY(store.accept(event, now).isEmpty());
        QCOMPARE(store.rowCount(), 1);
        QCOMPARE(store.events().first().latitude, 0.0);
        event.time = now.addSecs(-1);
        QVERIFY(!store.accept(event, now).isEmpty());
        store.expire(now.addSecs(301));
        QCOMPARE(store.rowCount(), 0);
        QVERIFY(store.selected().isEmpty());
        QVERIFY(!store.accept(event, now.addSecs(301)).isEmpty());
        event = sample();
        event.stale = now.addDays(8);
        QVERIFY(!store.accept(event, now).isEmpty());
        event = sample();
        event.start = now.addSecs(400);
        event.time = event.start;
        event.stale = now.addSecs(500);
        QVERIFY(!store.accept(event, now).isEmpty());
    }
    void storageLimit()
    {
        Store store;
        auto event = sample();
        const auto now = event.time;
        for (int row = 0; row < MaxEvents; ++row) {
            event.uid = QString::number(row);
            QVERIFY(store.accept(event, now).isEmpty());
        }
        event.uid = QStringLiteral("overflow");
        QVERIFY(!store.accept(event, now).isEmpty());
        event.uid = QStringLiteral("0");
        QVERIFY(store.accept(event, now).isEmpty());
        store.clear();
        QCOMPARE(store.rowCount(), 0);
        QVERIFY(store.accept(event, now).isEmpty());
    }
    void projections()
    {
        for (const auto point : {QPointF(0, 0), QPointF(180, 85), QPointF(-180, -85), QPointF(18.0686, 59.3293)}) {
            const auto restored = unproject(project(point.y(), point.x()));
            QVERIFY(std::abs(restored.x() - point.x()) < 1e-8);
            QVERIFY(std::abs(restored.y() - point.y()) < 1e-8);
        }
        QVERIFY(std::isfinite(project(90, 0).y()));
        Viewport view{{800, 600}, {0.5, 0.5}, 3};
        const auto coordinate = view.coordinate(view.screen(59.3, 18.0));
        QVERIFY(std::abs(coordinate.x() - 18) < 1e-8);
        QVERIFY(std::abs(coordinate.y() - 59.3) < 1e-8);
    }
    void fileBoundaries()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("event.xml"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = codec_->encode(sample());
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        QCOMPARE(readEventFile(path).bytes, bytes);
        QVERIFY(!readEventFile(directory.path()).error.isEmpty());
        QVERIFY(!readEventFile(path + QStringLiteral(".missing")).error.isEmpty());
        const auto pipe = directory.filePath(QStringLiteral("pipe"));
        QCOMPARE(::mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
        QVERIFY(!readEventFile(pipe).error.isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(MaxMessageBytes + 1, 'x')), MaxMessageBytes + 1);
        file.close();
        QVERIFY(!readEventFile(path).error.isEmpty());
    }
    void namespaceLimit()
    {
        auto xml = codec_->encode(sample());
        QByteArray declarations;
        for (int i = 0; i < 33; ++i) declarations += " xmlns:n" + QByteArray::number(i) + "=\"urn:" + QByteArray::number(i) + "\"";
        xml.replace("<event ", "<event" + declarations + " ");
        QVERIFY(!codec_->decode(xml).event.has_value());
    }
    void receiverSummaryRecovery()
    {
        Store store; Servers servers(store); QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0)); const auto port = sender.localPort(); sender.close();
        auto source = serverDefaults(); source.insert(QStringLiteral("name"), QStringLiteral("Local")); source.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1")); source.insert(QStringLiteral("port"), port); source.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers.put(QStringLiteral("test"), source).isEmpty()); QVERIFY(QTest::qWaitFor([&] { return servers.status(QStringLiteral("test")).contains(QStringLiteral("Listening")); }, 5000));
        sender.writeDatagram("bad", QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor([&] { return (servers.statistics().value(QStringLiteral("test")).toObject().value(QStringLiteral("rejected")).toInt()) == ( 1); }, 5000));
        sender.writeDatagram(codec_->encode(sample()), QHostAddress::LocalHost, port); QVERIFY(QTest::qWaitFor([&] { return (store.rowCount()) == ( 1); }, 5000));
    }
    void receiverLifecycle()
    {
        Store store; QUdpSocket reservation; QVERIFY(reservation.bind(QHostAddress::LocalHost, 0)); const auto port = reservation.localPort();
        auto servers = std::make_unique<Servers>(store);
        auto source = serverDefaults(); source.insert(QStringLiteral("name"), QStringLiteral("Local")); source.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1")); source.insert(QStringLiteral("port"), port); source.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers->put(QStringLiteral("test"), source).isEmpty()); QVERIFY(QTest::qWaitFor([&] { return servers->status(QStringLiteral("test")).contains(QStringLiteral("Cannot bind")); }, 5000));
        reservation.close(); servers->reconnect(); QVERIFY(QTest::qWaitFor([&] { return servers->status(QStringLiteral("test")).contains(QStringLiteral("Listening")); }, 5000));
        QUdpSocket sender; sender.writeDatagram(codec_->encode(sample()), QHostAddress::LocalHost, port); QVERIFY(QTest::qWaitFor([&] { return (store.rowCount()) == ( 1); }, 5000));
        sender.writeDatagram(QByteArray(MaxMessageBytes + 1, 'x'), QHostAddress::LocalHost, port);
        auto next = sample(); next.uid = QStringLiteral("test-2"); sender.writeDatagram(codec_->encode(next), QHostAddress::LocalHost, port); QVERIFY(QTest::qWaitFor([&] { return (store.rowCount()) == ( 2); }, 5000));
        servers.reset(); QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
    }

};
QTEST_MAIN(CoreTest)
#include "test_core.moc"
