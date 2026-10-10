#pragma once
#include "core/event.h"
#include "core/network_client.h"

#include <QJsonObject>

namespace swak {
class NetworkClient;
class Servers;
class Store;
class TakServerApi;
} // namespace swak

// Waits for one reply of a raw REST request to a server entry.
swak::NetworkReply call(swak::TakServerApi &api,
                        const QByteArray &method,
                        const QString &path,
                        const QByteArray &body = {},
                        const QByteArray &type = {},
                        const QString &serverId = QStringLiteral("test"));
// The HTTP status or the transport error of a reply.
QString replyText(const swak::NetworkReply &reply);

// Runs the TAK Server feature checks with two connected clients. Each check records passed and
// a detail text. A failed check does not stop the later checks.
QJsonObject runFeatureChecks(swak::Servers &left,
                             swak::Servers &right,
                             swak::Store &leftStore,
                             swak::Store &rightStore,
                             const swak::Event &a,
                             const swak::Event &b,
                             const QJsonObject &profile);

struct EnrolledClient
{
    bool signedIn = false;
    bool connected = false;
    QString error;
    QString status;
};
// Requests a certificate with the user name and password of the enrollment profile, then
// connects with it as server entry "enroll". The server object gives host, port, and CA. The
// caller keeps the directory for the certificate files while the connection can use them.
EnrolledClient enrollClient(swak::Servers &servers,
                            swak::NetworkClient &network,
                            const QJsonObject &server,
                            const QJsonObject &enrollment,
                            const QString &clientUid,
                            const QString &directory);
// Runs the multicast, LDAP, and federation checks of the profile and adds them to checks.
void runNetworkChecks(swak::Servers &left,
                      swak::Servers &right,
                      swak::Store &leftStore,
                      swak::Store &rightStore,
                      const swak::Event &a,
                      const QJsonObject &profile,
                      QJsonObject &checks);
// Sends positions from left to right at a fixed rate and measures delivery, latency, memory,
// and disconnects.
QJsonObject runLoad(swak::Servers &left,
                    const swak::Servers &right,
                    swak::Store &rightStore,
                    const QJsonObject &load);
