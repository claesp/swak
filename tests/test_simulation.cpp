#include "core/cot.h"
#include "core/servers.h"
#include "core/store.h"
#include "cot_simulation.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>

#include <cmath>
#include <limits>
using namespace swak;
namespace {
QString executable()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/swak_cot_emulator");
}
class Process final : public QProcess
{
public:
    ~Process() override
    {
        if (state() != NotRunning) {
            terminate();
            if (!waitForFinished(2000)) {
                kill();
                waitForFinished(2000);
            }
        }
    }
};
} // namespace
class SimulationTest final : public QObject
{
    Q_OBJECT
private slots:
    void routes()
    {
        const auto scenario = simulation::load({}, QStringLiteral("test"));
        QVERIFY2(scenario.error.isEmpty(), qPrintable(scenario.error));
        QCOMPARE(scenario.units.size(), 22);
        const auto now = QDateTime::currentDateTimeUtc();
        QSet<QString> ids, types, teams;
        for (const auto &unit : scenario.units) {
            const auto a = simulation::sample(scenario, unit, 0, now);
            const auto b = simulation::sample(scenario, unit, 1, now.addSecs(1));
            QVERIFY(eventError(a).isEmpty());
            const auto decoded = decodeCot(encodeCot(b));
            QVERIFY(decoded.event);
            QCOMPARE(decoded.event->uid, a.uid);
            QCOMPARE(decoded.event->team, a.team);
            QCOMPARE(b.time.secsTo(b.stale), 15);
            QCOMPARE(b.how, QStringLiteral("m-s"));
            QVERIFY(a.latitude != b.latitude || a.longitude != b.longitude);
            const auto dx = (b.longitude - a.longitude) * 111195 *
                            std::cos(a.latitude * 3.141592653589793 / 180);
            const auto dy = (b.latitude - a.latitude) * 111195;
            QVERIFY(std::abs(std::hypot(dx, dy) - unit.speed) < 0.3);
            ids.insert(a.uid);
            types.insert(a.type);
            teams.insert(a.team);
        }
        QCOMPARE(ids.size(), 22);
        QCOMPARE(types.size(), 8);
        QCOMPARE(teams.size(), 2);
        const auto again = simulation::load({}, QStringLiteral("test"));
        QCOMPARE(simulation::sample(scenario, scenario.units.first(), 123, now).latitude,
                 simulation::sample(again, again.units.first(), 123, now).latitude);
        QVERIFY(simulation::sample(
                    scenario, scenario.units.first(), std::numeric_limits<double>::infinity(), now)
                    .uid.isEmpty());
        for (double latitude : {-80.0, 80.0}) {
            for (double longitude : {-180.0, 180.0}) {
                const auto edge = simulation::load(
                    {{"latitude", latitude}, {"longitude", longitude}}, QStringLiteral("edge"));
                for (const auto &unit : edge.units) {
                    QVERIFY(eventError(simulation::sample(edge, unit, 86400, now)).isEmpty());
                }
            }
        }
    }
    void invalidConfiguration()
    {
        const QList<QJsonObject> bad{{{"latitude", 81}},
                                     {{"longitude", -181}},
                                     {{"frequency", 0}},
                                     {{"frequency", 21}},
                                     {{"lifetime", 1}},
                                     {{"radius", 0}},
                                     {{"seed", -1}},
                                     {{"seed", 1.5}},
                                     {{"teams", QJsonArray{}}},
                                     {{"types", QJsonArray{}}},
                                     {{"types", "bad"}},
                                     {{"frequency", "nan"}},
                                     {{"unknown", 1}}};
        for (const auto &value : bad) {
            QVERIFY2(!simulation::load(value, QStringLiteral("test")).error.isEmpty(),
                     QJsonDocument(value).toJson().constData());
        }
        for (const auto &key : {"count", "speed", "altitude"}) {
            auto types = simulation::defaults().value("types").toArray();
            auto type = types[0].toObject();
            type.insert(QLatin1String(key), 100000);
            types[0] = type;
            QVERIFY(!simulation::load({{"types", types}}, QStringLiteral("test")).error.isEmpty());
        }
        auto types = simulation::defaults().value("types").toArray();
        for (qsizetype i = 0; i < types.size(); ++i) {
            auto type = types[i].toObject();
            type.insert("count", 512);
            types[i] = type;
        }
        QVERIFY(!simulation::load({{"types", types}}, QStringLiteral("test")).error.isEmpty());
        QVERIFY(!simulation::load({}, QString(200, 'x')).error.isEmpty());
    }
    void tcpReception()
    {
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const auto port = reserve.serverPort();
        reserve.close();
        Process process;
        process.start(executable(),
                      {"--port", QString::number(port), "--frequency", "10", "--duration", "10"});
        QVERIFY(process.waitForStarted());
        Store store;
        Servers servers(store);
        auto source = serverDefaults();
        source.insert("name", "Emulator");
        source.insert("host", "127.0.0.1");
        source.insert("port", port);
        source.insert("protocol", "stcp");
        source.insert("enabled", true);
        source.insert("retrySeconds", 1);
        QVERIFY(servers.put(QStringLiteral("test"), source).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 22;
            },
            5000));
        const auto first = store.events().first();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.events().first().time > first.time;
            },
            2000));
        QCOMPARE(store.rowCount(), 22);
        process.terminate();
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitCode(), 0);
        const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QVERIFY(report.value("queued").toInt() >= 22);
    }
    void udpReception()
    {
        QUdpSocket socket;
        QVERIFY(socket.bind(QHostAddress::LocalHost, 0));
        Process process;
        process.start(executable(),
                      {"--udp",
                       "--port",
                       QString::number(socket.localPort()),
                       "--frequency",
                       "10",
                       "--duration",
                       "1"});
        QVERIFY(process.waitForStarted());
        QSet<QString> ids;
        connect(&socket, &QUdpSocket::readyRead, this, [&] {
            while (socket.hasPendingDatagrams()) {
                const auto result = decodeCot(socket.receiveDatagram().data());
                if (result.event) {
                    ids.insert(result.event->uid);
                }
            }
        });
        QVERIFY(QTest::qWaitFor(
            [&] {
                return process.state() == QProcess::NotRunning;
            },
            3000));
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(ids.size(), 22);
        const auto report = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QVERIFY(report.value("generated").toInt() >= 190);
        QCOMPARE(report.value("dropped").toInt(), 0);
    }
    void takClient()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QFile profile(directory.filePath("server.json"));
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(QJsonDocument(QJsonObject{{"protocol", "stcp"},
                                                {"host", "127.0.0.1"},
                                                {"port", server.serverPort()}})
                          .toJson());
        profile.close();
        Process process;
        process.start(executable(), {"--tak-profile", profile.fileName(), "--duration", "2"});
        QVERIFY(process.waitForStarted());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return server.hasPendingConnections();
            },
            2000));
        auto *socket = server.nextPendingConnection();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return socket->bytesAvailable() > 0;
            },
            2000));
        QVERIFY(socket->readAll().contains("how=\"m-s\""));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return process.state() == QProcess::NotRunning;
            },
            4000));
        QCOMPARE(process.exitCode(), 0);
    }
    void cliErrors()
    {
        for (const auto &arguments : QList<QStringList>{{"--frequency", "nan"},
                                                        {"--port", "0"},
                                                        {"--duration", "-1"},
                                                        {"--udp", "--address", "0.0.0.0"},
                                                        {"--scenario", "/missing/swak.json"},
                                                        {"--tak-profile", "/missing/swak.json"},
                                                        {"--tak-profile", "x", "--udp"}}) {
            Process process;
            process.start(executable(), arguments);
            QVERIFY(process.waitForFinished(3000));
            QCOMPARE(process.exitCode(), 2);
        }
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        Process process;
        process.start(executable(), {"--port", QString::number(occupied.serverPort())});
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitCode(), 1);
        const auto message = process.readAllStandardError();
        QVERIFY(message.contains("Cannot listen on TCP [127.0.0.1]:"));
        QVERIFY(message.contains(QByteArray::number(occupied.serverPort())));
        QVERIFY(message.contains("available port"));
    }
};
QTEST_GUILESS_MAIN(SimulationTest)
#include "test_simulation.moc"
