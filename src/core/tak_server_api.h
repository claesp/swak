#pragma once
#include "core/network_client.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>

namespace swak {
class Servers;
// What a TAK Server reported about itself.
struct TakServerInfo
{
    // For example 5.7.14-RELEASE.
    QString version;
    // The REST API version, for example 3.
    QString api;
    // Empty after a successful check.
    QString error;
    bool checking = false;
    bool available() const
    {
        return !version.isEmpty() && error.isEmpty();
    }
};
// The TAK Server REST API of the configured TLS server entries.
//
// Requests go to https://host:apiPort/Marti/api with the same CA and client certificate as
// the CoT stream. The default API port is 8443. Port 0 turns the API off for an entry.
// After each new TLS connection, the class reads /version/config to learn the server
// version. Plugins use request() for later TAK features.
class TakServerApi final : public QObject
{
    Q_OBJECT
public:
    TakServerApi(Servers &servers, NetworkClient &network, QObject *parent = nullptr);
    ~TakServerApi() override;
    TakServerApi(const TakServerApi &) = delete;
    TakServerApi &operator=(const TakServerApi &) = delete;
    // The REST URL of a path below /Marti/api for a server entry, or an empty URL when the
    // entry is not a TLS entry with an API port.
    static QUrl apiUrl(const QJsonObject &source, const QString &path);
    // Sends a request to the REST API of a server entry. The path starts with a slash.
    quint64 request(const QString &serverId,
                    const QByteArray &method,
                    const QString &path,
                    const QByteArray &body,
                    QObject *context,
                    NetworkClient::Callback done,
                    qsizetype maxBytes = 1024 * 1024);
    void cancel(quint64 id);
    TakServerInfo info(const QString &serverId) const;
    NetworkClient &network() const
    {
        return network_;
    }
signals:
    void infoChanged(const QString &serverId);

private:
    struct Entry
    {
        QJsonObject source;
        TakServerInfo info;
        bool connected = false;
        quint64 request = 0;
        QDateTime retryAt;
    };
    void refresh();
    void check(const QString &id);
    void publish(const QString &id);
    Servers &servers_;
    NetworkClient &network_;
    QHash<QString, Entry> entries_;
    // publish() updates Servers, which signals back into refresh(). Ignore that nested call.
    bool refreshing_ = false;
};
// Reads a /version/config reply. Returns an empty info with an error for other content.
TakServerInfo readTakServerConfig(const QByteArray &reply);
} // namespace swak
