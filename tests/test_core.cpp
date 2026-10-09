#include "core/activity.h"
#include "core/files.h"
#include "core/graphics.h"
#include "core/interfaces.h"
#include "core/map_decorations.h"
#include "core/map_grid.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/marker_collections.h"
#include "core/marker_symbols.h"
#include "core/measure.h"
#include "core/plugin_catalog.h"
#include "core/polygon.h"
#include "core/prediction.h"
#include "core/projection.h"
#include "core/routing.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/user_objects.h"

#include <QAbstractItemModelTester>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPluginLoader>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSurfaceFormat>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>

#include <archive.h>
#include <archive_entry.h>
#include <cmath>
#include <limits>
#include <memory>
#include <sys/stat.h>

static void answerAction(Qt::Key key = Qt::Key_Return)
{
    QTimer::singleShot(0, qApp, [key] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QCOMPARE(dialog->objectName(), QStringLiteral("actionConfirmation"));
        QCOMPARE(dialog->icon(), QMessageBox::NoIcon);
        QTest::keyClick(dialog, key);
    });
}
using namespace swak;
class CoreTest final : public QObject
{
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
    void routingFormatAndRequests()
    {
        RouteSources sources(QPluginLoader::staticInstances(), nullptr);
        auto *provider = sources.provider(QJsonObject{{"format", "org.swak.valhalla"}});
        QVERIFY(provider);
        for (const auto &mode : QStringList{"driving", "walking", "running", "cycling"}) {
            const auto payload = provider->request({18, 59}, {18.1, 59.1}, mode, 12);
            QCOMPARE(payload.value("shape_format").toString(), QStringLiteral("geojson"));
            const auto costing = payload.value("costing").toString();
            QVERIFY(payload.value("costing_options")
                        .toObject()
                        .value(costing)
                        .toObject()
                        .value("shortest")
                        .toBool());
            if (mode == QStringLiteral("running")) {
                QCOMPARE(payload.value("costing_options")
                             .toObject()
                             .value(costing)
                             .toObject()
                             .value("walking_speed")
                             .toDouble(),
                         12.0);
            }
        }
        const QByteArray valid =
            R"({"code":"Ok","routes":[{"distance":123,"duration":45,"geometry":{"type":"LineString","coordinates":[[18,59],[18.1,59.1]]}}]})";
        QVERIFY(provider->decode(valid).error.isEmpty());
        QCOMPARE(provider->decode(valid).coordinates.size(), 2);
        for (const auto &body :
             QList<QByteArray>{"{",
                               "{}",
                               "[]",
                               R"({"code":"NoRoute"})",
                               QByteArray(valid).replace("18.1", "999"),
                               QByteArray(valid).replace("123", "-1"),
                               QByteArray(valid).replace("LineString", "Polygon"),
                               QByteArray(valid).replace("[18,59]", "[null,59]"),
                               QByteArray(4 * 1024 * 1024 + 1, 'x')}) {
            QVERIFY(!provider->decode(body).error.isEmpty());
        }
        auto source = sources.sources().value("fossgis").toObject();
        source.insert("endpoint", "http://example.com/route");
        QVERIFY(!RouteSources::validate(source).isEmpty());
        source.insert("endpoint", "https://user:secret@example.com/route");
        QVERIFY(!RouteSources::validate(source).isEmpty());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray response = valid;
        QByteArray status = "200 OK";
        QByteArray received;
        bool hold = false;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                received += socket->readAll();
                if (!received.contains("\r\n\r\n") || hold) {
                    return;
                }
                socket->write("HTTP/1.1 " + status +
                              "\r\nContent-Length: " + QByteArray::number(response.size()) +
                              "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
        });
        source.insert("endpoint",
                      QStringLiteral("http://127.0.0.1:%1/route").arg(server.serverPort()));
        for (int test = 0; test < 5; ++test) {
            RouteSources client(QPluginLoader::staticInstances(), nullptr);
            QVERIFY(client.put("local", source).isEmpty());
            client.select("local");
            QSignalSpy complete(&client, &RouteSources::completed);
            received.clear();
            response = test == 1   ? QByteArray("invalid")
                       : test == 2 ? QByteArray(4 * 1024 * 1024 + 1, 'x')
                                   : valid;
            status = test == 3 ? QByteArray("302 Found") : QByteArray("200 OK");
            hold = test == 4;
            client.calculate({18, 59}, {18.1, 59.1}, "driving", 10);
            if (hold) {
                QVERIFY(QTest::qWaitFor(
                    [&] {
                        return !received.isEmpty();
                    },
                    5000));
                client.cancel();
                QVERIFY(!client.busy());
                QCOMPARE(complete.count(), 0);
            } else {
                QVERIFY(QTest::qWaitFor(
                    [&] {
                        return complete.count() == 1;
                    },
                    5000));
                QCOMPARE(client.result().error.isEmpty(), test == 0);
                QVERIFY(!client.busy());
            }
        }
        QSignalSpy complete(&sources, &RouteSources::completed);
        sources.calculate({18, 59}, {18, 59}, "walking", 10);
        QCOMPARE(complete.count(), 1);
        QVERIFY(!sources.result().error.isEmpty());
        source.insert("format", "missing-plugin");
        sources.restorePreferences(
            {{"sources", QJsonObject{{"missing", source}}}, {"active", "missing"}});
        QVERIFY(sources.sources().contains("missing"));
        QVERIFY(!sources.provider(source));
        sources.calculate({18, 59}, {18.1, 59.1}, "walking", 10);
        QCOMPARE(complete.count(), 2);
    }

    void graphicsConfiguration()
    {
        const bool set = qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION");
        const auto original = qgetenv("QT_XCB_GL_INTEGRATION");
        const auto format = QSurfaceFormat::defaultFormat();
        const auto restore = qScopeGuard([&] {
            if (set) {
                qputenv("QT_XCB_GL_INTEGRATION", original);
            } else {
                qunsetenv("QT_XCB_GL_INTEGRATION");
            }
            QSurfaceFormat::setDefaultFormat(format);
        });
        qunsetenv("QT_XCB_GL_INTEGRATION");
        configureGraphics();
        QCOMPARE(qgetenv("QT_XCB_GL_INTEGRATION"), QByteArray("xcb_egl"));
        QCOMPARE(QSurfaceFormat::defaultFormat().version(), qMakePair(3, 3));
        QCOMPARE(QSurfaceFormat::defaultFormat().profile(), QSurfaceFormat::CoreProfile);
        QCOMPARE(QSurfaceFormat::defaultFormat().stencilBufferSize(), 8);
        QCOMPARE(QSurfaceFormat::defaultFormat().depthBufferSize(), 0);
        for (const auto *value : {"xcb_glx", "xcb_egl", "none", ""}) {
            qputenv("QT_XCB_GL_INTEGRATION", value);
            configureGraphics();
            QVERIFY(qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION"));
            QCOMPARE(qgetenv("QT_XCB_GL_INTEGRATION"), QByteArray(value));
        }
    }
    void trackKinematics()
    {
        TrackMotion motion;
        auto event = sample();
        event.latitude = 0;
        event.longitude = 0;
        motion.update(event);
        QVERIFY(!motion.kinematics(event.uid).heading);
        QVERIFY(!motion.kinematics(event.uid).speed);
        event.time = event.time.addSecs(10);
        event.longitude = .001;
        motion.update(event);
        auto state = motion.kinematics(event.uid);
        QVERIFY(state.speed && std::abs(*state.speed - 11.11949) < .01);
        QVERIFY(state.heading && std::abs(*state.heading - 90) < .01);
        QVERIFY(state.estimatedHeading && state.calculatedSpeed);
        motion.update(event);
        QCOMPARE(motion.kinematics(event.uid).speed, state.speed);
        motion.setPaused(true);
        event.time = event.time.addSecs(10);
        event.latitude = .001;
        event.course = 360;
        event.speed = 20;
        motion.update(event);
        QCOMPARE(motion.kinematics(event.uid).heading, state.heading);
        motion.setPaused(false);
        state = motion.kinematics(event.uid);
        QCOMPARE(state.heading.value(), 0.0);
        QVERIFY(!state.estimatedHeading && state.calculatedSpeed);
        event.time = event.time.addSecs(10);
        event.longitude = 90;
        motion.update(event);
        QVERIFY(!motion.kinematics(event.uid).calculatedSpeed);
        QCOMPARE(motion.kinematics(event.uid).speed.value(), 20.0);
        event.course.reset();
        event.speed.reset();
        event.time = event.time.addSecs(601);
        motion.update(event);
        QVERIFY(!motion.kinematics(event.uid).heading);
        QVERIFY(!motion.kinematics(event.uid).speed);
        event.time = event.time.addSecs(10);
        motion.update(event);
        QCOMPARE(motion.kinematics(event.uid).speed.value(), 0.0);
        QVERIFY(!motion.kinematics(event.uid).heading);
        QVERIFY(!motion.kinematics(QStringLiteral("missing")).speed);
        TrackMotion reported;
        event.speed = 5;
        reported.update(event);
        event.speed = 6;
        reported.update(event);
        QCOMPARE(reported.kinematics(event.uid).speed.value(), 6.0);
        reported.expire(event.uid);
        event.speed.reset();
        event.time = event.time.addSecs(1);
        reported.update(event);
        QVERIFY(!reported.kinematics(event.uid).speed);
        const auto crossing = groundMotion(project(0, 179.999), project(0, -179.999), 1000);
        QVERIFY(crossing && crossing->heading);
        QVERIFY(std::abs(*crossing->heading - 90) < .01);
        QVERIFY(!groundMotion({.5, .5}, {.5, .5}, 0));
        QVERIFY(!groundMotion({.5, .5}, {.5, .5}, 600001));
        QVERIFY(!groundMotion({-1, .5}, {.5, .5}, 1000));
        QVERIFY(!groundMotion({.5, .5}, {.6, .5}, 50));
        QVERIFY(formatSpeed(10, QLocale(QLocale::English, QLocale::Sweden)).endsWith(" km/h"));
        QVERIFY(formatSpeed(10, QLocale(QLocale::English, QLocale::UnitedStates)).endsWith(" mph"));
        QVERIFY(formatSpeed(-1).isEmpty());
        QVERIFY(formatSpeed(std::numeric_limits<double>::infinity()).isEmpty());
        // CoT animation no longer holds the cone style. The FOV settings own it.
        for (const auto &field : motion.settingsFields()) {
            QVERIFY(!field.path.startsWith(QStringLiteral("headingCone")));
        }
        QVERIFY(!motion.savePreferences().contains(QStringLiteral("headingConeAngle")));
        HeadingConeStyle style;
        style.angle = 90;
        QSignalSpy changed(&motion, &TrackMotion::changed);
        motion.setHeadingConeStyle(style);
        motion.setHeadingConeStyle(style);
        QCOMPARE(changed.count(), 1);
        motion.restorePreferences(motion.savePreferences());
        QCOMPARE(motion.headingConeStyle().angle, 90.0);
    }
    void fovSettings()
    {
        Store store;
        ActivityModel activity;
        MapSources maps(QPluginLoader::staticInstances());
        MapTools tools;
        PluginServices services{store, activity, maps};
        services.mapTools = &tools;
        QMenu main;
        for (auto *plugin : builtInPlugins()) {
            if (QByteArray(plugin->metaObject()->className()).endsWith("TracksPlugin")) {
                qobject_cast<MenuPlugin *>(plugin)->populateMenu(services, main);
            }
        }
        auto *fov = dynamic_cast<SettingsParticipant *>(
            main.findChild<QObject *>(QStringLiteral("fovSettings")));
        QVERIFY(fov);
        QCOMPARE(fov->settingsTitle(), QStringLiteral("FOV"));
        QCOMPARE(fov->preferenceId(), QStringLiteral("org.swak.fov"));
        QCOMPARE(fov->preferenceFallbackId(), QStringLiteral("org.swak.track-motion"));
        QStringList paths;
        for (const auto &field : fov->settingsFields()) {
            paths.append(field.path);
        }
        QCOMPARE(paths,
                 (QStringList{QStringLiteral("headingConeEnabled"),
                              QStringLiteral("headingConeAngle"),
                              QStringLiteral("headingConeLength"),
                              QStringLiteral("headingConeOpacity"),
                              QStringLiteral("headingConeOutline"),
                              QStringLiteral("headingConeOutlineOpacity")}));
        // An earlier CoT animation section restores through the fallback identifier.
        fov->restorePreferences({{QStringLiteral("headingConeEnabled"), false},
                                 {QStringLiteral("headingConeAngle"), 90},
                                 {QStringLiteral("headingConeLength"), 100},
                                 {QStringLiteral("headingConeOpacity"), .3},
                                 {QStringLiteral("duration"), 50}});
        const auto &cone = store.motion().headingConeStyle();
        QVERIFY(!cone.enabled);
        QCOMPARE(cone.angle, 90.0);
        QCOMPARE(cone.length, 100.0);
        QCOMPARE(cone.opacity, .3);
        QCOMPARE(cone.outlineWidth, 0.0);
        QCOMPARE(cone.outlineOpacity, .6);
        // Values outside their ranges return to the defaults.
        fov->restorePreferences({{QStringLiteral("headingConeAngle"), 1000},
                                 {QStringLiteral("headingConeOpacity"), -1},
                                 {QStringLiteral("headingConeOutline"), 9},
                                 {QStringLiteral("headingConeOutlineOpacity"), 0}});
        QVERIFY(cone.enabled);
        QCOMPARE(cone.angle, 60.0);
        QCOMPARE(cone.opacity, .15);
        QCOMPARE(cone.outlineWidth, 0.0);
        QCOMPARE(cone.outlineOpacity, .6);
        auto values = fov->savePreferences();
        values.insert(QStringLiteral("headingConeOutline"), 2.5);
        values.insert(QStringLiteral("headingConeOutlineOpacity"), .8);
        fov->restorePreferences(values);
        QCOMPARE(fov->savePreferences(), values);
        QCOMPARE(cone.outlineWidth, 2.5);
    }
    void animationConfigurationChanges()
    {
        TrackMotion motion;
        QSignalSpy configuration(&motion, &TrackMotion::configurationChanged);
        motion.restorePreferences(motion.savePreferences());
        QCOMPARE(configuration.count(), 0);
        auto values = motion.savePreferences();
        values.insert(QStringLiteral("duration"), 0);
        motion.restorePreferences(values);
        QCOMPARE(configuration.count(), 1);
        motion.setHeadingConeEnabled(QStringLiteral("cone"), true);
        auto event = sample();
        for (int i = 0; i < 100; ++i) {
            event.time = event.time.addMSecs(10);
            event.longitude += .00001;
            motion.update(event, i * 10);
            motion.snapshot(i * 10);
        }
        QCOMPARE(configuration.count(), 1);
        motion.setPaused(true);
        motion.setPaused(false);
        motion.restorePreferences(motion.savePreferences());
        QCOMPARE(configuration.count(), 1);
        values.insert(QStringLiteral("teleportKm"), 20);
        motion.restorePreferences(values);
        QCOMPARE(configuration.count(), 2);
        QVERIFY(!motion.animationPreferences().contains(QStringLiteral("headingConeTargets")));
    }
    void coneTargetLimits()
    {
        TrackMotion motion;
        QSignalSpy changes(&motion, &TrackMotion::coneTargetsChanged);
        motion.setHeadingConeEnabled({}, true);
        motion.setHeadingConeEnabled(QString(129, 'x'), true);
        QCOMPARE(changes.count(), 0);
        for (int i = 0; i < 4097; ++i) {
            motion.setHeadingConeEnabled(QString::number(i), true);
        }
        QCOMPARE(changes.count(), 4096);
        QVERIFY(!motion.headingConeEnabled(QStringLiteral("4096")));
        QVERIFY(!motion.canEnableHeadingCone(QStringLiteral("4096")));
        QVERIFY(motion.canEnableHeadingCone(QStringLiteral("0")));
        motion.setHeadingConeEnabled(QStringLiteral("0"), false);
        QVERIFY(!motion.headingConeEnabled(QStringLiteral("0")));
        motion.setHeadingConeEnabled(QStringLiteral("4096"), true);
        QVERIFY(motion.headingConeEnabled(QStringLiteral("4096")));
        TrackMotion restored;
        restored.restorePreferences(motion.savePreferences());
        QCOMPARE(restored.savePreferences(), motion.savePreferences());
        restored.restorePreferences({{QStringLiteral("headingConeTargets"),
                                      QJsonArray{true,
                                                 42,
                                                 QString(129, 'x'),
                                                 QString(),
                                                 QStringLiteral("valid"),
                                                 QStringLiteral("valid")}}});
        QCOMPARE(
            restored.savePreferences().value(QStringLiteral("headingConeTargets")).toArray().size(),
            1);
        restored.restorePreferences({});
        QVERIFY(!restored.headingConeEnabled(QStringLiteral("valid")));
    }
    void trackDetails()
    {
        auto event = sample();
        event.course = 360;
        event.speed = 1000;
        const auto decoded = codec_->decode(codec_->encode(event));
        QVERIFY(decoded.event);
        QCOMPARE(decoded.event->course, event.course);
        QCOMPARE(decoded.event->speed, event.speed);
        const auto bytes = codec_->encode(sample());
        for (const QByteArray &detail : {QByteArray("<track course='-1'/>"),
                                         QByteArray("<track course='361'/>"),
                                         QByteArray("<track speed='1001'/>"),
                                         QByteArray("<track speed='-1'/>"),
                                         QByteArray("<track speed='NaN'/>"),
                                         QByteArray("<track course='inf'/>"),
                                         QByteArray("<track/><track/>"),
                                         QByteArray("<track speed='1e3'/>"),
                                         QByteArray("<track course=''/>")}) {
            auto bad = bytes;
            bad.replace("</detail>", detail + "</detail>");
            QVERIFY2(!codec_->decode(bad).event, detail.constData());
        }
        for (const QByteArray &detail : {QByteArray("<track course='90'/>"),
                                         QByteArray("<track speed='0'/>"),
                                         QByteArray("<track/>")}) {
            auto valid = bytes;
            valid.replace("</detail>", detail + "</detail>");
            QVERIFY2(codec_->decode(valid).event, detail.constData());
        }
        event.course = std::numeric_limits<double>::infinity();
        QVERIFY(!eventError(event).isEmpty());
        event.course = 0;
        event.speed = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!eventError(event).isEmpty());
    }
    void predictionTrends()
    {
        auto event = sample();
        event.latitude = 0;
        event.longitude = 0;
        const double step = 100.0 / 6371000 * 180 / 3.141592653589793;
        PredictionHistory history;
        history.append(event, 100);
        event.time = event.time.addSecs(10);
        event.longitude = step;
        history.append(event, 100);
        event.time = event.time.addSecs(10);
        event.longitude = 3 * step;
        history.append(event, 100);
        auto point = history.forecast(60);
        QVERIFY(point);
        QVERIFY(groundDistance(project(0, event.longitude), *point) > 20 * 60);
        event.time = event.time.addSecs(10);
        event.latitude = step;
        event.longitude += step;
        history.append(event, 100);
        point = history.forecast(60);
        QVERIFY(point);
        QVERIFY(unproject(*point).y() > event.latitude);
        QVERIFY(unproject(*point).y() - event.latitude >
                2 * std::abs(unproject(*point).x() - event.longitude));
        QVERIFY(history.forecast(600));
        QVERIFY(history.forecast(1800));
    }
    void predictionHistory()
    {
        PredictionHistory history;
        QVERIFY(!history.forecast(60));
        auto event = sample();
        event.latitude = 0;
        event.longitude = 0;
        const auto start = event.time;
        const double step = 10.0 / 6371000 * 180 / 3.141592653589793;
        for (int i = 0; i < 100; ++i) {
            event.time = start.addSecs(i);
            event.longitude = step * i;
            history.append(event, 100);
        }
        QCOMPARE(history.size(), 50);
        for (const int seconds : {60, 600, 1800}) {
            const auto forecast = history.forecast(seconds);
            QVERIFY(forecast);
            const auto distance =
                groundDistance(project(event.latitude, event.longitude), *forecast);
            QVERIFY2(std::abs(distance - seconds * 10) < 0.1,
                     qPrintable(QString::number(distance)));
        }
        QVERIFY(!history.forecast(-1));
        QVERIFY(!history.forecast(1801));
        const auto before = history.forecast(60);
        history.append(event, 100);
        event.time = start;
        history.append(event, 100);
        QCOMPARE(history.forecast(60), before);
        event.time = start.addSecs(100);
        event.latitude = std::numeric_limits<double>::quiet_NaN();
        history.append(event, 100);
        QCOMPARE(history.forecast(60), before);
        event.latitude = 0;
        event.longitude = 90;
        history.append(event, 100);
        QCOMPARE(history.size(), 1);
        QVERIFY(!history.forecast(60));
        event.time = event.time.addSecs(1);
        history.append(event, 100);
        QCOMPARE(history.forecast(1800).value(), project(0, 90));
        event.longitude = 90.001;
        history.append(event, 100);
        QCOMPARE(history.size(), 1);
        QVERIFY(!history.forecast(60));
        event.time = event.time.addMSecs(10);
        history.append(event, 100);
        QCOMPARE(history.size(), 1);
        history.append(event, 0);
        history.append(event, std::numeric_limits<double>::infinity());
        auto invalid = event;
        invalid.time = {};
        history.append(invalid, 100);
        invalid = event;
        invalid.latitude = 91;
        history.append(invalid, 100);
        invalid = event;
        invalid.longitude = 181;
        history.append(invalid, 100);
        QCOMPARE(history.size(), 1);
        event.time = event.time.addSecs(601);
        history.append(event, 100);
        QCOMPARE(history.size(), 1);
        PredictionHistory crossing;
        event.longitude = 179.999;
        crossing.append(event, 100);
        event.time = event.time.addSecs(1);
        event.longitude = -179.999;
        crossing.append(event, 100);
        const auto future = crossing.forecast(60);
        QVERIFY(future);
        QVERIFY(std::abs(unproject(*future).x() - (-179.879)) < 0.001);
        PredictionHistory pole;
        event.latitude = 84.99;
        event.longitude = 20;
        pole.append(event, 100);
        event.time = event.time.addSecs(1);
        event.latitude = 85;
        pole.append(event, 100);
        QVERIFY(!pole.forecast(60)); // Reject a speed above the bounded estimate.
        event.time = event.time.addSecs(20);
        event.latitude = 85.001;
        pole.append(event, 100);
        QVERIFY(pole.forecast(1800));
        QVERIFY(std::isfinite(pole.forecast(1800)->y()));
        PredictionHistory window;
        for (int i = 0; i < 30; ++i) {
            event.time = start.addSecs(i);
            event.longitude = step * i;
            window.append(event, 100, 10);
        }
        QCOMPARE(window.size(), 11);
        PredictionHistory fastSouth;
        for (int i = 0; i < 50; ++i) {
            event.time = start.addSecs(i * 10);
            event.longitude = 0;
            event.latitude = 60 - 999 * 10 * i / 6371000.0 * 180 / 3.141592653589793;
            fastSouth.append(event, 100, 600);
        }
        QCOMPARE(fastSouth.size(), 50);
        QVERIFY(fastSouth.forecast(1800));
    }
    void markerScaling()
    {
        Viewport view{QSizeF(400, 400), QPointF(.5, .5), 2};
        MapItem item;
        item.point = view.center;
        item.radius = 12;
        MapSelection selection;
        for (const int percent : {50, 75, 100, 125, 150, 200}) {
            view.markerScalePercent = percent;
            QCOMPARE(view.markerSize(12), 12.0 * percent / 100);
            for (const bool scale : {false, true}) {
                item.scaleIcon = scale;
                selection.select(item);
                QImage image(400, 400, QImage::Format_RGB32);
                image.fill(Qt::black);
                QPainter painter(&image);
                selection.paint(painter, view);
                painter.end();
                const int radius = qRound((scale ? 12.0 * percent / 100 : 12) + 2);
                QCOMPARE(image.pixelColor(200 + radius, 200), QColor(Qt::white));
                QCOMPARE(image.pixelColor(200 + radius + 3, 200), QColor(Qt::black));
            }
        }
        view.markerScalePercent = 9999;
        QCOMPARE(view.markerSize(12), 12.0);
    }
    void removeMotion()
    {
        TrackMotion motion;
        auto event = sample();
        motion.update(event);
        QCOMPARE(motion.snapshot().size(), 1);
        motion.setPaused(true);
        motion.remove(event.uid);
        QCOMPARE(motion.snapshot().size(), 1);
        motion.setPaused(false);
        QVERIFY(motion.snapshot().isEmpty());
        motion.remove(event.uid);
        motion.update(event);
        motion.remove(event.uid);
        QVERIFY(motion.snapshot().isEmpty());
    }
    void predictionLimits()
    {
        Store store;
        ActivityModel activity;
        MapSources maps({});
        MapTools tools;
        PluginServices services{store, activity, maps};
        services.mapTools = &tools;
        QMenu main;
        for (auto *plugin : builtInPlugins()) {
            if (auto *map = qobject_cast<MapExtension *>(plugin);
                map && map->mapId() == QStringLiteral("org.swak.predict")) {
                QCOMPARE(map->mapName(), QStringLiteral("Predict"));
                auto missing = services;
                missing.mapTools = nullptr;
                QMenu absent;
                qobject_cast<MenuPlugin *>(plugin)->populateMenu(missing, absent);
                QVERIFY(absent.isEmpty());
                qobject_cast<MenuPlugin *>(plugin)->populateMenu(services, main);
            }
        }
        auto *tool =
            dynamic_cast<MapTool *>(main.findChild<QObject *>(QStringLiteral("predictTool")));
        QVERIFY(tool);
        auto *settings = dynamic_cast<SettingsParticipant *>(tool);
        QVERIFY(settings);
        QWidget map;
        QEvent event(QEvent::User);
        QVERIFY(!tool->handle(event, {}, map));
        QVERIFY(!tool->cancel());
        for (int i = 0; i < 129; ++i) {
            MapItem item;
            item.owner = QStringLiteral("org.swak.events");
            item.id = QString::number(i);
            QMenu menu;
            tool->populateContextMenu(item, menu);
            auto *action = menu.findChild<QAction *>(QStringLiteral("predictMarker"));
            QVERIFY(action);
            QCOMPARE(action->isEnabled(), i < 128);
            if (action->isEnabled()) {
                action->trigger();
            }
        }
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(),
                 128);
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("opacity"), -1);
        values.insert(QStringLiteral("width"), 1000);
        values.insert(QStringLiteral("historySeconds"), -1);
        settings->restorePreferences(values);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("opacity")).toDouble(), .75);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("width")).toDouble(), 1.5);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("historySeconds")).toInt(), 300);
        QJsonArray invalid{
            true, 12, QString(129, 'a'), QString(), QStringLiteral("ok"), QStringLiteral("ok")};
        settings->restorePreferences({{QStringLiteral("targets"), invalid}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
        auto source = sample();
        source.uid = QStringLiteral("not-enabled");
        QVERIFY(store.accept(source).isEmpty());
        source.uid = QStringLiteral("ok");
        source.start = source.time.addSecs(30);
        QVERIFY(store.accept(source).isEmpty());
        QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        const auto empty = image.copy();
        {
            QPainter painter(&image);
            tool->paint(painter, Viewport{QSizeF(100, 100), QPointF(.5, .5), 2});
        }
        QCOMPARE(image, empty);
        MapItem area;
        area.owner = QStringLiteral("org.swak.events");
        area.kind = MapItem::Kind::Polygon;
        QMenu menu;
        tool->populateContextMenu(area, menu);
        QVERIFY(menu.isEmpty());
        answerAction();
        main.findChild<QAction *>(QStringLiteral("clearPredictions"))->trigger();
        QVERIFY(settings->savePreferences().value(QStringLiteral("targets")).toArray().isEmpty());
    }
    void dataDescriptions()
    {
        MapItem item;
        item.owner = QStringLiteral("org.swak.data/ndbc");
        item.description = QStringLiteral(
            "<b>Alaska Ocean Observing System</b><br /><b>Location:</b> 59.598N 151.828W\n"
            "<br /><b>Water Temperature:</b> 47.7 &#176;F&nbsp;<br />");
        const auto text = displayDescription(item);
        QVERIFY(!text.contains('<'));
        QVERIFY(!text.contains(QStringLiteral("&#")));
        QVERIFY(text.startsWith(QStringLiteral("Alaska Ocean Observing System\nLocation:")));
        QVERIFY(text.contains(QStringLiteral("47.7 °F")));
        // Plain feed text and local descriptions stay as written.
        item.description = QStringLiteral("Wave height a < b & c");
        QCOMPARE(displayDescription(item), item.description);
        item.owner = QStringLiteral("org.swak.markers");
        item.description = QStringLiteral("<b>typed by the user</b>");
        QCOMPARE(displayDescription(item), item.description);
    }
    void lineDistances()
    {
        const QPolygonF line{QPointF(0, 0), QPointF(10, 0), QPointF(10, 10)};
        QCOMPARE(lineDistance(line, QPointF(5, 3)), 3.0);
        QCOMPARE(lineDistance(line, QPointF(13, 5)), 3.0);
        QCOMPARE(lineDistance(line, QPointF(-4, 3)), 5.0);
        QCOMPARE(lineDistance(QPolygonF{QPointF(2, 2), QPointF(2, 2)}, QPointF(5, 6)), 5.0);
        QVERIFY(std::isinf(lineDistance(QPolygonF{QPointF(1, 1)}, QPointF(1, 1))));
        QVERIFY(std::isinf(lineDistance({}, QPointF(1, 1))));
    }
    void mapDecorationsHaveNoBackground()
    {
        const Viewport view{QSizeF(400, 300), QPointF(.5, .5), 8};
        QImage image(400, 300, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            paintMapDecorations(painter, view, true, true, 0, 19, {});
        }
        // The scale bar and the zoom ladder draw marks without a filled panel.
        const double y = view.size.height() - 18;
        QVERIFY(image.pixelColor(16, qRound(y)).alpha() > 0);
        QCOMPARE(image.pixelColor(11, qRound(y) + 9).alpha(), 0);
        QCOMPARE(image.pixelColor(60, qRound(y) + 9).alpha(), 0);
        const auto zoom = mapZoomLegendRect(view);
        QVERIFY(image.pixelColor(qRound(zoom.left() + 7), qRound(zoom.center().y())).alpha() > 0);
        QCOMPARE(image.pixelColor(qRound(zoom.left() + 32), qRound(zoom.top() + zoom.height() / 4))
                     .alpha(),
                 0);
        QCOMPARE(image.pixelColor(qRound(zoom.left() + 2), qRound(zoom.top() - 8)).alpha(), 0);
    }
    void traceLimits()
    {
        Store store;
        ActivityModel activity;
        MapSources maps(QPluginLoader::staticInstances());
        MapTools tools;
        UserObjects objects;
        PluginServices services{store, activity, maps};
        services.mapTools = &tools;
        services.objects = &objects;
        QMenu main;
        for (auto *plugin : builtInPlugins()) {
            if (auto *map = qobject_cast<MapExtension *>(plugin);
                map && map->mapId() == QStringLiteral("org.swak.trace")) {
                qobject_cast<MenuPlugin *>(plugin)->populateMenu(services, main);
            }
        }
        auto *tool =
            dynamic_cast<MapTool *>(main.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(tool);
        auto *settings = dynamic_cast<SettingsParticipant *>(tool);
        QVERIFY(settings);
        settings->restorePreferences(
            {{QStringLiteral("positions"), 10}, {QStringLiteral("opacity"), 0.35}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), 50);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("opacity")).toDouble(), 0.7);
        auto custom = settings->savePreferences();
        custom.insert(QStringLiteral("positions"), 10);
        custom.insert(QStringLiteral("opacity"), 0.35);
        settings->restorePreferences(custom);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), 10);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("opacity")).toDouble(), 0.35);
        settings->restorePreferences({});
        for (int i = 0; i < 129; ++i) {
            MapItem item;
            item.owner = QStringLiteral("org.swak.markers");
            item.id = QString::number(i);
            QMenu menu;
            tools.populateContextMenu(item, menu);
            auto *action = menu.findChild<QAction *>(QStringLiteral("traceMarker"));
            QVERIFY(action);
            QCOMPARE(action->isEnabled(), i < 128);
            if (action->isEnabled()) {
                action->trigger();
            }
        }
        auto values = settings->savePreferences();
        QCOMPARE(values.value(QStringLiteral("targets")).toArray().size(), 128);
        values.insert(QStringLiteral("positions"), 1000);
        values.insert(QStringLiteral("opacity"), -1);
        settings->restorePreferences(values);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), 50);
        QVERIFY(settings->savePreferences().value(QStringLiteral("opacity")).toDouble() > 0);
        answerAction();
        main.findChild<QAction *>(QStringLiteral("clearTraces"))->trigger();
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 0);
        settings->restorePreferences(values);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(),
                 128);
        MapItem area;
        area.kind = MapItem::Kind::Polygon;
        area.owner = QStringLiteral("org.swak.markers");
        QMenu excluded;
        tools.populateContextMenu(area, excluded);
        QVERIFY(excluded.isEmpty());
        answerAction();
        main.findChild<QAction *>(QStringLiteral("clearTraces"))->trigger();
        UserObject marker;
        marker.id = QStringLiteral("local-trace");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Moving local marker");
        marker.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}};
        QVERIFY(objects.put(marker).isEmpty());
        MapItem point;
        point.owner = QStringLiteral("org.swak.markers");
        point.id = marker.id;
        QMenu local;
        tools.populateContextMenu(point, local);
        local.findChild<QAction *>(QStringLiteral("traceMarker"))->trigger();
        for (int i = 1; i <= 52; ++i) {
            marker.geometry.insert(QStringLiteral("x"), 0.5 + i * 0.000007);
            QVERIFY(objects.put(marker).isEmpty());
        }
        QImage image(1000, 100, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        const Viewport view{QSizeF(1000, 100), QPointF(0.5, 0.5), 12};
        {
            QPainter painter(&image);
            tools.paint(painter, view);
        }
        QVERIFY(image.pixelColor(850, 50).alpha() > 0);
        QVERIFY(image.pixelColor(850, 50).red() > image.pixelColor(850, 50).blue());
        // A click on the trace line reports the traced marker.
        const auto traced = tools.itemsAt(view, QPointF(850, 52));
        QCOMPARE(traced.size(), 1);
        QCOMPARE(traced.first().owner, QStringLiteral("org.swak.markers"));
        QCOMPARE(traced.first().id, marker.id);
        QCOMPARE(traced.first().label, marker.title);
        QVERIFY(tools.itemsAt(view, QPointF(850, 80)).isEmpty());
        // Show traces hides the lines and their hits. Recording continues.
        auto hidden = settings->savePreferences();
        QVERIFY(hidden.value(QStringLiteral("visible")).toBool());
        hidden.insert(QStringLiteral("visible"), false);
        settings->restorePreferences(hidden);
        {
            QImage blank(1000, 100, QImage::Format_ARGB32_Premultiplied);
            blank.fill(Qt::transparent);
            QPainter painter(&blank);
            tools.paint(painter, view);
            painter.end();
            QCOMPARE(blank.pixelColor(850, 50).alpha(), 0);
        }
        QVERIFY(tools.itemsAt(view, QPointF(850, 52)).isEmpty());
        hidden.insert(QStringLiteral("visible"), true);
        settings->restorePreferences(hidden);
        QCOMPARE(tools.itemsAt(view, QPointF(850, 52)).size(), 1);
        QCOMPARE(image.pixelColor(510, 50).alpha(), 0);
        const auto beforePause = image;
        store.motion().setPaused(true);
        marker.geometry.insert(QStringLiteral("y"), 0.50003);
        QVERIFY(objects.put(marker).isEmpty());
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            tools.paint(painter, view);
        }
        QCOMPARE(image, beforePause);
        store.motion().setPaused(false);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            tools.paint(painter, view);
        }
        QVERIFY(image != beforePause);
        objects.remove(marker.id);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            tools.paint(painter, view);
        }
        QCOMPARE(image.pixelColor(850, 50).alpha(), 0);
        marker.geometry.insert(QStringLiteral("y"), 0.5);
        marker.id = QStringLiteral("date-line");
        marker.geometry.insert(QStringLiteral("x"), 0.99999);
        QVERIFY(objects.put(marker).isEmpty());
        point.id = marker.id;
        QMenu crossing;
        tools.populateContextMenu(point, crossing);
        crossing.findChild<QAction *>(QStringLiteral("traceMarker"))->trigger();
        marker.geometry.insert(QStringLiteral("x"), 0.00001);
        QVERIFY(objects.put(marker).isEmpty());
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            tools.paint(painter, Viewport{QSizeF(1000, 100), QPointF(0, 0.5), 12});
        }
        QVERIFY(image.pixelColor(500, 50).alpha() > 0);
        QCOMPARE(image.pixelColor(850, 50).alpha(), 0);
    }
    void markerCollections()
    {
        MarkerCollections collections(QPluginLoader::staticInstances());
        QCOMPARE(collections.collections().size(), 2);
        QVERIFY(MarkerCollections::validate(coreMarkerCollection()).isEmpty());
        QVERIFY(MarkerCollections::validate(militaryMarkerCollection()).isEmpty());
        QVERIFY(collections.symbol({}, {}));
        for (const auto &symbol : markerSymbols()) {
            const auto found = collections.symbol({}, symbol.code);
            QVERIFY(found);
            QCOMPARE(found->frame, markerFrame(symbol.code, {}, 1));
            QCOMPARE(found->glyph, markerGlyph(symbol.code));
        }
        QVERIFY(!collections.remove(CoreMarkerCollection).isEmpty());
        QVERIFY(!collections.put(coreMarkerCollection()).isEmpty());
        QVERIFY(collections.setEnabled(MilitaryMarkerCollection, false).isEmpty());
        QVERIFY(collections.symbol(MilitaryMarkerCollection, QStringLiteral("a-f-G-U")));
        QVERIFY(!collections.setEnabled(QStringLiteral("missing"), true).isEmpty());
        auto custom = coreMarkerCollection();
        custom.insert(QStringLiteral("id"), QStringLiteral("test.collection"));
        custom.insert(QStringLiteral("name"), QStringLiteral("Custom"));
        QVERIFY(collections.put(custom).isEmpty());
        auto saved = collections.savePreferences();
        MarkerCollections absent({});
        absent.restorePreferences(saved);
        QCOMPARE(absent.collections(), collections.collections());
        QVERIFY(absent.symbol(CoreMarkerCollection, {}));
        auto future = custom;
        future.insert(QStringLiteral("schemaVersion"), 2);
        auto entries = saved.value(QStringLiteral("collections")).toObject();
        entries.insert(QStringLiteral("test.collection"), future);
        saved.insert(QStringLiteral("collections"), entries);
        absent.restorePreferences(saved);
        QCOMPARE(absent.collections().value(QStringLiteral("test.collection")).toObject(), future);
        QVERIFY(!absent.symbol(QStringLiteral("test.collection"), {}));
        QVERIFY(!absent.setEnabled(QStringLiteral("test.collection"), true).isEmpty());
        QVERIFY(collections.remove(QStringLiteral("test.collection")).isEmpty());
        for (int i = 0; i < 14; ++i) {
            custom.insert(QStringLiteral("id"), QStringLiteral("custom.%1").arg(i));
            QVERIFY(collections.put(custom).isEmpty());
        }
        custom.insert(QStringLiteral("id"), QStringLiteral("overflow"));
        QVERIFY(!collections.put(custom).isEmpty());
        const auto before = collections.savePreferences();
        entries.insert(QStringLiteral("invalid/key"), custom);
        collections.restorePreferences({{QStringLiteral("collections"), entries}});
        QCOMPARE(collections.savePreferences(), before);
        UserObject record{QStringLiteral("test"),
                          QStringLiteral("circle"),
                          QStringLiteral("Custom marker"),
                          {},
                          {},
                          {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        record.collection = QStringLiteral("test.collection");
        QVERIFY(UserObjects::validate(record).isEmpty());
        QCOMPARE(UserObject::fromJson(record.json()).collection, record.collection);
        record.collection = QString(65, 'x');
        QVERIFY(!UserObjects::validate(record).isEmpty());
    }
    void markerCollectionVectors()
    {
        const auto original = coreMarkerCollection();
        for (const auto &path :
             QList<QJsonArray>{QJsonArray{},
                               QJsonArray{QJsonArray{}},
                               QJsonArray{QJsonArray{QStringLiteral("Z")}},
                               QJsonArray{QJsonArray{QStringLiteral("L"), 0, 0}},
                               QJsonArray{QJsonArray{QStringLiteral("M"), 5, 0}},
                               QJsonArray{QJsonArray{QStringLiteral("M"), 0, 0},
                                          QJsonArray{QStringLiteral("C"), 1}},
                               QJsonArray{QJsonArray{QStringLiteral("M"), 0, QJsonValue{}}}}) {
            auto value = original;
            auto symbols = value.value(QStringLiteral("symbols")).toArray();
            auto symbol = symbols[0].toObject();
            symbol.insert(QStringLiteral("frame"), path);
            symbols[0] = symbol;
            value.insert(QStringLiteral("symbols"), symbols);
            QVERIFY(!MarkerCollections::validate(value).isEmpty());
        }
        auto empty = original;
        empty.insert(QStringLiteral("symbols"), QJsonArray{});
        QVERIFY(!MarkerCollections::validate(empty).isEmpty());
        auto oversized = original;
        oversized.insert(QStringLiteral("extra"), QString(256 * 1024, 'x'));
        QVERIFY(!MarkerCollections::validate(oversized).isEmpty());
        auto value = original;
        auto symbols = value.value(QStringLiteral("symbols")).toArray();
        symbols.append(symbols[0]);
        value.insert(QStringLiteral("symbols"), symbols);
        QVERIFY(!MarkerCollections::validate(value).isEmpty());
        value = original;
        value.insert(QStringLiteral("name"), QStringLiteral("bad\nname"));
        QVERIFY(!MarkerCollections::validate(value).isEmpty());
        value = original;
        value.insert(QStringLiteral("enabled"), QStringLiteral("yes"));
        QVERIFY(!MarkerCollections::validate(value).isEmpty());
        value = original;
        value.insert(QStringLiteral("schemaVersion"), 2);
        QVERIFY(!MarkerCollections::validate(value).isEmpty());
    }
    void markerCollectionArchives()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        MarkerCollections source(QPluginLoader::staticInstances());
        auto custom = militaryMarkerCollection();
        custom.insert(QStringLiteral("id"), QStringLiteral("archive.test"));
        QVERIFY(source.put(custom).isEmpty());
        for (const auto &suffix : {QStringLiteral(".zip"), QStringLiteral(".tar.gz")}) {
            const auto path = directory.filePath(QStringLiteral("markers") + suffix);
            QVERIFY(source.exportArchive(QStringLiteral("archive.test"), path).isEmpty());
            MarkerCollections imported({});
            QVERIFY(imported.importArchive(path).isEmpty());
            QCOMPARE(imported.collections().value(QStringLiteral("archive.test")).toObject(),
                     custom);
            QVERIFY(!imported.importArchive(path).isEmpty());
            QCOMPARE(imported.collections().size(), 1);
            QVERIFY(imported.remove(QStringLiteral("archive.test")).isEmpty());
            QVERIFY(imported.importArchive(path).isEmpty());
        }
        QVERIFY(
            !source.exportArchive(QStringLiteral("archive.test"), QString(QChar::Null)).isEmpty());
        QVERIFY(!source
                     .exportArchive(QStringLiteral("missing"),
                                    directory.filePath(QStringLiteral("missing.zip")))
                     .isEmpty());
        QVERIFY(!source.exportArchive(QStringLiteral("archive.test"), directory.path()).isEmpty());
        QVERIFY(!source.importArchive(directory.path()).isEmpty());
        const auto zip =
            [&](const QString &member, const QByteArray &contents, bool extra, bool link) {
                QByteArray data(1024 * 1024, '\0');
                size_t used = 0;
                std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(),
                                                                               archive_write_free);
                archive_write_set_format_zip(writer.get());
                archive_write_open_memory(
                    writer.get(), data.data(), static_cast<size_t>(data.size()), &used);
                std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(
                    archive_entry_new(), archive_entry_free);
                archive_entry_set_pathname(entry.get(), member.toUtf8().constData());
                archive_entry_set_filetype(entry.get(), link ? AE_IFLNK : AE_IFREG);
                if (link) {
                    archive_entry_set_symlink(entry.get(), "outside");
                }
                archive_entry_set_perm(entry.get(), 0600);
                archive_entry_set_size(entry.get(), link ? 0 : contents.size());
                archive_write_header(writer.get(), entry.get());
                if (!link) {
                    archive_write_data(
                        writer.get(), contents.constData(), static_cast<size_t>(contents.size()));
                }
                if (extra) {
                    archive_entry_set_pathname(entry.get(), "extra.txt");
                    archive_entry_set_size(entry.get(), 0);
                    archive_write_header(writer.get(), entry.get());
                }
                archive_write_close(writer.get());
                data.resize(static_cast<qsizetype>(used));
                return data;
            };
        const auto json = QJsonDocument(custom).toJson();
        const auto valid = zip(QStringLiteral("collection.json"), json, false, false);
        const QList<QByteArray> invalid{
            QByteArrayLiteral("not an archive"),
            valid.left(valid.size() / 2),
            zip(QStringLiteral("../collection.json"), json, false, false),
            zip(QStringLiteral("/collection.json"), json, false, false),
            zip(QStringLiteral("collection.json"), json, true, false),
            zip(QStringLiteral("collection.json"), json, false, true),
            zip(QStringLiteral("collection.json"), QByteArray(256 * 1024 + 1, ' '), false, false),
            zip(QStringLiteral("collection.json"), QByteArrayLiteral("[]"), false, false),
            zip(QStringLiteral("collection.json"), QByteArrayLiteral("{"), false, false)};
        MarkerCollections target({});
        for (const auto &bytes : invalid) {
            const auto path = directory.filePath(QStringLiteral("invalid.zip"));
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), bytes.size());
            file.close();
            QVERIFY2(!target.importArchive(path).isEmpty(),
                     qPrintable(QString::number(bytes.size())));
            QVERIFY(target.collections().isEmpty());
        }
        const auto path = directory.filePath(QStringLiteral("recovery.zip"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(valid), valid.size());
        file.close();
        QVERIFY(target.importArchive(path).isEmpty());
        QVERIFY(target.symbol(QStringLiteral("archive.test"), QStringLiteral("a-h-G-U-C-I")));
    }
    void vectorMarkerSymbols()
    {
        QCOMPARE(markerSymbols().size(), 16);
        QImage sheet(320, 100, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::transparent);
        QPainter painter(&sheet);
        painter.setRenderHint(QPainter::Antialiasing);
        int column = 0;
        for (const auto &symbol : markerSymbols()) {
            QVERIFY(validMarkerSymbol(symbol.code));
            QCOMPARE(markerSymbolForCot(symbol.code), symbol.code);
            const auto path = markerFrame(symbol.code, QPointF{}, 7);
            QVERIFY(path.boundingRect().width() <= 14.01);
            QVERIFY(path.boundingRect().height() <= 14.01);
            QVERIFY(path.contains(QPointF{}));
            paintMarkerSymbol(
                painter, symbol.code, QPointF(10 + 20 * column++, 20), 7, symbol.color);
            UserObject item{QStringLiteral("test"),
                            QStringLiteral("circle"),
                            QStringLiteral("Example"),
                            {},
                            {},
                            {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
            item.symbol = symbol.code;
            QVERIFY(UserObjects::validate(item).isEmpty());
            QCOMPARE(UserObject::fromJson(item.json()).symbol, symbol.code);
            UserObjects objects;
            QVERIFY(objects.put(item).isEmpty());
            objects.remove(item.id);
            objects.undoStack().undo();
            QCOMPARE(objects.find(item.id)->symbol, symbol.code);
        }
        QVERIFY(markerSymbolForCot(QStringLiteral("a-f-A")).isEmpty());
        QVERIFY(markerSymbolForCot(QStringLiteral("a-f-G-U-C-I-custom")).isEmpty());
        QCOMPARE(markerFrame(QStringLiteral("future"), {}, 7), markerFrame({}, {}, 7));
        paintMarkerSymbol(painter, QStringLiteral("future"), QPointF(10, 50), 7, Qt::yellow, true);
        painter.end();
        QVERIFY(sheet.save(QStringLiteral("marker-symbols.png")));
        UserObject item{QStringLiteral("future"),
                        QStringLiteral("circle"),
                        QStringLiteral("Future"),
                        {},
                        {},
                        {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        item.symbol = QStringLiteral("future");
        QVERIFY(UserObjects::validate(item).isEmpty());
        item.symbol = QString(65, 'a');
        QVERIFY(!UserObjects::validate(item).isEmpty());
        auto malformed = item.json();
        malformed.insert(QStringLiteral("symbol"), 42);
        QVERIFY(!UserObjects::validate(UserObject::fromJson(malformed)).isEmpty());
    }

    void cotGroundUnitSymbols()
    {
        MapLayer *tracks = nullptr;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            auto *layer = qobject_cast<MapLayer *>(plugin);
            if (layer && layer->order() == 100) {
                tracks = layer;
            }
        }
        QVERIFY(tracks);
        for (const auto &symbol : markerSymbols()) {
            Store store;
            auto event = sample();
            event.type = symbol.code;
            event.latitude = 0;
            event.longitude = 0;
            QVERIFY(store.accept(event).isEmpty());
            for (const bool selected : {false, true}) {
                if (selected) {
                    store.select(event.uid);
                }
                QImage actual(64, 64, QImage::Format_ARGB32_Premultiplied);
                actual.fill(Qt::transparent);
                {
                    QPainter painter(&actual);
                    tracks->paint(painter, {QSizeF(64, 64), QPointF(0.5, 0.5), 3}, store);
                }
                QImage expected(64, 64, QImage::Format_ARGB32_Premultiplied);
                expected.fill(Qt::transparent);
                {
                    QPainter painter(&expected);
                    paintMarkerSymbol(painter,
                                      symbol.code,
                                      QPointF(32, 32),
                                      selected ? 8 : 6,
                                      symbol.color,
                                      selected);
                }
                QCOMPARE(actual.copy(22, 22, 20, 20), expected.copy(22, 22, 20, 20));
            }
        }
    }

    void markerColorStorage()
    {
        UserObject item{QStringLiteral("color"),
                        QStringLiteral("circle"),
                        QStringLiteral("Marker"),
                        {},
                        {},
                        {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        QCOMPARE(UserObject::fromJson(item.json()).color, QStringLiteral("#ffc857"));
        auto legacy = item.json();
        legacy.remove(QStringLiteral("color"));
        QCOMPARE(UserObject::fromJson(legacy).color, QStringLiteral("#ffc857"));
        item.color = QStringLiteral("#1e88e5");
        QVERIFY(UserObjects::validate(item).isEmpty());
        QCOMPARE(UserObject::fromJson(item.json()).color, item.color);
        item.color = QStringLiteral("not-a-color");
        QVERIFY(!UserObjects::validate(item).isEmpty());
    }
    void coordinateGrid()
    {
        QImage image(900, 600, QImage::Format_ARGB32_Premultiplied);
        for (double zoom : {2.0, 8.0, 19.0}) {
            for (double latitude : {-80.0, 0.0, 80.0}) {
                image.fill(Qt::transparent);
                QPainter painter(&image);
                Viewport view;
                view.size = image.size();
                view.zoom = zoom;
                view.center = project(latitude, 18);
                paintMapGrid(painter, view, 0.2, 140);
                painter.end();
                int top = 0, bottom = 0, left = 0, right = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const auto pixel = image.pixelColor(x, y);
                        if (!pixel.alpha()) {
                            continue;
                        }
                        QCOMPARE(pixel.red(), 0);
                        QCOMPARE(pixel.green(), 0);
                        QCOMPARE(pixel.blue(), 0);
                        if (y < 30) {
                            ++top;
                        }
                        if (y >= image.height() - 30) {
                            ++bottom;
                        }
                        if (x < 90) {
                            ++left;
                        }
                        if (x >= image.width() - 90) {
                            ++right;
                        }
                    }
                }
                QVERIFY(top > 60);
                QVERIFY(bottom > 60);
                QVERIFY(left > 60);
                QVERIFY(right > 60);
            }
        }
        image.fill(Qt::transparent);
        QPainter painter(&image);
        Viewport invalid;
        invalid.size = image.size();
        invalid.zoom = std::numeric_limits<double>::quiet_NaN();
        paintMapGrid(painter, invalid, 0.2, 140);
        painter.end();
        QCOMPARE(image.pixelColor(450, 300).alpha(), 0);
    }
    void measurements()
    {
        const auto origin = project(0, 0), east = project(0, 1);
        QVERIFY(std::abs(groundDistance(origin, east) - 111195.08) < 0.1);
        QCOMPARE(groundDistance(origin, origin), 0.0);
        QVERIFY(groundDistance(project(0, 179.9), project(0, -179.9)) < 23000);
        const QLocale metric(QLocale::English, QLocale::Sweden),
            imperial(QLocale::English, QLocale::UnitedStates);
        QVERIFY(formatDistance(999, metric).endsWith(QStringLiteral(" m")));
        QVERIFY(formatDistance(1000, metric).endsWith(QStringLiteral(" km")));
        QVERIFY(formatDistance(1, imperial).endsWith(QStringLiteral(" ft")));
        QVERIFY(formatDistance(1609.344, imperial).endsWith(QStringLiteral(" mi")));
        QVERIFY(formatDistance(-1).isEmpty());
        QVERIFY(formatDistance(std::numeric_limits<double>::infinity()).isEmpty());
        const QPolygonF points{origin, origin + QPointF(0.00001, 0)};
        const auto circle = circleBoundary(points[0], points[1]);
        QCOMPARE(circle.size(), 128);
        const double radius = groundDistance(points[0], points[1]);
        QVERIFY(std::abs(polygonArea(circle) / (3.141592653589793 * radius * radius) - 1) < 0.001);
        QVERIFY(validateTwoPointShape(QStringLiteral("range-circle"), polygonGeometry(points))
                    .isEmpty());
        QVERIFY(!validateTwoPointShape(QStringLiteral("line"), {}).isEmpty());
        QVERIFY(!validateTwoPointShape(QStringLiteral("line"), polygonGeometry({origin, origin}))
                     .isEmpty());
        QVERIFY(!validateTwoPointShape(QStringLiteral("range-circle"),
                                       polygonGeometry({QPointF(0.01, 0.5), QPointF(0.1, 0.5)}))
                     .isEmpty());
        UserObject object;
        object.id = QStringLiteral("line");
        object.type = QStringLiteral("line");
        object.title = QStringLiteral("Line");
        object.geometry = polygonGeometry(points);
        QVERIFY(UserObjects::validate(object).isEmpty());
        object.properties.insert(QStringLiteral("Distance"), QStringLiteral("bad"));
        QVERIFY(!UserObjects::validate(object).isEmpty());
    }
    void routes()
    {
        const QPolygonF line{project(0, 0), project(0, 1), project(1, 1)};
        QVERIFY(validateRoute(line).isEmpty());
        QVERIFY(!validateRoute({project(0, 0)}).isEmpty());
        QVERIFY(!validateRoute({project(0, 0), project(0, 0)}).isEmpty());
        QVERIFY(!validateRoute({QPointF(0.5, 0.5), QPointF(1.5, 0.5)}).isEmpty());
        QVERIFY(!validateRoute({QPointF(0.5, 0.5), QPointF(qQNaN(), 0.5)}).isEmpty());
        // Two one-degree legs near the equator are about 111 km each.
        QVERIFY(std::abs(routeLength(line) - 2 * 111195) < 400);
        // A segment across the date line takes the short way.
        const QPolygonF across{project(0, 179.5), project(0, -179.5)};
        QVERIFY(std::abs(routeLength(across) - 111195) < 400);
        QVERIFY(std::abs(unwrappedRoute(across).last().x() - unwrappedRoute(across).first().x()) <
                0.01);
        QCOMPARE(routePoints(polygonGeometry(line)), line);
        QPolygonF dense;
        for (int i = 0; i <= 2000; ++i) {
            const double longitude = i * 0.001;
            dense.append(project(std::sin(i / 100.0), longitude));
        }
        const auto simple = simplifiedRoute(dense);
        QVERIFY(simple.size() <= MaxRoutePoints);
        QVERIFY(simple.size() >= 2);
        QCOMPARE(simple.first(), dense.first());
        QCOMPARE(simple.last(), dense.last());
        QVERIFY(validateRoute(simple).isEmpty());
        QVERIFY(std::abs(routeLength(simple) / routeLength(dense) - 1) < 0.01);
        QCOMPARE(simplifiedRoute(line), line);
        UserObjects objects;
        UserObject route;
        route.id = QStringLiteral("route-test");
        route.type = QStringLiteral("route");
        route.title = QStringLiteral("Patrol");
        route.geometry = polygonGeometry(simple);
        // A route with the full point limit fits in one object.
        QPolygonF full;
        for (int i = 0; i < MaxRoutePoints; ++i) {
            full.append(QPointF(0.123456789 + i * 1e-5, 0.987654321 - i * 1e-5));
        }
        route.geometry = polygonGeometry(full);
        QVERIFY2(objects.put(route).isEmpty(), qPrintable(objects.put(route)));
        full.append(QPointF(0.2, 0.2));
        route.geometry = polygonGeometry(full);
        QVERIFY(!objects.put(route).isEmpty());
        route.geometry = polygonGeometry(line);
        route.properties.insert(QStringLiteral("Length"), QStringLiteral("1 km"));
        QVERIFY(!objects.put(route).isEmpty());
    }
    void polygons()
    {
        const QPolygonF square{project(0, 0), project(0, 1), project(1, 1), project(1, 0)};
        QVERIFY(validatePolygon(square).isEmpty());
        QCOMPARE(polygonVertices(polygonGeometry(square)), square);
        const double expected = 12363718145.18;
        QVERIFY(std::abs(polygonArea(square) / expected - 1) < 0.00001);
        auto reverse = square;
        std::reverse(reverse.begin(), reverse.end());
        QVERIFY(std::abs(polygonArea(reverse) - polygonArea(square)) < 0.1);
        QVERIFY(square.containsPoint(polygonLabelPoint(square), Qt::OddEvenFill));
        const QPolygonF concave{{0.1, 0.1},
                                {0.9, 0.1},
                                {0.9, 0.9},
                                {0.7, 0.9},
                                {0.7, 0.3},
                                {0.3, 0.3},
                                {0.3, 0.9},
                                {0.1, 0.9}};
        QVERIFY(validatePolygon(concave).isEmpty());
        QVERIFY(concave.containsPoint(polygonLabelPoint(concave), Qt::OddEvenFill));
        QVERIFY(!validatePolygon({}).isEmpty());
        QCOMPARE(polygonArea({}), 0);
        QCOMPARE(polygonLabelPoint({}), QPointF{});
        QVERIFY(!validatePolygon(QPolygonF{{0, 0}, {1e-13, 0}, {0, 1e-13}}).isEmpty());
        QVERIFY(!validatePolygon(QPolygonF{{0, 0}, {1, 1}, {0, 1}, {1, 0}}).isEmpty());
        QVERIFY(!validatePolygon(QPolygonF{{0, 0}, {0.5, 0}, {1, 0}}).isEmpty());
        auto bad = square;
        bad.append(square.first());
        QVERIFY(!validatePolygon(bad).isEmpty());
        bad = square;
        bad[0] = {-1, 0};
        QVERIFY(!validatePolygon(bad).isEmpty());
        bad[0] = {std::numeric_limits<double>::infinity(), 0};
        QVERIFY(!validatePolygon(bad).isEmpty());
        bad = QPolygonF{{0, 0}, {1, 0}, {0.5, 0}, {0.5, 1}};
        QVERIFY(!validatePolygon(bad).isEmpty());
        QPolygonF limit;
        for (int i = 0; i < MaxPolygonVertices; ++i) {
            const double angle = i * 6.283185307179586 / MaxPolygonVertices;
            limit.append({0.5 + 0.1 * std::cos(angle), 0.5 + 0.1 * std::sin(angle)});
        }
        QVERIFY(validatePolygon(limit).isEmpty());
        limit.append({0.5, 0.5});
        QVERIFY(!validatePolygon(limit).isEmpty());
        QVERIFY(polygonVertices(polygonGeometry(limit)).isEmpty());
        QVERIFY(polygonVertices({{QStringLiteral("vertices"),
                                  QJsonArray{QJsonArray{QStringLiteral("bad"), 0}}}})
                    .isEmpty());
        QCOMPARE(formatArea(999, QLocale(QLocale::Swedish, QLocale::Sweden)),
                 QString::fromUtf8("999,00 m²"));
        QCOMPARE(formatArea(1000, QLocale(QLocale::Swedish, QLocale::Sweden)),
                 QString::fromUtf8("0,0010 km²"));
        QVERIFY(formatArea(10, QLocale(QLocale::English, QLocale::UnitedStates))
                    .endsWith(QStringLiteral("ft²")));
        QVERIFY(formatArea(10000, QLocale(QLocale::English, QLocale::UnitedStates))
                    .endsWith(QStringLiteral("acres")));
        QVERIFY(formatArea(10000000, QLocale(QLocale::English, QLocale::UnitedStates))
                    .endsWith(QStringLiteral("mi²")));
        QVERIFY(formatArea(-1).isEmpty());
        QVERIFY(formatArea(std::numeric_limits<double>::infinity()).isEmpty());
        UserObjects objects;
        UserObject item;
        item.id = QStringLiteral("area");
        item.type = QStringLiteral("polygon");
        item.title = QStringLiteral("Area");
        item.geometry = polygonGeometry(square);
        QVERIFY(objects.put(item).isEmpty());
        const auto saved = objects.savePreferences();
        objects.remove(item.id);
        objects.undoStack().undo();
        QCOMPARE(objects.find(item.id)->geometry, item.geometry);
        UserObjects restored;
        restored.restorePreferences(saved);
        QCOMPARE(restored.find(item.id)->json(), item.json());
        item.properties.insert(QStringLiteral("Area"), QStringLiteral("fake"));
        QVERIFY(!objects.put(item).isEmpty());
        item.properties = {};
        item.geometry = polygonGeometry(bad);
        QVERIFY(!objects.put(item).isEmpty());
    }

    void userObjectsAndUndo()
    {
        UserObjects objects;
        UserObject item{QStringLiteral("test"),
                        QStringLiteral("circle"),
                        QStringLiteral("Marker"),
                        QStringLiteral("Description"),
                        {{QStringLiteral("unit"), QStringLiteral("Alpha")}},
                        {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}}};
        QVERIFY(objects.put(item).isEmpty());
        objects.remove(item.id);
        QVERIFY(objects.items().isEmpty());
        QVERIFY(!objects.put(item).isEmpty());
        objects.undoStack().undo();
        QCOMPARE(objects.find(item.id)->json(), item.json());
        objects.undoStack().redo();
        QVERIFY(objects.items().isEmpty());
        objects.undoStack().undo();
        auto invalid = item;
        invalid.geometry.insert(QStringLiteral("x"), 2);
        QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item;
        invalid.type = QStringLiteral("other");
        QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item;
        invalid.title = QString(129, 'x');
        QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item;
        invalid.properties.insert(QStringLiteral("Unit"), QString(257, 'x'));
        QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item;
        invalid.properties.insert(QStringLiteral("ID"), QStringLiteral("reserved"));
        QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item;
        QJsonObject nested{{QStringLiteral("leaf"), true}};
        for (int i = 0; i < 10; ++i) {
            nested = {{QStringLiteral("child"), nested}};
        }
        invalid.geometry = nested;
        QVERIFY(!objects.put(invalid).isEmpty());
        invalid = item;
        invalid.geometry.insert(QStringLiteral("path"), QJsonArray{1, 2, 3});
        QVERIFY(objects.put(invalid).isEmpty());
        QVERIFY(objects.put(item).isEmpty());
        const auto saved = objects.savePreferences();
        UserObjects restored;
        restored.restorePreferences(saved);
        QCOMPARE(restored.find(item.id)->json(), item.json());
        for (int i = 0; i < 300; ++i) {
            auto next = item;
            next.id = QString::number(i);
            objects.put(next);
        }
        QVERIFY(objects.items().size() <= 256);
        objects.remove(item.id);
        auto next = item;
        next.id = QStringLiteral("overflow");
        QVERIFY(!objects.put(next).isEmpty());
        objects.undoStack().undo();
        QVERIFY(objects.find(item.id));
        for (const auto &id : objects.items().keys()) {
            objects.remove(id);
        }
        QVERIFY(objects.undoStack().count() <= 100);
        objects.restorePreferences(saved);
        QVERIFY(!objects.undoStack().canUndo());
    }

    void pluginManifests()
    {
        QCOMPARE(builtInPlugins().size(), 33);
        for (const auto &entry : QPluginLoader::staticPlugins()) {
            if (entry.metaData().value(QStringLiteral("IID")).toString() !=
                QStringLiteral("org.swak.Plugin/2.0")) {
                continue;
            }
            const auto manifest = entry.metaData().value(QStringLiteral("MetaData")).toObject();
            QVERIFY2(validatePluginManifest(manifest, entry.instance()).isEmpty(),
                     qPrintable(validatePluginManifest(manifest, entry.instance())));
            for (const auto &key : {QStringLiteral("schemaVersion"),
                                    QStringLiteral("apiVersion"),
                                    QStringLiteral("id"),
                                    QStringLiteral("version"),
                                    QStringLiteral("name"),
                                    QStringLiteral("description"),
                                    QStringLiteral("entryPoint"),
                                    QStringLiteral("linkage"),
                                    QStringLiteral("platforms"),
                                    QStringLiteral("qtMajor"),
                                    QStringLiteral("qtMinimum"),
                                    QStringLiteral("interfaces"),
                                    QStringLiteral("requires"),
                                    QStringLiteral("permissions"),
                                    QStringLiteral("settingsSchemaVersion")}) {
                auto invalid = manifest;
                invalid.remove(key);
                QVERIFY2(!validatePluginManifest(invalid, entry.instance()).isEmpty(),
                         qPrintable(key));
            }
            for (const auto &key :
                 {QStringLiteral("id"), QStringLiteral("version"), QStringLiteral("qtMinimum")}) {
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
            if (auto *codec = qobject_cast<Codec *>(plugin)) {
                codec_ = codec;
            }
            if (auto *source = qobject_cast<SourcePlugin *>(plugin)) {
                source_ = source;
            }
            if (qobject_cast<MapLayer *>(plugin)) {
                ++layers;
            }
        }
        QVERIFY(codec_);
        QVERIFY(source_);
        QCOMPARE(layers, 5);
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
        QTest::newRow("doctype") << QByteArray(valid).replace(
            "?><event", "?><!DOCTYPE event [<!ENTITY e 'bad'>]><event");
        QTest::newRow("entity") << QByteArray(valid).replace("test-1", "&missing;");
        QTest::newRow("duplicate") << valid + valid;
        QTest::newRow("truncated") << valid.left(valid.size() - 20);
        QTest::newRow("version") << QByteArray(valid).replace("version=\"2.0\"", "version=\"3.0\"");
        QTest::newRow("nan") << QByteArray(valid).replace("59.32930000", "nan");
        QTest::newRow("range") << QByteArray(valid).replace("59.32930000", "90.00000001");
        QTest::newRow("numeric-overflow")
            << QByteArray(valid).replace("59.32930000", QByteArray(350, '9'));
        QTest::newRow("exponent") << QByteArray(valid).replace("59.32930000", "1e1");
        QTest::newRow("negative-error")
            << QByteArray(valid).replace("ce=\"9999999.000\"", "ce=\"-1\"");
        QTest::newRow("duplicate-point") << QByteArray(valid).replace(
            "<detail>", "<point lat=\"0\" lon=\"0\" hae=\"0\" ce=\"1\" le=\"1\"/><detail>");
        QTest::newRow("namespace")
            << QByteArray(valid).replace("<event ", "<event xmlns=\"urn:wrong\" ");
        QTest::newRow("depth") << QByteArray(valid)
                                      .replace("<detail>",
                                               "<detail>" + QByteArray("<a>").repeated(20))
                                      .replace("</detail>",
                                               QByteArray("</a>").repeated(20) + "</detail>");
        QTest::newRow("elements") << QByteArray(valid).replace(
            "<detail>", "<detail>" + QByteArray("<a/>").repeated(2050));
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
        for (qsizetype length = 0; length < valid.size() - 2; ++length) {
            QVERIFY(!codec_->decode(valid.left(length)).event);
        }
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
        QAbstractItemModelTester tester(&store,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
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
        for (const auto point :
             {QPointF(0, 0), QPointF(180, 85), QPointF(-180, -85), QPointF(18.0686, 59.3293)}) {
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
        for (int i = 0; i < 33; ++i) {
            declarations +=
                " xmlns:n" + QByteArray::number(i) + "=\"urn:" + QByteArray::number(i) + "\"";
        }
        xml.replace("<event ", "<event" + declarations + " ");
        QVERIFY(!codec_->decode(xml).event.has_value());
    }
    void receiverSummaryRecovery()
    {
        Store store;
        Servers servers(store);
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        const auto port = sender.localPort();
        sender.close();
        auto source = serverDefaults();
        source.insert(QStringLiteral("name"), QStringLiteral("Local"));
        source.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        source.insert(QStringLiteral("port"), port);
        source.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers.put(QStringLiteral("test"), source).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("test")).contains(QStringLiteral("Listening"));
            },
            5000));
        sender.writeDatagram("bad", QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (servers.statistics()
                            .value(QStringLiteral("test"))
                            .toObject()
                            .value(QStringLiteral("rejected"))
                            .toInt()) == (1);
            },
            5000));
        sender.writeDatagram(codec_->encode(sample()), QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
    }
    void receiverLifecycle()
    {
        Store store;
        QUdpSocket reservation;
        QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        const auto port = reservation.localPort();
        auto servers = std::make_unique<Servers>(store);
        auto source = serverDefaults();
        source.insert(QStringLiteral("name"), QStringLiteral("Local"));
        source.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        source.insert(QStringLiteral("port"), port);
        source.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers->put(QStringLiteral("test"), source).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers->status(QStringLiteral("test"))
                    .contains(QStringLiteral("Cannot bind"));
            },
            5000));
        reservation.close();
        servers->reconnect();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers->status(QStringLiteral("test"))
                    .contains(QStringLiteral("Listening"));
            },
            5000));
        QUdpSocket sender;
        sender.writeDatagram(codec_->encode(sample()), QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        sender.writeDatagram(QByteArray(MaxMessageBytes + 1, 'x'), QHostAddress::LocalHost, port);
        auto next = sample();
        next.uid = QStringLiteral("test-2");
        sender.writeDatagram(codec_->encode(next), QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (2);
            },
            5000));
        servers.reset();
        QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
    }
};
QTEST_MAIN(CoreTest)
#include "test_core.moc"
