#include "app/window.h"
#include "core/activity.h"
#include "core/data.h"
#include "core/graphics.h"
#include "core/interfaces.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/marker_collections.h"
#include "core/polygon.h"
#include "core/preferences.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/user_objects.h"
#include "core/vector_tiles.h"
#include "vector_fixture.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QOpenGLWidget>
#include <QPluginLoader>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>

#include <algorithm>
#include <ctime>
#include <memory>
#include <unistd.h>

using namespace swak;
namespace {
qint64 residentBytes()
{
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const auto fields = file.readAll().simplified().split(' ');
    return fields.size() > 1 ? fields.at(1).toLongLong() * ::sysconf(_SC_PAGESIZE) : -1;
}
int descriptors()
{
    return static_cast<int>(QDir(QStringLiteral("/proc/self/fd"))
                                .entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot)
                                .size());
}
} // namespace
class SoakTest final : public QObject
{
    Q_OBJECT
private slots:
    void sustainedWork()
    {
        bool ok = false;
        const auto secondsText = qgetenv("SWAK_SOAK_SECONDS");
        const int seconds = secondsText.isEmpty() ? 5 : secondsText.toInt(&ok);
        QVERIFY2(secondsText.isEmpty() || (ok && seconds >= 1 && seconds <= 86400),
                 "SWAK_SOAK_SECONDS must be between 1 and 86400.");
        // Graphics, font, and allocator caches fill during the first minutes of a GUI run.
        // The runner sets a longer warmup for long runs. Memory checks start after it.
        const auto warmupText = qgetenv("SWAK_SOAK_WARMUP");
        const int warmup = warmupText.isEmpty() ? 2 : warmupText.toInt(&ok);
        QVERIFY2(warmupText.isEmpty() || (ok && warmup >= 1 && warmup <= 600),
                 "SWAK_SOAK_WARMUP must be between 1 and 600.");
        DataProvider *dataProvider = nullptr;
        VectorTileProvider *vectorProvider = nullptr;
        Codec *codec = nullptr;
        SourcePlugin *source = nullptr;
        QList<ToolPlugin *> tools;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (auto *item = qobject_cast<DataProvider *>(plugin)) {
                dataProvider = item;
            }
            if (auto *item = qobject_cast<VectorTileProvider *>(plugin)) {
                vectorProvider = item;
            }
            if (auto *item = qobject_cast<Codec *>(plugin)) {
                codec = item;
            }
            if (auto *item = qobject_cast<SourcePlugin *>(plugin)) {
                source = item;
            }
            if (auto *item = qobject_cast<ToolPlugin *>(plugin)) {
                tools.append(item);
            }
        }
        QVERIFY(codec && source && dataProvider && !tools.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Preferences preferences(directory.filePath(QStringLiteral("preferences.json")));
        std::unique_ptr<Window> window;
        if (qEnvironmentVariableIsSet("SWAK_SOAK_GUI")) {
            window = std::make_unique<Window>(directory.filePath(QStringLiteral("window.json")),
                                              QPluginLoader::staticInstances());
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window.get()));
            auto *map = window->findChild<QOpenGLWidget *>();
            QVERIFY(map);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return map->isValid();
                },
                5000));
        }
        UserObjects localObjects;
        MarkerCollections collections(QPluginLoader::staticInstances());
        auto markerCollection = coreMarkerCollection();
        markerCollection.insert(QStringLiteral("id"), QStringLiteral("soak.collection"));
        Store headlessStore;
        Store &store = window ? window->store() : headlessStore;
        ActivityModel activity;
        MapSources maps({});
        PluginServices services{store, activity, maps};
        MapTools mapTools;
        QMenu traceMenu;
        services.mapTools = &mapTools;
        services.objects = &localObjects;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (auto *map = qobject_cast<MapExtension *>(plugin);
                map && (map->mapId() == QStringLiteral("org.swak.trace") ||
                        map->mapId() == QStringLiteral("org.swak.predict"))) {
                qobject_cast<MenuPlugin *>(plugin)->populateMenu(services, traceMenu);
            }
        }
        auto *trace = dynamic_cast<SettingsParticipant *>(
            traceMenu.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(trace);
        auto *predict = dynamic_cast<SettingsParticipant *>(
            traceMenu.findChild<QObject *>(QStringLiteral("predictTool")));
        QVERIFY(predict);
        QJsonArray predicted;
        QJsonArray traced;
        for (int i = 0; i < 128; ++i) {
            predicted.append(QStringLiteral("item-%1").arg(i));
            traced.append(QJsonObject{{QStringLiteral("owner"), QStringLiteral("org.swak.events")},
                                      {QStringLiteral("id"), QStringLiteral("item-%1").arg(i)}});
        }
        trace->restorePreferences({{QStringLiteral("targets"), traced}});
        predict->restorePreferences({{QStringLiteral("targets"), predicted}});
        Event event;
        event.type = QStringLiteral("a-f-G");
        event.how = QStringLiteral("h-e");
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addDays(1);
        for (int i = 0; i < MaxEvents; ++i) {
            event.uid = QString::number(i);
            QVERIFY(store.accept(event).isEmpty());
        }
        event.uid = QStringLiteral("over-capacity");
        QVERIFY(!store.accept(event).isEmpty());
        store.clear();
        auto receiver = std::unique_ptr<QWidget>(source->create(services, *codec, nullptr));
        auto *servers = receiver->findChild<swak::Servers *>();
        QVERIFY(servers);
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        const auto targetPort = sender.localPort();
        sender.close();
        auto sourceValue = swak::serverDefaults();
        sourceValue.insert(QStringLiteral("name"), QStringLiteral("Soak"));
        sourceValue.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        sourceValue.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        sourceValue.insert(QStringLiteral("port"), targetPort);
        sourceValue.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers->put(QStringLiteral("soak"), sourceValue).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers->status(QStringLiteral("soak"))
                    .contains(QStringLiteral("Listening"));
            },
            5000));
        qint64 maxGap = 0;
        QElapsedTimer heartbeat;
        heartbeat.start();
        QTimer pulse;
        pulse.setInterval(20);
        connect(&pulse, &QTimer::timeout, &pulse, [&] {
            maxGap = qMax(maxGap, heartbeat.restart());
        });
        pulse.start();
        QElapsedTimer elapsed;
        elapsed.start();
        // Sanitizer and graphics allocators move resident memory by several MiB between cycles.
        // Compare window means, so that one low baseline sample or one high sample does not
        // count as growth. A leak raises the later means.
        const qint64 windowMs = std::clamp<qint64>(static_cast<qint64>(seconds) * 50, 1000, 60000);
        qint64 baseline = -1;
        qint64 peak = 0;
        qint64 windowSum = 0;
        qint64 windowSamples = 0;
        qint64 windowStart = -1;
        int windows = 0;
        int baselineFiles = -1;
        const auto closeWindow = [&] {
            const qint64 mean = windowSum / windowSamples;
            if (baseline < 0) {
                baseline = mean;
            }
            peak = qMax(peak, mean);
            qInfo("memory_window=%d mean_resident_bytes=%lld growth_bytes=%lld",
                  ++windows,
                  static_cast<long long>(mean),
                  static_cast<long long>(mean - baseline));
            windowSum = 0;
            windowSamples = 0;
            return mean - baseline < 64 * 1024 * 1024;
        };
        int cycles = 0;
        quint32 random = 0x5357414b;
        while (elapsed.elapsed() < (static_cast<qint64>(seconds) + warmup) * 1000) {
            std::atomic_bool cancelled = false;
            QVERIFY(vectorProvider);
            const auto vector = vectorProvider->decodeTile(vector_fixture::sample(), cancelled);
            QVERIFY(vector.error.isEmpty());
            QCOMPARE(vector.features.size(), 3);
            const auto document = dataProvider->decode(
                "<kml "
                "xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark><Point><coordinates>0,0</"
                "coordinates></Point></Placemark></kml>",
                cancelled);
            QCOMPARE(document.shapes.size(), 1);
            event.uid = QStringLiteral("item-%1").arg(cycles % 128);
            event.time = QDateTime::currentDateTimeUtc();
            event.start = event.time;
            event.stale = event.time.addSecs(60);
            const auto xml = codec->encode(event);
            QVERIFY(codec->decode(xml).event.has_value());
            for (int i = 0; i < 32; ++i) {
                auto mutation = xml;
                random = random * 1664525U + 1013904223U;
                mutation[static_cast<qsizetype>(random % static_cast<quint32>(mutation.size()))] =
                    static_cast<char>(random >> 24);
                const auto result = codec->decode(mutation);
                if (result.event) {
                    QVERIFY(eventError(*result.event).isEmpty());
                }
                QCOMPARE(sender.writeDatagram(
                             i % 2 ? mutation : xml, QHostAddress::LocalHost, targetPort),
                         xml.size());
            }
            for (auto *tool : tools) {
                const auto widget =
                    std::unique_ptr<QWidget>(tool->create(services, *codec, nullptr));
                auto *participant = dynamic_cast<PreferenceParticipant *>(widget.get());
                QVERIFY(participant);
                participant->restorePreferences(participant->savePreferences());
            }
            QVERIFY(preferences.save(
                {{QStringLiteral("version"), 1}, {QStringLiteral("cycle"), cycles}}));
            QTest::qWait(40);
            QVERIFY2(QTest::qWaitFor(
                         [&] {
                             return store.rowCount() > 0;
                         },
                         2000),
                     "The receiver made no progress.");
            store.select(event.uid);
            auto tracedEvent = event;
            for (const auto &received : store.events()) {
                if (received.uid == event.uid) {
                    tracedEvent = received;
                }
            }
            for (int sample = 0; sample < 52; ++sample) {
                tracedEvent.time = tracedEvent.time.addMSecs(100);
                tracedEvent.longitude = static_cast<double>(sample) * 0.0001;
                // Mutations can place time near the accepted expiry or future limit.
                const auto sampleNow = qMax(QDateTime::currentDateTimeUtc(), tracedEvent.time);
                tracedEvent.stale = qMax(tracedEvent.stale, sampleNow.addSecs(60));
                const auto traceError = store.accept(tracedEvent, sampleNow);
                QVERIFY2(traceError.isEmpty(), qPrintable(traceError));
            }
            // Valid mutations can extend the expiry time.
            auto latestStale = event.stale;
            for (const auto &received : store.events()) {
                latestStale = qMax(latestStale, received.stale);
            }
            store.expire(latestStale.addSecs(1));
            QCOMPARE(store.rowCount(), 0);
            UserObject marker{QStringLiteral("marker-%1").arg(cycles % 128),
                              QStringLiteral("circle"),
                              QStringLiteral("Soak marker"),
                              {},
                              {},
                              {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
            QVERIFY(localObjects.put(marker).isEmpty());
            localObjects.remove(marker.id);
            localObjects.undoStack().undo();
            QVERIFY(localObjects.find(marker.id));
            localObjects.undoStack().redo();
            QVERIFY(!localObjects.find(marker.id));
            UserObject polygon;
            polygon.id = QStringLiteral("polygon-%1").arg(cycles % 128);
            polygon.type = QStringLiteral("polygon");
            polygon.title = QStringLiteral("Soak area");
            const QPolygonF corners{{0.4, 0.4}, {0.6, 0.4}, {0.6, 0.6}, {0.4, 0.6}};
            polygon.geometry = polygonGeometry(corners);
            QVERIFY(localObjects.put(polygon).isEmpty());
            QVERIFY(polygonArea(corners) > 0);
            localObjects.remove(polygon.id);
            localObjects.undoStack().undo();
            QVERIFY(localObjects.find(polygon.id));
            localObjects.undoStack().redo();
            for (const auto &type : {QStringLiteral("range-circle"), QStringLiteral("line")}) {
                UserObject shape;
                shape.id = type + QString::number(cycles % 128);
                shape.type = type;
                shape.title = QStringLiteral("Soak shape");
                shape.geometry = polygonGeometry({QPointF(0.5, 0.5), QPointF(0.51, 0.5)});
                QVERIFY(localObjects.put(shape).isEmpty());
                localObjects.remove(shape.id);
                localObjects.undoStack().undo();
                QVERIFY(localObjects.find(shape.id));
                localObjects.undoStack().redo();
            }
            QVERIFY(localObjects.undoStack().count() <= 100);
            QVERIFY(collections.put(markerCollection).isEmpty());
            QVERIFY(collections.symbol(QStringLiteral("soak.collection"), {}));
            QVERIFY(collections.remove(QStringLiteral("soak.collection")).isEmpty());
            ++cycles;
            const qint64 resident = residentBytes();
            QVERIFY(resident > 0);
            if (elapsed.elapsed() >= static_cast<qint64>(warmup) * 1000) {
                if (baselineFiles < 0) {
                    baselineFiles = descriptors();
                    windowStart = elapsed.elapsed();
                }
                windowSum += resident;
                ++windowSamples;
                if (elapsed.elapsed() - windowStart >= windowMs) {
                    windowStart = elapsed.elapsed();
                    QVERIFY2(closeWindow(),
                             "Mean resident memory grew by at least 64 MiB after warmup.");
                }
                QVERIFY2(descriptors() <= baselineFiles + 8,
                         "Open file descriptors grew after warmup.");
            }
            QVERIFY2(maxGap < 2000, "The event loop stalled for at least two seconds.");
        }
        QVERIFY(cycles > 0);
        if (windowSamples > 0) {
            QVERIFY2(closeWindow(), "Mean resident memory grew by at least 64 MiB after warmup.");
        }
        QVERIFY(windows > 1);
        QElapsedTimer quiet;
        quiet.start();
        QObject idleObserver;
        int modelUpdates = 0, motionUpdates = 0, frames = 0;
        const auto activityChanged = [&quiet, &frames] {
            ++frames;
            quiet.restart();
        };
        connect(&store, &Store::changed, &idleObserver, [&] {
            ++modelUpdates;
            quiet.restart();
        });
        connect(&store.motion(), &TrackMotion::changed, &idleObserver, [&] {
            ++motionUpdates;
            quiet.restart();
        });
        if (window) {
            connect(window->findChild<QOpenGLWidget *>(),
                    &QOpenGLWidget::frameSwapped,
                    &idleObserver,
                    activityChanged);
        }
        QVERIFY2(QTest::qWaitFor(
                     [&quiet] {
                         return quiet.elapsed() >= 500;
                     },
                     20000),
                 qPrintable(QStringLiteral("Idle did not settle: model=%1 motion=%2 frames=%3")
                                .arg(modelUpdates)
                                .arg(motionUpdates)
                                .arg(frames)));
        const auto cpuStart = std::clock();
        QTest::qWait(1000);
        const double idleCpu = static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
        QVERIFY2(
            idleCpu < 0.5,
            qPrintable(QStringLiteral("Idle CPU time was %1 seconds; limit is 0.5.").arg(idleCpu)));
        servers->remove(QStringLiteral("soak"));
        receiver.reset();
        sender.close();
        QVERIFY(sender.bind(QHostAddress::LocalHost, targetPort));
        qInfo("cycles=%d memory_windows=%d peak_growth_bytes=%lld max_heartbeat_gap_ms=%lld "
              "idle_cpu_seconds=%.3f",
              cycles,
              windows,
              static_cast<long long>(peak - baseline),
              static_cast<long long>(maxGap),
              idleCpu);
    }
};
int main(int argc, char **argv)
{
    swak::configureGraphics();
    QApplication app(argc, argv);
    SoakTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_soak.moc"
