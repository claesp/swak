#include "core/servers.h"

#include "core/cot.h"
#include "core/cot_detail.h"
#include "core/cot_stream.h"
#include "core/lan_protocol.h"
#include "core/logging.h"
#include "core/quic_stream.h"
#include "core/store.h"
#include "core/tak_protocol.h"
#include "core/tls_files.h"

#include <QBuffer>
#include <QCollator>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QUdpSocket>
#include <QUuid>
#include <QXmlStreamWriter>

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tuple>
#include <utility>
#include <vector>

namespace swak {
namespace {
QString tr(const char *s)
{
    return QCoreApplication::translate("Servers", s);
}
constexpr int MaxServers = 16;
constexpr int MaxPeers = 16;
// QUIC tries at most this many addresses of a host name, each for a few seconds.
constexpr int MaxQuicAddresses = 8;
constexpr qint64 QuicAddressTimeoutMs = 5000;
// Tests can replace the host name lookup of QUIC entries. Production code leaves it empty.
std::atomic<QuicHostLookup> quicHostLookup{nullptr};
constexpr int QueueLimit = 2048;
// Unsent bytes that one stream may hold before new events are dropped.
constexpr qint64 MaxWriteBacklog = 256 * 1024;
constexpr quint16 DiscoveryPort = 17421;
bool safe(const QString &s, int limit, bool empty = false)
{
    return (empty || !s.isEmpty()) && s.size() <= limit &&
           std::none_of(s.cbegin(), s.cend(), [](QChar c) {
               return c.isNull() || c.isSurrogate() || c.category() == QChar::Other_Control ||
                      c.category() == QChar::Other_Format;
           });
}
} // namespace
// Only the server tests call it. The analysis does not read the tests.
// cppcheck-suppress unusedFunction
void setQuicHostLookupForTests(QuicHostLookup lookup)
{
    quicHostLookup.store(lookup);
}
QJsonObject serverDefaults()
{
    return {{QStringLiteral("name"), QString{}},
            {QStringLiteral("host"), QString{}},
            {QStringLiteral("protocol"), QStringLiteral("tls")},
            {QStringLiteral("port"), 8089},
            {QStringLiteral("enabled"), false},
            {QStringLiteral("localAddress"), QString{}},
            {QStringLiteral("ca"), QString{}},
            {QStringLiteral("certificate"), QString{}},
            {QStringLiteral("key"), QString{}},
            {QStringLiteral("passwordEnv"), QString{}},
            {QStringLiteral("apiPort"), 8443},
            {QStringLiteral("authUser"), QString{}},
            {QStringLiteral("authPasswordEnv"), QString{}},
            {QStringLiteral("takProtocol"), QStringLiteral("binary")},
            {QStringLiteral("retrySeconds"), 5},
            {QStringLiteral("timeoutSeconds"), 15}};
}
QList<SettingField> serverFields(const QJsonObject &source)
{
    QStringList interfaceLabels{tr("System default")};
    QJsonArray interfaceValues{QString{}};
    QMap<QString, QString> addresses;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)) {
            continue;
        }
        for (const auto &entry : interface.addressEntries()) {
            if (!entry.ip().isNull()) {
                addresses.insert(entry.ip().toString(),
                                 interface.humanReadableName() + QStringLiteral(" / ") +
                                     entry.ip().toString());
            }
        }
    }
    const auto current = source.value("localAddress").toString();
    if (!current.isEmpty() && !addresses.contains(current)) {
        addresses.insert(current, tr("Unavailable: %1").arg(current));
    }
    auto keys = addresses.keys();
    const QCollator collator;
    std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
        return collator.compare(addresses.value(a), addresses.value(b)) < 0;
    });
    for (const auto &address : keys) {
        interfaceLabels.append(addresses.value(address));
        interfaceValues.append(address);
    }
    QList<SettingField> fields{
        textSetting(QStringLiteral("name"), tr("Name"), 128),
        textSetting(QStringLiteral("host"), tr("Host or local bind address"), 253),
        choiceSetting(QStringLiteral("protocol"),
                      tr("Protocol"),
                      {tr("TLS CoT stream"),
                       tr("TCP CoT stream"),
                       tr("QUIC CoT stream (TAK Server 5.7)"),
                       tr("UDP input"),
                       tr("UDP output to a server"),
                       tr("Multicast mesh (ATAK)"),
                       tr("Local LAN")},
                      {QStringLiteral("tls"),
                       QStringLiteral("stcp"),
                       QStringLiteral("quic"),
                       QStringLiteral("udp"),
                       QStringLiteral("udpout"),
                       QStringLiteral("mcast"),
                       QStringLiteral("lan")}),
        choiceSetting(
            QStringLiteral("localAddress"), tr("Interface"), interfaceLabels, interfaceValues),
        numberSetting(QStringLiteral("port"), tr("Port"), 1, 65535),
        textSetting(QStringLiteral("ca"), tr("CA certificate (PEM)"), 4096),
        textSetting(QStringLiteral("certificate"), tr("Client certificate (PEM or PKCS12)"), 4096),
        textSetting(QStringLiteral("key"), tr("Private key (PEM, for PEM certificate)"), 4096),
        textSetting(QStringLiteral("passwordEnv"), tr("Password environment variable"), 128),
        numberSetting(QStringLiteral("apiPort"), tr("TAK REST API port (0 = off)"), 0, 65535),
        textSetting(QStringLiteral("authUser"), tr("TAK username (optional)"), 128),
        choiceSetting(QStringLiteral("takProtocol"),
                      tr("TAK message format"),
                      {tr("Use TAK Protocol 1 (binary) when the server offers it"), tr("XML only")},
                      {QStringLiteral("binary"), QStringLiteral("xml")}),
        textSetting(QStringLiteral("authPasswordEnv"),
                    tr("TAK password environment variable (SWAK_COT_...)"),
                    128),
        numberSetting(QStringLiteral("retrySeconds"), tr("Retry interval (seconds)"), 1, 60),
        numberSetting(QStringLiteral("timeoutSeconds"),
                      tr("Connection and frame timeout (seconds)"),
                      2,
                      120)};
    for (auto &field : fields) {
        if (field.path == "takProtocol") {
            field.enabledBy = QStringLiteral("protocol");
            field.enabledValue = QJsonArray{"tls", "stcp", "quic"};
        }
        if (field.path == "host" || field.path == "port") {
            field.enabledBy = QStringLiteral("protocol");
            field.enabledValue = QJsonArray{"tls", "stcp", "quic", "udp", "udpout", "mcast"};
        }
        if (QStringList{QStringLiteral("ca"),
                        QStringLiteral("certificate"),
                        QStringLiteral("key"),
                        QStringLiteral("passwordEnv"),
                        QStringLiteral("apiPort"),
                        QStringLiteral("authUser"),
                        QStringLiteral("authPasswordEnv")}
                .contains(field.path)) {
            field.enabledBy = QStringLiteral("protocol");
            field.enabledValue = QJsonArray{"tls", "quic"};
        }
    }
    return fields;
}
QString validateServer(const QJsonObject &v)
{
    if (!safe(v.value(QStringLiteral("name")).toString(), 128)) {
        return tr("Enter a server name up to 128 characters.");
    }
    const auto protocol = v.value(QStringLiteral("protocol")).toString();
    if (protocol != QLatin1String("tls") && protocol != QLatin1String("stcp") &&
        protocol != QLatin1String("quic") && protocol != QLatin1String("udp") &&
        protocol != QLatin1String("udpout") && protocol != QLatin1String("mcast") &&
        protocol != QLatin1String("lan")) {
        return tr("The server protocol is unavailable.");
    }
    if (v.contains("localAddress") && !v.value("localAddress").isString()) {
        return tr("Select a local interface address.");
    }
    const auto local = v.value("localAddress").toString();
    QHostAddress localAddress;
    if (!local.isEmpty() &&
        (!safe(local, 128) || !localAddress.setAddress(local) || localAddress.isMulticast() ||
         localAddress == QHostAddress::Any || localAddress == QHostAddress::AnyIPv4 ||
         localAddress == QHostAddress::AnyIPv6 || localAddress == QHostAddress::Broadcast)) {
        return tr("Select a local interface address.");
    }
    if (protocol == QLatin1String("lan")) {
        if (local.isEmpty() || localAddress.isLoopback() ||
            localAddress.protocol() != QAbstractSocket::IPv4Protocol) {
            return tr("Local LAN requires a non-loopback IPv4 interface.");
        }
    } else if (protocol == QLatin1String("mcast")) {
        // ATAK uses 239.2.3.1:6969 for SA and 224.10.10.1:17012 for GeoChat.
        QHostAddress group;
        if (!group.setAddress(v.value(QStringLiteral("host")).toString()) || !group.isMulticast() ||
            group.protocol() != QAbstractSocket::IPv4Protocol) {
            return tr("Multicast mesh needs an IPv4 multicast group, such as 239.2.3.1.");
        }
        if (local.isEmpty() || localAddress.protocol() != QAbstractSocket::IPv4Protocol) {
            return tr("Multicast mesh needs an IPv4 interface.");
        }
    } else {
        const auto host = v.value(QStringLiteral("host")).toString();
        QHostAddress address;
        static const QRegularExpression hostname(
            QStringLiteral("\\A[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?\\z"));
        if (!safe(host, 253) || (!address.setAddress(host) && !hostname.match(host).hasMatch())) {
            return tr("Enter a host name or numeric address without a URL or port.");
        }
        if (address.isMulticast() || address == QHostAddress::Broadcast) {
            return tr("Use a unicast address.");
        }
        if (protocol == QLatin1String("udp") && !address.setAddress(host)) {
            return tr("UDP input requires a numeric local address.");
        }
        if (protocol == QLatin1String("udpout") && !address.setAddress(host)) {
            return tr("UDP output requires the numeric address of the server.");
        }
    }
    for (const auto &[key, low, high] :
         {std::tuple{"port", 1, 65535}, {"retrySeconds", 1, 60}, {"timeoutSeconds", 2, 120}}) {
        const auto n = v.value(QLatin1String(key));
        if (!n.isDouble() || n.toDouble() != n.toInt() || n.toInt() < low || n.toInt() > high) {
            return tr("A server number is outside its allowed range.");
        }
    }
    // Entries from earlier profiles have no API port. They use the default 8443.
    if (const auto api = v.value(QStringLiteral("apiPort"));
        !api.isUndefined() && (!api.isDouble() || api.toDouble() != api.toInt() ||
                               api.toInt() < 0 || api.toInt() > 65535)) {
        return tr("A server number is outside its allowed range.");
    }
    for (const auto *key : {"ca", "certificate", "key"}) {
        if (!safe(v.value(QLatin1String(key)).toString(), 4096, true)) {
            return tr("A certificate path is invalid.");
        }
    }
    const auto env = v.value(QStringLiteral("passwordEnv")).toString();
    static const QRegularExpression envName(QStringLiteral("\\ASWAK_COT_[A-Z0-9_]{1,96}\\z"));
    if (!env.isEmpty() && !envName.match(env).hasMatch()) {
        return tr("Use a dedicated SWAK_COT_ environment variable for the certificate password.");
    }
    if (const auto format = v.value(QStringLiteral("takProtocol"));
        !format.isUndefined() && format != QStringLiteral("binary") &&
        format != QStringLiteral("xml")) {
        return tr("Select a TAK message format.");
    }
    // TAK file and LDAP inputs accept a username and password before the first CoT event.
    const auto user = v.value(QStringLiteral("authUser")).toString();
    const auto userPassword = v.value(QStringLiteral("authPasswordEnv")).toString();
    if (!safe(user, 128, true) ||
        (!userPassword.isEmpty() && !envName.match(userPassword).hasMatch())) {
        return tr("Use a TAK username up to 128 characters and a SWAK_COT_ password variable.");
    }
    const bool encrypted = protocol == QLatin1String("tls") || protocol == QLatin1String("quic");
    if (!user.isEmpty() && (!encrypted || userPassword.isEmpty())) {
        return tr(
            "A TAK username needs TLS or QUIC and a SWAK_COT_ password environment variable.");
    }
    return {};
}
struct ServerMailbox
{
    QMutex mutex;
    QHash<QString, Event> events;
    std::deque<QString> order;
    QJsonObject stats;
    std::deque<QByteArray> outbound;
    quint64 generation = 0;
    quint64 dropped = 0;
};
class ServerWorker final : public QObject
{
public:
    explicit ServerWorker(std::shared_ptr<ServerMailbox> mailbox) : mailbox_(std::move(mailbox))
    {
        clock_.start();
    }
    void configure(const QJsonObject &sources, quint64 generation)
    {
        std::erase_if(sessions_, [&sources](const auto &session) {
            return !session->peer && (!sources.contains(session->id) ||
                                      sources.value(session->id).toObject() != session->source ||
                                      !session->source.value(QStringLiteral("enabled")).toBool());
        });
        generation_ = generation;
        sources_ = sources;
        {
            const QMutexLocker lock(&mailbox_->mutex);
            mailbox_->events.clear();
            mailbox_->order.clear();
            mailbox_->outbound.clear();
            mailbox_->stats = {};
            mailbox_->dropped = 0;
            mailbox_->generation = generation;
        }
        std::erase_if(sessions_, [&sources](const auto &session) {
            return session->peer &&
                   (!sources.contains(session->lanId) ||
                    sources.value(session->lanId).toObject() != session->lanSource);
        });
        std::erase_if(lans_, [&sources](const auto &lan) {
            return !sources.contains(lan->id) || sources.value(lan->id).toObject() != lan->source;
        });
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            const auto source = it.value().toObject();
            if (!source.value("enabled").toBool() || !validateServer(source).isEmpty()) {
                continue;
            }
            if (source.value("protocol") == QLatin1String("lan")) {
                if (std::none_of(lans_.begin(), lans_.end(), [&it](const auto &lan) {
                        return lan->id == it.key();
                    })) {
                    auto lan = std::make_unique<Lan>();
                    lan->id = it.key();
                    lan->source = source;
                    setupLan(*lan);
                    lans_.push_back(std::move(lan));
                }
            } else if (std::none_of(sessions_.begin(), sessions_.end(), [&it](const auto &session) {
                           return session->id == it.key();
                       })) {
                auto session = std::make_unique<Session>();
                session->id = it.key();
                session->source = source;
                sessions_.push_back(std::move(session));
            }
        }
        if (!timer_) {
            timer_ = new QTimer(this);
            timer_->setInterval(5);
            connect(timer_, &QTimer::timeout, this, [this] {
                tick();
            });
        }
        if (!sessions_.empty() || !lans_.empty()) {
            timer_->start();
        } else {
            timer_->stop();
        }
        publish();
    }
    void reconnect()
    {
        sessions_.clear();
        lans_.clear();
        const auto sources = sources_;
        configure(sources, generation_);
    }
    void send(const QByteArray &payload, quint64 generation)
    {
        if (generation != generation_ || payload.size() > MaxMessageBytes) {
            return;
        }
        // A slow peer keeps a bounded backlog. Count each event that does not fit, so the
        // status shows the loss.
        quint64 dropped = 0;
        for (const auto &s : sessions_) {
            if (s->tcp && s->ready && (!s->peer || s->greeted)) {
                if (s->tcp->bytesToWrite() + payload.size() <= MaxWriteBacklog) {
                    writeTo(*s, payload);
                } else {
                    ++dropped;
                }
            } else if (s->quic && s->ready) {
                if (s->quic->bytesToWrite() + payload.size() <= MaxWriteBacklog) {
                    writeTo(*s, payload);
                } else {
                    ++dropped;
                }
            } else if ((s->output || s->mesh) && s->udp && s->ready) {
                // One event per datagram. The 64 KiB message limit stays below the UDP limit.
                const QHostAddress target(s->source.value(QStringLiteral("host")).toString());
                const auto port =
                    static_cast<quint16>(s->source.value(QStringLiteral("port")).toInt());
                if (s->udp->writeDatagram(payload, target, port) == payload.size()) {
                    ++s->sent;
                } else {
                    ++dropped;
                }
            }
        }
        if (dropped > 0) {
            const QMutexLocker lock(&mailbox_->mutex);
            mailbox_->dropped += dropped;
        }
    }

