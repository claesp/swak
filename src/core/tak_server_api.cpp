#include "core/tak_server_api.h"

#include "core/logging.h"
#include "core/servers.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>

namespace swak {
namespace {
constexpr int DefaultApiPort = 8443;
constexpr int RetrySeconds = 300;
QString tr(const char *text)
{
    return QCoreApplication::translate("TakServerApi", text);
}
int apiPort(const QJsonObject &source)
{
    return source.value(QStringLiteral("apiPort")).toInt(DefaultApiPort);
}
bool usesApi(const QJsonObject &source)
{
    const auto protocol = source.value(QStringLiteral("protocol")).toString();
    return (protocol == u"tls" || protocol == u"quic") && validateServer(source).isEmpty() &&
           apiPort(source) > 0;
}
} // namespace
TakServerInfo readTakServerConfig(const QByteArray &reply)
{
    TakServerInfo info;
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(reply, &parse);
    const auto data = document.object().value(QStringLiteral("data")).toObject();
    static const QRegularExpression text(QStringLiteral("\\A[A-Za-z0-9._ -]{1,64}\\z"));
    const auto version = data.value(QStringLiteral("version")).toString();
    const auto api = data.value(QStringLiteral("api")).toString();
    if (parse.error != QJsonParseError::NoError ||
        document.object().value(QStringLiteral("type")).toString() != u"ServerConfig" ||
        !text.match(version).hasMatch() || !text.match(api).hasMatch()) {
        info.error = tr("The server did not report a TAK Server version.");
        return info;
    }
    info.version = version;
    info.api = api;
    return info;
}
TakServerApi::TakServerApi(Servers &servers, NetworkClient &network, QObject *parent)
    : QObject(parent), servers_(servers), network_(network)
{
    connect(&servers_, &Servers::changed, this, &TakServerApi::refresh);
    connect(&servers_, &Servers::statusChanged, this, &TakServerApi::refresh);
    refresh();
}
TakServerApi::~TakServerApi()
{
    for (const auto &entry : std::as_const(entries_)) {
        network_.cancel(entry.request);
    }
}
QUrl TakServerApi::apiUrl(const QJsonObject &source, const QString &path)
{
    if (!path.startsWith(QLatin1Char('/'))) {
        return {};
    }
    return martiUrl(source, QStringLiteral("/api") + path);
}
QUrl TakServerApi::martiUrl(const QJsonObject &source, const QString &path)
{
    if (!usesApi(source) || !path.startsWith(QLatin1Char('/')) ||
        path.contains(QStringLiteral(".."))) {
        return {};
    }
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(source.value(QStringLiteral("host")).toString());
    url.setPort(apiPort(source));
    const auto question = path.indexOf(QLatin1Char('?'));
    url.setPath(QStringLiteral("/Marti") + path.left(question < 0 ? path.size() : question));
    if (question >= 0) {
        url.setQuery(path.mid(question + 1));
    }
    return url.isValid() ? url : QUrl{};
}
quint64 TakServerApi::request(const QString &serverId,
                              const QByteArray &method,
                              const QString &path,
                              const QByteArray &body,
                              QObject *context,
                              NetworkClient::Callback done,
                              qsizetype maxBytes)
{
    const auto source = servers_.sources().value(serverId).toObject();
    NetworkRequest request;
    request.url = apiUrl(source, path);
    request.method = method;
    request.body = body;
    request.maxBytes = maxBytes;
    request.expectedHost = source.value(QStringLiteral("host")).toString();
    request.expectedPort = apiPort(source);
    request.headers.append(qMakePair(QByteArray("Accept"), QByteArray("application/json")));
    if (!body.isEmpty()) {
        request.headers.append(
            qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    }
    if (request.url.isValid()) {
        request.tls = readTlsFiles(source);
    }
    return network_.send(std::move(request), context, std::move(done));
}
quint64 TakServerApi::martiRequest(const QString &serverId,
                                   const QByteArray &method,
                                   const QString &path,
                                   const QByteArray &body,
                                   const QByteArray &contentType,
                                   QObject *context,
                                   NetworkClient::Callback done,
                                   qsizetype maxBytes,
                                   int timeoutMs,
                                   const QList<std::pair<QByteArray, QByteArray>> &headers)
{
    const auto source = servers_.sources().value(serverId).toObject();
    NetworkRequest outgoing;
    outgoing.url = martiUrl(source, path);
    outgoing.method = method;
    outgoing.body = body;
    outgoing.maxBytes = maxBytes;
    outgoing.timeoutMs = timeoutMs;
    outgoing.expectedHost = source.value(QStringLiteral("host")).toString();
    outgoing.expectedPort = apiPort(source);
    if (!contentType.isEmpty()) {
        outgoing.headers.append(qMakePair(QByteArray("Content-Type"), contentType));
    }
    for (const auto &[name, value] : headers) {
        outgoing.headers.append(qMakePair(name, value));
    }
    if (outgoing.url.isValid()) {
        outgoing.tls = readTlsFiles(source);
    }
    return network_.send(std::move(outgoing), context, std::move(done));
}
void TakServerApi::cancel(quint64 id)
{
    network_.cancel(id);
}
// TAK feature plugins and the server tests read the check result.
// cppcheck-suppress unusedFunction
TakServerInfo TakServerApi::info(const QString &serverId) const
{
    return entries_.value(serverId).info;
}
void TakServerApi::refresh()
{
    if (refreshing_) {
        return;
    }
    const QScopedValueRollback guard(refreshing_, true);
    const auto sources = servers_.sources();
    const auto stats = servers_.statistics();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!usesApi(sources.value(it.key()).toObject())) {
            network_.cancel(it->request);
            const auto id = it.key();
            it = entries_.erase(it);
            servers_.setServerInfo(id, {});
            emit infoChanged(id);
        } else {
            ++it;
        }
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        const auto source = it.value().toObject();
        if (!usesApi(source)) {
            continue;
        }
        auto &entry = entries_[it.key()];
        if (entry.source != source) {
            network_.cancel(entry.request);
            entry = Entry{source, {}, false, 0, {}};
        }
        const bool connected =
            stats.value(it.key()).toObject().value(QStringLiteral("connected")).toBool();
        // A new connection can reach another server build, so it checks again.
        const bool reconnected = connected && !entry.connected;
        const bool retry = connected && !entry.info.error.isEmpty() && entry.retryAt <= now;
        entry.connected = connected;
        if (!entry.info.checking && (reconnected || retry)) {
            check(it.key());
        }
    }
}
void TakServerApi::check(const QString &id)
{
    auto &entry = entries_[id];
    entry.info.checking = true;
    publish(id);
    entry.request = request(
        id,
        "GET",
        QStringLiteral("/version/config"),
        {},
        this,
        [this, id, source = entry.source](const NetworkReply &reply) {
            auto it = entries_.find(id);
            if (it == entries_.end() || it->source != source) {
                return;
            }
            auto result = reply.ok() ? readTakServerConfig(reply.body) : TakServerInfo{};
            if (!reply.ok()) {
                result.error =
                    !reply.error.isEmpty()
                        ? reply.error
                        : tr("The REST API answered with HTTP status %1.").arg(reply.status);
            }
            it->request = 0;
            it->info = result;
            const auto name = source.value(QStringLiteral("name")).toString();
            if (result.available()) {
                logMessage(
                    Severity::Info,
                    Urgency::Routine,
                    QStringLiteral("swak.tak"),
                    tr("%1: TAK Server %2, REST API %3.").arg(name, result.version, result.api));
            } else {
                it->retryAt = QDateTime::currentDateTimeUtc().addSecs(RetrySeconds);
                logMessage(Severity::Warning,
                           Urgency::Attention,
                           QStringLiteral("swak.tak"),
                           tr("%1: the TAK REST API is unavailable. %2").arg(name, result.error));
            }
            publish(id);
        },
        64 * 1024);
}
void TakServerApi::publish(const QString &id)
{
    // Copy the value. QHash::value returns a temporary.
    const auto current = entries_.value(id).info;
    QString text;
    if (current.checking) {
        text = tr("TAK REST API: checking.");
    } else if (current.available()) {
        text = tr("TAK Server %1, REST API %2.").arg(current.version, current.api);
    } else if (!current.error.isEmpty()) {
        text = tr("TAK REST API unavailable: %1").arg(current.error);
    }
    servers_.setServerInfo(id, text);
    emit infoChanged(id);
}
} // namespace swak
