#include "core/cot.h"
#include "core/cot_stream.h"
#include "core/identity.h"
#include "core/lan_protocol.h"
#include "core/projection.h"
#include "core/server_browser.h"
#include "core/servers.h"
#include "core/store.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>
using namespace swak;
static Event sampleEvent(const QString &id = QStringLiteral("test"))
{
    Event e;
    e.uid = id;
    e.callsign = QStringLiteral("Test contact");
    e.type = QStringLiteral("a-f-G-U");
    e.how = QStringLiteral("m-g");
    e.time = e.start = QDateTime::currentDateTimeUtc();
    e.stale = e.time.addSecs(120);
    return e;
}
static QJsonObject source(quint16 port)
{
    auto v = serverDefaults();
    v.insert(QStringLiteral("name"), QStringLiteral("Test"));
    v.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    v.insert(QStringLiteral("port"), port);
    v.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
    v.insert(QStringLiteral("enabled"), true);
    v.insert(QStringLiteral("retrySeconds"), 1);
    v.insert(QStringLiteral("timeoutSeconds"), 2);
    return v;
}
class TlsHarness final : public QTcpServer
{
public:
    QSslCertificate certificate;
    QSslKey key;
    QByteArray payload;
    bool requireClient = false;
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        auto configuration = socket->sslConfiguration();
        configuration.setCaCertificates({certificate});
        configuration.setPeerVerifyMode(requireClient ? QSslSocket::VerifyPeer
                                                      : QSslSocket::VerifyNone);
        socket->setSslConfiguration(configuration);
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        connect(socket, &QSslSocket::encrypted, socket, [this, socket] {
            socket->write(payload);
        });
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }
};
class ServerTest final : public QObject
{
    Q_OBJECT
private slots:
    void streamFragments()
    {
        auto e = sampleEvent();
        e.callsign = QString::fromUtf8("Ångström & <tag>");
        auto xml = encodeCot(e);
        xml.replace("</detail>",
                    "<remarks><![CDATA[</event> & >]]></remarks><!-- </event> --></detail>");
        for (qsizetype split = 0; split <= xml.size(); ++split) {
            CotStream stream;
            auto frames = stream.feed(xml.left(split));
            frames.append(stream.feed(xml.mid(split)));
            QVERIFY(!stream.failed());
            QCOMPARE(frames.size(), 1);
            QCOMPARE(decodeCot(frames.first()).event->callsign, e.callsign);
            QVERIFY(!stream.pending());
        }
        CotStream stream;
        QList<QByteArray> frames;
        for (const auto ch : xml) {
            frames.append(stream.feed(QByteArray(1, ch)));
        }
        QCOMPARE(frames.size(), 1);
        QVERIFY(!stream.failed());
        QCOMPARE(stream.feed(xml + "\r\n" + xml).size(), 2);
    }
    void hostileInput()
    {
        for (const auto &payload :
             {QByteArray("<!DOCTYPE event [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><event/>"),
              QByteArray("<wrong/>"),
              QByteArray("\0", 1),
              QByteArray("garbage"),
              QByteArray(MaxMessageBytes + 1, 'x')}) {
            CotStream stream;
            stream.feed(payload);
            QVERIFY(stream.failed());
            stream.reset();
            QCOMPARE(stream.feed(encodeCot(sampleEvent())).size(), 1);
        }
        CotStream stream;
        stream.feed("<event x='");
        stream.feed(QByteArray(MaxMessageBytes - 20, 'x'));
        stream.feed(QByteArray(30, 'x'));
        QVERIFY(stream.failed());
        auto xml = encodeCot(sampleEvent());
        xml.replace("<detail>", "<detail><__group name='a' role='b'/><__group name='a' role='b'/>");
        QVERIFY(!decodeCot(xml).event);
        xml = encodeCot(sampleEvent());
        xml.replace("<detail>", "<detail><?unsafe value?>");
        QVERIFY(!decodeCot(xml).event);
        xml = encodeCot(sampleEvent());
        xml.replace("lat=\"0.00000000\"", "lat=\"NaN\"");
        QVERIFY(!decodeCot(xml).event);
    }
    void identityRoundTrip()
    {
        Identity identity;
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("team"), QStringLiteral("Blue"));
        values.insert(QStringLiteral("role"), QStringLiteral("Medic"));
        QVERIFY(identity.validateSettings(values).isEmpty());
        identity.restorePreferences(values);
        auto e = sampleEvent();
        identity.apply(e);
        const auto result = decodeCot(encodeCot(e));
        QVERIFY(result.event);
        QCOMPARE(result.event->team, e.team);
        QCOMPARE(result.event->role, e.role);
        values.insert(QStringLiteral("how"), QStringLiteral("bad"));
        QVERIFY(!identity.validateSettings(values).isEmpty());
        identity.restorePreferences(values);
        QCOMPARE(identity.savePreferences().value(QStringLiteral("how")).toString(),
                 QStringLiteral("h-e"));
    }
    void lanDiscovery()
    {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto bytes = lanAnnouncement(id, 1234);
        const QHostAddress local(QStringLiteral("192.168.10.1")),
            remote(QStringLiteral("192.168.10.2"));
        const auto peer = parseLanAnnouncement(bytes, remote, local, 24);
        QVERIFY(peer);
        QCOMPARE(peer->id, id);
        QCOMPARE(peer->port, 1234);
        for (const auto &address : {QStringLiteral("192.168.11.2"),
                                    QStringLiteral("192.168.10.255"),
                                    QStringLiteral("192.168.10.0"),
                                    QStringLiteral("192.168.10.1"),
                                    QStringLiteral("224.0.0.1"),
                                    QStringLiteral("::1")}) {
            QVERIFY(!parseLanAnnouncement(bytes, QHostAddress(address), local, 24));
        }
        for (int prefix : {-1, 0, 31, 32}) {
            QVERIFY(!parseLanAnnouncement(bytes, remote, local, prefix));
        }
        for (const auto &invalid : {QByteArray{},
                                    QByteArray(513, 'x'),
                                    QByteArray("{}"),
                                    lanAnnouncement(QStringLiteral("bad"), 1234),
                                    lanAnnouncement(id, 0)}) {
            QVERIFY(!parseLanAnnouncement(invalid, remote, local, 24));
        }
        auto version = bytes;
        version.replace("\"version\":1", "\"version\":2");
        QVERIFY(!parseLanAnnouncement(version, remote, local, 24));
    }
    void lanPeers()
    {
        if (qEnvironmentVariable("SWAK_TEST_LAN") != QLatin1String("1")) {
            QSKIP("Run tools/test_lan.py for isolated LAN integration.");
        }
        Store firstStore, secondStore;
        Servers first(firstStore), second(secondStore);
        auto firstLan = serverDefaults();
        firstLan.insert("name", "First LAN");
        firstLan.insert("protocol", "lan");
        firstLan.insert("enabled", true);
        firstLan.insert("localAddress", "192.0.2.1");
        auto secondLan = firstLan;
        secondLan.insert("localAddress", "192.0.2.2");
        QVERIFY(first.put("lan", firstLan).isEmpty());
        QVERIFY(second.put("lan", secondLan).isEmpty());
        const auto connected = [](const Servers &servers) {
            const auto states = servers.statistics();
            return std::any_of(states.begin(), states.end(), [](const auto &state) {
                return state.toObject().value(QStringLiteral("state")) ==
                       QLatin1String("Connected");
            });
        };
        const bool ready = QTest::qWaitFor(
            [&] {
                return connected(first) && connected(second);
            },
            12000);
        if (!ready) {
            qInfo() << first.statistics() << second.statistics();
        }
        QVERIFY(ready);
        QVERIFY(first.send(sampleEvent(QStringLiteral("from-first"))).isEmpty());
        QVERIFY(second.send(sampleEvent(QStringLiteral("from-second"))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return firstStore.rowCount() == 1 && secondStore.rowCount() == 1;
            },
            3000));
        QCOMPARE(firstStore.events().first().uid, QStringLiteral("from-second"));
        QCOMPARE(secondStore.events().first().uid, QStringLiteral("from-first"));
        QTest::qWait(100);
        QCOMPARE(firstStore.rowCount(), 1);
        QCOMPARE(secondStore.rowCount(), 1);
        LocalLanStatus pane(first);
        auto *table = pane.findChild<QTableWidget *>("localLanPeers");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("192.0.2.2"));
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Connected"));
        QVERIFY(!(table->item(0, 0)->flags() & Qt::ItemIsEditable));
        table->setCurrentCell(0, 0);
        auto otherFirst = firstLan, otherSecond = secondLan;
        otherFirst.insert("localAddress", "198.51.100.1");
        otherSecond.insert("localAddress", "198.51.100.2");
        QVERIFY(first.put("other", otherFirst).isEmpty());
        QVERIFY(second.put("other", otherSecond).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return table->rowCount() == 2 &&
                       table->item(1, 4)->text() == QStringLiteral("Connected");
            },
            12000));
        QCOMPARE(table->item(table->currentRow(), 2)->text(), QStringLiteral("192.0.2.2"));
        first.remove("other");
        second.remove("other");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return table->rowCount() == 1;
            },
            3000));
        QVERIFY(connected(first) && connected(second));
        QSignalSpy statusChanges(&first, &Servers::statusChanged);
        first.reconnect();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return statusChanges.count() >= 2;
            },
            12000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return connected(first) && connected(second);
            },
            12000));
        QVERIFY(second.send(sampleEvent(QStringLiteral("after-reconnect"))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return firstStore.rowCount() == 2;
            },
            3000));
        auto disabled = secondLan;
        disabled.insert(QStringLiteral("enabled"), false);
        QVERIFY(second.put("lan", disabled).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !connected(first) && !connected(second);
            },
            3000));
    }
    void serverMigration()
    {
        Store store;
        Servers servers(store);
        servers.restorePreferences({{"address", "127.0.0.1"}, {"port", 6969}, {"running", true}});
        QVERIFY(servers.sources().isEmpty());
        servers.restorePreferences(
            {{"servers", QJsonObject{{"legacy-udp", source(6969)}, {"saved", source(8088)}}},
             {"lan", QJsonObject{{"enabled", true}, {"address", "192.0.2.1"}}}});
        QVERIFY(!servers.sources().contains("legacy-udp"));
        QCOMPARE(servers.sources().size(), 2);
        QCOMPARE(servers.sources().value("migrated-lan").toObject().value("protocol").toString(),
                 QStringLiteral("lan"));
        servers.remove("migrated-lan");
        auto saved = servers.savePreferences();
        saved.insert("lan", QJsonObject{{"enabled", true}, {"address", "192.0.2.1"}});
        servers.restorePreferences(saved);
        QCOMPARE(servers.sources().size(), 1);
        LocalLanStatus pane(servers);
        auto *summary = pane.findChild<QPlainTextEdit *>("localLanSummary");
        QVERIFY(summary->toPlainText().contains("No Local LAN servers"));
        auto lan = serverDefaults();
        lan.insert("name", "Unavailable LAN");
        lan.insert("protocol", "lan");
        lan.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put("lan", lan).isEmpty());
        QVERIFY(summary->toPlainText().contains("Disconnected"));
        lan.insert("enabled", true);
        QVERIFY(servers.put("lan", lan).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return summary->toPlainText().contains("active, non-loopback");
            },
            3000));
        servers.remove("lan");
        QVERIFY(summary->toPlainText().contains("No Local LAN servers"));
        const auto fields = serverFields(lan);
        const auto interface = std::find_if(fields.begin(), fields.end(), [](const auto &f) {
            return f.path == "localAddress";
        });
        QVERIFY(interface != fields.end());
    }
    void interfaceBinding()
    {
        Store store;
        Servers servers(store);
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        auto entry = source(listener.serverPort());
        entry.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put("bound", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("bound").contains("Cannot bind interface 203.0.113.254");
            },
            3000));
        QVERIFY(!listener.hasPendingConnections());
        entry.insert("localAddress", "127.0.0.1");
        QVERIFY(servers.put("bound", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return listener.hasPendingConnections();
            },
            3000));
        std::unique_ptr<QTcpSocket> peer(listener.nextPendingConnection());
        peer->setParent(nullptr);
        QCOMPARE(peer->peerAddress(), QHostAddress(QHostAddress::LocalHost));
        servers.remove("bound");
        QUdpSocket reservation;
        QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        entry.insert("port", reservation.localPort());
        reservation.close();
        entry.insert("protocol", "udp");
        entry.insert("host", "0.0.0.0");
        QVERIFY(servers.put("udp", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("udp").contains("Listening");
            },
            3000));
        QUdpSocket sender;
        sender.writeDatagram(encodeCot(sampleEvent()),
                             QHostAddress::LocalHost,
                             static_cast<quint16>(entry.value("port").toInt()));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 1;
            },
            3000));
        entry.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put("udp", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("udp").contains("Cannot bind UDP");
            },
            3000));
    }
    void validation()
    {
        auto v = source(8088);
        QVERIFY(validateServer(v).isEmpty());
        for (const auto &host : {QStringLiteral("https://example.com"),
                                 QStringLiteral("224.0.0.1"),
                                 QStringLiteral("255.255.255.255"),
                                 QStringLiteral("a\nb")}) {
            auto copy = v;
            copy.insert(QStringLiteral("host"), host);
            QVERIFY(!validateServer(copy).isEmpty());
        }
        for (const auto &local :
             QJsonArray{42, "invalid", "0.0.0.0", "::", "224.0.0.1", "255.255.255.255"}) {
            auto invalid = v;
            invalid.insert("localAddress", local);
            QVERIFY(!validateServer(invalid).isEmpty());
        }
        for (int port : {0, 65536, -1}) {
            auto copy = v;
            copy.insert(QStringLiteral("port"), port);
            QVERIFY(!validateServer(copy).isEmpty());
        }
        auto copy = v;
        copy.insert(QStringLiteral("passwordEnv"), QStringLiteral("HOME"));
        QVERIFY(!validateServer(copy).isEmpty());
        Store store;
        Servers servers(store);
        auto invalidLan = serverDefaults();
        invalidLan.insert("name", "Invalid LAN");
        invalidLan.insert("protocol", "lan");
        invalidLan.insert("localAddress", "127.0.0.1");
        QVERIFY(!servers.put("lan", invalidLan).isEmpty());
        for (int i = 0; i < 16; ++i) {
            copy = v;
            copy.insert(QStringLiteral("enabled"), false);
            QVERIFY(servers.put(QString::number(i), copy).isEmpty());
        }
        QVERIFY(!servers.put(QStringLiteral("overflow"), v).isEmpty());
        QVERIFY(!servers.send(sampleEvent()).isEmpty());
    }
    void multipleStreams()
    {
        QTcpServer a, b;
        QVERIFY(a.listen(QHostAddress::LocalHost, 0));
        QVERIFY(b.listen(QHostAddress::LocalHost, 0));
        Store store;
        QAbstractItemModelTester tester(&store,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("a"), source(a.serverPort())).isEmpty());
        QVERIFY(servers.put(QStringLiteral("b"), source(b.serverPort())).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return a.hasPendingConnections();
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return b.hasPendingConnections();
            },
            5000));
        // Configuration replacement can close an earlier pending connection.
        QTcpSocket *first = nullptr;
        while (a.hasPendingConnections()) {
            auto *candidate = a.nextPendingConnection();
            if (first) {
                first->deleteLater();
            }
            first = candidate;
        }
        auto *second = b.nextPendingConnection();
        QVERIFY(first && second);
        auto xml = encodeCot(sampleEvent(QStringLiteral("one")));
        first->write(xml.left(7));
        second->write(encodeCot(sampleEvent(QStringLiteral("two"))));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        first->write(xml.mid(7));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (2);
            },
            5000));
        auto update = sampleEvent(QStringLiteral("one"));
        update.latitude = 12;
        first->write(encodeCot(update));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(store.events().begin(), store.events().end(), [](const auto &e) {
                    return e.uid == QLatin1String("one") && e.latitude == 12;
                });
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("a")).contains(QStringLiteral("Connected"));
            },
            5000));
        QVERIFY(servers.send(sampleEvent(QStringLiteral("outgoing"))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return first->bytesAvailable() > 0;
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return second->bytesAvailable() > 0;
            },
            5000));
        QVERIFY(first->readAll().contains("outgoing"));
        QVERIFY(second->readAll().contains("outgoing"));
        first->write("<event/>");
        first->write(encodeCot(sampleEvent(QStringLiteral("recovered"))));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (3);
            },
            5000));
        first->write(QByteArray(MaxMessageBytes + 1, 'x'));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("a")).contains(QStringLiteral("Invalid"));
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return a.hasPendingConnections();
            },
            4000));
        auto *retry = a.nextPendingConnection();
        retry->write(encodeCot(sampleEvent(QStringLiteral("retry"))));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (4);
            },
            5000));
        retry->write("<event ");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("a")).contains(QStringLiteral("timed out"));
            },
            5000));
    }
    void flood()
    {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("load"), source(listener.serverPort())).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return listener.hasPendingConnections();
            },
            5000));
        auto *socket = listener.nextPendingConnection();
        QByteArray block;
        for (int i = 0; i < 12000; ++i) {
            auto e = sampleEvent(QString::number(i % 500));
            e.longitude = static_cast<double>(i % 180);
            block += encodeCot(e);
        }
        QElapsedTimer elapsed;
        elapsed.start();
        QSignalSpy updates(&store, &Store::changed);
        int beats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&beats] {
            ++beats;
        });
        heartbeat.start();
        QCOMPARE(socket->write(block), block.size());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.statistics()
                           .value(QStringLiteral("load"))
                           .toObject()
                           .value(QStringLiteral("received"))
                           .toInt() >= 12000;
            },
            20000));
        QCOMPARE(store.rowCount(), 500);
        QVERIFY(beats > 10);
        QVERIFY(updates.count() < 1200);
        qInfo("CoT load: 12000 messages in %lld ms, %d GUI heartbeat ticks",
              static_cast<long long>(elapsed.elapsed()),
              beats);
        servers.remove(QStringLiteral("load"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (socket->state()) == (QAbstractSocket::UnconnectedState);
            },
            5000));
    }
    void connectionDiagnostics()
    {
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        Store store;
        Servers servers(store);
        auto values = source(port);
        values.insert(QStringLiteral("retrySeconds"), 5);
        QVERIFY(servers.put(QStringLiteral("refused"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("refused"))
                    .contains(QStringLiteral("Connection refused"));
            },
            3000));
        const auto message = servers.status(QStringLiteral("refused"));
        QVERIFY(message.contains(QStringLiteral("TCP [127.0.0.1]:%1").arg(port)));
        QVERIFY(message.contains(QStringLiteral("Retry in 5 seconds")));
        QVERIFY(message.contains(QStringLiteral("Start the server")));
        QVERIFY(reservation.listen(QHostAddress::LocalHost, port));
        servers.reconnect();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("refused"))
                    .contains(QStringLiteral("Connected"));
            },
            3000));
        QVERIFY(!servers.status(QStringLiteral("refused"))
                     .contains(QStringLiteral("Connection refused")));
        QTcpServer mismatch;
        QVERIFY(mismatch.listen(QHostAddress::LocalHost, 0));
        values.insert(QStringLiteral("port"), mismatch.serverPort());
        values.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        QVERIFY(servers.put(QStringLiteral("tls-mismatch"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return mismatch.hasPendingConnections();
            },
            3000));
        auto *plain = mismatch.nextPendingConnection();
        plain->write("not TLS");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls-mismatch"))
                    .contains(QStringLiteral("TLS handshake failed"));
            },
            3000));
        QVERIFY(
            servers.status(QStringLiteral("tls-mismatch")).contains(QStringLiteral("plain TCP")));
    }
    void tlsCertificates()
    {
        qputenv("SWAK_COT_TEST_PASSWORD", "swak-test-only");
        qputenv("SWAK_COT_TEST_WRONG", "wrong");
        TlsHarness listener;
        QFile cert(QFINDTESTDATA("fixtures/map-test-cert.pem")),
            key(QFINDTESTDATA("fixtures/map-test-key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly));
        QVERIFY(key.open(QIODevice::ReadOnly));
        listener.certificate = QSslCertificate(cert.readAll());
        listener.key = QSslKey(key.readAll(), QSsl::Rsa);
        listener.payload = encodeCot(sampleEvent());
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        auto v = source(listener.serverPort());
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        v.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("tls").contains("Cannot bind interface");
            },
            3000));
        v.insert("localAddress", "127.0.0.1");
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls"))
                    .contains(QStringLiteral("certificate validation failed"));
            },
            5000));
        QCOMPARE(store.rowCount(), 0);
        v.insert(QStringLiteral("ca"), cert.fileName());
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        listener.requireClient = true;
        store.clear();
        v.insert(QStringLiteral("certificate"), cert.fileName());
        v.insert(QStringLiteral("key"), key.fileName());
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        store.clear();
        v.insert(QStringLiteral("certificate"), QFINDTESTDATA("fixtures/cot-test-client.p12"));
        v.insert(QStringLiteral("key"), QString{});
        v.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_PASSWORD"));
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        v.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_WRONG"));
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls"))
                    .contains(QStringLiteral("credentials"));
            },
            5000));
        v.insert(QStringLiteral("certificate"), QStringLiteral("/missing/client.p12"));
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls"))
                    .contains(QStringLiteral("credentials"));
            },
            5000));
    }
    void pausedMotion()
    {
        TrackMotion motion;
        Event event;
        event.uid = QStringLiteral("paused");
        event.time = QDateTime::currentDateTimeUtc();
        motion.update(event);
        motion.setPaused(true);
        const auto frozen = motion.snapshot();
        QCOMPARE(frozen.size(), 1);
        event.longitude = 20;
        event.time = event.time.addSecs(1);
        motion.update(event);
        QCOMPARE(motion.snapshot().first().world, frozen.first().world);
        auto added = event;
        added.uid = QStringLiteral("new");
        motion.update(added);
        QCOMPARE(motion.snapshot().size(), 1);
        motion.setPaused(false);
        QCOMPARE(motion.snapshot().size(), 2);
        for (const auto &visual : motion.snapshot()) {
            QCOMPARE(visual.world, project(0, 20));
            QCOMPARE(visual.opacity, 1.0);
        }
        event.longitude = 21;
        event.time = event.time.addSecs(1);
        motion.update(event);
        QVERIFY(motion.snapshot().first().world.x() <= project(0, 21).x());
        motion.setPaused(true);
        motion.expire(event.uid);
        motion.setPaused(false);
        const auto expired = motion.snapshot();
        QVERIFY(std::any_of(expired.cbegin(), expired.cend(), [](const auto &item) {
            return item.expired;
        }));
        motion.setPaused(true);
        motion.clear();
        QVERIFY(motion.snapshot().isEmpty());
        motion.setPaused(false);
        QVERIFY(motion.snapshot().isEmpty());
    }
    void predictiveMotion()
    {
        TrackMotion motion;
        auto event = sampleEvent();
        motion.update(event, 1000);
        event.longitude = 0.001;
        event.time = event.time.addSecs(1);
        motion.update(event, 2000);
        const auto a = motion.snapshot(2600).first().world;
        const auto b = motion.snapshot(2900).first().world;
        QVERIFY(b.x() > a.x());
        const auto before = motion.snapshot(3000).first().world;
        event.longitude = 0.002;
        event.time = event.time.addSecs(1);
        motion.update(event, 3000);
        QCOMPARE(motion.snapshot(3000).first().world, before);
        QVERIFY(motion.snapshot(3600).first().world.x() > before.x());
        QCOMPARE(motion.snapshot(6000).first().world, motion.snapshot(7000).first().world);
        motion.expire(event.uid, 7000);
        QCOMPARE(motion.snapshot(7200).first().world, motion.snapshot(7300).first().world);
        auto values = motion.savePreferences();
        values.insert(QStringLiteral("predictionMs"), 0);
        motion.restorePreferences(values);
        QCOMPARE(motion.savePreferences().value(QStringLiteral("predictionMs")).toInt(), 0);
    }
    void motion()
    {
        TrackMotion motion;
        auto e = sampleEvent();
        motion.update(e, 1000);
        e.longitude = 0.5;
        motion.update(e, 2000);
        const auto start = motion.snapshot(2000).first();
        const auto halfway = motion.snapshot(2300).first();
        const auto end = motion.snapshot(2600).first();
        QCOMPARE(start.world.x(), 0.5);
        QVERIFY(halfway.world.x() > start.world.x());
        QVERIFY(halfway.world.x() < end.world.x());
        e.longitude = 0.6;
        motion.update(e, 2300);
        QCOMPARE(motion.snapshot(2300).first().world, halfway.world);
        e.longitude = 100;
        motion.update(e, 3000);
        QVERIFY(motion.snapshot(3300).isEmpty());
    }
    void dateLineMotion()
    {
        TrackMotion motion;
        auto e = sampleEvent();
        e.longitude = 179.9;
        motion.update(e, 1000);
        e.longitude = -179.9;
        motion.update(e, 2000);
        const auto half = motion.snapshot(2300).first();
        QVERIFY(half.world.x() < 0.01 || half.world.x() > 0.99);
        QCOMPARE(half.opacity, 1.0);
        e.longitude = 100;
        motion.update(e, 3000);
        e.longitude = 101;
        motion.update(e, 3100);
        QVERIFY(motion.snapshot(3300).isEmpty());
        QCOMPARE(motion.snapshot(3600).first().world, project(e.latitude, e.longitude));
        auto settings = motion.savePreferences();
        settings.insert(QStringLiteral("duration"), 0);
        motion.restorePreferences(settings);
        e.longitude = 101.1;
        motion.update(e, 4000);
        QCOMPARE(motion.snapshot(4000).first().world, project(e.latitude, e.longitude));
    }
    void staleMotion()
    {
        TrackMotion motion;
        auto e = sampleEvent();
        motion.update(e, 1000);
        motion.expire(e.uid, 2000);
        QVERIFY(motion.snapshot(2300).first().gray > 0);
        QCOMPARE(motion.snapshot(2600).first().gray, 1.0);
        QVERIFY(motion.snapshot(10000).first().expired);
        QVERIFY(motion.snapshot(14000).isEmpty());
        e.time = e.time.addSecs(1);
        motion.update(e, 10000);
        QVERIFY(!motion.snapshot(11000).first().expired);
        motion.clear();
        QVERIFY(motion.snapshot().isEmpty());
        motion.update(e, 1000);
        e.longitude = 179.9;
        motion.update(e, 2000);
        QVERIFY(motion.snapshot(2300).isEmpty());
        QCOMPARE(motion.snapshot(2600).first().opacity, 1.0);
    }
};
QTEST_MAIN(ServerTest)
#include "test_servers.moc"
