#pragma once
#include "core/tls_files.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <thread>

namespace swak {
// One HTTPS request. The client rejects a URL whose scheme, host, or port differs from
// the expected destination, so a credential cannot reach another server.
struct NetworkRequest
{
    QUrl url;
    QByteArray method = "GET";
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    TlsFiles tls;
    QString expectedHost;
    int expectedPort = 443;
    qsizetype maxBytes = 1024 * 1024;
    int timeoutMs = 15000;
    int connectTimeoutMs = 10000;
};
struct NetworkReply
{
    int status = 0;
    QByteArray body;
    QByteArray contentType;
    // Empty on transport success. A non-2xx status is not a transport error.
    QString error;
    bool ok() const
    {
        return error.isEmpty() && status >= 200 && status < 300;
    }
};
// An HTTPS client on libcurl. One worker thread runs a curl multi handle, so the interface
// thread never waits for the network. Replies return on the thread of this object.
//
// Every request uses HTTPS only, certificate and host verification, TLS 1.2 or later, no
// redirects, no cookies, a size limit, and a time limit. Proxy environment variables apply.
class NetworkClient final : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const NetworkReply &)>;
    explicit NetworkClient(QObject *parent = nullptr);
    ~NetworkClient() override;
    NetworkClient(const NetworkClient &) = delete;
    NetworkClient &operator=(const NetworkClient &) = delete;
    // Starts a request. The callback runs once, unless the request is cancelled or the
    // context object is destroyed first. Returns the request identifier, or zero when the
    // request was rejected at once; the callback then reports the reason.
    quint64 send(NetworkRequest request, QObject *context, Callback done);
    // Stops a request. Its callback does not run.
    void cancel(quint64 id);
    // Requests that are queued or running. Tests use it.
    int activeCount() const;

private:
    struct Shared;
    void deliver(quint64 id, QPointer<QObject> context, Callback done, NetworkReply reply);
    void run();
    std::shared_ptr<Shared> shared_;
    std::thread worker_;
    // Requests whose callback can still run. Only the interface thread uses it. cancel removes
    // an identifier, so that a reply already queued for delivery does not run its callback.
    QSet<quint64> pending_;
};
} // namespace swak
