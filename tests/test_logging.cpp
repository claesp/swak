#include "core/logging.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

class LoggingTest final : public QObject
{
    Q_OBJECT
private:
    QByteArray probe(const QJsonObject &values, const QString &mode = QStringLiteral("normal"))
    {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SWAK_LOG_PROBE"), mode);
        environment.insert(QStringLiteral("SWAK_LOG_VALUES"),
                           QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact)));
        environment.remove(QStringLiteral("QT_FATAL_WARNINGS"));
        environment.remove(QStringLiteral("QT_FATAL_CRITICALS"));
        process.setProcessEnvironment(environment);
        process.start(QCoreApplication::applicationFilePath(), {});
        if (!process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished();
            return "TIMEOUT";
        }
        if (mode == QStringLiteral("fatal")) {
            if (process.exitStatus() != QProcess::CrashExit) {
                return "FATAL DID NOT TERMINATE";
            }
        } else if (process.exitCode() != 0) {
            return "CHILD FAILED: " + process.readAllStandardError();
        }
        return process.readAllStandardError();
    }
private slots:
    void silentByDefault()
    {
        QCOMPARE(probe({}), QByteArray{});
        QCOMPARE(probe({{QStringLiteral("enabled"), QStringLiteral("true")}}), QByteArray{});
        QCOMPARE(probe({}, QStringLiteral("fatal")), QByteArray{});
    }
    void thresholds()
    {
        const QJsonObject values{{QStringLiteral("enabled"), true},
                                 {QStringLiteral("severity"), 2},
                                 {QStringLiteral("urgency"), 1}};
        const auto output = probe(values);
        QVERIFY(output.contains("[warning/attention] qt.default: qt-warning"));
        QVERIFY(output.contains("[error/immediate] qt.default: qt-error"));
        QVERIFY(!output.contains("qt-info"));
        QVERIFY(!output.contains("app-routine"));
        QVERIFY(!output.contains("early-warning"));
        auto filtered = values;
        filtered.insert(QStringLiteral("prefix"), QStringLiteral("swak.receiver"));
        const auto filteredOutput = probe(filtered);
        QVERIFY(filteredOutput.contains("app-attention"));
        QVERIFY(!filteredOutput.contains("qt-warning"));
        filtered = values;
        filtered.insert(QStringLiteral("qt"), false);
        QVERIFY(!probe(filtered).contains("qt-warning"));
        filtered = values;
        filtered.insert(QStringLiteral("severity"), 4);
        const auto fatal = probe(filtered, QStringLiteral("fatal"));
        QVERIFY(fatal.contains("[fatal/immediate]"));
    }
    void boundedAndEscaped()
    {
        const auto output = probe({{QStringLiteral("enabled"), true},
                                   {QStringLiteral("severity"), 0},
                                   {QStringLiteral("rate"), 1}});
        QCOMPARE(output.count('\n'), 1);
        const auto escaped = probe({{QStringLiteral("enabled"), true}}, QStringLiteral("escape"));
        QCOMPARE(escaped.count('\n'), 1);
        QVERIFY(escaped.contains("hello\\nforged\\r"));
        QVERIFY(!escaped.contains('\033'));
        QVERIFY(escaped.size() < 2300);
    }
    void concurrentCalls()
    {
        const auto output =
            probe({{QStringLiteral("enabled"), true}, {QStringLiteral("rate"), 1000}},
                  QStringLiteral("threads"));
        QCOMPARE(output.count('\n'), 400);
    }
    void unavailableOutput()
    {
        QCOMPARE(probe({{QStringLiteral("enabled"), true}}, QStringLiteral("broken-pipe")),
                 QByteArray{});
        QCOMPARE(probe({{QStringLiteral("enabled"), true}}, QStringLiteral("full-pipe")),
                 QByteArray{});
        QCOMPARE(probe({{QStringLiteral("enabled"), true}}, QStringLiteral("closed-socket")),
                 QByteArray{});
        QVERIFY(probe({{QStringLiteral("enabled"), true}}, QStringLiteral("socket"))
                    .contains("socket-message"));
    }
    void sharedFileOffset()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("output")));
        QVERIFY(file.open(QIODevice::ReadWrite));
        const int saved = ::dup(STDERR_FILENO);
        QVERIFY(saved >= 0);
        const bool redirected = ::dup2(file.handle(), STDERR_FILENO) >= 0;
        if (redirected) {
            swak::configureLogging({{QStringLiteral("enabled"), true}});
            swak::logMessage(swak::Severity::Error,
                             swak::Urgency::Attention,
                             QStringLiteral("swak.test"),
                             QStringLiteral("logger-line"));
            ::write(STDERR_FILENO, "other-line\n", 11);
            swak::configureLogging({});
        }
        ::dup2(saved, STDERR_FILENO);
        ::close(saved);
        QVERIFY(redirected);
        QVERIFY(file.seek(0));
        const auto bytes = file.readAll();
        QVERIFY(bytes.contains("logger-line\nother-line\n"));
    }
    void persistedPolicy()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("profile.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"version\":1,\"logging\":{\"enabled\":true,\"severity\":3}}");
        file.close();
        swak::configureLoggingFromFile(path);
        QVERIFY(swak::loggingConfiguration().value(QStringLiteral("enabled")).toBool());
        QCOMPARE(swak::loggingConfiguration().value(QStringLiteral("severity")).toInt(), 3);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{\"version\":2,\"logging\":{\"enabled\":true}}");
        file.close();
        swak::configureLoggingFromFile(path);
        QVERIFY(!swak::loggingConfiguration().value(QStringLiteral("enabled")).toBool());
        swak::configureLoggingFromFile(directory.filePath(QStringLiteral("missing")));
        QVERIFY(!swak::loggingConfiguration().value(QStringLiteral("enabled")).toBool());
    }
};
int main(int argc, char **argv)
{
    const auto mode = qgetenv("SWAK_LOG_PROBE");
    if (!mode.isEmpty()) {
        const rlimit limit{0, 0};
        ::setrlimit(RLIMIT_CORE, &limit);
        swak::installLogHandler();
        qWarning("early-warning");
        QCoreApplication app(argc, argv);
        if (mode == "broken-pipe" || mode == "full-pipe") {
            int descriptors[2];
            if (::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0) {
                return 2;
            }
            if (::dup2(descriptors[1], STDERR_FILENO) < 0) {
                return 2;
            }
            ::close(descriptors[1]);
            if (mode == "full-pipe") {
                const QByteArray bytes(4096, 'x');
                while (::write(STDERR_FILENO,
                               bytes.constData(),
                               static_cast<size_t>(bytes.size())) > 0) {
                }
            }
            swak::configureLogging(QJsonDocument::fromJson(qgetenv("SWAK_LOG_VALUES")).object());
            if (mode == "broken-pipe") {
                ::close(descriptors[0]);
            }
            qWarning("output-unavailable");
            if (mode == "full-pipe") {
                ::close(descriptors[0]);
            }
            return 0;
        }
        if (mode == "socket" || mode == "closed-socket") {
            int descriptors[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, descriptors) !=
                0) {
                return 2;
            }
            const int original = ::dup(STDERR_FILENO);
            if (original < 0 || ::dup2(descriptors[1], STDERR_FILENO) < 0) {
                return 2;
            }
            ::close(descriptors[1]);
            swak::configureLogging(QJsonDocument::fromJson(qgetenv("SWAK_LOG_VALUES")).object());
            if (mode == "closed-socket") {
                ::close(descriptors[0]);
            }
            qWarning("socket-message");
            if (mode == "socket") {
                char bytes[4096];
                const auto count = ::read(descriptors[0], bytes, sizeof(bytes));
                if (count > 0) {
                    ::write(original, bytes, static_cast<size_t>(count));
                }
                ::close(descriptors[0]);
            }
            ::close(original);
            return 0;
        }
        swak::configureLogging(QJsonDocument::fromJson(qgetenv("SWAK_LOG_VALUES")).object());
        if (mode == "fatal") {
            qFatal("fatal-probe");
        }
        if (mode == "threads") {
            std::vector<std::thread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([] {
                    for (int j = 0; j < 100; ++j) {
                        swak::logMessage(swak::Severity::Warning,
                                         swak::Urgency::Attention,
                                         QStringLiteral("swak.test"),
                                         QStringLiteral("thread"));
                    }
                });
            }
            for (auto &thread : threads) {
                thread.join();
            }
        } else if (mode == "escape") {
            swak::logMessage(swak::Severity::Warning,
                             swak::Urgency::Attention,
                             QStringLiteral("swak.test"),
                             QStringLiteral("hello\nforged\r\033") + QString(10000, 'x'));
        } else {
            qDebug("qt-debug");
            qInfo("qt-info");
            qWarning("qt-warning");
            qCritical("qt-error");
            swak::logMessage(swak::Severity::Warning,
                             swak::Urgency::Routine,
                             QStringLiteral("swak.test"),
                             QStringLiteral("app-routine"));
            swak::logMessage(swak::Severity::Warning,
                             swak::Urgency::Attention,
                             QStringLiteral("swak.receiver"),
                             QStringLiteral("app-attention"));
        }
        return 0;
    }
    QCoreApplication app(argc, argv);
    LoggingTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_logging.moc"
