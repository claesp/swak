#include "core/logging.h"

#include "core/activity.h"
#include "core/files.h"
#include "core/preferences.h"

#include <QDateTime>
#include <QGlobalStatic>
#include <QJsonDocument>
#include <QStringList>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swak {
namespace {
struct LoggingState
{
    ~LoggingState()
    {
        if (output >= 0) {
            ::close(output);
        }
    }
    std::atomic<bool> enabled{false};
    std::mutex mutex;
    int severity = 2;
    int urgency = 0;
    int rate = 20;
    bool qt = true;
    QString prefix;
    std::chrono::steady_clock::time_point period{};
    int count = 0;
    int output = -1;
    bool socket = false;
};
Q_GLOBAL_STATIC(LoggingState, loggingState)
LoggingState &state()
{
    return *loggingState;
}
QString safeText(const QString &text, qsizetype limit)
{
    QString result;
    for (const QChar ch : text.left(limit)) {
        if (ch == '\n') {
            result += QStringLiteral("\\n");
        } else if (ch == '\r') {
            result += QStringLiteral("\\r");
        } else if (ch.category() != QChar::Other_Control && ch.category() != QChar::Other_Format) {
            result += ch;
        }
    }
    return result;
}
void qtMessage(QtMsgType type, const QMessageLogContext &context, const QString &text)
{
    if (loggingState.isDestroyed() || !state().enabled) {
        return;
    }
    Severity severity = Severity::Debug;
    Urgency urgency = Urgency::Routine;
    switch (type) {
    case QtDebugMsg:
        break;
    case QtInfoMsg:
        severity = Severity::Info;
        break;
    case QtWarningMsg:
        severity = Severity::Warning;
        urgency = Urgency::Attention;
        break;
    case QtCriticalMsg:
        severity = Severity::Error;
        urgency = Urgency::Immediate;
        break;
    case QtFatalMsg:
        severity = Severity::Fatal;
        urgency = Urgency::Immediate;
        break;
    }
    try {
        const auto category = QString::fromUtf8(context.category ? context.category : "default");
        logMessage(severity,
                   urgency,
                   category.startsWith(QStringLiteral("qt.")) ? category
                                                              : QStringLiteral("qt.") + category,
                   text);
    } catch (...) {
        // A diagnostic must not throw through Qt.
    }
}
} // namespace
QtMessageHandler installLogHandler()
{
    return qInstallMessageHandler(qtMessage);
}
void configureLogging(const QJsonObject &values)
{
    auto &log = state();
    const std::lock_guard lock(log.mutex);
    log.severity = preferenceInteger(values, QStringLiteral("severity"), 2, 0, 4);
    log.urgency = preferenceInteger(values, QStringLiteral("urgency"), 0, 0, 2);
    log.rate = preferenceInteger(values, QStringLiteral("rate"), 20, 1, 1000);
    log.qt = values.value(QStringLiteral("qt")).toBool(true);
    log.prefix = preferenceText(values, QStringLiteral("prefix"), {}, 128);
    log.count = 0;
    log.period = std::chrono::steady_clock::now();
    log.enabled = values.value(QStringLiteral("enabled")).toBool(false);
    if (log.output >= 0) {
        ::close(log.output);
        log.output = -1;
    }
    struct stat information{};
    log.socket = ::fstat(STDERR_FILENO, &information) == 0 && S_ISSOCK(information.st_mode);
    if (log.enabled && !log.socket) {
        if (S_ISREG(information.st_mode)) {
            log.output = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
        } else {
            log.output = ::open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_APPEND | O_CLOEXEC);
        }
    }
}
QJsonObject loggingConfiguration()
{
    auto &log = state();
    const std::lock_guard lock(log.mutex);
    return {{QStringLiteral("enabled"), log.enabled.load()},
            {QStringLiteral("severity"), log.severity},
            {QStringLiteral("urgency"), log.urgency},
            {QStringLiteral("rate"), log.rate},
            {QStringLiteral("qt"), log.qt},
            {QStringLiteral("prefix"), log.prefix}};
}
void configureLoggingFromFile(const QString &path)
{
    configureLogging({});
    const auto input = readRegularFile(path, 1024 * 1024);
    if (!input.error.isEmpty()) {
        return;
    }
    const auto document = QJsonDocument::fromJson(input.bytes).object();
    if (document.value(QStringLiteral("version")).toDouble() != 1) {
        return;
    }
    configureLogging(document.value(QStringLiteral("logging")).toObject());
}
void logMessage(Severity severity,
                Urgency urgency,
                const QString &category,
                const QString &message) noexcept
{
    try {
        if (category.startsWith(QStringLiteral("swak."))) {
            publishActivity(severity, urgency, category, message);
        }
        if (loggingState.isDestroyed()) {
            return;
        }
        auto &log = state();
        if (!log.enabled) {
            return;
        }
        const std::lock_guard lock(log.mutex);
        if (!log.enabled || static_cast<int>(severity) < log.severity ||
            static_cast<int>(urgency) < log.urgency ||
            (!log.qt && category.startsWith(QStringLiteral("qt."))) ||
            !category.startsWith(log.prefix)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - log.period >= std::chrono::seconds(1)) {
            log.period = now;
            log.count = 0;
        }
        if (log.count >= log.rate) {
            return;
        }
        ++log.count;
        const QStringList severities{QStringLiteral("debug"),
                                     QStringLiteral("info"),
                                     QStringLiteral("warning"),
                                     QStringLiteral("error"),
                                     QStringLiteral("fatal")};
        const QStringList urgencies{
            QStringLiteral("routine"), QStringLiteral("attention"), QStringLiteral("immediate")};
        const int level = static_cast<int>(severity);
        const int priority = static_cast<int>(urgency);
        if (level < 0 || level >= severities.size() || priority < 0 ||
            priority >= urgencies.size()) {
            return;
        }
        const auto line = QStringLiteral("%1 [%2/%3] %4: %5\n")
                              .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                                   severities.at(level),
                                   urgencies.at(priority),
                                   safeText(category, 128),
                                   safeText(message, 2048))
                              .toUtf8();
        if (log.socket) {
            ::send(STDERR_FILENO,
                   line.constData(),
                   static_cast<size_t>(line.size()),
                   MSG_DONTWAIT | MSG_NOSIGNAL);
        } else if (log.output >= 0) {
            sigset_t blocked, previous, pending;
            ::sigemptyset(&blocked);
            ::sigaddset(&blocked, SIGPIPE);
            if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
                return;
            }
            ::sigpending(&pending);
            const bool alreadyPending = ::sigismember(&pending, SIGPIPE) == 1;
            const auto written =
                ::write(log.output, line.constData(), static_cast<size_t>(line.size()));
            if (written < 0 && errno == EPIPE && !alreadyPending) {
                const timespec timeout{};
                ::sigtimedwait(&blocked, nullptr, &timeout);
            }
            ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        }
    } catch (...) {
        // Logging failure must not throw through a Qt callback.
    }
}
} // namespace swak
