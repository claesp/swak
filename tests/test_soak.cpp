#include "core/interfaces.h"
#include "app/window.h"
#include "core/preferences.h"
#include "core/store.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPluginLoader>
#include <QOpenGLWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>
#include <ctime>
#include <memory>
#include <unistd.h>

using namespace swak;
namespace {
qint64 residentBytes()
{
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly)) return -1;
    const auto fields = file.readAll().simplified().split(' ');
    return fields.size() > 1 ? fields.at(1).toLongLong() * ::sysconf(_SC_PAGESIZE) : -1;
}
int descriptors() { return static_cast<int>(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot).size()); }
}
class SoakTest final : public QObject {
    Q_OBJECT
private slots:
    void sustainedWork()
    {
        bool ok = false;
        const auto secondsText = qgetenv("SWAK_SOAK_SECONDS");
        const int seconds = secondsText.isEmpty() ? 5 : secondsText.toInt(&ok);
        QVERIFY2(secondsText.isEmpty() || (ok && seconds >= 1 && seconds <= 86400), "SWAK_SOAK_SECONDS must be between 1 and 86400.");
        Codec *codec = nullptr;
        SourcePlugin *source = nullptr;
        QList<ToolPlugin *> tools;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (auto *item = qobject_cast<Codec *>(plugin)) codec = item;
            if (auto *item = qobject_cast<SourcePlugin *>(plugin)) source = item;
            if (auto *item = qobject_cast<ToolPlugin *>(plugin)) tools.append(item);
        }
        QVERIFY(codec && source && !tools.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Preferences preferences(directory.filePath(QStringLiteral("preferences.json")));
        std::unique_ptr<Window> window;
        if (qEnvironmentVariableIsSet("SWAK_SOAK_GUI")) {
            window = std::make_unique<Window>(directory.filePath(QStringLiteral("window.json")), QPluginLoader::staticInstances());
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window.get()));
            auto *map = window->findChild<QOpenGLWidget *>();
            QVERIFY(map);
            QTRY_VERIFY_WITH_TIMEOUT(map->isValid(), 5000);
        }
        Store headlessStore;
        Store &store = window ? window->store() : headlessStore;
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
        auto receiver = std::unique_ptr<QWidget>(source->create(store, *codec, nullptr));
        auto *port = receiver->findChild<QSpinBox *>(QStringLiteral("bindPort"));
        auto *toggle = receiver->findChild<QPushButton *>(QStringLiteral("receiverToggle"));
        QVERIFY(port && toggle);
        QUdpSocket sender;
        QVERIFY(sender.bind(QHostAddress::LocalHost, 0));
        const auto targetPort = sender.localPort();
        sender.close();
        port->setValue(targetPort);
        toggle->click();
        QVERIFY(!port->isEnabled());
        qint64 maxGap = 0;
        QElapsedTimer heartbeat;
        heartbeat.start();
        QTimer pulse;
        pulse.setInterval(20);
        connect(&pulse, &QTimer::timeout, &pulse, [&] { maxGap = qMax(maxGap, heartbeat.restart()); });
        pulse.start();
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 baseline = -1;
        qint64 peak = 0;
        int baselineFiles = 0;
        int cycles = 0;
        quint32 random = 0x5357414b;
        while (elapsed.elapsed() < (static_cast<qint64>(seconds) + 2) * 1000) {
            event.uid = QStringLiteral("item-%1").arg(cycles % 128);
            event.time = QDateTime::currentDateTimeUtc();
            event.start = event.time;
            event.stale = event.time.addSecs(60);
            const auto xml = codec->encode(event);
            QVERIFY(codec->decode(xml).event.has_value());
            for (int i = 0; i < 32; ++i) {
                auto mutation = xml;
                random = random * 1664525U + 1013904223U;
                mutation[static_cast<qsizetype>(random % static_cast<quint32>(mutation.size()))] = static_cast<char>(random >> 24);
                const auto result = codec->decode(mutation);
                if (result.event) QVERIFY(eventError(*result.event).isEmpty());
                QCOMPARE(sender.writeDatagram(i % 2 ? mutation : xml, QHostAddress::LocalHost, targetPort), xml.size());
            }
            for (auto *tool : tools) {
                const auto widget = std::unique_ptr<QWidget>(tool->create(store, *codec, nullptr));
                auto *participant = dynamic_cast<PreferenceParticipant *>(widget.get());
                QVERIFY(participant);
                participant->restorePreferences(participant->savePreferences());
            }
            QVERIFY(preferences.save({{QStringLiteral("version"), 1}, {QStringLiteral("cycle"), cycles}}));
            QTest::qWait(40);
            QVERIFY2(store.rowCount() > 0, "The receiver made no progress.");
            store.select(event.uid);
            store.expire(event.stale.addSecs(1));
            QCOMPARE(store.rowCount(), 0);
            ++cycles;
            const qint64 resident = residentBytes();
            QVERIFY(resident > 0);
            if (elapsed.elapsed() >= 2000) {
                if (baseline < 0) { baseline = resident; baselineFiles = descriptors(); }
                peak = qMax(peak, resident);
                QVERIFY2(resident - baseline < 64 * 1024 * 1024, "Resident memory grew by at least 64 MiB after warmup.");
                QVERIFY2(descriptors() <= baselineFiles + 8, "Open file descriptors grew after warmup.");
            }
            QVERIFY2(maxGap < 2000, "The event loop stalled for at least two seconds.");
        }
        QVERIFY(cycles > 0);
        const auto cpuStart = std::clock();
        QTest::qWait(1000);
        const double idleCpu = static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
        QVERIFY2(idleCpu < 0.5, "The idle receiver consumed at least half of one CPU core.");
        toggle->click();
        QVERIFY(port->isEnabled());
        receiver.reset();
        sender.close();
        QVERIFY(sender.bind(QHostAddress::LocalHost, targetPort));
        qInfo("cycles=%d peak_growth_bytes=%lld max_heartbeat_gap_ms=%lld idle_cpu_seconds=%.3f", cycles,
              static_cast<long long>(peak - baseline), static_cast<long long>(maxGap), idleCpu);
    }
};
QTEST_MAIN(SoakTest)
#include "test_soak.moc"
