#include "core/plugin_catalog.h"
#include <QJsonArray>
#include "core/activity.h"
#include "core/maps.h"
#include "core/interfaces.h"
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
    void pluginManifests()
    {
        QCOMPARE(builtInPlugins().size(), 10);
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
        Store store;
        ActivityModel activity;
        MapSources maps({});
        PluginServices services{store, activity, maps};
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        const auto port = sender.localPort();
        sender.close();
        auto receiver = std::unique_ptr<QWidget>(source_->create(services, *codec_, nullptr));
        receiver->findChild<QSpinBox *>(QStringLiteral("bindPort"))->setValue(port);
        receiver->findChild<QPushButton *>(QStringLiteral("receiverToggle"))->click();
        activity.clear();
        sender.writeDatagram("bad", QHostAddress::LocalHost, port);
        QVERIFY(QTest::qWaitFor([&] { return activity.rowCount() >= 1; }, 2000));
        QCOMPARE(activity.index(activity.rowCount() - 1, 1).data(ActivityModel::SortRole).toInt(), static_cast<int>(Severity::Warning));
        activity.clear();
        QTimer traffic;
        traffic.setInterval(30);
        connect(&traffic, &QTimer::timeout, &traffic, [&] { sender.writeDatagram(codec_->encode(sample()), QHostAddress::LocalHost, port); });
        traffic.start();
        QVERIFY(QTest::qWaitFor([&] { return activity.rowCount() >= 1; }, 2000));
        QCOMPARE(activity.index(0, 1).data(ActivityModel::SortRole).toInt(), static_cast<int>(Severity::Info));
        QVERIFY(activity.rowCount() <= 2);
        QVERIFY(store.rowCount() > 0);
    }
    void receiverLifecycle()
    {
        Store store;
        QUdpSocket reservation;
        QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        const quint16 port = reservation.localPort();
        ActivityModel activity;
        MapSources maps({});
        PluginServices services{store, activity, maps};
        auto receiver = std::unique_ptr<QWidget>(source_->create(services, *codec_, nullptr));
        auto *portField = receiver->findChild<QSpinBox *>(QStringLiteral("bindPort"));
        auto *toggle = receiver->findChild<QPushButton *>(QStringLiteral("receiverToggle"));
        auto *address = receiver->findChild<QLineEdit *>(QStringLiteral("bindAddress"));
        QVERIFY(portField && toggle && address);
        portField->setValue(port);
        toggle->click();
        QVERIFY(portField->isEnabled());
        reservation.close();
        address->setText(QStringLiteral("not-an-address"));
        toggle->click();
        QVERIFY(portField->isEnabled());
        address->setText(QStringLiteral("127.0.0.1"));
        toggle->click();
        QVERIFY(!portField->isEnabled());
        QUdpSocket sender;
        const auto data = codec_->encode(sample());
        QSignalSpy arrivals(&store, &Store::changed);
        QCOMPARE(sender.writeDatagram(data, QHostAddress::LocalHost, port), data.size());
        QVERIFY(arrivals.wait(2000));
        QCOMPARE(arrivals.count(), 1);
        QCOMPARE(store.rowCount(), 1);
        sender.writeDatagram(QByteArray(MaxMessageBytes + 1, 'x'), QHostAddress::LocalHost, port);
        auto next = sample();
        next.uid = QStringLiteral("test-2");
        sender.writeDatagram(codec_->encode(next), QHostAddress::LocalHost, port);
        QVERIFY(arrivals.wait(2000));
        QCOMPARE(store.rowCount(), 2);
        toggle->click();
        QVERIFY(portField->isEnabled());
        QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
        reservation.close();
        toggle->click();
        QVERIFY(!portField->isEnabled());
        receiver.reset();
        QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
    }
};
QTEST_MAIN(CoreTest)
#include "test_core.moc"