private:
    struct Session
    {
        ~Session()
        {
            if (tcp) {
                QObject::disconnect(tcp.get(), nullptr, nullptr, nullptr);
            }
        }
        QString id;
        QJsonObject source;
        std::unique_ptr<QTcpSocket> tcp;
        std::unique_ptr<QUdpSocket> udp;
        std::unique_ptr<QuicStream> quic;
        // The other resolved addresses to try when the QUIC handshake fails.
        QList<QHostAddress> quicAddresses;
        qint64 quicAttempt = 0;
        // TAK Protocol version 1 after a successful negotiation.
        TakStream proto;
        bool binary = false, requested = false;
        int lookup = -1;
        CotStream stream;
        QString state;
        quint64 accepted = 0, rejected = 0, controls = 0, sent = 0, binaryDatagrams = 0;
        qint64 next = 0, started = 0, partial = 0, lastRead = 0, lastPing = 0;
        int failures = 0;
        bool ready = false, peer = false, greeted = false, output = false, mesh = false;
        QByteArray greeting;
        QString peerId, lanId;
        QJsonObject lanSource;
    };
    struct Peer
    {
        QHostAddress address;
        quint16 port = 0;
        qint64 seen = 0;
    };
    struct Lan
    {
        QString id, state;
        QJsonObject source;
        QHostAddress address, broadcast;
        int prefix = 0;
        uint interfaceIndex = 0;
        qint64 nextAnnounce = 0, retryAt = 0;
        QHash<QString, Peer> peers;
        std::unique_ptr<QUdpSocket> discovery, announcer;
        std::unique_ptr<QTcpServer> listener;
    };
    void state(Session &s, const QString &text, Severity severity = Severity::Info)
    {
        if (s.state == text) {
            return;
        }
        s.state = text;
        logMessage(severity,
                   severity >= Severity::Warning ? Urgency::Attention : Urgency::Routine,
                   QStringLiteral("swak.servers"),
                   tr("Connection %1: %2").arg(s.id, text));
    }
    void fail(Session &s, const QString &text)
    {
        if (s.tcp) {
            auto *socket = s.tcp.release();
            socket->disconnect(this);
            socket->abort();
            socket->deleteLater();
        }
        if (s.udp) {
            s.udp->close();
            s.udp.reset();
        }
        s.quic.reset();
        s.quicAddresses.clear();
        if (s.lookup >= 0) {
            QHostInfo::abortHostLookup(s.lookup);
            s.lookup = -1;
        }
        s.ready = false;
        s.stream.reset();
        s.proto.reset();
        s.binary = false;
        s.requested = false;
        s.partial = 0;
        s.greeting.clear();
        s.greeted = false;
        const int delay = std::min(60000,
                                   s.source.value(QStringLiteral("retrySeconds")).toInt(5) * 1000 *
                                       (1 << std::min(s.failures, 3)));
        s.failures = std::min(s.failures + 1, 3);
        s.next = clock_.elapsed() + delay;
        const auto protocol = s.source.value(QStringLiteral("protocol")).toString();
        const auto label =
            protocol == QLatin1String("stcp") ? QStringLiteral("TCP") : protocol.toUpper();
        // Use one substitution pass. A marker such as %1 in an error text then stays literal.
        state(s,
              tr("%1 [%2]:%3: %4 Retry in %5 seconds.")
                  .arg(label,
                       s.source.value(QStringLiteral("host")).toString(),
                       QString::number(s.source.value(QStringLiteral("port")).toInt()),
                       text,
                       QString::number(delay / 1000)),
              Severity::Warning);
    }
    bool tls(QSslSocket &socket, const QJsonObject &source)
    {
        if (!QSslSocket::supportsSsl()) {
            return false;
        }
        auto configuration = QSslConfiguration::defaultConfiguration();
        configuration.setProtocol(QSsl::TlsV1_2OrLater);
        configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
        auto files = readTlsFiles(source);
        if (!files.error.isEmpty()) {
            return false;
        }
        if (!files.ca.isEmpty()) {
            const auto certs = QSslCertificate::fromData(files.ca);
            if (certs.isEmpty()) {
                return false;
            }
            configuration.setCaCertificates(certs);
        }
        if (!files.certificate.isEmpty()) {
            QSslKey key;
            QList<QSslCertificate> chain;
            if (files.pkcs12) {
                QBuffer buffer;
                buffer.setData(files.certificate);
                buffer.open(QIODevice::ReadOnly);
                QSslCertificate cert;
                QList<QSslCertificate> extra;
                if (!QSslCertificate::importPkcs12(&buffer, &key, &cert, &extra, files.password)) {
                    return false;
                }
                chain.append(cert);
                chain.append(extra);
            } else {
                chain = QSslCertificate::fromData(files.certificate);
                for (const auto algorithm : {QSsl::Rsa, QSsl::Ec, QSsl::Dsa}) {
                    key =
                        QSslKey(files.key, algorithm, QSsl::Pem, QSsl::PrivateKey, files.password);
                    if (!key.isNull()) {
                        break;
                    }
                }
            }
            if (key.isNull() || chain.isEmpty()) {
                return false;
            }
            configuration.setLocalCertificateChain(chain);
            configuration.setPrivateKey(key);
        }
        socket.setSslConfiguration(configuration);
        return true;
    }
    void attach(Session &s)
    {
        s.tcp->setReadBufferSize(64 * 1024);
        s.tcp->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        connect(
            s.tcp.get(),
            &QTcpSocket::errorOccurred,
            this,
            [this, &s](QAbstractSocket::SocketError error) {
                QString detail;
                switch (error) {
                case QAbstractSocket::ConnectionRefusedError:
                    detail = tr("Connection refused. Start the server and make sure that its "
                                "address and port match this connection.");
                    break;
                case QAbstractSocket::HostNotFoundError:
                    detail = tr(
                        "Host name not found. Correct the host name or use its numeric address.");
                    break;
                case QAbstractSocket::SocketTimeoutError:
                    detail = tr("Connection timed out. Make sure that the server is reachable and "
                                "the firewall allows this port.");
                    break;
                case QAbstractSocket::RemoteHostClosedError:
                    detail = tr("The server closed the connection. Make sure that the protocol "
                                "matches the server.");
                    break;
                case QAbstractSocket::SslHandshakeFailedError:
                    detail =
                        tr("TLS handshake failed. Make sure that the server supports TLS and "
                           "accepts the client certificate. The default emulator uses plain TCP.");
                    break;
                default:
                    detail = tr("Socket error %1: %2")
                                 .arg(static_cast<int>(error))
                                 .arg(s.tcp->errorString().left(256));
                    break;
                }
                fail(s, detail);
            });
        connect(s.tcp.get(), &QTcpSocket::disconnected, this, [this, &s] {
            fail(s,
                 tr("The server closed the connection. Make sure that the protocol matches the "
                    "server."));
        });
    }
    void connected(Session &s)
    {
        s.ready = true;
        s.failures = 0;
        s.lastRead = s.lastPing = clock_.elapsed();
        state(s, s.peer ? tr("LAN handshake") : tr("Connected"));
        if (s.peer) {
            s.tcp->write("SWAK-COT/1 " + localId_.toLatin1() + '\n');
        } else if (!s.source.value(QStringLiteral("authUser")).toString().isEmpty()) {
            sendLogin(s);
        } else if (s.quic) {
            // A QUIC client opens the TAK stream with its first write. Send a ping now.
            sendPing(s);
        }
    }
    // Resolves the host without blocking the worker, then starts the QUIC handshake.
    void startQuic(Session &s)
    {
        s.quic = std::make_unique<QuicStream>();
        state(s, tr("Connecting"));
        const auto host = s.source.value(QStringLiteral("host")).toString();
        const QHostAddress address(host);
        if (!address.isNull()) {
            connectQuic(s, address);
            return;
        }
        const auto id = s.id;
        if (const auto lookup = quicHostLookup.load()) {
            // Report the test result later, as QHostInfo does.
            s.lookup = 0;
            const auto addresses = lookup(host);
            QMetaObject::invokeMethod(
                this,
                [this, id, addresses] {
                    useQuicAddresses(id, 0, addresses);
                },
                Qt::QueuedConnection);
            return;
        }
        s.lookup = QHostInfo::lookupHost(host, this, [this, id](const QHostInfo &info) {
            useQuicAddresses(id,
                             info.lookupId(),
                             info.error() == QHostInfo::NoError ? info.addresses()
                                                                : QList<QHostAddress>{});
        });
    }
    // Continues a QUIC session after its host name lookup.
    void useQuicAddresses(const QString &id, int lookupId, QList<QHostAddress> addresses)
    {
        const auto found =
            std::find_if(sessions_.begin(), sessions_.end(), [&id](const auto &item) {
                return item->id == id && !item->peer;
            });
        if (found == sessions_.end() || !(*found)->quic || (*found)->lookup != lookupId) {
            return;
        }
        auto &session = **found;
        session.lookup = -1;
        if (addresses.isEmpty()) {
            fail(session, tr("The server host name cannot be resolved."));
            return;
        }
        // A name such as localhost can resolve to IPv6 and IPv4. A server can listen on one
        // of them only, so try each address. IPv4 comes first, as for most TAK servers.
        std::stable_partition(addresses.begin(), addresses.end(), [](const QHostAddress &item) {
            return item.protocol() == QAbstractSocket::IPv4Protocol;
        });
        if (addresses.size() > MaxQuicAddresses) {
            addresses.resize(MaxQuicAddresses);
        }
        session.quicAddresses = addresses.mid(1);
        connectQuic(session, addresses.first());
    }
    // Starts a new QUIC handshake with the next resolved address. Returns false when no
    // address remains.
    bool nextQuicAddress(Session &s)
    {
        if (s.quicAddresses.isEmpty()) {
            return false;
        }
        s.quic = std::make_unique<QuicStream>();
        connectQuic(s, s.quicAddresses.takeFirst());
        return true;
    }
    void connectQuic(Session &s, const QHostAddress &address)
    {
        s.quicAttempt = clock_.elapsed();
        const auto files = readTlsFiles(s.source);
        if (!files.error.isEmpty()) {
            fail(s, files.error);
            return;
        }
        const auto local = s.source.value(QStringLiteral("localAddress")).toString();
        const auto error =
            s.quic->start(address,
                          static_cast<quint16>(s.source.value(QStringLiteral("port")).toInt()),
                          s.source.value(QStringLiteral("host")).toString(),
                          files,
                          local.isEmpty() ? QHostAddress{} : QHostAddress(local));
        if (!error.isEmpty()) {
            fail(s, error);
        }
    }
    // Writes to the stream of a TCP, TLS, or QUIC session.
    void writeTo(Session &s, const QByteArray &xmlBytes)
    {
        auto bytes = xmlBytes;
        if (s.binary) {
            // Generated events are CoT XML. Convert them after the switch.
            const auto event = decodeCot(xmlBytes).event;
            bytes = event ? takStreamFrame(encodeTakMessage(*event)) : QByteArray{};
            if (bytes.isEmpty()) {
                return;
            }
        }
        if (s.tcp) {
            s.tcp->write(bytes);
        } else if (s.quic) {
            s.quic->write(bytes);
        }
    }
    qsizetype pendingWrite(const Session &s) const
    {
        return s.tcp ? s.tcp->bytesToWrite() : s.quic ? s.quic->bytesToWrite() : 0;
    }
    // Joins an ATAK multicast group on one interface. The socket receives and sends CoT.
    void startMesh(Session &s)
    {
        const QHostAddress group(s.source.value(QStringLiteral("host")).toString());
        const QHostAddress local(s.source.value(QStringLiteral("localAddress")).toString());
        const auto port = static_cast<quint16>(s.source.value(QStringLiteral("port")).toInt());
        QNetworkInterface selected;
        for (const auto &interface : QNetworkInterface::allInterfaces()) {
            for (const auto &entry : interface.addressEntries()) {
                if (entry.ip() == local) {
                    selected = interface;
                }
            }
        }
        s.udp = std::make_unique<QUdpSocket>();
        if (!selected.isValid() ||
            !s.udp->bind(QHostAddress::AnyIPv4,
                         port,
                         QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint) ||
            !s.udp->joinMulticastGroup(group, selected)) {
            fail(s,
                 tr("Cannot join the multicast group on the selected interface: %1.")
                     .arg(s.udp->errorString().left(256)));
            return;
        }
        s.udp->setMulticastInterface(selected);
        // Stay on the local network, and do not receive the datagrams that SWAK sends.
        s.udp->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
        s.udp->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 0);
        s.udp->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 256 * 1024);
        s.mesh = true;
        s.ready = true;
        state(s, tr("Joined multicast group"));
    }
    // Sends the TAK login element before any CoT. TAK reads it on file and LDAP inputs.
    void sendLogin(Session &s)
    {
        const auto variable =
            s.source.value(QStringLiteral("authPasswordEnv")).toString().toLatin1();
        auto password = qgetenv(variable.constData());
        if (password.isEmpty() || password.size() > 1024) {
            password.fill('\0');
            fail(s,
                 tr("The TAK password is unavailable. Set its SWAK_COT_ environment variable "
                    "before you start SWAK."));
            return;
        }
        QByteArray login;
        QXmlStreamWriter xml(&login);
        xml.writeStartElement(QStringLiteral("auth"));
        xml.writeStartElement(QStringLiteral("cot"));
        xml.writeAttribute(QStringLiteral("username"),
                           s.source.value(QStringLiteral("authUser")).toString());
        xml.writeAttribute(QStringLiteral("password"), QString::fromUtf8(password));
        for (const auto &[attribute, key] :
             {std::pair{"uid", "senderUid"}, std::pair{"callsign", "senderCallsign"}}) {
            const auto value = s.source.value(QLatin1String(key)).toString();
            if (!value.isEmpty()) {
                xml.writeAttribute(QLatin1String(attribute), value);
            }
        }
        xml.writeEndElement();
        xml.writeEndElement();
        password.fill('\0');
        writeTo(s, login);
        login.fill('\0');
    }
    bool bindInterface(Session &s)
    {
        const auto local = s.source.value("localAddress").toString();
        if (!local.isEmpty() && !s.tcp->bind(QHostAddress(local), 0)) {
            fail(s, tr("Cannot bind interface %1: %2.").arg(local, s.tcp->errorString().left(256)));
            return false;
        }
        return true;
    }
    void start(Session &s)
    {
        s.started = clock_.elapsed();
        if (s.source.value(QStringLiteral("protocol")) == QLatin1String("mcast")) {
            startMesh(s);
            return;
        }
        if (s.source.value(QStringLiteral("protocol")) == QLatin1String("quic")) {
            startQuic(s);
            return;
        }
        if (s.source.value(QStringLiteral("protocol")) == QLatin1String("udpout")) {
            // Output only. The socket sends datagrams and reads nothing.
            s.udp = std::make_unique<QUdpSocket>();
            const auto local = s.source.value("localAddress").toString();
            if (!s.udp->bind(
                    local.isEmpty() ? QHostAddress(QHostAddress::Any) : QHostAddress(local), 0)) {
                fail(s, tr("Cannot bind UDP output: %1.").arg(s.udp->errorString().left(256)));
                return;
            }
            s.output = true;
            s.ready = true;
            state(s, tr("Sending"));
            return;
        }
        if (s.source.value(QStringLiteral("protocol")) == QLatin1String("udp")) {
            s.udp = std::make_unique<QUdpSocket>();
            const auto local = s.source.value("localAddress").toString();
            if (!s.udp->bind(
                    QHostAddress(local.isEmpty() ? s.source.value("host").toString() : local),
                    static_cast<quint16>(s.source.value(QStringLiteral("port")).toInt()),
                    QUdpSocket::DontShareAddress)) {
                fail(s,
                     tr("Cannot bind UDP input: %1. Use an available port and an address assigned "
                        "to this computer.")
                         .arg(s.udp->errorString().left(256)));
                return;
            }
            s.udp->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 256 * 1024);
            s.ready = true;
            state(s, tr("Listening"));
            return;
        }
        const bool encrypted = s.source.value(QStringLiteral("protocol")) == QLatin1String("tls");
        if (encrypted) {
            auto socket = std::make_unique<QSslSocket>();
            if (!tls(*socket, s.source)) {
                fail(s,
                     tr("TLS credentials are unavailable or invalid. Make sure that the CA, client "
                        "certificate, private key, and password configuration are readable and "
                        "valid."));
                return;
            }
            auto *ssl = socket.get();
            s.tcp = std::move(socket);
            if (!bindInterface(s)) {
                return;
            }
            attach(s);
            connect(ssl, &QSslSocket::encrypted, this, [this, &s] {
                connected(s);
            });
            connect(ssl, &QSslSocket::sslErrors, this, [this, &s](const QList<QSslError> &errors) {
                fail(s,
                     tr("TLS certificate validation failed: %1. Use a trusted certificate that "
                        "matches the host.")
                         .arg(errors.isEmpty() ? tr("Unknown certificate error")
                                               : errors.first().errorString().left(256)));
            });
            state(s, tr("Connecting"));
            ssl->connectToHostEncrypted(
                s.source.value(QStringLiteral("host")).toString(),
                static_cast<quint16>(s.source.value(QStringLiteral("port")).toInt()));
        } else {
            s.tcp = std::make_unique<QTcpSocket>();
            if (!bindInterface(s)) {
                return;
            }
            attach(s);
            connect(s.tcp.get(), &QTcpSocket::connected, this, [this, &s] {
                connected(s);
            });
            state(s, tr("Connecting"));
            s.tcp->connectToHost(
                s.source.value(QStringLiteral("host")).toString(),
                static_cast<quint16>(s.source.value(QStringLiteral("port")).toInt()));
        }
    }
    void decode(Session &s, const QByteArray &frame)
    {
        accept(s, decodeCot(frame));
    }
    void decodeBinary(Session &s, const QByteArray &payload)
    {
        auto result = decodeTakMessage(payload);
        if (result.control) {
            ++s.controls;
            return;
        }
        accept(s, {std::move(result.event), result.error});
    }
    // Handles the TAK protocol offer and response on a stream in XML mode.
    void negotiate(Session &s, const Event &event)
    {
        const bool stream = (s.tcp && !s.peer) || s.quic;
        if (!stream || s.source.value(QStringLiteral("takProtocol")).toString() == u"xml") {
            return;
        }
        const auto control = detailElement(event, u"TakControl");
        if (!control) {
            return;
        }
        if (event.type == u"t-x-takp-v" && !s.requested && !s.binary) {
            const bool versionOne = std::any_of(
                control->children.cbegin(), control->children.cend(), [](const auto &child) {
                    return child.name == u"TakProtocolSupport" &&
                           child.attribute(u"version") == u"1";
                });
            if (versionOne) {
                Event request;
                request.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
                request.type = QStringLiteral("t-x-takp-q");
                request.how = QStringLiteral("m-g");
                request.time = request.start = QDateTime::currentDateTimeUtc();
                request.stale = request.time.addSecs(60);
                CotElement ask{QStringLiteral("TakRequest"),
                               {{QStringLiteral("version"), QStringLiteral("1")}},
                               {},
                               {}};
                CotElement wrapper{QStringLiteral("TakControl"), {}, {}, {ask}};
                request.detail = detailText({wrapper});
                writeTo(s, encodeCot(request));
                s.requested = true;
            }
        } else if (event.type == u"t-x-takp-r" && s.requested && !s.binary) {
            const auto *response = control->child(u"TakResponse");
            if (response &&
                response->attribute(u"status").compare(u"true", Qt::CaseInsensitive) == 0) {
                // From now on both directions use TAK Protocol version 1.
                s.binary = true;
                state(s, tr("Connected, TAK Protocol 1"));
            }
        }
    }
    void accept(Session &s, const DecodeResult &result)
    {
        if (!result.event) {
            ++s.rejected;
            return;
        }
        if (result.event->type.startsWith(QLatin1String("t-x-takp-"))) {
            ++s.controls;
            negotiate(s, *result.event);
            return;
        }
        if (result.event->type == QLatin1String("t-x-c-t") ||
            result.event->type == QLatin1String("t-x-c-t") ||
            result.event->type == QLatin1String("t-x-c-t-r")) {
            ++s.controls;
            return;
        }
        const auto now = QDateTime::currentDateTimeUtc();
        if (result.event->stale <= now || result.event->stale > now.addDays(7) ||
            result.event->time > now.addSecs(300) || result.event->start > now.addSecs(300)) {
            ++s.rejected;
            return;
        }
        const QMutexLocker lock(&mailbox_->mutex);
        if (!mailbox_->events.contains(result.event->uid)) {
            if (mailbox_->events.size() >= QueueLimit) {
                ++mailbox_->dropped;
                return;
            }
            mailbox_->order.push_back(result.event->uid);
        } else if (mailbox_->events.value(result.event->uid).time > result.event->time) {
            ++s.rejected;
            return;
        }
        auto event = *result.event;
        event.origin = s.peer ? s.lanId : s.id;
        mailbox_->events.insert(event.uid, event);
        ++s.accepted;
    }
    void receive(Session &s)
    {
        if (s.output) {
            // An output socket accepts no input. Discard stray datagrams in bounded steps.
            for (int i = 0; i < 32 && s.udp && s.udp->hasPendingDatagrams(); ++i) {
                s.udp->readDatagram(nullptr, 0);
            }
            return;
        }
        if (s.udp) {
            for (int i = 0; i < 32 && s.udp->hasPendingDatagrams(); ++i) {
                const auto size = s.udp->pendingDatagramSize();
                if (size <= 0 || size > MaxMessageBytes) {
                    s.udp->readDatagram(nullptr, 0);
                    ++s.rejected;
                    continue;
                }
                QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
                if (s.udp->readDatagram(bytes.data(), bytes.size()) != size) {
                    ++s.rejected;
                } else if (s.mesh && bytes.startsWith(TakMagic)) {
                    // TAK Protocol version 1 mesh datagram.
                    ++s.binaryDatagrams;
                    if (const auto payload = takMeshPayload(bytes)) {
                        decodeBinary(s, *payload);
                    } else {
                        ++s.rejected;
                    }
                } else {
                    decode(s, bytes);
                }
            }
            return;
        }
        if (s.quic) {
            // The stream starts after the host name lookup. Until then, it has nothing to poll.
            if (s.lookup >= 0) {
                return;
            }
            QByteArray bytes;
            if (!s.quic->poll(bytes, MaxMessageBytes)) {
                // Before the connection, try the next address of the host name.
                if (!s.ready && nextQuicAddress(s)) {
                    return;
                }
                // Copy the text. fail() destroys the QUIC stream that owns it.
                const QString error = s.quic->error();
                fail(s, error);
                return;
            }
            if (!s.ready && s.quic->connected()) {
                connected(s);
            }
            if (!s.ready || (!bytes.isEmpty() && !consume(s, bytes))) {
                return;
            }
            maintain(s);
            return;
        }
        if (!s.tcp || !s.ready) {
            return;
        }
        if (s.tcp->bytesAvailable() && !consume(s, s.tcp->read(MaxMessageBytes))) {
            return;
        }
        maintain(s);
    }
    // Handles received stream bytes. Returns false after the session failed.
    bool consume(Session &s, QByteArray bytes)
    {
        {
            s.lastRead = clock_.elapsed();
            if (s.peer && !s.greeted) {
                const auto end = bytes.indexOf('\n');
                s.greeting.append(end < 0 ? bytes : bytes.left(end));
                if (s.greeting.size() > 47) {
                    fail(s, tr("Invalid LAN greeting."));
                    return false;
                }
                if (end < 0) {
                    // Wait for the rest of the greeting.
                    return true;
                }
                if (s.greeting != "SWAK-COT/1 " + s.peerId.toLatin1()) {
                    fail(s, tr("LAN identity does not match discovery."));
                    return false;
                }
                s.greeted = true;
                state(s, tr("Connected"));
                s.greeting.clear();
                bytes.remove(0, end + 1);
            }
            if (s.binary) {
                return consumeBinary(s, bytes);
            }
            // Stop after a protocol response. Binary data can follow it in the same read.
            const auto frames = s.stream.feed(bytes, [](const QByteArray &frame) {
                return frame.contains("t-x-takp-r");
            });
            if (!frames.isEmpty()) {
                s.partial = 0;
            }
            for (const auto &frame : frames) {
                decode(s, frame);
            }
            if (s.binary) {
                const auto rest = s.stream.takeRemainder();
                s.stream.reset();
                return rest.isEmpty() || consumeBinary(s, rest);
            }
            const auto rest = s.stream.takeRemainder();
            if (!rest.isEmpty()) {
                // The response kept XML mode. Read the rest as XML.
                return consume(s, rest);
            }
            if (s.stream.failed()) {
                ++s.rejected;
                fail(s, tr("Invalid or oversized CoT stream."));
                return false;
            }
            if (!s.stream.pending()) {
                s.partial = 0;
            } else if (!s.partial) {
                s.partial = clock_.elapsed();
            }
        }
        return true;
    }
    bool consumeBinary(Session &s, const QByteArray &bytes)
    {
        for (const auto &payload : s.proto.feed(bytes)) {
            decodeBinary(s, payload);
        }
        if (s.proto.failed()) {
            ++s.rejected;
            fail(s, tr("Invalid or oversized TAK Protocol stream."));
            return false;
        }
        s.partial = s.proto.pending() ? (s.partial ? s.partial : clock_.elapsed()) : 0;
        return true;
    }
    // Applies stream timeouts and sends the keepalive ping.
    void maintain(Session &s)
    {
        const auto now = clock_.elapsed();
        if ((s.partial &&
             now - s.partial > s.source.value(QStringLiteral("timeoutSeconds")).toInt(15) * 1000) ||
            (s.peer && !s.greeted &&
             now - s.started > s.source.value(QStringLiteral("timeoutSeconds")).toInt(15) * 1000)) {
            fail(s, tr("Incomplete message timed out."));
            return;
        }
        if (now - s.lastRead > 90000) {
            fail(s, tr("Idle connection timed out."));
            return;
        }
        if (now - s.lastPing > 30000 && pendingWrite(s) < 64 * 1024) {
            sendPing(s);
        }
    }
    void sendPing(Session &s)
    {
        Event ping;
        ping.uid = localId_;
        ping.type = QStringLiteral("t-x-c-t");
        ping.how = QStringLiteral("m-g");
        ping.time = ping.start = QDateTime::currentDateTimeUtc();
        ping.stale = ping.time.addSecs(60);
        writeTo(s, encodeCot(ping));
        s.lastPing = clock_.elapsed();
    }
    bool onLink(const Lan &lan, const QHostAddress &address) const
    {
        return address.protocol() == QAbstractSocket::IPv4Protocol && address != lan.address &&
               address.isInSubnet(lan.address, lan.prefix) && !address.isMulticast() &&
               address != lan.broadcast && address != QHostAddress::Broadcast;
    }
    void setupLan(Lan &lan)
    {
        lan.discovery.reset();
        lan.announcer.reset();
        lan.listener.reset();
        lan.retryAt = clock_.elapsed() + lan.source.value("retrySeconds").toInt(5) * 1000;
        lan.state = tr("LAN discovery inactive");
        if (!lan.source.value(QStringLiteral("enabled")).toBool()) {
            return;
        }
        lan.address = QHostAddress(lan.source.value(QStringLiteral("localAddress")).toString());
        lan.interfaceIndex = 0;
        for (const auto &interface : QNetworkInterface::allInterfaces()) {
            if (!interface.flags().testFlag(QNetworkInterface::IsUp)) {
                continue;
            }
            for (const auto &entry : interface.addressEntries()) {
                if (entry.ip() == lan.address) {
                    lan.interfaceIndex = static_cast<uint>(interface.index());
                    lan.prefix = entry.prefixLength();
                    lan.broadcast = entry.broadcast();
                }
            }
        }
        if (!lan.interfaceIndex) {
            lan.state = tr("LAN discovery requires an active, non-loopback local IPv4 address.");
            logMessage(
                Severity::Warning, Urgency::Attention, QStringLiteral("swak.servers"), lan.state);
            return;
        }
        lan.state = tr("Cannot start discovery on the selected LAN interface.");
        if (lan.prefix < 1 || lan.prefix > 30 || lan.broadcast.isNull()) {
            return;
        }
        lan.listener = std::make_unique<QTcpServer>();
        lan.listener->setMaxPendingConnections(MaxPeers);
        if (!lan.listener->listen(lan.address, 0)) {
            lan.listener.reset();
            return;
        }
        lan.discovery = std::make_unique<QUdpSocket>();
        if (!lan.discovery->bind(QHostAddress::AnyIPv4,
                                 DiscoveryPort,
                                 QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            lan.discovery.reset();
            lan.listener.reset();
            return;
        }
        lan.discovery->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 32 * 1024);
        lan.announcer = std::make_unique<QUdpSocket>();
        const int ttl = 1;
        if (!lan.announcer->bind(lan.address, 0) ||
            ::setsockopt(static_cast<int>(lan.announcer->socketDescriptor()),
                         IPPROTO_IP,
                         IP_TTL,
                         &ttl,
                         sizeof(ttl)) != 0) {
            lan.discovery.reset();
            lan.announcer.reset();
            lan.listener.reset();
            return;
        }
        lan.state = tr("LAN discovery active");
        lan.nextAnnounce = 0;
    }
    void discover(Lan &lan)
    {
        if (!lan.discovery) {
            return;
        }
        const auto now = clock_.elapsed();
        if (now >= lan.nextAnnounce) {
            lan.announcer->writeDatagram(lanAnnouncement(localId_, lan.listener->serverPort()),
                                         lan.broadcast,
                                         DiscoveryPort);
            lan.nextAnnounce = now + 5000;
        }
        for (int i = 0; i < 16 && lan.discovery->hasPendingDatagrams(); ++i) {
            if (lan.discovery->pendingDatagramSize() > 512 ||
                lan.discovery->pendingDatagramSize() <= 0) {
                lan.discovery->readDatagram(nullptr, 0);
                continue;
            }
            const auto datagram = lan.discovery->receiveDatagram(512);
            const auto from = datagram.senderAddress();
            const auto bytes = datagram.data();
            if (bytes.isEmpty() || datagram.interfaceIndex() != lan.interfaceIndex ||
                !onLink(lan, from)) {
                continue;
            }
            const auto peer = parseLanAnnouncement(bytes, from, lan.address, lan.prefix);
            if (!peer || peer->id == localId_) {
                continue;
            }
            const auto &id = peer->id;
            const auto port = peer->port;
            if (!lan.peers.contains(id) && lan.peers.size() >= MaxPeers) {
                continue;
            }
            const auto previous = lan.peers.constFind(id);
            if (previous != lan.peers.cend() &&
                (previous->address != from || previous->port != port)) {
                std::erase_if(sessions_, [&lan, &id](const auto &session) {
                    return session->lanId == lan.id && session->peerId == id;
                });
            }
            lan.peers.insert(id, {from, static_cast<quint16>(port), now});
            if (localId_ < id && std::none_of(
                                     sessions_.begin(),
                                     sessions_.end(),
                                     [&id, &lan](const auto &s) {
                                         return s->lanId == lan.id && s->peerId == id;
                                     })) {
                addPeer(lan, id, from, static_cast<quint16>(port), {});
            }
        }
        for (int i = 0; i < MaxPeers && lan.listener->hasPendingConnections(); ++i) {
            std::unique_ptr<QTcpSocket> socket(lan.listener->nextPendingConnection());
            if (!socket || !onLink(lan, socket->peerAddress())) {
                continue;
            }
            for (auto it = lan.peers.begin(); it != lan.peers.end(); ++it) {
                if (it->address == socket->peerAddress() && it.key() < localId_ &&
                    now - it->seen <= 15000 &&
                    std::none_of(sessions_.begin(), sessions_.end(), [&it, &lan](const auto &s) {
                        return s->lanId == lan.id && s->peerId == it.key();
                    })) {
                    addPeer(lan, it.key(), it->address, it->port, std::move(socket));
                    break;
                }
            }
        }
        for (auto it = lan.peers.begin(); it != lan.peers.end();) {
            if (now - it->seen > 20000) {
                it = lan.peers.erase(it);
            } else {
                ++it;
            }
        }
        std::erase_if(sessions_, [this, &lan](const auto &s) {
            return s->peer && s->lanId == lan.id &&
                   (!lan.peers.contains(s->peerId) || (!s->tcp && s->peerId < localId_));
        });
    }
    void addPeer(const Lan &lan,
                 const QString &id,
                 const QHostAddress &address,
                 quint16 port,
                 std::unique_ptr<QTcpSocket> socket)
    {
        if (std::count_if(sessions_.begin(), sessions_.end(), [](const auto &session) {
                return session->peer;
            }) >= MaxPeers) {
            return;
        }
        auto s = std::make_unique<Session>();
        s->id = QStringLiteral("LAN/") + lan.id + '/' + id;
        s->lanId = lan.id;
        s->lanSource = lan.source;
        s->peerId = id;
        s->peer = true;
        s->source = lan.source;
        s->source.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        s->source.insert(QStringLiteral("host"), address.toString());
        s->source.insert(QStringLiteral("port"), port);
        if (socket) {
            socket->setParent(nullptr);
            s->tcp = std::move(socket);
            s->started = clock_.elapsed();
            attach(*s);
            connected(*s);
        }
        sessions_.push_back(std::move(s));
    }
    void publish()
    {
        QJsonObject stats;
        for (const auto &s : sessions_) {
            stats.insert(
                s->id,
                QJsonObject{{QStringLiteral("state"), s->state},
                            {QStringLiteral("connected"),
                             s->ready && ((s->tcp && (!s->peer || s->greeted)) || s->quic ||
                                          s->output || s->mesh)},
                            {QStringLiteral("received"), static_cast<double>(s->accepted)},
                            {QStringLiteral("rejected"), static_cast<double>(s->rejected)},
                            {QStringLiteral("control"), static_cast<double>(s->controls)},
                            {QStringLiteral("sent"), static_cast<double>(s->sent)},
                            {QStringLiteral("binary"), static_cast<double>(s->binaryDatagrams)},
                            {QStringLiteral("mesh"), s->mesh},
                            {QStringLiteral("output"), s->output}});
        }
        for (const auto &lan : lans_) {
            stats.insert(lan->id,
                         QJsonObject{{"state", lan->state},
                                     {"lan", true},
                                     {"address", lan->address.toString()},
                                     {"peers", lan->peers.size()}});
            for (auto it = lan->peers.begin(); it != lan->peers.end(); ++it) {
                const auto key = QStringLiteral("LAN/") + lan->id + '/' + it.key();
                auto detail = stats.value(key).toObject();
                if (detail.isEmpty()) {
                    detail.insert("state", tr("Discovered"));
                }
                detail.insert("serverId", lan->id);
                detail.insert("peerId", it.key());
                detail.insert("address", it->address.toString());
                detail.insert("port", it->port);
                stats.insert(key, detail);
            }
        }
        const QMutexLocker lock(&mailbox_->mutex);
        stats.insert(QStringLiteral("dropped"), static_cast<double>(mailbox_->dropped));
        mailbox_->stats = stats;
    }
    void tick()
    {
        std::deque<QByteArray> outbound;
        {
            const QMutexLocker lock(&mailbox_->mutex);
            outbound.swap(mailbox_->outbound);
        }
        for (const auto &payload : outbound) {
            send(payload, generation_);
        }
        QElapsedTimer discoveryBudget;
        discoveryBudget.start();
        for (size_t n = 0; n < lans_.size() && discoveryBudget.elapsed() < 2; ++n) {
            if (lanCursor_ >= lans_.size()) {
                lanCursor_ = 0;
            }
            const auto &lan = lans_[lanCursor_++];
            if (!lan->discovery && clock_.elapsed() >= lan->retryAt) {
                setupLan(*lan);
            }
            discover(*lan);
        }
        QElapsedTimer budget;
        budget.start();
        const auto count = sessions_.size();
        for (size_t n = 0; n < count * 8 && budget.elapsed() < 4; ++n) {
            if (cursor_ >= count) {
                cursor_ = 0;
            }
            auto &s = *sessions_[cursor_++];
            const auto now = clock_.elapsed();
            if (!s.tcp && !s.udp && !s.quic && now >= s.next) {
                start(s);
            }
            // An address that does not answer gets a few seconds before the next one.
            if (s.quic && !s.ready && !s.quicAddresses.isEmpty() &&
                now - s.quicAttempt > QuicAddressTimeoutMs) {
                nextQuicAddress(s);
            }
            if ((s.tcp || s.quic) && !s.ready &&
                now - s.started >
                    s.source.value(QStringLiteral("timeoutSeconds")).toInt(15) * 1000) {
                fail(s,
                     tr("Connection timed out after %1 seconds. Make sure that the address, port, "
                        "protocol, and firewall rules match the server.")
                         .arg(s.source.value(QStringLiteral("timeoutSeconds")).toInt(15)));
            }
            receive(s);
        }
        if (clock_.elapsed() - lastPublish_ >= 500) {
            publish();
            lastPublish_ = clock_.elapsed();
        }
    }
    QJsonObject sources_;
    std::vector<std::unique_ptr<Lan>> lans_;
    std::shared_ptr<ServerMailbox> mailbox_;
    std::vector<std::unique_ptr<Session>> sessions_;
    QTimer *timer_ = nullptr;
    QElapsedTimer clock_;
    quint64 generation_ = 0;
    size_t cursor_ = 0, lanCursor_ = 0;
    qint64 lastPublish_ = 0;
    const QString localId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
};
Servers::Servers(Store &store, QObject *parent)
    : QObject(parent), store_(store), mailbox_(std::make_shared<ServerMailbox>())
{
    worker_ = new ServerWorker(mailbox_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.start();
    timer_.setInterval(33);
    connect(&timer_, &QTimer::timeout, this, &Servers::collect);
    timer_.start();
}
Servers::~Servers()
{
    timer_.stop();
    thread_.quit();
    thread_.wait();
}
QList<SettingField> Servers::settingsFields() const
{
    return {};
}
QJsonObject Servers::savePreferences() const
{
    return {{QStringLiteral("servers"), sources_}, {QStringLiteral("serverSchemaVersion"), 2}};
}
void Servers::restorePreferences(const QJsonObject &values)
{
    sources_ = {};
    if (values.contains(QStringLiteral("servers"))) {
        const auto entries = values.value(QStringLiteral("servers")).toObject();
        for (auto it = entries.begin(); it != entries.end() && sources_.size() < MaxServers; ++it) {
            if (it.key() != QLatin1String("legacy-udp") && safe(it.key(), 64) &&
                it.value().isObject()) {
                sources_.insert(it.key(), it.value());
            }
        }
    }
    const auto oldLan = values.value("lan").toObject();
    if (values.value("serverSchemaVersion").toInt() < 2 && oldLan.value("enabled").toBool() &&
        sources_.size() < MaxServers) {
        auto source = serverDefaults();
        source.insert("name", tr("Local LAN"));
        source.insert("protocol", "lan");
        source.insert("localAddress", oldLan.value("address"));
        source.insert("enabled", true);
        auto id = QStringLiteral("migrated-lan");
        for (int suffix = 1; sources_.contains(id); ++suffix) {
            id = QStringLiteral("migrated-lan-%1").arg(suffix);
        }
        sources_.insert(id, source);
    }
    configure();
    emit changed();
}
QString Servers::put(const QString &id, const QJsonObject &source)
{
    if (!safe(id, 64) || (!sources_.contains(id) && sources_.size() >= MaxServers)) {
        return tr("Use at most 16 servers with valid identifiers.");
    }
    const auto error = validateServer(source);
    if (!error.isEmpty()) {
        return error;
    }
    sources_.insert(id, source);
    configure();
    emit changed();
    return {};
}
void Servers::remove(const QString &id)
{
    if (sources_.contains(id)) {
        sources_.remove(id);
        configure();
        emit changed();
    }
}
void Servers::reconnect()
{
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_] {
            worker->reconnect();
        },
        Qt::QueuedConnection);
}
void Servers::setSenderIdentity(const QString &uid, const QString &callsign)
{
    const QJsonObject sender{{QStringLiteral("senderUid"), uid.left(128)},
                             {QStringLiteral("senderCallsign"), callsign.left(128)}};
    if (sender == sender_) {
        return;
    }
    sender_ = sender;
    // Only entries with a TAK login carry the sender, so other connections stay open.
    if (std::any_of(sources_.begin(), sources_.end(), [](const QJsonValue &value) {
            return !value.toObject().value(QStringLiteral("authUser")).toString().isEmpty();
        })) {
        configure();
    }
}
void Servers::configure()
{
    ++generation_;
    stats_ = {};
    auto sourceValues = sources_;
    for (auto it = sourceValues.begin(); it != sourceValues.end(); ++it) {
        auto source = it.value().toObject();
        if (!source.value(QStringLiteral("authUser")).toString().isEmpty()) {
            for (auto field = sender_.begin(); field != sender_.end(); ++field) {
                source.insert(field.key(), field.value());
            }
            it.value() = source;
        }
    }
    const auto generation = generation_;
    QMetaObject::invokeMethod(
        worker_,
        [worker = worker_, sourceValues, generation] {
            worker->configure(sourceValues, generation);
        },
        Qt::QueuedConnection);
}
void Servers::collect()
{
    QList<Event> events;
    QJsonObject stats;
    {
        const QMutexLocker lock(&mailbox_->mutex);
        if (mailbox_->generation != generation_) {
            return;
        }
        for (int i = 0; i < 512 && !mailbox_->order.empty(); ++i) {
            events.append(mailbox_->events.take(mailbox_->order.front()));
            mailbox_->order.pop_front();
        }
        stats = mailbox_->stats;
    }
    if (!events.isEmpty()) {
        store_.acceptBatch(events);
    }
    if (stats_ != stats) {
        stats_ = stats;
        emit statusChanged();
    }
}
QJsonObject Servers::statistics() const
{
    return stats_;
}
void Servers::setServerInfo(const QString &id, const QString &text)
{
    if (info_.value(id) == text) {
        return;
    }
    if (text.isEmpty()) {
        info_.remove(id);
    } else {
        info_.insert(id, text.left(512));
    }
    emit statusChanged();
}
QString Servers::status(const QString &id) const
{
    const auto base = connectionStatus(id);
    const auto extra = info_.value(id);
    return extra.isEmpty() ? base : base + QLatin1Char('\n') + extra;
}
QString Servers::connectionStatus(const QString &id) const
{
    const auto entry = sources_.value(id).toObject();
    if (!entry.isEmpty() && !validateServer(entry).isEmpty()) {
        return tr("Invalid or unavailable server configuration.");
    }
    const auto state = stats_.value(id).toObject();
    if (state.value("lan").toBool()) {
        return tr("%1. Interface: %2. Peers: %3.")
            .arg(state.value("state").toString(),
                 entry.value("localAddress").toString(),
                 QString::number(state.value("peers").toInt()));
    }
    if (state.isEmpty()) {
        return entry.value(QStringLiteral("enabled")).toBool() ? tr("Connecting")
                                                               : tr("Disconnected");
    }
    if (state.value(QStringLiteral("mesh")).toBool()) {
        return tr("%1. Received: %2. Sent: %3. Rejected: %4. TAK Protocol 1 datagrams: %5.")
            .arg(state.value(QStringLiteral("state")).toString(),
                 QString::number(state.value(QStringLiteral("received")).toDouble(), 'f', 0),
                 QString::number(state.value(QStringLiteral("sent")).toDouble(), 'f', 0),
                 QString::number(state.value(QStringLiteral("rejected")).toDouble(), 'f', 0),
                 QString::number(state.value(QStringLiteral("binary")).toDouble(), 'f', 0));
    }
    if (state.value(QStringLiteral("output")).toBool()) {
        return tr("%1. Sent: %2.")
            .arg(state.value(QStringLiteral("state")).toString(),
                 QString::number(state.value(QStringLiteral("sent")).toDouble(), 'f', 0));
    }
    return tr("%1. Received: %2. Rejected: %3. Queue drops: %4.")
        .arg(state.value(QStringLiteral("state")).toString(),
             QString::number(state.value(QStringLiteral("received")).toDouble(), 'f', 0),
             QString::number(state.value(QStringLiteral("rejected")).toDouble(), 'f', 0),
             QString::number(stats_.value(QStringLiteral("dropped")).toDouble(), 'f', 0));
}
QString Servers::send(const Event &event)
{
    const auto bytes = encodeCot(event);
    if (bytes.isEmpty()) {
        return tr("The CoT event is invalid.");
    }
    bool connected = false;
    for (const auto &value : stats_) {
        if (value.toObject().value(QStringLiteral("connected")).toBool()) {
            connected = true;
        }
    }
    if (!connected) {
        return tr("No stream server, UDP output, or LAN peer is connected.");
    }
    const QMutexLocker lock(&mailbox_->mutex);
    if (mailbox_->generation != generation_ || mailbox_->outbound.size() >= 32) {
        return tr("The outgoing CoT queue is full or restarting.");
    }
    mailbox_->outbound.push_back(bytes);
    return {};
}
} // namespace swak
