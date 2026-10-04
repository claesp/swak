#include "core/servers.h"

#include "core/cot.h"
#include "core/cot_stream.h"
#include "core/files.h"
#include "core/lan_protocol.h"
#include "core/logging.h"
#include "core/store.h"

#include <QBuffer>
#include <QCollator>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
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

#include <algorithm>
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
constexpr int QueueLimit = 2048;
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
        choiceSetting(
            QStringLiteral("protocol"),
            tr("Protocol"),
            {tr("TLS CoT stream"), tr("TCP CoT stream"), tr("UDP input"), tr("Local LAN")},
            {QStringLiteral("tls"),
             QStringLiteral("stcp"),
             QStringLiteral("udp"),
             QStringLiteral("lan")}),
        choiceSetting(
            QStringLiteral("localAddress"), tr("Interface"), interfaceLabels, interfaceValues),
        numberSetting(QStringLiteral("port"), tr("Port"), 1, 65535),
        textSetting(QStringLiteral("ca"), tr("CA certificate (PEM)"), 4096),
        textSetting(QStringLiteral("certificate"), tr("Client certificate (PEM or PKCS12)"), 4096),
        textSetting(QStringLiteral("key"), tr("Private key (PEM, for PEM certificate)"), 4096),
        textSetting(QStringLiteral("passwordEnv"), tr("Password environment variable"), 128),
        numberSetting(QStringLiteral("retrySeconds"), tr("Retry interval (seconds)"), 1, 60),
        numberSetting(QStringLiteral("timeoutSeconds"),
                      tr("Connection and frame timeout (seconds)"),
                      2,
                      120)};
    for (auto &field : fields) {
        if (field.path == "host" || field.path == "port") {
            field.enabledBy = QStringLiteral("protocol");
            field.enabledValue = QJsonArray{"tls", "stcp", "udp"};
        }
        if (QStringList{QStringLiteral("ca"),
                        QStringLiteral("certificate"),
                        QStringLiteral("key"),
                        QStringLiteral("passwordEnv")}
                .contains(field.path)) {
            field.enabledBy = QStringLiteral("protocol");
            field.enabledValue = QStringLiteral("tls");
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
        protocol != QLatin1String("udp") && protocol != QLatin1String("lan")) {
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
    }
    for (const auto &[key, low, high] :
         {std::tuple{"port", 1, 65535}, {"retrySeconds", 1, 60}, {"timeoutSeconds", 2, 120}}) {
        const auto n = v.value(QLatin1String(key));
        if (!n.isDouble() || n.toDouble() != n.toInt() || n.toInt() < low || n.toInt() > high) {
            return tr("A server number is outside its allowed range.");
        }
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
        for (const auto &s : sessions_) {
            if (s->tcp && s->ready && (!s->peer || s->greeted) &&
                s->tcp->bytesToWrite() + payload.size() <= 256 * 1024) {
                s->tcp->write(payload);
            }
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
        CotStream stream;
        QString state;
        quint64 accepted = 0, rejected = 0, controls = 0;
        qint64 next = 0, started = 0, partial = 0, lastRead = 0, lastPing = 0;
        int failures = 0;
        bool ready = false, peer = false, greeted = false;
        QByteArray greeting;
        QString peerId, lanId;
        QJsonObject lanSource;
    };
    struct Peer
    {
        QHostAddress address;
        quint16 port;
        qint64 seen;
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
        s.ready = false;
        s.stream.reset();
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
        state(s,
              tr("%1 [%2]:%3: %4 Retry in %5 seconds.")
                  .arg(label, s.source.value(QStringLiteral("host")).toString())
                  .arg(s.source.value(QStringLiteral("port")).toInt())
                  .arg(text)
                  .arg(delay / 1000),
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
        const auto caPath = source.value(QStringLiteral("ca")).toString();
        if (!caPath.isEmpty()) {
            const auto ca = readRegularFile(caPath, 64 * 1024);
            if (!ca.error.isEmpty()) {
                return false;
            }
            const auto certs = QSslCertificate::fromData(ca.bytes);
            if (certs.isEmpty()) {
                return false;
            }
            configuration.setCaCertificates(certs);
        }
        const auto certPath = source.value(QStringLiteral("certificate")).toString();
        if (!certPath.isEmpty()) {
            const auto input = readRegularFile(certPath, 64 * 1024);
            if (!input.error.isEmpty()) {
                return false;
            }
            const auto env = source.value(QStringLiteral("passwordEnv")).toString().toLatin1();
            QByteArray password = env.isEmpty() ? QByteArray{} : qgetenv(env.constData());
            if (password.size() > 1024) {
                return false;
            }
            QSslKey key;
            QList<QSslCertificate> chain;
            const auto keyPath = source.value(QStringLiteral("key")).toString();
            if (keyPath.isEmpty()) {
                QBuffer buffer;
                buffer.setData(input.bytes);
                buffer.open(QIODevice::ReadOnly);
                QSslCertificate cert;
                QList<QSslCertificate> extra;
                if (!QSslCertificate::importPkcs12(&buffer, &key, &cert, &extra, password)) {
                    return false;
                }
                chain.append(cert);
                chain.append(extra);
            } else {
                chain = QSslCertificate::fromData(input.bytes);
                const auto keyInput = readRegularFile(keyPath, 64 * 1024);
                if (!keyInput.error.isEmpty()) {
                    return false;
                }
                for (const auto algorithm : {QSsl::Rsa, QSsl::Ec, QSsl::Dsa}) {
                    key = QSslKey(keyInput.bytes, algorithm, QSsl::Pem, QSsl::PrivateKey, password);
                    if (!key.isNull()) {
                        break;
                    }
                }
            }
            password.fill('\0');
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
        }
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
        const auto result = decodeCot(frame);
        if (!result.event) {
            ++s.rejected;
            return;
        }
        if (result.event->type.startsWith(QLatin1String("t-x-takp-")) ||
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
        mailbox_->events.insert(result.event->uid, *result.event);
        ++s.accepted;
    }
    void receive(Session &s)
    {
        if (s.udp) {
            for (int i = 0; i < 32 && s.udp->hasPendingDatagrams(); ++i) {
                const auto size = s.udp->pendingDatagramSize();
                if (size <= 0 || size > MaxMessageBytes) {
                    s.udp->readDatagram(nullptr, 0);
                    ++s.rejected;
                    continue;
                }
                QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
                if (s.udp->readDatagram(bytes.data(), bytes.size()) == size) {
                    decode(s, bytes);
                } else {
                    ++s.rejected;
                }
            }
            return;
        }
        if (!s.tcp || !s.ready) {
            return;
        }
        if (s.tcp->bytesAvailable()) {
            auto bytes = s.tcp->read(MaxMessageBytes);
            s.lastRead = clock_.elapsed();
            if (s.peer && !s.greeted) {
                const auto end = bytes.indexOf('\n');
                s.greeting.append(end < 0 ? bytes : bytes.left(end));
                if (s.greeting.size() > 47) {
                    fail(s, tr("Invalid LAN greeting."));
                    return;
                }
                if (end < 0) {
                    return;
                }
                if (s.greeting != "SWAK-COT/1 " + s.peerId.toLatin1()) {
                    fail(s, tr("LAN identity does not match discovery."));
                    return;
                }
                s.greeted = true;
                state(s, tr("Connected"));
                s.greeting.clear();
                bytes.remove(0, end + 1);
            }
            const auto frames = s.stream.feed(bytes);
            if (!frames.isEmpty()) {
                s.partial = 0;
            }
            for (const auto &frame : frames) {
                decode(s, frame);
            }
            if (s.stream.failed()) {
                ++s.rejected;
                fail(s, tr("Invalid or oversized CoT stream."));
                return;
            }
            if (!s.stream.pending()) {
                s.partial = 0;
            } else if (!s.partial) {
                s.partial = clock_.elapsed();
            }
        }
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
        if (now - s.lastPing > 30000 && s.tcp->bytesToWrite() < 64 * 1024) {
            Event ping;
            ping.uid = localId_;
            ping.type = QStringLiteral("t-x-c-t");
            ping.how = QStringLiteral("m-g");
            ping.time = ping.start = QDateTime::currentDateTimeUtc();
            ping.stale = ping.time.addSecs(60);
            s.tcp->write(encodeCot(ping));
            s.lastPing = now;
        }
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
                QJsonObject{
                    {QStringLiteral("state"), s->state},
                    {QStringLiteral("connected"), s->ready && s->tcp && (!s->peer || s->greeted)},
                    {QStringLiteral("received"), static_cast<double>(s->accepted)},
                    {QStringLiteral("rejected"), static_cast<double>(s->rejected)},
                    {QStringLiteral("control"), static_cast<double>(s->controls)}});
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
            if (!s.tcp && !s.udp && now >= s.next) {
                start(s);
            }
            if (s.tcp && !s.ready &&
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
void Servers::configure()
{
    ++generation_;
    stats_ = {};
    const auto sourceValues = sources_;
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
QString Servers::status(const QString &id) const
{
    const auto entry = sources_.value(id).toObject();
    if (!entry.isEmpty() && !validateServer(entry).isEmpty()) {
        return tr("Invalid or unavailable server configuration.");
    }
    const auto state = stats_.value(id).toObject();
    if (state.value("lan").toBool()) {
        return tr("%1. Interface: %2. Peers: %3.")
            .arg(state.value("state").toString(), entry.value("localAddress").toString())
            .arg(state.value("peers").toInt());
    }
    if (state.isEmpty()) {
        return entry.value(QStringLiteral("enabled")).toBool() ? tr("Connecting")
                                                               : tr("Disconnected");
    }
    return tr("%1. Received: %2. Rejected: %3. Queue drops: %4.")
        .arg(state.value(QStringLiteral("state")).toString())
        .arg(state.value(QStringLiteral("received")).toDouble(), 0, 'f', 0)
        .arg(state.value(QStringLiteral("rejected")).toDouble(), 0, 'f', 0)
        .arg(stats_.value(QStringLiteral("dropped")).toDouble(), 0, 'f', 0);
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
        return tr("No stream server or LAN peer is connected.");
    }
    const QMutexLocker lock(&mailbox_->mutex);
    if (mailbox_->generation != generation_ || mailbox_->outbound.size() >= 32) {
        return tr("The outgoing CoT queue is full or restarting.");
    }
    mailbox_->outbound.push_back(bytes);
    return {};
}
} // namespace swak
