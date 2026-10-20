#include "core/chat.h"
#include "core/citrap.h"
#include "core/contacts.h"
#include "core/cot.h"
#include "core/cot_detail.h"
#include "core/cot_stream.h"
#include "core/data.h"
#include "core/data_feeds.h"
#include "core/data_package.h"
#include "core/emergency.h"
#include "core/excheck.h"
#include "core/file_shares.h"
#include "core/icon_cache.h"
#include "core/identity.h"
#include "core/lan_protocol.h"
#include "core/map_selection.h"
#include "core/maps.h"
#include "core/marker_collections.h"
#include "core/measure.h"
#include "core/missions.h"
#include "core/network_client.h"
#include "core/object_sharing.h"
#include "core/package_import.h"
#include "core/polygon.h"
#include "core/projection.h"
#include "core/quic_stream.h"
#include "core/server_browser.h"
#include "core/server_layers.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_channels.h"
#include "core/tak_drawings.h"
#include "core/tak_enrollment.h"
#include "core/tak_protocol.h"
#include "core/tak_server_api.h"
#include "core/tak_shapes.h"
#include "core/track_history.h"
#include "core/user_objects.h"
#include "core/video_sources.h"
#include "https_harness.h"

#include <QAbstractItemModelTester>
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <arpa/inet.h>
#include <functional>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <optional>
#include <tak_protocol.pb.h>
using namespace swak;
static Event sampleEvent(const QString &id = QStringLiteral("test"))
{
    Event e;
    e.uid = id;
    e.callsign = QStringLiteral("Test contact");
    e.type = QStringLiteral("a-f-G-U");
    e.how = QStringLiteral("m-g");
    e.time = e.start = QDateTime::currentDateTimeUtc();
    e.stale = e.time.addSecs(120);
    return e;
}
static QJsonObject vertices(std::initializer_list<std::pair<double, double>> positions)
{
    QJsonArray array;
    for (const auto &[latitude, longitude] : positions) {
        const auto point = project(latitude, longitude);
        array.append(QJsonArray{point.x(), point.y()});
    }
    return {{QStringLiteral("vertices"), array}};
}
static UserObject localObject(const QString &id, const QString &type, const QJsonObject &geometry)
{
    UserObject item;
    item.id = id;
    item.type = type;
    item.title = QStringLiteral("Object ") + id;
    item.geometry = geometry;
    item.color = QStringLiteral("#ff0000");
    return item;
}
static QJsonObject markerGeometry(double latitude, double longitude)
{
    const auto point = project(latitude, longitude);
    return {{QStringLiteral("x"), point.x()}, {QStringLiteral("y"), point.y()}};
}
// Links without the sender link.
static QList<CotLink> shapeLinks(const Event &event)
{
    auto result = links(event);
    result.removeIf([](const CotLink &link) {
        return link.relation == QStringLiteral("p-p");
    });
    return result;
}
// A GeoChat message in the format that ATAK sends.
static Event geoChat(const QString &messageId,
                     const QString &senderUid,
                     const QString &roomId,
                     const QString &text,
                     const QString &parent = QStringLiteral("RootContactGroup"))
{
    auto event = sampleEvent(QStringLiteral("GeoChat.") + senderUid + QLatin1Char('.') + messageId);
    event.type = QStringLiteral("b-t-f");
    event.how = QStringLiteral("h-e");
    event.callsign.clear();
    const auto escaped = text.toHtmlEscaped();
    event.detail =
        QStringLiteral(
            "<__chat parent=\"%1\" groupOwner=\"false\" messageId=\"%2\" chatroom=\"%3\" "
            "id=\"%3\" senderCallsign=\"Echo\"><chatgrp uid0=\"%4\" uid1=\"%3\" "
            "id=\"%3\"/></__chat><link uid=\"%4\" type=\"a-f-G-U-C\" relation=\"p-p\"/>"
            "<remarks source=\"BAO.F.ATAK.%4\" to=\"%3\">%5</remarks>")
            .arg(parent, messageId, roomId, senderUid, escaped);
    return event;
}
// Builds a zip archive with raw entries, including entries that SWAK must reject.
struct RawEntry
{
    QByteArray path;
    QByteArray bytes;
    bool symlink = false;
};
static QByteArray rawZip(const QList<RawEntry> &entries)
{
    QByteArray output(4 * 1024 * 1024, '\0');
    size_t used = 0;
    archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    archive_write_open_memory(writer, output.data(), static_cast<size_t>(output.size()), &used);
    for (const auto &entry : entries) {
        archive_entry *header = archive_entry_new();
        archive_entry_set_pathname(header, entry.path.constData());
        if (entry.symlink) {
            archive_entry_set_filetype(header, AE_IFLNK);
            archive_entry_set_symlink(header, "/etc/passwd");
            archive_entry_set_size(header, 0);
        } else {
            archive_entry_set_filetype(header, AE_IFREG);
            archive_entry_set_size(header, entry.bytes.size());
        }
        archive_entry_set_perm(header, 0644);
        archive_write_header(writer, header);
        if (!entry.symlink) {
            archive_write_data(
                writer, entry.bytes.constData(), static_cast<size_t>(entry.bytes.size()));
        }
        archive_entry_free(header);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    output.truncate(static_cast<qsizetype>(used));
    return output;
}
static QJsonObject source(quint16 port)
{
    auto v = serverDefaults();
    v.insert(QStringLiteral("name"), QStringLiteral("Test"));
    v.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    v.insert(QStringLiteral("port"), port);
    v.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
    v.insert(QStringLiteral("enabled"), true);
    v.insert(QStringLiteral("retrySeconds"), 1);
    v.insert(QStringLiteral("timeoutSeconds"), 2);
    return v;
}
class TlsHarness final : public QTcpServer
{
public:
    QSslCertificate certificate;
    QSslKey key;
    QByteArray payload;
    QByteArray received;
    bool requireClient = false;
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        auto configuration = socket->sslConfiguration();
        configuration.setCaCertificates({certificate});
        configuration.setPeerVerifyMode(requireClient ? QSslSocket::VerifyPeer
                                                      : QSslSocket::VerifyNone);
        socket->setSslConfiguration(configuration);
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        connect(socket, &QSslSocket::encrypted, socket, [this, socket] {
            socket->write(payload);
        });
        connect(socket, &QSslSocket::readyRead, socket, [this, socket] {
            received.append(socket->readAll().left(64 * 1024));
        });
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }
};
// A QUIC server for TAK stream tests on the OpenSSL 3.5 listener API. A timer in the test
// thread drives it without blocking. It accepts one connection and one client stream.
class QuicHarness final : public QObject
{
public:
    QByteArray payload;
    QByteArray received;
    bool requireClient = false;
    bool sawClientCertificate = false;
    // Listen on ::1 instead of 127.0.0.1. A nonzero port selects the port.
    bool ipv6 = false;
    // Refuse every ALPN offer, so the handshake fails at once.
    bool rejectAlpn = false;
    quint16 port = 0;
    ~QuicHarness() override
    {
        SSL_free(stream_);
        SSL_free(connection_);
        SSL_free(listener_);
        SSL_CTX_free(context_);
    }
    bool start()
    {
        const auto cert = QFINDTESTDATA("fixtures/map-test-cert.pem").toUtf8();
        const auto key = QFINDTESTDATA("fixtures/map-test-key.pem").toUtf8();
        context_ = SSL_CTX_new(OSSL_QUIC_server_method());
        if (!context_ || SSL_CTX_use_certificate_chain_file(context_, cert.constData()) != 1 ||
            SSL_CTX_use_PrivateKey_file(context_, key.constData(), SSL_FILETYPE_PEM) != 1) {
            return false;
        }
        SSL_CTX_set_alpn_select_cb(
            context_,
            [](SSL *,
               const unsigned char **out,
               unsigned char *outLength,
               const unsigned char *in,
               unsigned int inLength,
               void *argument) {
                if (static_cast<const QuicHarness *>(argument)->rejectAlpn) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                static const unsigned char supported[] = {
                    9, 't', 'a', 'k', 's', 't', 'r', 'e', 'a', 'm'};
                unsigned char *selected = nullptr;
                if (SSL_select_next_proto(
                        &selected, outLength, supported, sizeof(supported), in, inLength) !=
                    OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                *out = selected;
                return SSL_TLSEXT_ERR_OK;
            },
            this);
        if (requireClient) {
            SSL_CTX_set_verify(
                context_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
            SSL_CTX_load_verify_locations(context_, cert.constData(), nullptr);
        }
        const int socket = BIO_socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, 0);
        sockaddr_storage address{};
        socklen_t length = 0;
        if (ipv6) {
            auto *v6 = reinterpret_cast<sockaddr_in6 *>(&address);
            v6->sin6_family = AF_INET6;
            v6->sin6_addr = in6addr_loopback;
            v6->sin6_port = htons(port);
            length = sizeof(sockaddr_in6);
        } else {
            auto *v4 = reinterpret_cast<sockaddr_in *>(&address);
            v4->sin_family = AF_INET;
            v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            v4->sin_port = htons(port);
            length = sizeof(sockaddr_in);
        }
        if (socket < 0 || ::bind(socket, reinterpret_cast<sockaddr *>(&address), length) != 0 ||
            ::getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) != 0 ||
            BIO_socket_nbio(socket, 1) != 1) {
            if (socket >= 0) {
                BIO_closesocket(socket);
            }
            return false;
        }
        port = ntohs(ipv6 ? reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port
                          : reinterpret_cast<sockaddr_in *>(&address)->sin_port);
        listener_ = SSL_new_listener(context_, 0);
        if (!listener_ || SSL_set_fd(listener_, socket) != 1 ||
            SSL_set_blocking_mode(listener_, 0) != 1 || SSL_listen(listener_) != 1) {
            return false;
        }
        timer_.setInterval(2);
        connect(&timer_, &QTimer::timeout, this, [this] {
            poll();
        });
        timer_.start();
        return true;
    }

private:
    void poll()
    {
        SSL_handle_events(listener_);
        if (!connection_) {
            connection_ = SSL_accept_connection(listener_, SSL_ACCEPT_CONNECTION_NO_BLOCK);
            if (connection_) {
                SSL_set_blocking_mode(connection_, 0);
                SSL_set_default_stream_mode(connection_, SSL_DEFAULT_STREAM_MODE_NONE);
            }
        }
        if (connection_ && !stream_) {
            SSL_handle_events(connection_);
            stream_ = SSL_accept_stream(connection_, SSL_ACCEPT_STREAM_NO_BLOCK);
        }
        if (stream_) {
            char buffer[4096];
            size_t count = 0;
            while (SSL_read_ex(stream_, buffer, sizeof(buffer), &count) == 1) {
                received.append(buffer, static_cast<qsizetype>(count));
            }
            if (!wrote_ && !received.isEmpty()) {
                X509 *peer = SSL_get1_peer_certificate(connection_);
                sawClientCertificate = peer != nullptr;
                X509_free(peer);
                size_t written = 0;
                wrote_ = SSL_write_ex(stream_,
                                      payload.constData(),
                                      static_cast<size_t>(payload.size()),
                                      &written) == 1;
            }
            SSL_handle_events(connection_);
        }
        ERR_clear_error();
    }
    SSL_CTX *context_ = nullptr;
    SSL *listener_ = nullptr;
    SSL *connection_ = nullptr;
    SSL *stream_ = nullptr;
    bool wrote_ = false;
    QTimer timer_;
};
// Signs a base64 DER certificate request with the fixture CA. Returns base64 DER.
static QByteArray signRequest(const QByteArray &base64, QString *subject)
{
    QFile certFile(QFINDTESTDATA("fixtures/map-test-cert.pem")),
        keyFile(QFINDTESTDATA("fixtures/map-test-key.pem"));
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto certPem = certFile.readAll();
    const auto keyPem = keyFile.readAll();
    const auto der = QByteArray::fromBase64(base64);
    const auto *cursor = reinterpret_cast<const unsigned char *>(der.constData());
    X509_REQ *request = d2i_X509_REQ(nullptr, &cursor, static_cast<long>(der.size()));
    BIO *certBio = BIO_new_mem_buf(certPem.constData(), static_cast<int>(certPem.size()));
    BIO *keyBio = BIO_new_mem_buf(keyPem.constData(), static_cast<int>(keyPem.size()));
    X509 *issuer = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    EVP_PKEY *issuerKey = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    QByteArray result;
    if (request && issuer && issuerKey &&
        X509_REQ_verify(request, X509_REQ_get0_pubkey(request)) == 1) {
        char text[256] = {};
        X509_NAME_oneline(X509_REQ_get_subject_name(request), text, sizeof(text));
        *subject = QString::fromUtf8(text);
        X509 *certificate = X509_new();
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 4242);
        X509_set_issuer_name(certificate, X509_get_subject_name(issuer));
        X509_set_subject_name(certificate, X509_REQ_get_subject_name(request));
        X509_gmtime_adj(X509_getm_notBefore(certificate), -60);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
        X509_set_pubkey(certificate, X509_REQ_get0_pubkey(request));
        if (X509_sign(certificate, issuerKey, EVP_sha256()) > 0) {
            const int size = i2d_X509(certificate, nullptr);
            QByteArray out(size, Qt::Uninitialized);
            auto *write = reinterpret_cast<unsigned char *>(out.data());
            i2d_X509(certificate, &write);
            result = out.toBase64();
        }
        X509_free(certificate);
    }
    X509_REQ_free(request);
    X509_free(issuer);
    EVP_PKEY_free(issuerKey);
    BIO_free(certBio);
    BIO_free(keyBio);
    return result;
}
static QByteArray fixtureCertificateBase64()
{
    QFile file(QFINDTESTDATA("fixtures/map-test-cert.pem"));
    return file.open(QIODevice::ReadOnly) ? QSslCertificate(file.readAll()).toDer().toBase64()
                                          : QByteArray{};
}
// A plaintext TAK stream server that offers TAK Protocol version 1 like TAK Server.
class NegotiatingServer final : public QTcpServer
{
public:
    QByteArray received;
    QByteArray afterResponse;
    QPointer<QTcpSocket> client;
    static QByteArray control(const QString &type, const QString &inner)
    {
        const auto now = QDateTime::currentDateTimeUtc();
        return QStringLiteral(
                   "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
                   "<event version='2.0' uid='negotiation' type='%1' time='%2' start='%2' "
                   "stale='%3' how='m-g'><point lat='0.0' lon='0.0' hae='0.0' "
                   "ce='999999' le='999999'/><detail><TakControl>%4</TakControl></detail>"
                   "</event>")
            .arg(type,
                 now.toString(Qt::ISODateWithMs),
                 now.addSecs(60).toString(Qt::ISODateWithMs),
                 inner)
            .toUtf8();
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(descriptor);
        client = socket;
        answered_ = false;
        socket->write(
            control(QStringLiteral("t-x-takp-v"),
                    QStringLiteral("<TakProtocolSupport version='1'/>"
                                   "<TakServerVersionInfo serverVersion='5.7' apiVersion='3'/>")));
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
            received.append(socket->readAll());
            if (!answered_ && received.contains("t-x-takp-q")) {
                answered_ = true;
                socket->write(control(QStringLiteral("t-x-takp-r"),
                                      QStringLiteral("<TakResponse status='true'/>")) +
                              afterResponse);
            }
        });
    }

private:
    bool answered_ = false;
};
class ServerTest final : public QObject
{
    Q_OBJECT
private slots:
    void streamFragments()
    {
        auto e = sampleEvent();
        e.callsign = QString::fromUtf8("Ångström & <tag>");
        auto xml = encodeCot(e);
        xml.replace("</detail>",
                    "<remarks><![CDATA[</event> & >]]></remarks><!-- </event> --></detail>");
        for (qsizetype split = 0; split <= xml.size(); ++split) {
            CotStream stream;
            auto frames = stream.feed(xml.left(split));
            frames.append(stream.feed(xml.mid(split)));
            QVERIFY(!stream.failed());
            QCOMPARE(frames.size(), 1);
            QCOMPARE(decodeCot(frames.first()).event->callsign, e.callsign);
            QVERIFY(!stream.pending());
        }
        CotStream stream;
        QList<QByteArray> frames;
        for (const auto ch : xml) {
            frames.append(stream.feed(QByteArray(1, ch)));
        }
        QCOMPARE(frames.size(), 1);
        QVERIFY(!stream.failed());
        QCOMPARE(stream.feed(xml + "\r\n" + xml).size(), 2);
    }
    void hostileInput()
    {
        for (const auto &payload :
             {QByteArray("<!DOCTYPE event [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><event/>"),
              QByteArray("<wrong/>"),
              QByteArray("\0", 1),
              QByteArray("garbage"),
              QByteArray(MaxMessageBytes + 1, 'x')}) {
            CotStream stream;
            stream.feed(payload);
            QVERIFY(stream.failed());
            stream.reset();
            QCOMPARE(stream.feed(encodeCot(sampleEvent())).size(), 1);
        }
        CotStream stream;
        stream.feed("<event x='");
        stream.feed(QByteArray(MaxMessageBytes - 20, 'x'));
        stream.feed(QByteArray(30, 'x'));
        QVERIFY(stream.failed());
        auto xml = encodeCot(sampleEvent());
        xml.replace("<detail>", "<detail><__group name='a' role='b'/><__group name='a' role='b'/>");
        QVERIFY(!decodeCot(xml).event);
        xml = encodeCot(sampleEvent());
        xml.replace("<detail>", "<detail><?unsafe value?>");
        QVERIFY(!decodeCot(xml).event);
        xml = encodeCot(sampleEvent());
        xml.replace("lat=\"0.00000000\"", "lat=\"NaN\"");
        QVERIFY(!decodeCot(xml).event);
    }
    static NetworkReply fetch(NetworkClient &client, NetworkRequest request, int waitMs = 10000)
    {
        std::optional<NetworkReply> result;
        QObject context;
        client.send(std::move(request), &context, [&result](const NetworkReply &reply) {
            result = reply;
        });
        [[maybe_unused]] const bool answered = QTest::qWaitFor(
            [&result] {
                return result.has_value();
            },
            waitMs);
        NetworkReply missing;
        missing.error = QStringLiteral("no reply");
        return result.value_or(missing);
    }
    static NetworkRequest localRequest(const HttpsHarness &server, const QString &path)
    {
        NetworkRequest request;
        request.url =
            QUrl(QStringLiteral("https://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
        request.expectedHost = QStringLiteral("127.0.0.1");
        request.expectedPort = server.serverPort();
        QFile ca(QFINDTESTDATA("fixtures/map-test-cert.pem"));
        if (ca.open(QIODevice::ReadOnly)) {
            request.tls.ca = ca.readAll();
        }
        return request;
    }
    void networkClient()
    {
        HttpsHarness server;
        QVERIFY(server.load());
        NetworkClient client;
        auto reply = fetch(client, localRequest(server, QStringLiteral("/hello?x=1")));
        QVERIFY2(reply.ok(), qPrintable(reply.error));
        QCOMPARE(reply.body, QByteArray("{\"ok\":true}"));
        QCOMPARE(reply.contentType, QByteArray("application/json"));
        QVERIFY(server.requests.last().startsWith("GET /hello?x=1 HTTP/"));
        QVERIFY(server.requests.last().contains("User-Agent: SWAK/"));
        // An unknown CA fails certificate validation.
        auto untrusted = localRequest(server, QStringLiteral("/"));
        untrusted.tls.ca.clear();
        QVERIFY(fetch(client, untrusted).error.contains(QStringLiteral("TLS")));
        // The client rejects other hosts, ports, and schemes before it connects.
        const auto before = server.requests.size();
        auto other = localRequest(server, QStringLiteral("/"));
        other.expectedHost = QStringLiteral("localhost");
        QVERIFY(fetch(client, other).error.contains(QStringLiteral("configured HTTPS server")));
        other = localRequest(server, QStringLiteral("/"));
        other.expectedPort = 1;
        QVERIFY(!fetch(client, other).error.isEmpty());
        other = localRequest(server, QStringLiteral("/"));
        other.url.setScheme(QStringLiteral("http"));
        QVERIFY(!fetch(client, other).error.isEmpty());
        other = localRequest(server, QStringLiteral("/"));
        other.headers.append(qMakePair(QByteArray("X-Test"), QByteArray("a\r\nInjected: b")));
        QVERIFY(fetch(client, other).error.contains(QStringLiteral("header")));
        other = localRequest(server, QStringLiteral("/"));
        other.method = "TRACE";
        QVERIFY(!fetch(client, other).error.isEmpty());
        QCOMPARE(server.requests.size(), before);
        // A reply above the limit fails. A redirect is not followed.
        server.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 2000\r\n\r\n") +
                   QByteArray(2000, 'x');
        };
        auto limited = localRequest(server, QStringLiteral("/large"));
        limited.maxBytes = 100;
        QVERIFY(fetch(client, limited).error.contains(QStringLiteral("size limit")));
        server.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 302 Found\r\nLocation: https://example.test/\r\n"
                              "Content-Length: 0\r\n\r\n");
        };
        const auto redirects = server.requests.size();
        reply = fetch(client, localRequest(server, QStringLiteral("/moved")));
        QVERIFY(reply.error.isEmpty());
        QCOMPARE(reply.status, 302);
        QVERIFY(!reply.ok());
        QCOMPARE(server.requests.size(), redirects + 1);
        // POST sends the body and headers.
        server.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
        };
        auto post = localRequest(server, QStringLiteral("/submit"));
        post.method = "POST";
        post.body = "{\"name\":\"value\"}";
        post.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
        reply = fetch(client, post);
        QCOMPARE(reply.status, 201);
        QVERIFY(server.requests.last().startsWith("POST /submit HTTP/"));
        QVERIFY(server.requests.last().endsWith("{\"name\":\"value\"}"));
        QVERIFY(server.requests.last().contains("Content-Type: application/json"));
        // A silent server times out. Cancel and a destroyed context stop the callback.
        server.hold = true;
        auto slow = localRequest(server, QStringLiteral("/slow"));
        slow.timeoutMs = 500;
        slow.connectTimeoutMs = 400;
        QVERIFY(fetch(client, slow).error.contains(QStringLiteral("in time")));
        bool called = false;
        {
            QObject context;
            const auto id = client.send(localRequest(server, QStringLiteral("/cancel")),
                                        &context,
                                        [&called](const NetworkReply &) {
                                            called = true;
                                        });
            QVERIFY(id);
            client.cancel(id);
        }
        auto *context = new QObject;
        client.send(
            localRequest(server, QStringLiteral("/gone")), context, [&called](const auto &) {
                called = true;
            });
        delete context;
        QVERIFY(QTest::qWaitFor(
            [&client] {
                return client.activeCount() == 1;
            },
            5000));
        server.hold = false;
        QTest::qWait(300);
        QVERIFY(!called);
        // A reply that the worker already queued does not run its callback after cancel. A
        // closed port fails at once in the worker. The test blocks the event loop meanwhile.
        quint16 closedPort = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            closedPort = probe.serverPort();
        }
        auto refused = localRequest(server, QStringLiteral("/refused"));
        refused.url.setPort(closedPort);
        refused.expectedPort = closedPort;
        QObject requester;
        const auto queued = client.send(refused, &requester, [&called](const NetworkReply &) {
            called = true;
        });
        QVERIFY(queued);
        QTest::qSleep(1000);
        client.cancel(queued);
        QTest::qWait(300);
        QVERIFY(!called);
        // Destruction with a pending request joins the worker without a callback.
        {
            NetworkClient pending;
            QObject owner;
            pending.send(
                localRequest(server, QStringLiteral("/pending")), &owner, [&called](const auto &) {
                    called = true;
                });
        }
        QVERIFY(!called);
    }
    void networkClientCertificates()
    {
        qputenv("SWAK_COT_TEST_PASSWORD", "swak-test-only");
        qputenv("SWAK_COT_TEST_WRONG", "wrong");
        HttpsHarness server;
        QVERIFY(server.load());
        server.requireClient = true;
        NetworkClient client;
        // TLS 1.3 servers reject a missing client certificate after the handshake, so the
        // client can see a closed connection instead of a TLS error.
        const auto anonymous = fetch(client, localRequest(server, QStringLiteral("/")));
        QVERIFY(!anonymous.ok());
        QVERIFY(!anonymous.error.isEmpty());
        QCOMPARE(server.clientCertificates, 0);
        auto entry = serverDefaults();
        entry.insert(QStringLiteral("certificate"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("key"), QFINDTESTDATA("fixtures/map-test-key.pem"));
        auto request = localRequest(server, QStringLiteral("/pem"));
        auto ca = request.tls.ca;
        request.tls = readTlsFiles(entry);
        request.tls.ca = ca;
        QVERIFY2(fetch(client, request).ok(), "PEM client certificate");
        QCOMPARE(server.clientCertificates, 1);
        entry.insert(QStringLiteral("certificate"), QFINDTESTDATA("fixtures/cot-test-client.p12"));
        entry.insert(QStringLiteral("key"), QString{});
        entry.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_PASSWORD"));
        request = localRequest(server, QStringLiteral("/p12"));
        request.tls = readTlsFiles(entry);
        request.tls.ca = ca;
        QVERIFY(request.tls.pkcs12);
        const auto pkcs12 = fetch(client, request);
        QVERIFY2(pkcs12.ok(), qPrintable(pkcs12.error));
        QCOMPARE(server.clientCertificates, 2);
        entry.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_WRONG"));
        request.tls = readTlsFiles(entry);
        request.tls.ca = ca;
        QVERIFY(!fetch(client, request).ok());
        entry.insert(QStringLiteral("certificate"), QStringLiteral("/missing/client.p12"));
        request.tls = readTlsFiles(entry);
        QVERIFY(!request.tls.error.isEmpty());
        QCOMPARE(fetch(client, request).error, request.tls.error);
    }
    void receiverPolicy()
    {
        // A UDP input counts control messages, and rejects stale, future, older, and invalid
        // events. Only valid events reach the store.
        quint16 port = 0;
        {
            QUdpSocket probe;
            QVERIFY(probe.bind(QHostAddress::LocalHost, 0));
            port = probe.localPort();
        }
        Store store;
        Servers servers(store);
        auto entry = source(port);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        QVERIFY(servers.put(QStringLiteral("in"), entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("in")).startsWith(QStringLiteral("Listening"));
            },
            5000));
        QUdpSocket sender;
        const auto send = [&](const Event &event) {
            const auto bytes = encodeCot(event);
            QCOMPARE(sender.writeDatagram(bytes, QHostAddress::LocalHost, port), bytes.size());
        };
        const auto counter = [&](const QString &name) {
            return servers.statistics().value(QStringLiteral("in")).toObject().value(name).toInt();
        };
        const auto waitFor = [&](const QString &name, int value) {
            return QTest::qWaitFor(
                [&] {
                    return counter(name) == value;
                },
                5000);
        };
        const auto now = QDateTime::currentDateTimeUtc();
        for (const auto *type : {"t-x-c-t", "t-x-c-t-r"}) {
            auto control = sampleEvent(QStringLiteral("ping"));
            control.type = QString::fromLatin1(type);
            send(control);
        }
        QVERIFY(waitFor(QStringLiteral("control"), 2));
        auto stale = sampleEvent(QStringLiteral("stale"));
        stale.stale = now.addSecs(-1);
        send(stale);
        auto distant = sampleEvent(QStringLiteral("distant"));
        distant.stale = now.addDays(8);
        send(distant);
        auto future = sampleEvent(QStringLiteral("future"));
        future.time = now.addSecs(600);
        future.stale = now.addSecs(900);
        send(future);
        auto early = sampleEvent(QStringLiteral("early"));
        early.start = now.addSecs(600);
        early.stale = now.addSecs(900);
        send(early);
        QCOMPARE(sender.writeDatagram("<event", QHostAddress::LocalHost, port), 6);
        QVERIFY(waitFor(QStringLiteral("rejected"), 5));
        // An older copy of a queued event is rejected. A newer copy replaces it.
        auto current = sampleEvent(QStringLiteral("current"));
        send(current);
        auto older = current;
        older.time = older.start = now.addSecs(-30);
        older.callsign = QStringLiteral("Older");
        send(older);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return counter(QStringLiteral("received")) + counter(QStringLiteral("rejected")) ==
                       7;
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 1;
            },
            5000));
        QCOMPARE(store.events().first().uid, QStringLiteral("current"));
        QVERIFY(store.events().first().callsign != QStringLiteral("Older"));
        QCOMPARE(counter(QStringLiteral("control")), 2);
    }
    void udpOutput()
    {
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        auto v = source(receiver.localPort());
        v.insert(QStringLiteral("protocol"), QStringLiteral("udpout"));
        v.insert(QStringLiteral("host"), QStringLiteral("server.example"));
        QVERIFY(validateServer(v).contains(QStringLiteral("numeric")));
        v.insert(QStringLiteral("host"), QStringLiteral("239.2.3.1"));
        QVERIFY(!validateServer(v).isEmpty());
        v.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        QVERIFY(validateServer(v).isEmpty());
        Store store;
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("out"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("out")).startsWith(QStringLiteral("Sending"));
            },
            5000));
        const auto event = sampleEvent();
        QVERIFY(servers.send(event).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return receiver.hasPendingDatagrams();
            },
            5000));
        QByteArray datagram(static_cast<qsizetype>(receiver.pendingDatagramSize()),
                            Qt::Uninitialized);
        receiver.readDatagram(datagram.data(), datagram.size());
        QCOMPARE(datagram, encodeCot(event));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("out")).contains(QStringLiteral("Sent: 1."));
            },
            5000));
        // The output socket ignores input. Received events stay zero.
        QCOMPARE(store.rowCount(), 0);
    }
    void multicastMesh()
    {
        auto v = source(17999);
        v.insert(QStringLiteral("protocol"), QStringLiteral("mcast"));
        v.insert(QStringLiteral("host"), QStringLiteral("10.0.0.1"));
        v.insert(QStringLiteral("localAddress"), QStringLiteral("127.0.0.1"));
        QVERIFY(validateServer(v).contains(QStringLiteral("multicast group")));
        v.insert(QStringLiteral("host"), QStringLiteral("239.255.71.71"));
        v.insert(QStringLiteral("localAddress"), QString{});
        QVERIFY(validateServer(v).contains(QStringLiteral("interface")));
        // Use a real multicast interface. The test sender uses TTL 0, so no datagram leaves
        // this computer. SWAK's own sends use TTL 1, so this test does not send.
        QNetworkInterface selected;
        QHostAddress local;
        for (const auto &interface : QNetworkInterface::allInterfaces()) {
            const auto flags = interface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp) ||
                !flags.testFlag(QNetworkInterface::CanMulticast) ||
                flags.testFlag(QNetworkInterface::IsLoopBack)) {
                continue;
            }
            for (const auto &entry : interface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && local.isNull()) {
                    selected = interface;
                    local = entry.ip();
                }
            }
        }
        if (local.isNull()) {
            QSKIP("No IPv4 multicast interface is available.");
        }
        QUdpSocket probe;
        QVERIFY(probe.bind(QHostAddress::AnyIPv4, 0));
        const auto port = probe.localPort();
        probe.close();
        v.insert(QStringLiteral("port"), port);
        v.insert(QStringLiteral("localAddress"), local.toString());
        QVERIFY(validateServer(v).isEmpty());
        Store store;
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("mesh"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("mesh")).startsWith(QStringLiteral("Joined"));
            },
            5000));
        QUdpSocket sender;
        QVERIFY(sender.bind(local, 0));
        sender.setMulticastInterface(selected);
        sender.setSocketOption(QAbstractSocket::MulticastTtlOption, 0);
        sender.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
        const QHostAddress group(QStringLiteral("239.255.71.71"));
        QCOMPARE(sender.writeDatagram(encodeCot(sampleEvent()), group, port),
                 encodeCot(sampleEvent()).size());
        // ATAK can also send TAK Protocol version 1 mesh datagrams.
        auto binary = sampleEvent();
        binary.uid = QStringLiteral("mesh-binary");
        const auto datagram = takMeshDatagram(encodeTakMessage(binary));
        QCOMPARE(sender.writeDatagram(datagram, group, port), datagram.size());
        QCOMPARE(sender.writeDatagram(QByteArray("\xbf\x02\xbf\x12\x00", 5), group, port), 5);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 2 &&
                       servers.status(QStringLiteral("mesh"))
                           .contains(QStringLiteral("TAK Protocol 1 datagrams: 2."));
            },
            5000));
        QVERIFY(servers.status(QStringLiteral("mesh")).contains(QStringLiteral("Rejected: 1.")));
    }
    void quicStream()
    {
        QVERIFY(quicAvailable());
        auto v = source(8090);
        v.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
        QVERIFY(validateServer(v).isEmpty());
        v.insert(QStringLiteral("authUser"), QStringLiteral("alice"));
        v.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("SWAK_COT_QUIC_LOGIN"));
        QVERIFY(validateServer(v).isEmpty());
        QCOMPARE(TakServerApi::apiUrl(v, QStringLiteral("/version")).port(), 8443);
        // A trusted server accepts the stream. SWAK opens it with a ping.
        {
            QuicHarness server;
            server.payload = encodeCot(sampleEvent());
            QVERIFY(server.start());
            Store store;
            Servers servers(store);
            auto entry = source(server.port);
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
            entry.insert(QStringLiteral("apiPort"), 0);
            QVERIFY(servers.put(QStringLiteral("quic"), entry).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return store.rowCount() == 1;
                },
                10000));
            QVERIFY(server.received.contains("type=\"t-x-c-t\""));
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return servers.status(QStringLiteral("quic"))
                        .startsWith(QStringLiteral("Connected"));
                },
                5000));
        }
        // A host name needs a lookup first. It can resolve to IPv6 and IPv4. The listener has
        // IPv4 only, so SWAK must try each address.
        {
            QuicHarness server;
            server.payload = encodeCot(sampleEvent());
            QVERIFY(server.start());
            Store store;
            Servers servers(store);
            auto entry = source(server.port);
            entry.insert(QStringLiteral("host"), QStringLiteral("localhost"));
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
            entry.insert(QStringLiteral("apiPort"), 0);
            QVERIFY(servers.put(QStringLiteral("quic"), entry).isEmpty());
            QVERIFY2(QTest::qWaitFor(
                         [&] {
                             return store.rowCount() == 1;
                         },
                         15000),
                     qPrintable(servers.status(QStringLiteral("quic"))));
        }
        // When the first address of a host name fails or does not answer, SWAK tries the next
        // address. The test lookups put ::1 first, but SWAK tries IPv4 first.
        const auto restore = qScopeGuard([] {
            setQuicHostLookupForTests(nullptr);
        });
        const auto fallback = [](QUdpSocket *silent, QuicHarness *refusing) {
            QuicHarness server;
            server.ipv6 = true;
            server.payload = encodeCot(sampleEvent());
            server.port = silent ? silent->localPort() : refusing->port;
            if (!server.start()) {
                QSKIP("IPv6 loopback is not available.");
            }
            Store store;
            Servers servers(store);
            auto entry = source(server.port);
            entry.insert(QStringLiteral("host"), QStringLiteral("localhost"));
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
            entry.insert(QStringLiteral("apiPort"), 0);
            entry.insert(QStringLiteral("timeoutSeconds"), 30);
            QElapsedTimer elapsed;
            elapsed.start();
            QVERIFY(servers.put(QStringLiteral("quic"), entry).isEmpty());
            QVERIFY2(QTest::qWaitFor(
                         [&] {
                             return store.rowCount() == 1;
                         },
                         20000),
                     qPrintable(servers.status(QStringLiteral("quic"))));
            if (silent) {
                // The silent IPv4 address keeps the stream for five seconds.
                QVERIFY(elapsed.elapsed() >= 4500);
                QVERIFY(silent->hasPendingDatagrams());
            } else {
                // Seven refused handshakes move on before one address time ends.
                QVERIFY2(elapsed.elapsed() < 4500, qPrintable(QString::number(elapsed.elapsed())));
            }
        };
        {
            // Seven IPv4 addresses refuse, then ::1 answers. The ninth address ::2 stays unused.
            setQuicHostLookupForTests([](const QString &) {
                QList<QHostAddress> result{QHostAddress(QHostAddress::LocalHostIPv6)};
                for (int i = 0; i < 7; ++i) {
                    result.append(QHostAddress(QHostAddress::LocalHost));
                }
                result.append(QHostAddress(QStringLiteral("::2")));
                return result;
            });
            QuicHarness refusing;
            refusing.rejectAlpn = true;
            QVERIFY(refusing.start());
            fallback(nullptr, &refusing);
        }
        {
            setQuicHostLookupForTests([](const QString &) {
                return QList<QHostAddress>{QHostAddress(QHostAddress::LocalHostIPv6),
                                           QHostAddress(QHostAddress::LocalHost)};
            });
            QUdpSocket silent;
            QVERIFY(silent.bind(QHostAddress::LocalHost, 0));
            fallback(&silent, nullptr);
        }
        // A name without addresses fails.
        setQuicHostLookupForTests([](const QString &) {
            return QList<QHostAddress>{};
        });
        {
            Store store;
            Servers servers(store);
            auto entry = source(1);
            entry.insert(QStringLiteral("host"), QStringLiteral("none.swak.test"));
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("apiPort"), 0);
            QVERIFY(servers.put(QStringLiteral("quic"), entry).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return servers.status(QStringLiteral("quic"))
                        .contains(QStringLiteral("cannot be resolved"));
                },
                5000));
        }
        // Without the CA, certificate validation fails.
        {
            QuicHarness server;
            QVERIFY(server.start());
            Store store;
            Servers servers(store);
            auto entry = source(server.port);
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("apiPort"), 0);
            QVERIFY(servers.put(QStringLiteral("quic"), entry).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return servers.status(QStringLiteral("quic"))
                        .contains(QStringLiteral("certificate validation"));
                },
                10000));
        }
        // A client certificate and a login travel over QUIC.
        {
            qputenv("SWAK_COT_TEST_PASSWORD", "swak-test-only");
            qputenv("SWAK_COT_QUIC_LOGIN", "secret");
            QuicHarness server;
            server.requireClient = true;
            server.payload = encodeCot(sampleEvent());
            QVERIFY(server.start());
            Store store;
            Servers servers(store);
            servers.setSenderIdentity(QStringLiteral("SWAK-Q"), QStringLiteral("QUIC"));
            auto entry = source(server.port);
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
            entry.insert(QStringLiteral("certificate"),
                         QFINDTESTDATA("fixtures/cot-test-client.p12"));
            entry.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_PASSWORD"));
            entry.insert(QStringLiteral("authUser"), QStringLiteral("alice"));
            entry.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("SWAK_COT_QUIC_LOGIN"));
            entry.insert(QStringLiteral("apiPort"), 0);
            QVERIFY(servers.put(QStringLiteral("quic"), entry).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return store.rowCount() == 1;
                },
                10000));
            QVERIFY(server.sawClientCertificate);
            QVERIFY(
                server.received.startsWith("<auth><cot username=\"alice\" password=\"secret\""));
            QVERIFY(server.received.contains("uid=\"SWAK-Q\""));
        }
        // A PEM certificate with a PEM key, plain or encrypted, travels over QUIC too.
        QTemporaryDir keys;
        QFile plain(QFINDTESTDATA("fixtures/map-test-key.pem"));
        QVERIFY(plain.open(QIODevice::ReadOnly));
        const QSslKey key(plain.readAll(), QSsl::Rsa);
        QVERIFY(!key.isNull());
        const auto encrypted = keys.filePath(QStringLiteral("encrypted.pem"));
        QFile output(encrypted);
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write(key.toPem("swak-pem-test"));
        output.close();
        qputenv("SWAK_COT_PEM_PASSWORD", "swak-pem-test");
        qputenv("SWAK_COT_PEM_WRONG", "wrong");
        const auto pemClient = [&](const QString &keyPath, const QString &passwordEnv) {
            auto server = std::make_unique<QuicHarness>();
            server->requireClient = true;
            server->payload = encodeCot(sampleEvent());
            if (!server->start()) {
                return std::make_pair(QString(QStringLiteral("no server")), false);
            }
            Store store;
            Servers servers(store);
            auto entry = source(server->port);
            entry.insert(QStringLiteral("protocol"), QStringLiteral("quic"));
            entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
            entry.insert(QStringLiteral("certificate"),
                         QFINDTESTDATA("fixtures/map-test-cert.pem"));
            entry.insert(QStringLiteral("key"), keyPath);
            entry.insert(QStringLiteral("passwordEnv"), passwordEnv);
            entry.insert(QStringLiteral("apiPort"), 0);
            if (!servers.put(QStringLiteral("quic"), entry).isEmpty()) {
                return std::make_pair(QString(QStringLiteral("not saved")), false);
            }
            // The caller checks the result, so the wait result does not matter.
            static_cast<void>(QTest::qWaitFor(
                [&] {
                    return store.rowCount() == 1 || servers.status(QStringLiteral("quic"))
                                                        .contains(QStringLiteral("cannot be used"));
                },
                10000));
            return std::make_pair(servers.status(QStringLiteral("quic")),
                                  store.rowCount() == 1 && server->sawClientCertificate);
        };
        QVERIFY(pemClient(QFINDTESTDATA("fixtures/map-test-key.pem"), {}).second);
        QVERIFY(pemClient(encrypted, QStringLiteral("SWAK_COT_PEM_PASSWORD")).second);
        const auto wrong = pemClient(encrypted, QStringLiteral("SWAK_COT_PEM_WRONG"));
        QVERIFY(!wrong.second);
        QVERIFY2(wrong.first.contains(QStringLiteral("cannot be used")), qPrintable(wrong.first));
        // Without a password, OpenSSL cannot read the encrypted key and does not ask for one.
        QVERIFY(pemClient(encrypted, {}).first.contains(QStringLiteral("cannot be used")));
    }
    void takProtocolMessages()
    {
        const auto event =
            decodeCot(
                QByteArray("<event version='2.0' uid='ANDROID-1' type='a-f-G-U-C' how='m-g' "
                           "time='2026-10-07T10:00:00.123Z' start='2026-10-07T10:00:00.123Z' "
                           "stale='2030-10-07T10:05:00.000Z'><point lat='59.3' lon='18.1' hae='12' "
                           "ce='5' le='9'/><detail><contact endpoint='10.0.0.5:4242:tcp' "
                           "callsign='VIPER'/>"
                           "<__group name='Cyan' role='Team Lead'/><status battery='87'/>"
                           "<takv device='Pixel' platform='ATAK-CIV' os='34' version='5.4'/>"
                           "<track course='90.5' speed='2.5'/><precisionlocation geopointsrc='GPS' "
                           "altsrc='GPS'/>"
                           "<remarks>Hold &amp; observe</remarks><link uid='RP-1' type='b-m-p-w' "
                           "relation='c'/>"
                           "</detail></event>"))
                .event;
        QVERIFY(event);
        const auto payload = encodeTakMessage(*event);
        QVERIFY(!payload.isEmpty());
        // Typed elements use their protobuf fields. The rest travels as XML.
        swak::tak::v1::TakMessage message;
        QVERIFY(message.ParseFromArray(payload.constData(), static_cast<int>(payload.size())));
        QCOMPARE(message.event().time_ms(), static_cast<quint64>(event->time.toMSecsSinceEpoch()));
        QCOMPARE(QString::fromStdString(message.event().detail().contact().endpoint()),
                 QStringLiteral("10.0.0.5:4242:tcp"));
        QCOMPARE(message.event().detail().status().battery_percent(), 87U);
        QCOMPARE(message.event().detail().track().course(), 90.5);
        QVERIFY(message.event().detail().has_device_version());
        QVERIFY(message.event().detail().has_precision_location());
        const auto other = QString::fromStdString(message.event().detail().other_xml());
        QVERIFY(other.contains(QStringLiteral("<remarks>Hold &amp; observe</remarks>")));
        QVERIFY(!other.contains(QStringLiteral("contact")));
        const auto decoded = decodeTakMessage(payload);
        QVERIFY2(decoded.event, qPrintable(decoded.error));
        QCOMPARE(decoded.event->uid, event->uid);
        QCOMPARE(decoded.event->time, event->time);
        QCOMPARE(decoded.event->stale, event->stale);
        QCOMPARE(decoded.event->latitude, event->latitude);
        QCOMPARE(decoded.event->callsign, QStringLiteral("VIPER"));
        QCOMPARE(decoded.event->team, QStringLiteral("Cyan"));
        QCOMPARE(decoded.event->course, event->course);
        QCOMPARE(decoded.event->speed, event->speed);
        QCOMPARE(remarks(*decoded.event), QStringLiteral("Hold & observe"));
        QCOMPARE(contactEndpoint(*decoded.event), QStringLiteral("10.0.0.5:4242:tcp"));
        QCOMPARE(batteryPercent(*decoded.event), std::optional<int>(87));
        QCOMPARE(links(*decoded.event).size(), 1);
        // A contact with more attributes and a partial track stay XML, so no value is lost.
        auto extra = *event;
        extra.detail.replace(QStringLiteral("callsign=\"VIPER\""),
                             QStringLiteral("callsign=\"VIPER\" phone=\"123\""));
        extra.speed.reset();
        const auto extraPayload = encodeTakMessage(extra);
        QVERIFY(message.ParseFromArray(extraPayload.constData(),
                                       static_cast<int>(extraPayload.size())));
        QVERIFY(!message.event().detail().has_contact());
        QVERIFY(!message.event().detail().has_track());
        const auto extraDecoded = decodeTakMessage(extraPayload);
        QVERIFY(extraDecoded.event);
        QCOMPARE(detailElement(*extraDecoded.event, u"contact")->attribute(u"phone"),
                 QStringLiteral("123"));
        QVERIFY(!extraDecoded.event->speed);
        // A control-only message, encoded by hand from the field numbers: control { 1: 1, 2: 1 }.
        QVERIFY(decodeTakMessage(QByteArray("\x0a\x04\x08\x01\x10\x01", 6)).control);
        QVERIFY(!decodeTakMessage(QByteArray("\x12\xff", 2)).error.isEmpty());
        QVERIFY(!decodeTakMessage({}).error.isEmpty());
        // Detail XML cannot leave its element or add a second event.
        QVERIFY(message.ParseFromArray(payload.constData(), static_cast<int>(payload.size())));
        message.mutable_event()->mutable_detail()->set_other_xml(
            "</detail></event><event version='2.0'><detail>");
        std::string injected;
        QVERIFY(message.SerializeToString(&injected));
        QVERIFY(!decodeTakMessage(QByteArray::fromStdString(injected)).event);
        message.mutable_event()->mutable_detail()->set_other_xml("<!DOCTYPE x><x/>");
        QVERIFY(message.SerializeToString(&injected));
        QVERIFY(!decodeTakMessage(QByteArray::fromStdString(injected)).event);
        message.mutable_event()->mutable_detail()->clear_other_xml();
        message.mutable_event()->set_latitude(std::numeric_limits<double>::quiet_NaN());
        QVERIFY(message.SerializeToString(&injected));
        QVERIFY(!decodeTakMessage(QByteArray::fromStdString(injected)).event);
        // Stream frames: split reads, two frames, and failures.
        const auto frame = takStreamFrame(payload);
        QCOMPARE(frame.at(0), TakMagic);
        TakStream stream;
        QVERIFY(stream.feed(frame.left(1)).isEmpty());
        QVERIFY(stream.feed(frame.mid(1, 2)).isEmpty());
        QVERIFY(stream.pending());
        const auto payloads = stream.feed(frame.mid(3) + frame);
        QCOMPARE(payloads.size(), 2);
        QCOMPARE(payloads.first(), payload);
        QVERIFY(!stream.pending());
        for (const QByteArray &bad : {QByteArray("<event"),
                                      QByteArray("\xbf\x00", 2),
                                      QByteArray("\xbf\xff\xff\xff\xff\xff", 6),
                                      // A length of 70000 exceeds the 64 KiB message limit.
                                      QByteArray("\xbf\xf0\xa2\x04", 4)}) {
            TakStream broken;
            broken.feed(bad);
            QVERIFY2(broken.failed(), bad.toHex().constData());
        }
        QVERIFY(takStreamFrame(QByteArray(MaxMessageBytes + 1, 'x')).isEmpty());
        // Mesh datagrams: 0xbf, version 1, 0xbf, payload.
        const auto datagram = takMeshDatagram(payload);
        QVERIFY(datagram.startsWith(QByteArray("\xbf\x01\xbf", 3)));
        QCOMPARE(takMeshPayload(datagram), std::optional<QByteArray>(payload));
        QVERIFY(!takMeshPayload(QByteArray("\xbf\x02\xbf", 3) + payload));
        QVERIFY(!takMeshPayload(QByteArray("\xbf\x01\x00", 3) + payload));
        QVERIFY(!takMeshPayload(QByteArray("<event")));
    }
    void takProtocolNegotiation()
    {
        auto binaryEvent = sampleEvent();
        binaryEvent.uid = QStringLiteral("after-switch");
        NegotiatingServer server;
        // The response and the first binary frame arrive in one write.
        server.afterResponse = takStreamFrame(encodeTakMessage(binaryEvent));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("tak"), source(server.serverPort())).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 1;
            },
            5000));
        QCOMPARE(store.events().first().uid, QStringLiteral("after-switch"));
        QCOMPARE(store.events().first().origin, QStringLiteral("tak"));
        QVERIFY(server.received.contains("type=\"t-x-takp-q\""));
        QVERIFY(server.received.contains("<TakRequest version=\"1\"/>"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tak"))
                    .contains(QStringLiteral("TAK Protocol 1"));
            },
            5000));
        // After the switch, SWAK sends binary frames.
        server.received.clear();
        auto outgoing = sampleEvent();
        outgoing.uid = QStringLiteral("from-swak");
        QVERIFY(servers.send(outgoing).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                TakStream reader;
                const auto frames = reader.feed(server.received);
                return std::any_of(frames.cbegin(), frames.cend(), [](const QByteArray &item) {
                    const auto result = decodeTakMessage(item);
                    return result.event && result.event->uid == u"from-swak";
                });
            },
            5000));
        QVERIFY(!server.received.contains("<event"));
        // An invalid binary frame stops the connection with a reason.
        server.client->write(QByteArray("\xbf\x00", 2));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tak"))
                    .contains(QStringLiteral("TAK Protocol stream"));
            },
            5000));
        // XML only never asks for the switch.
        NegotiatingServer plain;
        QVERIFY(plain.listen(QHostAddress::LocalHost, 0));
        Store xmlStore;
        Servers xmlServers(xmlStore);
        auto xml = source(plain.serverPort());
        xml.insert(QStringLiteral("takProtocol"), QStringLiteral("xml"));
        QVERIFY(xmlServers.put(QStringLiteral("xml"), xml).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return xmlServers.status(QStringLiteral("xml"))
                    .startsWith(QStringLiteral("Connected"));
            },
            5000));
        QVERIFY(xmlServers.send(sampleEvent()).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return plain.received.contains("<event");
            },
            5000));
        QVERIFY(!plain.received.contains("t-x-takp-q"));
        // A read full of protocol responses that SWAK did not ask for stays one XML pass. The
        // event after them arrives, and the stream stays connected.
        QByteArray flood;
        const auto response = NegotiatingServer::control(
            QStringLiteral("t-x-takp-r"), QStringLiteral("<TakResponse status='true'/>"));
        while (flood.size() + response.size() < 60000) {
            flood += response;
        }
        auto marker = sampleEvent();
        marker.uid = QStringLiteral("after-flood");
        QVERIFY(plain.client);
        plain.client->write(flood + encodeCot(marker));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(
                    xmlStore.events().cbegin(), xmlStore.events().cend(), [](const Event &event) {
                        return event.uid == u"after-flood";
                    });
            },
            5000));
        QVERIFY(xmlServers.status(QStringLiteral("xml")).startsWith(QStringLiteral("Connected")));
        xml.insert(QStringLiteral("takProtocol"), QStringLiteral("other"));
        QVERIFY(!validateServer(xml).isEmpty());
    }
    void binaryStreamTimeout()
    {
        // The incomplete-message timeout measures one frame. A server that streams without a
        // pause, so that each read ends inside the next frame, stays connected.
        NegotiatingServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        auto entry = source(server.serverPort());
        entry.insert(QStringLiteral("timeoutSeconds"), 2);
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tak"))
                    .contains(QStringLiteral("TAK Protocol 1"));
            },
            5000));
        QVERIFY(server.client);
        QByteArray rest;
        QElapsedTimer clock;
        clock.start();
        int sent = 0;
        while (clock.elapsed() < 3500) {
            auto event = sampleEvent();
            event.uid = QStringLiteral("stream-%1").arg(sent++);
            const auto frame = takStreamFrame(encodeTakMessage(event));
            server.client->write(rest + frame.left(frame.size() / 2));
            rest = frame.mid(frame.size() / 2);
            QTest::qWait(250);
        }
        const auto status = servers.status(QStringLiteral("tak"));
        QVERIFY2(status.contains(QStringLiteral("TAK Protocol 1")), qPrintable(status));
        QVERIFY(store.rowCount() >= sent - 1);
    }
    void takStreamLogin()
    {
        auto v = source(1);
        v.insert(QStringLiteral("authUser"), QStringLiteral("alice"));
        v.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("SWAK_COT_TEST_LOGIN"));
        QVERIFY(validateServer(v).contains(QStringLiteral("TLS")));
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        QVERIFY(validateServer(v).isEmpty());
        v.insert(QStringLiteral("authPasswordEnv"), QString{});
        QVERIFY(!validateServer(v).isEmpty());
        v.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("PASSWORD"));
        QVERIFY(!validateServer(v).isEmpty());
        v.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("SWAK_COT_TEST_LOGIN"));
        v.insert(QStringLiteral("authUser"), QString(129, 'a'));
        QVERIFY(!validateServer(v).isEmpty());
        TlsHarness listener;
        QFile cert(QFINDTESTDATA("fixtures/map-test-cert.pem")),
            key(QFINDTESTDATA("fixtures/map-test-key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly));
        listener.certificate = QSslCertificate(cert.readAll());
        listener.key = QSslKey(key.readAll(), QSsl::Rsa);
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        qputenv("SWAK_COT_TEST_LOGIN", "p&w'\"<x>");
        Store store;
        Servers servers(store);
        servers.setSenderIdentity(QStringLiteral("SWAK-1"), QStringLiteral("VIPER"));
        v = source(listener.serverPort());
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        v.insert(QStringLiteral("ca"), cert.fileName());
        v.insert(QStringLiteral("authUser"), QStringLiteral("alice"));
        v.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("SWAK_COT_TEST_LOGIN"));
        QVERIFY(servers.put(QStringLiteral("login"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return listener.received.contains("</auth>");
            },
            5000));
        // The login comes first, escaped, with the sender identity.
        QVERIFY(listener.received.startsWith("<auth><cot username=\"alice\""));
        QVERIFY(listener.received.contains("password=\"p&amp;w'&quot;&lt;x&gt;\""));
        QVERIFY(listener.received.contains("uid=\"SWAK-1\""));
        QVERIFY(listener.received.contains("callsign=\"VIPER\""));
        // A missing password stops the connection with a reason.
        qunsetenv("SWAK_COT_TEST_LOGIN");
        listener.received.clear();
        servers.reconnect();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("login"))
                    .contains(QStringLiteral("TAK password is unavailable"));
            },
            5000));
        QVERIFY(!listener.received.contains("<auth>"));
    }
    void enrollmentPieces()
    {
        const auto plain = generateEnrollmentKey({});
        QVERIFY2(plain.error.isEmpty(), qPrintable(plain.error));
        QVERIFY(plain.pem.contains("BEGIN PRIVATE KEY"));
        QVERIFY(readEnrollmentKey(plain.pem, {}).error.isEmpty());
        const auto locked = generateEnrollmentKey("secret");
        QVERIFY(locked.pem.contains("ENCRYPTED"));
        QVERIFY(readEnrollmentKey(locked.pem, "secret").error.isEmpty());
        QVERIFY(!readEnrollmentKey(locked.pem, "wrong").error.isEmpty());
        QVERIFY(!readEnrollmentKey(locked.pem, {}).error.isEmpty());
        QVERIFY(!readEnrollmentKey("not a key", {}).error.isEmpty());
        QList<SubjectEntry> entries;
        QVERIFY(readCertificateConfig(
            "<?xml version=\"1.0\"?><ns2:certificateConfig xmlns:ns2=\"com.bbn.marti.config\">"
            "<nameEntries><nameEntry name=\"O\" value=\"TAK\"/>"
            "<nameEntry name=\"OU\" value=\"Ops Team\"/></nameEntries></ns2:certificateConfig>",
            &entries));
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(1), (SubjectEntry{QStringLiteral("OU"), QStringLiteral("Ops Team")}));
        QList<SubjectEntry> rejected;
        QVERIFY(!readCertificateConfig("<certificateConfig><nameEntry name=\"CN\" value=\"x\"/>"
                                       "</certificateConfig>",
                                       &rejected));
        QVERIFY(!readCertificateConfig("<!DOCTYPE x><certificateConfig/>", &rejected));
        QVERIFY(!readCertificateConfig("<other/>", &rejected));
        QVERIFY(!readCertificateConfig("<certificateConfig><nameEntry name=\"1\" value=\"x\"/>"
                                       "</certificateConfig>",
                                       &rejected));
        QString error;
        const auto csr =
            enrollmentRequest(locked.pem, "secret", QStringLiteral("alice"), entries, &error);
        QVERIFY2(!csr.isEmpty(), qPrintable(error));
        QString subject;
        const auto signedCert = signRequest(csr, &subject);
        QCOMPARE(subject, QStringLiteral("/CN=alice/O=TAK/OU=Ops Team"));
        QVERIFY(enrollmentRequest("bad", {}, QStringLiteral("alice"), entries, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        // The reply must hold valid certificates that match the key.
        const auto reply =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("signedCert"), QString::fromLatin1(signedCert)},
                    {QStringLiteral("ca0"), QString::fromLatin1(fixtureCertificateBase64())}})
                .toJson();
        const auto result = readEnrollmentReply(reply, locked.pem, "secret");
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.certificate.startsWith("-----BEGIN CERTIFICATE-----"));
        QCOMPARE(QSslCertificate::fromData(result.authorities).size(), 1);
        QVERIFY(!readEnrollmentReply(reply, plain.pem, {}).error.isEmpty());
        // TAK Server wraps the base64 text at 64 characters.
        const auto wrap = [](QByteArray text) {
            for (qsizetype i = 64; i < text.size(); i += 65) {
                text.insert(i, '\n');
            }
            return QString::fromLatin1(text);
        };
        const auto wrapped =
            QJsonDocument(QJsonObject{{QStringLiteral("signedCert"), wrap(signedCert)},
                                      {QStringLiteral("ca0"), wrap(fixtureCertificateBase64())}})
                .toJson();
        QVERIFY2(readEnrollmentReply(wrapped, locked.pem, "secret").error.isEmpty(),
                 qPrintable(readEnrollmentReply(wrapped, locked.pem, "secret").error));
        QVERIFY(
            !readEnrollmentReply("{\"signedCert\":\"!!\"}", locked.pem, "secret").error.isEmpty());
        QVERIFY(!readEnrollmentReply(
                     QJsonDocument(QJsonObject{{QStringLiteral("signedCert"),
                                                QString::fromLatin1(signedCert)},
                                               {QStringLiteral("ca0"), QStringLiteral("bad")}})
                         .toJson(),
                     locked.pem,
                     "secret")
                     .error.isEmpty());
    }
    void enrollmentFlow()
    {
        HttpsHarness enroll;
        QVERIFY(enroll.load());
        QString subject;
        enroll.respond = [&subject](const QByteArray &request) {
            const auto ok = [](const QByteArray &type, const QByteArray &body) {
                return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: ") + type +
                       "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            };
            if (!request.contains("Authorization: Basic " + QByteArray("alice:pa:ss").toBase64())) {
                return QByteArray("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
            }
            if (request.startsWith("GET /Marti/api/tls/config ")) {
                return ok("application/xml",
                          "<certificateConfig><nameEntries><nameEntry name=\"O\" value=\"TAK\"/>"
                          "</nameEntries></certificateConfig>");
            }
            if (request.startsWith("POST /Marti/api/tls/signClient/v2?")) {
                const auto body = request.mid(request.indexOf("\r\n\r\n") + 4);
                const auto signedCert = signRequest(body, &subject);
                return ok(
                    "application/json",
                    QJsonDocument(
                        QJsonObject{{QStringLiteral("signedCert"), QString::fromLatin1(signedCert)},
                                    {QStringLiteral("ca0"),
                                     QString::fromLatin1(fixtureCertificateBase64())}})
                        .toJson());
            }
            return QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        };
        TlsHarness stream;
        QFile cert(QFINDTESTDATA("fixtures/map-test-cert.pem")),
            key(QFINDTESTDATA("fixtures/map-test-key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly));
        stream.certificate = QSslCertificate(cert.readAll());
        stream.key = QSslKey(key.readAll(), QSsl::Rsa);
        stream.requireClient = true;
        stream.payload = encodeCot(sampleEvent());
        QVERIFY(stream.listen(QHostAddress::LocalHost, 0));
        QTemporaryDir directory;
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakEnrollment enrollment(servers, network);
        enrollment.setDirectory(directory.path());
        auto v = source(stream.serverPort());
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        v.insert(QStringLiteral("apiPort"), 0);
        v.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_KEY"));
        v.insert(QStringLiteral("enabled"), false);
        QVERIFY(servers.put(QStringLiteral("tak/1"), v).isEmpty());
        qputenv("SWAK_COT_TEST_KEY", "key-secret");
        QSignalSpy finished(&enrollment, &TakEnrollment::finished);
        TakEnrollment::Options options;
        options.serverId = QStringLiteral("tak/1");
        options.username = QStringLiteral("alice");
        options.password = "wrong";
        options.port = enroll.serverPort();
        options.clientUid = QStringLiteral("SWAK-1");
        // Without the CA in the entry, the server certificate is not trusted.
        QVERIFY(enrollment.start(options).isEmpty());
        QVERIFY(finished.wait(10000));
        QVERIFY(finished.last().first().toString().contains(QStringLiteral("TLS")));
        // An unreadable CA file reports itself, not a TLS failure.
        v.insert(QStringLiteral("ca"), directory.filePath(QStringLiteral("missing-ca.pem")));
        QVERIFY(servers.put(QStringLiteral("tak/1"), v).isEmpty());
        QVERIFY(enrollment.start(options).isEmpty());
        QVERIFY(finished.wait(10000));
        QVERIFY2(finished.last().first().toString().contains(QStringLiteral("CA file")),
                 qPrintable(finished.last().first().toString()));
        v.insert(QStringLiteral("ca"), cert.fileName());
        QVERIFY(servers.put(QStringLiteral("tak/1"), v).isEmpty());
        QVERIFY(enrollment.start(options).isEmpty());
        QVERIFY(!enrollment.start(options).isEmpty());
        QVERIFY(finished.wait(10000));
        QVERIFY(
            finished.last().first().toString().contains(QStringLiteral("username or password")));
        options.password = "pa:ss";
        QVERIFY(enrollment.start(options).isEmpty());
        QVERIFY(finished.wait(20000));
        QVERIFY2(finished.last().first().toString().isEmpty(),
                 qPrintable(finished.last().first().toString()));
        QCOMPARE(subject, QStringLiteral("/CN=alice/O=TAK"));
        QVERIFY(enroll.requests.last().contains("clientUid=SWAK-1"));
        QVERIFY(enroll.requests.last().contains("version=SWAK"));
        // The entry points at the new files. The folder and files are private.
        const auto updated = servers.sources().value(QStringLiteral("tak/1")).toObject();
        const auto keyPath = updated.value(QStringLiteral("key")).toString();
        const auto certPath = updated.value(QStringLiteral("certificate")).toString();
        QVERIFY(keyPath.startsWith(directory.path()));
        // An identifier with a slash names the folder through a hash.
        QCOMPARE(QFileInfo(QFileInfo(keyPath).path()).path(), directory.path());
        QVERIFY(
            QFileInfo(QFileInfo(keyPath).path()).fileName().startsWith(QStringLiteral("server-")));
        QCOMPARE(updated.value(QStringLiteral("ca")).toString(), cert.fileName());
        QCOMPARE(QFile::permissions(keyPath) & 0x0077, QFileDevice::Permissions{});
        QCOMPARE(QFile::permissions(QFileInfo(keyPath).path()),
                 QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                     QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser);
        QFile keyFile(keyPath);
        QVERIFY(keyFile.open(QIODevice::ReadOnly));
        QVERIFY(keyFile.readAll().contains("ENCRYPTED"));
        QVERIFY(QFile::exists(certPath));
        // The new certificate authenticates a CoT stream that requires client certificates.
        auto enabled = updated;
        enabled.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers.put(QStringLiteral("tak/1"), enabled).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 1;
            },
            10000));
        // An existing key file stays in place. A cancelled enrollment reports so.
        enabled.insert(QStringLiteral("enabled"), false);
        QVERIFY(servers.put(QStringLiteral("tak/1"), enabled).isEmpty());
        options.keyFile = keyPath;
        QVERIFY(enrollment.start(options).isEmpty());
        QVERIFY(finished.wait(20000));
        QVERIFY(finished.last().first().toString().isEmpty());
        QCOMPARE(servers.sources()
                     .value(QStringLiteral("tak/1"))
                     .toObject()
                     .value(QStringLiteral("key"))
                     .toString(),
                 keyPath);
        enroll.hold = true;
        QVERIFY(enrollment.start(options).isEmpty());
        enrollment.cancel();
        QCOMPARE(finished.last().first().toString(), QStringLiteral("Enrollment cancelled."));
        QVERIFY(!enrollment.running());
        // A crafted identifier, such as "..", cannot put the files outside the folder.
        enroll.hold = false;
        QVERIFY(servers.put(QStringLiteral(".."), v).isEmpty());
        auto parent = options;
        parent.serverId = QStringLiteral("..");
        parent.keyFile.clear();
        QVERIFY(enrollment.start(parent).isEmpty());
        QVERIFY(finished.wait(20000));
        QVERIFY2(finished.last().first().toString().isEmpty(),
                 qPrintable(finished.last().first().toString()));
        const auto crafted = servers.sources()
                                 .value(QStringLiteral(".."))
                                 .toObject()
                                 .value(QStringLiteral("key"))
                                 .toString();
        QCOMPARE(QFileInfo(QFileInfo(crafted).path()).path(), directory.path());
        options.username = QStringLiteral("a:b");
        QVERIFY(!enrollment.start(options).isEmpty());
        options.username = QStringLiteral("alice");
        options.keyFile = QStringLiteral("/missing.key");
        QVERIFY(!enrollment.start(options).isEmpty());
        options.serverId = QStringLiteral("missing");
        QVERIFY(!enrollment.start(options).isEmpty());
    }
    void enrollmentFailures()
    {
        // A short RSA key is refused before any request.
        EVP_PKEY *weak = EVP_RSA_gen(1024);
        QVERIFY(weak);
        BIO *memory = BIO_new(BIO_s_mem());
        QCOMPARE(PEM_write_bio_PrivateKey(memory, weak, nullptr, nullptr, 0, nullptr, nullptr), 1);
        char *data = nullptr;
        const auto length = BIO_get_mem_data(memory, &data);
        const QByteArray weakPem(data, static_cast<qsizetype>(length));
        BIO_free(memory);
        EVP_PKEY_free(weak);
        QCOMPARE(readEnrollmentKey(weakPem, {}).error,
                 QStringLiteral("Use an RSA key with at least 2048 bits, or an EC key."));
        // Each step reports a server failure.
        HttpsHarness enroll;
        QVERIFY(enroll.load());
        QByteArray config = "<certificateConfig><nameEntries><nameEntry name=\"O\" value=\"TAK\"/>"
                            "</nameEntries></certificateConfig>";
        int configStatus = 200;
        int signStatus = 200;
        QByteArray signBody;
        bool dropSign = false;
        enroll.respond = [&](const QByteArray &request) {
            if (request.startsWith("GET /Marti/api/tls/config ")) {
                return httpReply(configStatus, config);
            }
            if (request.startsWith("POST /Marti/api/tls/signClient/v2?")) {
                if (dropSign) {
                    // The server closes the connection without an answer.
                    return QByteArray();
                }
                if (!signBody.isEmpty()) {
                    return httpReply(200, signBody);
                }
                if (signStatus != 200) {
                    return httpReply(signStatus, {});
                }
                QString subject;
                const auto body = request.mid(request.indexOf("\r\n\r\n") + 4);
                return httpReply(
                    200,
                    QJsonDocument(QJsonObject{{QStringLiteral("signedCert"),
                                               QString::fromLatin1(signRequest(body, &subject))},
                                              {QStringLiteral("ca0"),
                                               QString::fromLatin1(fixtureCertificateBase64())}})
                        .toJson());
            }
            return httpReply(404, {});
        };
        QTemporaryDir directory;
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakEnrollment enrollment(servers, network);
        enrollment.setDirectory(directory.path());
        auto v = source(9);
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        v.insert(QStringLiteral("apiPort"), 0);
        v.insert(QStringLiteral("enabled"), false);
        v.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        v.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_KEY"));
        qputenv("SWAK_COT_TEST_KEY", "key-secret");
        QVERIFY(servers.put(QStringLiteral("tak"), v).isEmpty());
        QSignalSpy finished(&enrollment, &TakEnrollment::finished);
        TakEnrollment::Options options;
        options.serverId = QStringLiteral("tak");
        options.username = QStringLiteral("alice");
        options.password = "secret";
        options.clientUid = QStringLiteral("SWAK-1");
        options.port = 0;
        QCOMPARE(enrollment.start(options),
                 QStringLiteral("Use an enrollment port from 1 through 65535."));
        options.port = enroll.serverPort();
        const auto result = [&] {
            if (!enrollment.start(options).isEmpty() || !finished.wait(20000)) {
                return QStringLiteral("not finished");
            }
            return finished.last().first().toString();
        };
        configStatus = 500;
        QCOMPARE(result(), QStringLiteral("The enrollment service answered with HTTP status 500."));
        configStatus = 200;
        config = "<other/>";
        QCOMPARE(result(), QStringLiteral("The server sent invalid certificate settings."));
        config = "<certificateConfig><nameEntries/></certificateConfig>";
        signStatus = 500;
        QVERIFY(result().startsWith(QStringLiteral("The server did not sign the certificate.")));
        // The certificate folder cannot be made below a regular file.
        signStatus = 200;
        const auto blocker = directory.filePath(QStringLiteral("file"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        enrollment.setDirectory(blocker);
        QCOMPARE(result(), QStringLiteral("The certificate directory cannot be created."));
        const auto unchanged = [&] {
            return servers.sources()
                .value(QStringLiteral("tak"))
                .toObject()
                .value(QStringLiteral("certificate"))
                .toString()
                .isEmpty();
        };
        QVERIFY(unchanged());
        enrollment.setDirectory(directory.path());
        // OpenSSL refuses a subject field that the server asks for.
        config = "<certificateConfig><nameEntries><nameEntry name=\"ZZ\" value=\"x\"/>"
                 "</nameEntries></certificateConfig>";
        QCOMPARE(
            result(),
            QStringLiteral("The certificate request cannot be signed. Check the subject entries."));
        config = "<certificateConfig><nameEntries/></certificateConfig>";
        // The signing step refuses the account, drops the connection, or answers without a
        // certificate.
        signStatus = 401;
        QCOMPARE(result(), QStringLiteral("The server rejected the username or password."));
        signStatus = 200;
        dropSign = true;
        const auto dropped = result();
        QVERIFY(!dropped.isEmpty() && dropped != QStringLiteral("not finished"));
        dropSign = false;
        signBody = "{}";
        QCOMPARE(result(), QStringLiteral("The server did not return a valid signed certificate."));
        signBody.clear();
        // A directory in place of each file stops the enrollment before the entry changes.
        const auto blocked = [&](const QString &folder, const QString &name) {
            const auto root = directory.filePath(folder);
            QDir().mkpath(root + QStringLiteral("/tak/") + name);
            enrollment.setDirectory(root);
            return result();
        };
        QCOMPARE(blocked(QStringLiteral("key"), QStringLiteral("client.key")),
                 QStringLiteral("The private key cannot be saved."));
        QCOMPARE(blocked(QStringLiteral("certificate"), QStringLiteral("client.pem")),
                 QStringLiteral("The certificates cannot be saved."));
        QCOMPARE(blocked(QStringLiteral("authority"), QStringLiteral("ca.pem")),
                 QStringLiteral("The certificates cannot be saved."));
        QVERIFY(unchanged());
        // The entry check refuses certificate paths with control characters. The entry stays.
        enrollment.setDirectory(directory.filePath(QStringLiteral("bad\u0001folder")));
        QCOMPARE(result(), QStringLiteral("A certificate path is invalid."));
        QVERIFY(unchanged());
        // The same enrollment then succeeds in a usable folder.
        enrollment.setDirectory(directory.filePath(QStringLiteral("good")));
        QCOMPARE(result(), QString());
        QVERIFY(!unchanged());
    }
    void takChannels()
    {
        const QByteArray list =
            "{\"version\":\"3\",\"type\":\"com.bbn.marti.remote.groups.Group\",\"data\":["
            "{\"name\":\"Cyan\",\"direction\":\"IN\",\"bitpos\":2,\"active\":true,\"type\":"
            "\"SYSTEM\"},"
            "{\"name\":\"Cyan\",\"direction\":\"OUT\",\"bitpos\":2,\"active\":true,\"type\":"
            "\"SYSTEM\"},"
            "{\"name\":\"Ops\",\"direction\":\"IN\",\"bitpos\":3,\"active\":false,"
            "\"description\":\"Operations\"}]}";
        QList<TakGroup> groups;
        QVERIFY(readTakGroups(list, &groups));
        QCOMPARE(groups.size(), 3);
        QCOMPARE(channelNames(groups),
                 (QStringList{QStringLiteral("Cyan"), QStringLiteral("Ops")}));
        QCOMPARE(activeChannels(groups), (QSet<QString>{QStringLiteral("Cyan")}));
        QCOMPARE(groups.at(2).description, QStringLiteral("Operations"));
        const auto body =
            QJsonDocument::fromJson(activeGroupsBody(groups, {QStringLiteral("Ops")})).array();
        QCOMPARE(body.size(), 3);
        QVERIFY(!body.at(0).toObject().value(QStringLiteral("active")).toBool());
        QVERIFY(body.at(2).toObject().value(QStringLiteral("active")).toBool());
        QCOMPARE(body.at(2).toObject().value(QStringLiteral("bitpos")).toInt(), 3);
        QCOMPARE(body.at(1).toObject().value(QStringLiteral("direction")).toString(),
                 QStringLiteral("OUT"));
        QList<TakGroup> rejected;
        QVERIFY(!readTakGroups("{\"data\":{}}", &rejected));
        QVERIFY(!readTakGroups("{\"data\":[{\"name\":\"\"}]}", &rejected));
        QVERIFY(!readTakGroups("{\"data\":[{\"name\":\"a\\u0007\"}]}", &rejected));
        QVERIFY(!readTakGroups("not json", &rejected));
        // Load and apply through the REST API of a TLS entry.
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        api.respond = [&list, &status](const QByteArray &request) {
            if (request.startsWith("GET /Marti/api/groups/all?useCache=true ")) {
                return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: "
                                  "application/json\r\nContent-Length: ") +
                       QByteArray::number(list.size()) + "\r\n\r\n" + list;
            }
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: 0\r\n\r\n";
        };
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakServerApi tak(servers, network);
        TakChannels channels(tak);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QSignalSpy loaded(&channels, &TakChannels::loaded);
        QSignalSpy applied(&channels, &TakChannels::applied);
        channels.load(QStringLiteral("tak"));
        QVERIFY(loaded.wait(10000));
        QVERIFY(loaded.last().at(2).toString().isEmpty());
        const auto received = loaded.last().at(1).value<QList<TakGroup>>();
        QCOMPARE(received.size(), 3);
        channels.apply(
            QStringLiteral("tak"), received, {QStringLiteral("Ops")}, QStringLiteral("SWAK-1"));
        QVERIFY(applied.wait(10000));
        QVERIFY(applied.last().at(1).toString().isEmpty());
        const auto put = api.requests.last();
        QVERIFY(put.startsWith("PUT /Marti/api/groups/active?clientUid=SWAK-1 "));
        QVERIFY(put.contains("Content-Type: application/json"));
        const auto sent = QJsonDocument::fromJson(put.mid(put.indexOf("\r\n\r\n") + 4)).array();
        QCOMPARE(sent.size(), 3);
        QVERIFY(sent.at(2).toObject().value(QStringLiteral("active")).toBool());
        status = 403;
        channels.apply(QStringLiteral("tak"), received, {}, QStringLiteral("SWAK-1"));
        QVERIFY(applied.wait(10000));
        QVERIFY(applied.last().at(1).toString().contains(QStringLiteral("does not allow")));
    }
    void takContacts()
    {
        const QByteArray list =
            "[{\"uid\":\"ANDROID-1\",\"callsign\":\"Bravo\",\"team\":\"Cyan\",\"role\":"
            "\"Medic\",\"takv\":\"ATAK 5.4\",\"notes\":\"\",\"filterGroups\":null},"
            "{\"uid\":\"ANDROID-2\",\"callsign\":\"Alpha\\u0007\",\"team\":\"Red\"},"
            "{\"callsign\":\"No UID\"}]";
        QList<Contact> parsed;
        QVERIFY(readTakContacts(list, QStringLiteral("tak"), &parsed));
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed.at(0).device, QStringLiteral("ATAK 5.4"));
        QCOMPARE(parsed.at(1).callsign, QStringLiteral("Alpha"));
        QCOMPARE(parsed.at(1).origin, QStringLiteral("tak"));
        QList<Contact> rejected;
        QVERIFY(!readTakContacts("{\"data\":[]}", {}, &rejected));
        QVERIFY(!readTakContacts("not json", {}, &rejected));
        QByteArray tooMany = "[";
        for (int i = 0; i <= Contacts::MaxContacts; ++i) {
            tooMany += i ? ",{}" : "{}";
        }
        QVERIFY(!readTakContacts(tooMany + "]", {}, &rejected));
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        api.respond = [&list, &status](const QByteArray &request) {
            if (status == 200 && request.startsWith("GET /Marti/api/contacts/all ")) {
                return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: "
                                  "application/json\r\nContent-Length: ") +
                       QByteArray::number(list.size()) + "\r\n\r\n" + list;
            }
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: 0\r\n\r\n";
        };
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakServerApi tak(servers, network);
        Contacts contacts(store, &servers, &tak);
        QAbstractItemModelTester tester(&contacts);
        QSignalSpy refreshed(&contacts, &Contacts::refreshed);
        // Without an API entry, the refresh ends at once without error.
        contacts.refreshServers();
        QCOMPARE(refreshed.size(), 1);
        QVERIFY(refreshed.last().at(0).toString().isEmpty());
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        contacts.refreshServers();
        QVERIFY(refreshed.wait(10000));
        QVERIFY(refreshed.last().at(0).toString().isEmpty());
        QCOMPARE(contacts.rowCount(), 2);
        QCOMPARE(contacts.index(0, Contacts::CallsignColumn).data().toString(),
                 QStringLiteral("Alpha"));
        QCOMPARE(contacts.index(0, Contacts::ServerColumn).data().toString(),
                 QStringLiteral("Test"));
        QCOMPARE(contacts.index(1, Contacts::ReportColumn).data().toString(),
                 QStringLiteral("No current position"));
        // The report column sorts by time. A contact without a report sorts first.
        QCOMPARE(contacts.index(1, Contacts::ReportColumn).data(Contacts::SortRole).toLongLong(),
                 std::numeric_limits<qint64>::min());
        QCOMPARE(contacts.index(0, Contacts::CallsignColumn).data(Contacts::SortRole).toString(),
                 QStringLiteral("Alpha"));
        // Contact details from the server show as plain text.
        QVERIFY(contacts.index(0, 0)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .startsWith(QStringLiteral("<qt>")));
        QVERIFY(!contacts.contacts().at(1).online);
        // A position report with a contact endpoint marks the user online.
        auto report = sampleEvent(QStringLiteral("ANDROID-1"));
        report.callsign = QStringLiteral("Bravo");
        report.origin = QStringLiteral("tak");
        report.detail = QStringLiteral("<contact endpoint=\"*:-1:stcp\"/>"
                                       "<takv platform=\"ATAK-CIV\" version=\"5.4\"/>");
        auto local = sampleEvent(QStringLiteral("local"));
        local.detail = report.detail;
        auto marker = sampleEvent(QStringLiteral("marker"));
        marker.origin = QStringLiteral("tak");
        QVERIFY(store.accept(report).isEmpty());
        QVERIFY(store.accept(local).isEmpty());
        QVERIFY(store.accept(marker).isEmpty());
        contacts.rebuild();
        QCOMPARE(contacts.rowCount(), 2);
        const auto &bravo = contacts.contacts().at(1);
        QCOMPARE(bravo.uid, QStringLiteral("ANDROID-1"));
        QVERIFY(bravo.online);
        QCOMPARE(bravo.endpoint, QStringLiteral("*:-1:stcp"));
        QCOMPARE(bravo.device, QStringLiteral("ATAK-CIV 5.4"));
        QCOMPARE(contacts.index(1, 0).data(Qt::UserRole).toString(), bravo.uid);
        // The store change rebuilds the list later, but an equal list keeps the model.
        QSignalSpy reset(&contacts, &QAbstractItemModel::modelReset);
        QVERIFY(store.accept(sampleEvent(QStringLiteral("other"))).isEmpty());
        QTest::qWait(1200);
        QCOMPARE(reset.size(), 0);
        // A server error names the entry and keeps the last list.
        status = 403;
        contacts.refreshServers();
        QVERIFY(refreshed.wait(10000));
        QVERIFY(refreshed.last().at(0).toString().startsWith(QStringLiteral("Test: ")));
        QCOMPARE(contacts.rowCount(), 2);
        // A deleted entry removes its server list.
        servers.remove(QStringLiteral("tak"));
        contacts.rebuild();
        QCOMPARE(contacts.rowCount(), 1);
        Contacts offline(store, nullptr, nullptr);
        QSignalSpy unavailable(&offline, &Contacts::refreshed);
        offline.refreshServers();
        QVERIFY(!unavailable.last().at(0).toString().isEmpty());
    }
    void takShapes()
    {
        auto sender = sampleEvent(QStringLiteral("SWAK-1"));
        sender.callsign = QStringLiteral("Swak");
        sender.type = QStringLiteral("a-f-G-U-C");
        const auto now = QDateTime::currentDateTimeUtc();
        QCOMPARE(takArgb(QColor(255, 0, 0)), QStringLiteral("-65536"));
        QCOMPARE(takArgb(QColor(0, 0, 255, 0x60)), QString::number(0x600000ff));
        auto marker = localObject(
            QStringLiteral("m1"), QStringLiteral("circle"), markerGeometry(59.5, 18.25));
        marker.description = QStringLiteral("Meet <here> & now");
        auto event = sharedObjectEvent(marker, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(event->type, QStringLiteral("b-m-p-s-m"));
        QCOMPARE(event->uid, marker.id);
        QCOMPARE(event->callsign, marker.title);
        QVERIFY(std::abs(event->latitude - 59.5) < 1e-6);
        QVERIFY(std::abs(event->longitude - 18.25) < 1e-6);
        QCOMPARE(event->stale, now.addSecs(SharedObjectStaleSeconds));
        QCOMPARE(detailColor(*event, u"color")->rgb(), QColor(255, 0, 0).rgb());
        QVERIFY(event->detail.contains(QStringLiteral("COT_MAPPING_SPOTMAP/b-m-p-s-m/-65536")));
        QVERIFY(detailElement(*event, u"archive"));
        QVERIFY(!detailElement(*event, u"marti"));
        QCOMPARE(links(*event).first().uid, sender.uid);
        const auto decoded = decodeCot(encodeCot(*event));
        QVERIFY(decoded.event);
        QCOMPARE(remarks(*decoded.event), marker.description);
        QCOMPARE(decoded.event->callsign, marker.title);
        // A military marker keeps its CoT unit type, so TAK clients show the same symbol.
        auto unit = marker;
        unit.symbol = QStringLiteral("a-f-G-U-C-I");
        unit.collection = MilitaryMarkerCollection;
        event = sharedObjectEvent(unit, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(event->type, QStringLiteral("a-f-G-U-C-I"));
        QCOMPARE(sharedCotType(unit), QStringLiteral("a-f-G-U-C-I"));
        QVERIFY(!detailElement(*event, u"usericon"));
        QVERIFY(!detailElement(*event, u"color"));
        unit.collection.clear();
        unit.symbol = QStringLiteral("a-h-G-U");
        QCOMPARE(sharedCotType(unit), QStringLiteral("a-h-G-U"));
        // Other collections and unknown symbols stay spot markers.
        unit.collection = CoreMarkerCollection;
        QCOMPARE(sharedCotType(unit), QStringLiteral("b-m-p-s-m"));
        unit.collection = MilitaryMarkerCollection;
        unit.symbol = QStringLiteral("a-f-G-X");
        QCOMPARE(sharedCotType(unit), QStringLiteral("b-m-p-s-m"));
        // A circle carries its radius in meters.
        const auto circle = localObject(QStringLiteral("c1"),
                                        QStringLiteral("range-circle"),
                                        vertices({{59, 18}, {59.01, 18}}));
        event = sharedObjectEvent(circle, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(event->type, QStringLiteral("u-d-c-c"));
        const auto shape = detailElement(*event, u"shape");
        QVERIFY(shape);
        const auto *ellipse = shape->child(u"ellipse");
        QVERIFY(ellipse);
        const auto major = ellipse->attribute(u"major").toDouble();
        QVERIFY(major > 1100 && major < 1125);
        QCOMPARE(ellipse->attribute(u"minor"), ellipse->attribute(u"major"));
        QCOMPARE(detailColor(*event, u"fillColor")->alpha(), 0x60);
        QVERIFY(std::abs(event->latitude - 59) < 1e-6);
        // A line is an open u-d-f shape.
        const auto line = localObject(
            QStringLiteral("l1"), QStringLiteral("line"), vertices({{59, 18}, {59.1, 18.1}}));
        event = sharedObjectEvent(line, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(event->type, QStringLiteral("u-d-f"));
        QCOMPARE(shapeLinks(*event).size(), 2);
        QCOMPARE(shapeLinks(*event).first().point, QStringLiteral("59.0000000,18.0000000"));
        QVERIFY(!detailElement(*event, u"fillColor"));
        QVERIFY(detailColor(*event, u"strokeColor"));
        // An area repeats the first point to close the shape.
        const auto area = localObject(QStringLiteral("a1"),
                                      QStringLiteral("polygon"),
                                      vertices({{59, 18}, {59.1, 18}, {59.1, 18.1}}));
        event = sharedObjectEvent(area, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(event->type, QStringLiteral("u-d-f"));
        const auto areaLinks = shapeLinks(*event);
        QCOMPARE(areaLinks.size(), 4);
        QCOMPARE(areaLinks.last().point, areaLinks.first().point);
        QCOMPARE(detailColor(*event, u"fillColor")->alpha(), 0x60);
        // A route has check points and route attributes.
        const auto route = localObject(QStringLiteral("r1"),
                                       QStringLiteral("route"),
                                       vertices({{59, 18}, {59.1, 18}, {59.2, 18.1}}));
        event = sharedObjectEvent(route, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(event->type, QStringLiteral("b-m-r"));
        const auto checkPoints = shapeLinks(*event);
        QCOMPARE(checkPoints.size(), 3);
        QCOMPARE(checkPoints.at(1).uid, QStringLiteral("r1.1"));
        QCOMPARE(checkPoints.at(1).type, QStringLiteral("b-m-p-w"));
        QCOMPARE(checkPoints.at(2).relation, QStringLiteral("c"));
        QVERIFY(detailElement(*event, u"link_attr"));
        // The longest route fits in one message.
        QJsonArray many;
        for (int i = 0; i < MaxRoutePoints; ++i) {
            const auto point = project(59 + i * 0.001, 18 + (i % 2) * 0.001);
            many.append(QJsonArray{point.x(), point.y()});
        }
        const auto longRoute = localObject(
            QStringLiteral("r2"), QStringLiteral("route"), {{QStringLiteral("vertices"), many}});
        event = sharedObjectEvent(longRoute, sender, {}, now);
        QVERIFY(event);
        QCOMPARE(shapeLinks(*event).size(), MaxRoutePoints);
        QVERIFY(encodeCot(*event).size() < MaxMessageBytes);
        // Recipients direct the delivery.
        event = sharedObjectEvent(marker,
                                  sender,
                                  {{QStringLiteral("ANDROID-1"), QStringLiteral("Bravo")},
                                   {QStringLiteral("ANDROID-2"), {}}},
                                  now);
        QVERIFY(event);
        const auto targets = destinations(*event);
        QCOMPARE(targets.size(), 2);
        QCOMPARE(targets.at(0).callsign, QStringLiteral("Bravo"));
        QCOMPARE(targets.at(1).uid, QStringLiteral("ANDROID-2"));
        // Unknown types and invalid objects give no event.
        QVERIFY(
            sharedCotType(localObject(QStringLiteral("x"), QStringLiteral("note"), {})).isEmpty());
        QVERIFY(!sharedObjectEvent(
            localObject(QStringLiteral("x"), QStringLiteral("note"), {{QStringLiteral("a"), 1}}),
            sender,
            {},
            now));
        QVERIFY(!sharedObjectEvent(
            localObject(QStringLiteral("x"), QStringLiteral("line"), vertices({{59, 18}})),
            sender,
            {},
            now));
        // A delete removes the shared object on a receiver with the same origin.
        const auto remove =
            sharedDeleteEvent(QStringLiteral("a1"),
                              QStringLiteral("u-d-f"),
                              {{QStringLiteral("ANDROID-1"), QStringLiteral("Bravo")}},
                              now);
        QVERIFY(eventError(remove).isEmpty());
        QCOMPARE(remove.type, QStringLiteral("t-x-d-d"));
        QCOMPARE(links(remove).first().uid, QStringLiteral("a1"));
        QCOMPARE(links(remove).first().relation, QStringLiteral("none"));
        QVERIFY(detailElement(remove, u"__forcedelete"));
        QCOMPARE(destinations(remove).size(), 1);
        Store store;
        auto received = *sharedObjectEvent(area, sender, {}, now);
        received.origin = QStringLiteral("peer");
        QVERIFY(store.accept(received).isEmpty());
        auto incoming = remove;
        incoming.origin = QStringLiteral("peer");
        QVERIFY(store.accept(incoming).isEmpty());
        QVERIFY(store.events().isEmpty());
    }
    void takDrawings()
    {
        auto sender = sampleEvent(QStringLiteral("SWAK-1"));
        sender.type = QStringLiteral("a-f-G-U-C");
        const auto now = QDateTime::currentDateTimeUtc();
        QVERIFY(takDrawingType(QStringLiteral("u-d-r")));
        QVERIFY(takDrawingType(QStringLiteral("b-m-r")));
        QVERIFY(!takDrawingType(QStringLiteral("b-m-p-s-m")));
        QVERIFY(!readTakDrawing(sampleEvent()));
        // Shapes that SWAK shares read back with their geometry and colors.
        auto circle = *sharedObjectEvent(localObject(QStringLiteral("c1"),
                                                     QStringLiteral("range-circle"),
                                                     vertices({{59, 18}, {59.01, 18}})),
                                         sender,
                                         {},
                                         now);
        auto drawing = readTakDrawing(circle);
        QVERIFY(drawing);
        QCOMPARE(drawing->kind, TakDrawing::Kind::Circle);
        QCOMPARE(drawing->points.size(), 72);
        const auto center = project(59, 18);
        QVERIFY(std::abs(groundDistance(center, drawing->points.at(18)) - 1112) < 15);
        QVERIFY(std::abs(groundDistance(center, drawing->points.at(0)) - 1112) < 15);
        QCOMPARE(drawing->anchor, center);
        QCOMPARE(drawing->stroke.rgb(), QColor(255, 0, 0).rgb());
        QCOMPARE(drawing->fill.alpha(), 0x60);
        QCOMPARE(drawing->strokeWidth, 3.0);
        const auto area =
            *sharedObjectEvent(localObject(QStringLiteral("a1"),
                                           QStringLiteral("polygon"),
                                           vertices({{59, 18}, {59.1, 18}, {59.1, 18.1}})),
                               sender,
                               {},
                               now);
        drawing = readTakDrawing(area);
        QVERIFY(drawing);
        QCOMPARE(drawing->kind, TakDrawing::Kind::Area);
        QCOMPARE(drawing->points.size(), 3);
        QVERIFY(drawing->fill.isValid());
        const auto line = *sharedObjectEvent(localObject(QStringLiteral("l1"),
                                                         QStringLiteral("line"),
                                                         vertices({{59, 18}, {59.1, 18.1}})),
                                             sender,
                                             {},
                                             now);
        drawing = readTakDrawing(line);
        QVERIFY(drawing);
        QCOMPARE(drawing->kind, TakDrawing::Kind::Line);
        QCOMPARE(drawing->points.size(), 2);
        QVERIFY(!drawing->fill.isValid());
        auto route = *sharedObjectEvent(localObject(QStringLiteral("r1"),
                                                    QStringLiteral("route"),
                                                    vertices({{59, 18}, {59.1, 18}, {59.2, 18.1}})),
                                        sender,
                                        {},
                                        now);
        drawing = readTakDrawing(route);
        QVERIFY(drawing);
        QCOMPARE(drawing->kind, TakDrawing::Kind::Route);
        QCOMPARE(drawing->points.size(), 3);
        QCOMPARE(drawing->anchor, drawing->points.first());
        QCOMPARE(drawing->stroke.rgb(), QColor(255, 0, 0).rgb());
        // An ATAK rectangle, a rotated ellipse, and stroke limits.
        auto rectangle = sampleEvent(QStringLiteral("rect"));
        rectangle.type = QStringLiteral("u-d-r");
        rectangle.detail = QStringLiteral(
            "<link point=\"59.0,18.0\"/><link point=\"59.0,18.1,10\"/><link point=\"59.1,18.1\"/>"
            "<link point=\"59.1,18.0\"/><strokeWeight value=\"40\"/>"
            "<fillColor value=\"-1761607681\"/>");
        drawing = readTakDrawing(rectangle);
        QVERIFY(drawing);
        QCOMPARE(drawing->kind, TakDrawing::Kind::Area);
        QCOMPARE(drawing->points.size(), 4);
        QCOMPARE(drawing->strokeWidth, 10.0);
        QCOMPARE(drawing->stroke, QColor(Qt::white));
        QCOMPARE(drawing->fill.alpha(), 0x96);
        auto ellipse = sampleEvent(QStringLiteral("ellipse"));
        ellipse.type = QStringLiteral("u-d-c-e");
        ellipse.latitude = 59;
        ellipse.longitude = 18;
        ellipse.detail =
            QStringLiteral("<shape><ellipse major=\"2000\" minor=\"500\" angle=\"90\"/></shape>"
                           "<strokeWeight value=\"bad\"/>");
        drawing = readTakDrawing(ellipse);
        QVERIFY(drawing);
        QCOMPARE(drawing->strokeWidth, 3.0);
        QVERIFY(!drawing->fill.isValid());
        // The major radius points east after a 90 degree rotation.
        QVERIFY(std::abs(groundDistance(center, drawing->points.at(18)) - 2000) < 30);
        QVERIFY(std::abs(groundDistance(center, drawing->points.at(0)) - 500) < 10);
        QVERIFY(drawing->points.at(18).x() > center.x());
        auto colored = route;
        colored.detail.replace(QStringLiteral("<link_attr color=\"-65536\""),
                               QStringLiteral("<link_attr color=\"-16711936\""));
        QCOMPARE(readTakDrawing(colored)->stroke.rgb(), QColor(0, 255, 0).rgb());
        // Invalid content gives no drawing.
        const auto rejected = [](const QString &type, const QString &detail) {
            auto event = sampleEvent(QStringLiteral("bad"));
            event.type = type;
            event.detail = detail;
            return !readTakDrawing(event);
        };
        QVERIFY(rejected(QStringLiteral("u-d-f"), QStringLiteral("<link point=\"59,18\"/>")));
        QVERIFY(rejected(QStringLiteral("u-d-f"),
                         QStringLiteral("<link point=\"59,18\"/><link point=\"x,18\"/>")));
        QVERIFY(rejected(QStringLiteral("u-d-f"),
                         QStringLiteral("<link point=\"89,18\"/><link point=\"59,18\"/>")));
        QVERIFY(rejected(QStringLiteral("u-d-f"),
                         QStringLiteral("<link point=\"59\"/><link point=\"59,18\"/>")));
        QVERIFY(rejected(QStringLiteral("u-d-c-c"), QStringLiteral("<shape/>")));
        QVERIFY(rejected(QStringLiteral("u-d-c-c"),
                         QStringLiteral("<shape><ellipse major=\"-1\" minor=\"5\"/></shape>")));
        QVERIFY(rejected(QStringLiteral("u-d-c-c"),
                         QStringLiteral("<shape><ellipse major=\"5\" minor=\"5e9\"/></shape>")));
        QString many;
        for (int i = 0; i <= MaxDrawingPoints; ++i) {
            many += QStringLiteral("<link point=\"59.%1,18\"/>").arg(i, 4, 10, QLatin1Char('0'));
        }
        QVERIFY(rejected(QStringLiteral("u-d-f"), many));
        auto truncated = line;
        truncated.detailTruncated = true;
        QVERIFY(!readTakDrawing(truncated));
        // A circle without a valid angle uses no rotation.
        ellipse.detail = QStringLiteral("<shape><ellipse major=\"100\" minor=\"100\"/></shape>");
        QVERIFY(readTakDrawing(ellipse));
    }
    void sharingRecipientRules()
    {
        // Recipients become event destinations, so they follow the identity rule of events.
        Store store;
        Servers servers(store);
        UserObjects objects;
        Identity identity;
        ObjectSharing sharing(objects, &servers, &identity);
        QVERIFY(objects
                    .put(localObject(QStringLiteral("m1"),
                                     QStringLiteral("circle"),
                                     markerGeometry(59.5, 18.25)))
                    .isEmpty());
        const ShareRecipient good{QStringLiteral("ANDROID-1"), QStringLiteral("Echo")};
        const ShareRecipient emoji{QStringLiteral("ANDROID-2"),
                                   QStringLiteral("Fox") + QString::fromUtf8("\xF0\x9F\xA6\x8A")};
        QVERIFY(!sharing.share(QStringLiteral("m1"), {good, emoji}).isEmpty());
        QVERIFY(!sharing.isShared(QStringLiteral("m1")));
        // One bad saved recipient is dropped. The rest of the sharing state stays.
        const QJsonObject values{
            {QStringLiteral("automatic"), true},
            {QStringLiteral("shared"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("id"), QStringLiteral("m1")},
                 {QStringLiteral("type"), QStringLiteral("b-m-p-s-m")},
                 {QStringLiteral("recipients"),
                  QJsonArray{QJsonObject{{QStringLiteral("uid"), good.uid},
                                         {QStringLiteral("callsign"), good.callsign}},
                             QJsonObject{{QStringLiteral("uid"), emoji.uid},
                                         {QStringLiteral("callsign"), emoji.callsign}}}}}}}};
        QVERIFY(sharing.validateSettings(values).isEmpty());
        sharing.restorePreferences(values);
        QVERIFY(sharing.automatic());
        QVERIFY(sharing.isShared(QStringLiteral("m1")));
        QCOMPARE(sharing.recipients(QStringLiteral("m1")), QList<ShareRecipient>{good});
    }
    void objectSharing()
    {
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        const auto next = [&receiver]() -> Event {
            if (!QTest::qWaitFor(
                    [&] {
                        return receiver.hasPendingDatagrams();
                    },
                    5000)) {
                return {};
            }
            QByteArray datagram(static_cast<qsizetype>(receiver.pendingDatagramSize()),
                                Qt::Uninitialized);
            receiver.readDatagram(datagram.data(), datagram.size());
            return decodeCot(datagram).event.value_or(Event{});
        };
        Store store;
        Servers servers(store);
        UserObjects objects;
        Identity identity;
        ObjectSharing sharing(objects, &servers, &identity);
        QSignalSpy added(&objects, &UserObjects::added);
        auto marker = localObject(
            QStringLiteral("m1"), QStringLiteral("circle"), markerGeometry(59.5, 18.25));
        QVERIFY(objects.put(marker).isEmpty());
        QCOMPARE(added.size(), 1);
        QVERIFY(!sharing.share(QStringLiteral("missing")).isEmpty());
        // Without a connection, the object waits and the error is kept.
        QVERIFY(sharing.share(QStringLiteral("m1")).isEmpty());
        QVERIFY(sharing.isShared(QStringLiteral("m1")));
        QVERIFY(!sharing.lastError().isEmpty());
        auto output = source(receiver.localPort());
        output.insert(QStringLiteral("protocol"), QStringLiteral("udpout"));
        QVERIFY(servers.put(QStringLiteral("out"), output).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("out")).startsWith(QStringLiteral("Sending"));
            },
            5000));
        sharing.flush();
        QVERIFY(sharing.lastError().isEmpty());
        auto sent = next();
        QCOMPARE(sent.uid, QStringLiteral("m1"));
        QCOMPARE(sent.type, QStringLiteral("b-m-p-s-m"));
        Event self;
        identity.apply(self);
        QCOMPARE(links(sent).first().uid, self.uid);
        // A change sends an update. An equal put sends nothing.
        marker.title = QStringLiteral("Rally point");
        QVERIFY(objects.put(marker).isEmpty());
        QCOMPARE(next().callsign, QStringLiteral("Rally point"));
        QCOMPARE(added.size(), 1);
        QVERIFY(objects.put(marker).isEmpty());
        QTest::qWait(200);
        QVERIFY(!receiver.hasPendingDatagrams());
        // New recipients get the object. Removed recipients get a delete first.
        const ShareRecipient bravo{QStringLiteral("ANDROID-1"), QStringLiteral("Bravo")};
        const ShareRecipient charlie{QStringLiteral("ANDROID-2"), QStringLiteral("Charlie")};
        QVERIFY(sharing.share(QStringLiteral("m1"), {bravo, charlie}).isEmpty());
        QCOMPARE(destinations(next()).size(), 2);
        QCOMPARE(sharing.recipients(QStringLiteral("m1")).size(), 2);
        QVERIFY(sharing.share(QStringLiteral("m1"), {bravo}).isEmpty());
        sent = next();
        QCOMPARE(sent.type, QStringLiteral("t-x-d-d"));
        QCOMPARE(destinations(sent).first().callsign, QStringLiteral("Charlie"));
        sent = next();
        QCOMPARE(sent.uid, QStringLiteral("m1"));
        QCOMPARE(destinations(sent).size(), 1);
        // A delete removes the object from receivers. Undo shares it again.
        objects.remove(QStringLiteral("m1"));
        sent = next();
        QCOMPARE(sent.type, QStringLiteral("t-x-d-d"));
        QCOMPARE(links(sent).first().uid, QStringLiteral("m1"));
        QCOMPARE(links(sent).first().type, QStringLiteral("b-m-p-s-m"));
        QVERIFY(!sharing.isShared(QStringLiteral("m1")));
        QVERIFY(!sharing.savePreferences().value(QStringLiteral("shared")).toArray().size());
        objects.undoStack().undo();
        QCOMPARE(next().uid, QStringLiteral("m1"));
        QVERIFY(sharing.isShared(QStringLiteral("m1")));
        // Automatic sharing sends new objects of supported types.
        sharing.setAutomatic(true);
        QVERIFY(objects
                    .put(localObject(QStringLiteral("l1"),
                                     QStringLiteral("line"),
                                     vertices({{59, 18}, {59.1, 18.1}})))
                    .isEmpty());
        sent = next();
        QCOMPARE(sent.uid, QStringLiteral("l1"));
        QCOMPARE(sent.type, QStringLiteral("u-d-f"));
        QVERIFY(sharing.isShared(QStringLiteral("l1")));
        // The preferences keep the shared objects. A restore sends them again.
        const auto saved = sharing.savePreferences();
        QVERIFY(saved.value(QStringLiteral("automatic")).toBool());
        QCOMPARE(saved.value(QStringLiteral("shared")).toArray().size(), 2);
        QVERIFY(sharing.validateSettings(saved).isEmpty());
        QVERIFY(!sharing.validateSettings({{QStringLiteral("automatic"), 1}}).isEmpty());
        QVERIFY(!sharing.validateSettings({{QStringLiteral("shared"), QJsonArray{1}}}).isEmpty());
        QVERIFY(
            !sharing
                 .validateSettings(
                     {{QStringLiteral("shared"),
                       QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("x")},
                                              {QStringLiteral("type"), QStringLiteral("a-f-G")}}}}})
                 .isEmpty());
        const auto objectPreferences = objects.savePreferences();
        objects.restorePreferences(objectPreferences);
        QCOMPARE(added.size(), 2);
        QTest::qWait(200);
        QVERIFY(!receiver.hasPendingDatagrams());
        UserObjects restoredObjects;
        ObjectSharing restored(restoredObjects, &servers, &identity);
        restored.restorePreferences(saved);
        restoredObjects.restorePreferences(objectPreferences);
        QVERIFY(restored.automatic());
        QVERIFY(restored.isShared(QStringLiteral("l1")));
        QStringList uids{next().uid, next().uid};
        uids.sort();
        QCOMPARE(uids, (QStringList{QStringLiteral("l1"), QStringLiteral("m1")}));
        // Settings keep the send state of known objects.
        restored.restorePreferences({{QStringLiteral("automatic"), false}});
        QVERIFY(!restored.automatic());
        QVERIFY(restored.isShared(QStringLiteral("m1")));
        restored.restorePreferences({{QStringLiteral("automatic"), 3}});
        QVERIFY(!restored.automatic());
        // Stop sharing sends a delete, and later changes stay local.
        sharing.stopSharing(QStringLiteral("l1"));
        sent = next();
        QCOMPARE(sent.type, QStringLiteral("t-x-d-d"));
        QCOMPARE(links(sent).first().type, QStringLiteral("u-d-f"));
        QVERIFY(!sharing.isShared(QStringLiteral("l1")));
        sharing.stopSharing(QStringLiteral("l1"));
        auto changedLine = *objects.find(QStringLiteral("l1"));
        changedLine.title = QStringLiteral("Changed");
        QVERIFY(objects.put(changedLine).isEmpty());
        QTest::qWait(200);
        QVERIFY(!receiver.hasPendingDatagrams());
        // Types without a TAK equal cannot be shared.
        QVERIFY(objects
                    .put(localObject(
                        QStringLiteral("n1"), QStringLiteral("note"), {{QStringLiteral("a"), 1}}))
                    .isEmpty());
        QVERIFY(!sharing.isShared(QStringLiteral("n1")));
        QVERIFY(!sharing.share(QStringLiteral("n1")).isEmpty());
        // Without servers, a share waits without error.
        ObjectSharing offline(objects, nullptr, nullptr);
        QVERIFY(offline.share(QStringLiteral("m1")).isEmpty());
        offline.flush();
        QVERIFY(offline.lastError().isEmpty());
    }
    void chatProtocol()
    {
        // An ATAK message to All Chat Rooms.
        auto all = geoChat(QStringLiteral("m-1"),
                           QStringLiteral("ANDROID-5"),
                           AllChatRooms,
                           QStringLiteral("Line one\nLine two ") + QChar(0x202E) +
                               QStringLiteral("evil 😀"));
        auto incoming = readChatMessage(all);
        QVERIFY(incoming);
        QCOMPARE(incoming->kind, ChatRoom::Kind::All);
        QCOMPARE(incoming->roomId, AllChatRooms);
        QCOMPARE(incoming->message.messageId, QStringLiteral("m-1"));
        QCOMPARE(incoming->message.senderUid, QStringLiteral("ANDROID-5"));
        QCOMPARE(incoming->message.senderCallsign, QStringLiteral("Echo"));
        QCOMPARE(incoming->message.text, QStringLiteral("Line one\nLine two evil 😀"));
        QVERIFY(!incoming->message.own());
        // Long text is cut with a mark.
        incoming = readChatMessage(geoChat(QStringLiteral("m-2"),
                                           QStringLiteral("ANDROID-5"),
                                           AllChatRooms,
                                           QString(MaxChatText + 10, QLatin1Char('x'))));
        QVERIFY(incoming);
        QCOMPARE(incoming->message.text.size(), MaxChatText + 2);
        QVERIFY(incoming->message.text.endsWith(QStringLiteral(" …")));
        // Team and direct rooms.
        incoming = readChatMessage(geoChat(QStringLiteral("m-3"),
                                           QStringLiteral("ANDROID-5"),
                                           QStringLiteral("Cyan"),
                                           QStringLiteral("team"),
                                           QStringLiteral("TeamGroups")));
        QVERIFY(incoming);
        QCOMPARE(incoming->kind, ChatRoom::Kind::Team);
        incoming = readChatMessage(geoChat(QStringLiteral("m-4"),
                                           QStringLiteral("ANDROID-5"),
                                           QStringLiteral("SWAK-1"),
                                           QStringLiteral("hi")));
        QVERIFY(incoming);
        QCOMPARE(incoming->kind, ChatRoom::Kind::Direct);
        QCOMPARE(incoming->roomId, QStringLiteral("SWAK-1"));
        // The chat group names the sender when no parent link exists.
        auto grouped = all;
        grouped.detail.replace(QStringLiteral("relation=\"p-p\""),
                               QStringLiteral("relation=\"x\""));
        QCOMPARE(readChatMessage(grouped)->message.senderUid, QStringLiteral("ANDROID-5"));
        // Invalid messages give no value.
        auto bad = all;
        bad.type = QStringLiteral("b-t-f-d");
        QVERIFY(!readChatMessage(bad));
        QVERIFY(
            !readChatMessage(geoChat({}, QStringLiteral("A"), AllChatRooms, QStringLiteral("x"))));
        QVERIFY(!readChatMessage(
            geoChat(QStringLiteral("m"), QStringLiteral("A"), AllChatRooms, QStringLiteral("  "))));
        QVERIFY(
            !readChatMessage(geoChat(QStringLiteral("m"), {}, AllChatRooms, QStringLiteral("x"))));
        bad = all;
        bad.detail = QStringLiteral("<remarks>x</remarks>");
        QVERIFY(!readChatMessage(bad));
        // A message ID becomes a receipt UID, so it follows the identity rule. Emoji in the text
        // stay.
        const auto smile = QString::fromUtf8("\xF0\x9F\x98\x80");
        QVERIFY(!readChatMessage(geoChat(QStringLiteral("id") + smile,
                                         QStringLiteral("ANDROID-5"),
                                         AllChatRooms,
                                         QStringLiteral("x"))));
        const auto withEmoji = readChatMessage(geoChat(QStringLiteral("m-emoji"),
                                                       QStringLiteral("ANDROID-5"),
                                                       AllChatRooms,
                                                       QStringLiteral("hi ") + smile));
        QVERIFY(withEmoji);
        QCOMPARE(withEmoji->message.text, QStringLiteral("hi ") + smile);
        // Built messages read back.
        auto sender = sampleEvent(QStringLiteral("SWAK-1"));
        sender.callsign = QStringLiteral("Swak");
        sender.type = QStringLiteral("a-f-G-U-C");
        ChatMessage message;
        message.messageId = QStringLiteral("own-1");
        message.text = QStringLiteral("<hello> & \"you\"");
        message.state = ChatMessage::State::Queued;
        const auto now = QDateTime::currentDateTimeUtc();
        const ChatRoom allRoom{AllChatRooms, AllChatRooms, ChatRoom::Kind::All, {}, 0};
        auto event = chatMessageEvent(sender, allRoom, {}, message, now);
        QVERIFY(eventError(event).isEmpty());
        QCOMPARE(event.uid, QStringLiteral("GeoChat.SWAK-1.All Chat Rooms.own-1"));
        const auto decoded = decodeCot(encodeCot(event));
        QVERIFY(decoded.event);
        incoming = readChatMessage(*decoded.event);
        QVERIFY(incoming);
        QCOMPARE(incoming->message.text, message.text);
        QCOMPARE(incoming->message.senderUid, sender.uid);
        QCOMPARE(incoming->message.senderCallsign, QStringLiteral("Swak"));
        QVERIFY(destinations(event).isEmpty());
        // Emoji are valid surrogate pairs. The text arrives complete.
        auto emoji = message;
        emoji.text = QStringLiteral("hello ") + QString::fromUtf8("\xF0\x9F\x98\x80");
        const auto emojiEvent =
            decodeCot(encodeCot(chatMessageEvent(sender, allRoom, {}, emoji, now)));
        QVERIFY(emojiEvent.event);
        const auto emojiChat = readChatMessage(*emojiEvent.event);
        QVERIFY(emojiChat);
        QCOMPARE(emojiChat->message.text, emoji.text);
        // A lone surrogate cannot travel in XML. The writer refuses the detail instead of dropping
        // the text.
        auto lone = message;
        lone.text = QStringLiteral("bad ") + QChar(0xD83D);
        QVERIFY(chatMessageEvent(sender, allRoom, {}, lone, now).detail.isEmpty());
        const ChatRoom direct{
            QStringLiteral("ANDROID-5"), QStringLiteral("Echo"), ChatRoom::Kind::Direct, {}, 0};
        event = chatMessageEvent(sender, direct, QStringLiteral("Echo"), message, now);
        QCOMPARE(destinations(event).first().callsign, QStringLiteral("Echo"));
        QCOMPARE(chat(event)->id, QStringLiteral("ANDROID-5"));
        const ChatRoom team{
            QStringLiteral("Cyan"), QStringLiteral("Cyan"), ChatRoom::Kind::Team, {}, 0};
        event = chatMessageEvent(sender, team, {}, message, now);
        QCOMPARE(chat(event)->parent, QStringLiteral("TeamGroups"));
        const ChatRoom longRoom{
            QString(120, QLatin1Char('r')), QStringLiteral("Long"), ChatRoom::Kind::Direct, {}, 0};
        event = chatMessageEvent(sender, longRoom, {}, message, now);
        QCOMPARE(event.uid, QStringLiteral("GeoChat.own-1"));
        // Receipts.
        const auto received = readChatMessage(all)->message;
        auto receipt = chatReceiptEvent(sender, received, ChatMessage::State::Delivered, now);
        QVERIFY(eventError(receipt).isEmpty());
        QCOMPARE(receipt.type, QStringLiteral("b-t-f-d"));
        QCOMPARE(destinations(receipt).first().callsign, QStringLiteral("Echo"));
        auto read = readChatReceipt(receipt);
        QVERIFY(read);
        QCOMPARE(read->messageId, QStringLiteral("m-1"));
        QCOMPARE(read->state, ChatMessage::State::Delivered);
        QCOMPARE(read->senderUid, sender.uid);
        receipt = chatReceiptEvent(sender, received, ChatMessage::State::Read, now);
        QCOMPARE(readChatReceipt(receipt)->state, ChatMessage::State::Read);
        auto pending = sampleEvent(QStringLiteral("own-1"));
        pending.type = QStringLiteral("b-t-f-p");
        QCOMPARE(readChatReceipt(pending)->messageId, QStringLiteral("own-1"));
        QCOMPARE(readChatReceipt(pending)->state, ChatMessage::State::Pending);
        pending.detail = QStringLiteral("<__chat messageId=\"other\"/>");
        QCOMPARE(readChatReceipt(pending)->messageId, QStringLiteral("other"));
        pending.detail = QStringLiteral("<__chat messageId=\"bad&#x9;id\"/>");
        QVERIFY(!readChatReceipt(pending));
        QVERIFY(!readChatReceipt(all));
    }
    void chatService()
    {
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        const auto next = [&receiver]() -> Event {
            if (!QTest::qWaitFor(
                    [&] {
                        return receiver.hasPendingDatagrams();
                    },
                    5000)) {
                return {};
            }
            QByteArray datagram(static_cast<qsizetype>(receiver.pendingDatagramSize()),
                                Qt::Uninitialized);
            receiver.readDatagram(datagram.data(), datagram.size());
            return decodeCot(datagram).event.value_or(Event{});
        };
        const auto quiet = [&receiver] {
            QTest::qWait(150);
            return !receiver.hasPendingDatagrams();
        };
        Store store;
        Servers servers(store);
        Identity identity;
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("uid"), QStringLiteral("SWAK-1"));
        identity.restorePreferences(values);
        Chat chat(store, &servers, &identity);
        QSignalSpy requested(&chat, &Chat::roomRequested);
        QCOMPARE(chat.rooms().size(), 2);
        QCOMPARE(chat.rooms().at(0).key, AllChatRooms);
        QCOMPARE(chat.rooms().at(1).key, QStringLiteral("Cyan"));
        // Invalid sends.
        QVERIFY(!chat.send(QStringLiteral("missing"), QStringLiteral("x")).isEmpty());
        QVERIFY(!chat.send(AllChatRooms, QStringLiteral("   ")).isEmpty());
        QVERIFY(!chat.send(AllChatRooms, QString(MaxChatText + 1, QLatin1Char('x'))).isEmpty());
        QVERIFY(
            !chat.send(AllChatRooms, QStringLiteral("bad") + QChar(0x202E) + QStringLiteral("text"))
                 .isEmpty());
        // Without a connection, a message waits.
        QVERIFY(chat.send(AllChatRooms, QStringLiteral("hello")).isEmpty());
        const auto &allRoom = *chat.room(AllChatRooms);
        QCOMPARE(allRoom.messages.size(), 1);
        QCOMPARE(allRoom.messages.first().state, ChatMessage::State::Queued);
        QCOMPARE(allRoom.unread, 0);
        auto output = source(receiver.localPort());
        output.insert(QStringLiteral("protocol"), QStringLiteral("udpout"));
        QVERIFY(servers.put(QStringLiteral("out"), output).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("out")).startsWith(QStringLiteral("Sending"));
            },
            5000));
        chat.flush();
        auto sent = next();
        QCOMPARE(sent.type, QStringLiteral("b-t-f"));
        QCOMPARE(remarks(sent), QStringLiteral("hello"));
        const auto ownId = swak::chat(sent)->messageId;
        QCOMPARE(chat.room(AllChatRooms)->messages.first().state, ChatMessage::State::Sent);
        // Receipts move the state forward only. The Store keeps no chat events.
        const auto receiptFor = [&ownId](const QString &type) {
            auto event = sampleEvent(ownId);
            event.type = type;
            return event;
        };
        QVERIFY(store.accept(receiptFor(QStringLiteral("b-t-f-p"))).isEmpty());
        QCOMPARE(chat.room(AllChatRooms)->messages.first().state, ChatMessage::State::Pending);
        QVERIFY(store.accept(receiptFor(QStringLiteral("b-t-f-r"))).isEmpty());
        QCOMPARE(chat.room(AllChatRooms)->messages.first().state, ChatMessage::State::Read);
        QVERIFY(store.accept(receiptFor(QStringLiteral("b-t-f-d"))).isEmpty());
        QCOMPARE(chat.room(AllChatRooms)->messages.first().state, ChatMessage::State::Read);
        QVERIFY(store.events().isEmpty());
        // Received group messages, duplicates, and late messages.
        auto first = geoChat(QStringLiteral("in-1"),
                             QStringLiteral("ANDROID-5"),
                             AllChatRooms,
                             QStringLiteral("one"));
        QVERIFY(store.accept(first).isEmpty());
        QVERIFY(store.accept(first).isEmpty());
        auto late = geoChat(QStringLiteral("in-0"),
                            QStringLiteral("ANDROID-5"),
                            AllChatRooms,
                            QStringLiteral("zero"));
        late.time = late.start = first.time.addSecs(-60);
        QVERIFY(store.accept(late).isEmpty());
        QCOMPARE(chat.room(AllChatRooms)->messages.size(), 3);
        QCOMPARE(chat.room(AllChatRooms)->messages.at(0).text, QStringLiteral("zero"));
        QCOMPARE(chat.room(AllChatRooms)->unread, 2);
        QVERIFY(quiet());
        chat.markRead(AllChatRooms);
        QCOMPARE(chat.room(AllChatRooms)->unread, 0);
        QVERIFY(quiet());
        // Own messages that come back, other teams, and direct messages to others stay out.
        QVERIFY(store
                    .accept(geoChat(QStringLiteral("echo"),
                                    QStringLiteral("SWAK-1"),
                                    AllChatRooms,
                                    QStringLiteral("me")))
                    .isEmpty());
        QVERIFY(store
                    .accept(geoChat(QStringLiteral("t-1"),
                                    QStringLiteral("ANDROID-5"),
                                    QStringLiteral("Red"),
                                    QStringLiteral("red"),
                                    QStringLiteral("TeamGroups")))
                    .isEmpty());
        QVERIFY(store
                    .accept(geoChat(QStringLiteral("d-0"),
                                    QStringLiteral("ANDROID-5"),
                                    QStringLiteral("ANDROID-9"),
                                    QStringLiteral("not me")))
                    .isEmpty());
        QCOMPARE(chat.rooms().size(), 2);
        QCOMPARE(chat.room(AllChatRooms)->messages.size(), 3);
        QVERIFY(store
                    .accept(geoChat(QStringLiteral("t-2"),
                                    QStringLiteral("ANDROID-5"),
                                    QStringLiteral("Cyan"),
                                    QStringLiteral("cyan"),
                                    QStringLiteral("TeamGroups")))
                    .isEmpty());
        QCOMPARE(chat.room(QStringLiteral("Cyan"))->messages.size(), 1);
        QVERIFY(quiet());
        // A direct message opens a room and sends a delivered receipt. Reading sends a read
        // receipt once.
        QVERIFY(store
                    .accept(geoChat(QStringLiteral("d-1"),
                                    QStringLiteral("ANDROID-5"),
                                    QStringLiteral("SWAK-1"),
                                    QStringLiteral("direct")))
                    .isEmpty());
        QCOMPARE(chat.rooms().size(), 3);
        const auto *direct = chat.room(QStringLiteral("ANDROID-5"));
        QVERIFY(direct);
        QCOMPARE(direct->title, QStringLiteral("Echo"));
        QCOMPARE(direct->kind, ChatRoom::Kind::Direct);
        QCOMPARE(direct->unread, 1);
        sent = next();
        QCOMPARE(sent.type, QStringLiteral("b-t-f-d"));
        QCOMPARE(readChatReceipt(sent)->messageId, QStringLiteral("d-1"));
        chat.markRead(QStringLiteral("ANDROID-5"));
        sent = next();
        QCOMPARE(sent.type, QStringLiteral("b-t-f-r"));
        chat.markRead(QStringLiteral("ANDROID-5"));
        QVERIFY(quiet());
        // A reply goes to the user with a marti destination.
        QVERIFY(chat.send(QStringLiteral("ANDROID-5"), QStringLiteral("reply")).isEmpty());
        sent = next();
        QCOMPARE(destinations(sent).first().callsign, QStringLiteral("Echo"));
        // Direct rooms open on request and stay within the limit.
        QVERIFY(chat.openDirect({}, {}).isEmpty());
        QVERIFY(chat.openDirect(QStringLiteral("SWAK-1"), {}).isEmpty());
        QVERIFY(chat.openDirect(QStringLiteral("Cyan"), {}).isEmpty());
        QCOMPARE(chat.openDirect(QStringLiteral("ANDROID-5"), QStringLiteral("Echo 2")),
                 QStringLiteral("ANDROID-5"));
        QCOMPARE(chat.room(QStringLiteral("ANDROID-5"))->title, QStringLiteral("Echo 2"));
        QCOMPARE(requested.last().at(0).toString(), QStringLiteral("ANDROID-5"));
        for (auto i = static_cast<int>(chat.rooms().size()); i < MaxChatRooms; ++i) {
            QVERIFY(!chat.openDirect(QStringLiteral("U-%1").arg(i), {}).isEmpty());
        }
        QVERIFY(chat.openDirect(QStringLiteral("U-full"), {}).isEmpty());
        QVERIFY(store
                    .accept(geoChat(QStringLiteral("d-2"),
                                    QStringLiteral("NEW"),
                                    QStringLiteral("SWAK-1"),
                                    QStringLiteral("lost")))
                    .isEmpty());
        QVERIFY(!chat.room(QStringLiteral("NEW")));
        // History, queue, and duplicate limits.
        Store quietStore;
        Chat offline(quietStore, nullptr, &identity);
        for (int i = 0; i <= MaxRoomMessages; ++i) {
            offline.receive(geoChat(QStringLiteral("h-%1").arg(i),
                                    QStringLiteral("A"),
                                    AllChatRooms,
                                    QStringLiteral("x")));
        }
        QCOMPARE(offline.room(AllChatRooms)->messages.size(), MaxRoomMessages);
        QCOMPARE(offline.room(AllChatRooms)->messages.first().messageId, QStringLiteral("h-1"));
        for (int i = MaxRoomMessages + 1; i <= Chat::MaxKnownMessages; ++i) {
            offline.receive(geoChat(QStringLiteral("h-%1").arg(i),
                                    QStringLiteral("A"),
                                    AllChatRooms,
                                    QStringLiteral("x")));
        }
        // The oldest message ID is forgotten after MaxKnownMessages newer IDs.
        offline.receive(geoChat(
            QStringLiteral("h-0"), QStringLiteral("A"), AllChatRooms, QStringLiteral("again")));
        QCOMPARE(offline.room(AllChatRooms)->messages.last().text, QStringLiteral("again"));
        offline.receive(geoChat(
            QStringLiteral("h-0"), QStringLiteral("A"), AllChatRooms, QStringLiteral("third")));
        QCOMPARE(offline.room(AllChatRooms)->messages.last().text, QStringLiteral("again"));
        for (int i = 0; i < Chat::MaxQueued; ++i) {
            QVERIFY(offline.send(AllChatRooms, QStringLiteral("wait")).isEmpty());
        }
        QVERIFY(!offline.send(AllChatRooms, QStringLiteral("full")).isEmpty());
        offline.flush();
        // Without an identity UID, nothing is sent.
        Chat anonymous(quietStore, nullptr, nullptr);
        QCOMPARE(anonymous.rooms().size(), 1);
        QVERIFY(!anonymous.send(AllChatRooms, QStringLiteral("x")).isEmpty());
    }
    void dataPackages()
    {
        QVERIFY(safePackagePath(QStringLiteral("a/b.cot")));
        for (const auto *bad :
             {"", "/etc/passwd", "../x", "a/../x", "a//b", "./a", "a\\b", "c:x", "a/."}) {
            QVERIFY2(!safePackagePath(QString::fromLatin1(bad)), bad);
        }
        QVERIFY(!safePackagePath(QString(256, QLatin1Char('a'))));
        QVERIFY(!safePackagePath(QStringLiteral("a") + QChar(0x202E)));
        // A written package reads back with its manifest data.
        QString error;
        const QList<PackageEntry> entries{{QStringLiteral("one/one.cot"), "<event/>"},
                                          {QStringLiteral("two.kml"), "<kml/>"}};
        const auto zip =
            writeDataPackage(QStringLiteral("pkg-1"), QStringLiteral("Patrol"), entries, &error);
        QVERIFY2(!zip.isEmpty(), qPrintable(error));
        auto package = readDataPackage(zip);
        QVERIFY2(package.error.isEmpty(), qPrintable(package.error));
        QCOMPARE(package.uid, QStringLiteral("pkg-1"));
        QCOMPARE(package.name, QStringLiteral("Patrol"));
        QCOMPARE(package.entries.size(), 2);
        QCOMPARE(package.entries.at(0).path, QStringLiteral("one/one.cot"));
        QCOMPARE(package.entries.at(1).bytes, QByteArray("<kml/>"));
        // Java readers, such as TAK Server and ATAK, need deflated entries. Each local file
        // header names compression method 8.
        for (qsizetype at = zip.indexOf("PK\x03\x04"); at >= 0;
             at = zip.indexOf("PK\x03\x04", at + 4)) {
            QCOMPARE(static_cast<unsigned char>(zip.at(at + 8)), 8);
            QCOMPARE(static_cast<unsigned char>(zip.at(at + 9)), 0);
        }
        // The writer rejects unsafe content.
        QVERIFY(writeDataPackage(QStringLiteral("x"), {}, {{QStringLiteral("../x"), "x"}}, &error)
                    .isEmpty());
        QVERIFY(writeDataPackage(QStringLiteral("x"),
                                 {},
                                 {{QStringLiteral("a"), "x"}, {QStringLiteral("a"), "y"}},
                                 &error)
                    .isEmpty());
        QVERIFY(
            writeDataPackage(
                QStringLiteral("x"), {}, {{QStringLiteral("MANIFEST/manifest.xml"), "x"}}, &error)
                .isEmpty());
        QVERIFY(writeDataPackage({}, {}, {}, &error).isEmpty());
        QVERIFY(writeDataPackage(QStringLiteral("x\ny"), {}, {}, nullptr).isEmpty());
        // A manifest selects and ignores entries. Missing entries are skipped.
        const QByteArray manifest =
            "<MissionPackageManifest version=\"2\"><Configuration>"
            "<Parameter name=\"uid\" value=\"u\"/><Parameter name=\"name\" value=\"N\"/>"
            "</Configuration><Contents><Content zipEntry=\"keep.cot\"/>"
            "<Content zipEntry=\"skip.cot\" ignore=\"true\"/><Content zipEntry=\"missing.cot\"/>"
            "<Content zipEntry=\"keep.cot\"/></Contents></MissionPackageManifest>";
        package = readDataPackage(rawZip({{"MANIFEST/manifest.xml", manifest},
                                          {"keep.cot", "a"},
                                          {"skip.cot", "b"},
                                          {"other.cot", "c"}}));
        QVERIFY(package.error.isEmpty());
        QCOMPARE(package.entries.size(), 1);
        QCOMPARE(package.entries.first().path, QStringLiteral("keep.cot"));
        // Without a manifest, all files count. Folders are skipped.
        package = readDataPackage(rawZip({{"folder/", {}}, {"a.cot", "a"}, {"b/c.kml", "c"}}));
        QVERIFY(package.error.isEmpty());
        QCOMPARE(package.entries.size(), 2);
        QVERIFY(package.uid.isEmpty());
        // Unsafe archives fail.
        const auto rejected = [](const QByteArray &bytes) {
            return !readDataPackage(bytes).error.isEmpty();
        };
        QVERIFY(rejected({}));
        QVERIFY(rejected("not a zip"));
        QVERIFY(rejected(rawZip({{"../evil.cot", "x"}})));
        QVERIFY(rejected(rawZip({{"/abs.cot", "x"}})));
        QVERIFY(rejected(rawZip({{"a.cot", "x"}, {"a.cot", "y"}})));
        QVERIFY(rejected(rawZip({{"link", {}, true}})));
        QVERIFY(rejected(rawZip({{"MANIFEST/manifest.xml", "<!DOCTYPE x><x/>"}})));
        QVERIFY(rejected(rawZip({{"MANIFEST/manifest.xml", "<a><b>"}})));
        QVERIFY(
            rejected(rawZip({{"MANIFEST/manifest.xml",
                              "<a><a><a><a><a><a><a><a><a/></a></a></a></a></a></a></a></a>"}})));
        QByteArray many = "<M><Contents>";
        for (int i = 0; i <= 1024; ++i) {
            many += "<Content zipEntry=\"x\"/>";
        }
        QVERIFY(rejected(rawZip({{"MANIFEST/manifest.xml", many + "</Contents></M>"}})));
        QVERIFY(rejected(rawZip({{"MANIFEST/manifest.xml", QByteArray(300 * 1024, ' ')}})));
        QList<RawEntry> crowd;
        for (int i = 0; i <= MaxPackageEntries; ++i) {
            crowd.append({QByteArray("f") + QByteArray::number(i), "x"});
        }
        QVERIFY(rejected(rawZip(crowd)));
        QVERIFY(rejected(QByteArray(MaxPackageBytes + 1, 'x')));
        auto damaged = zip;
        damaged.truncate(damaged.size() / 2);
        QVERIFY(rejected(damaged));
    }
    void cotToLocalObjects()
    {
        auto marker = sampleEvent(QStringLiteral("spot-1"));
        marker.type = QStringLiteral("b-m-p-s-m");
        marker.callsign = QStringLiteral("Rally");
        marker.latitude = 59.5;
        marker.longitude = 18.25;
        marker.detail = QStringLiteral("<color argb=\"-16711936\"/><remarks>Meet here</remarks>");
        auto item = localObjectFromCot(marker);
        QVERIFY(item);
        QCOMPARE(item->id, QStringLiteral("spot-1"));
        QCOMPARE(item->type, QStringLiteral("circle"));
        QCOMPARE(item->title, QStringLiteral("Rally"));
        QCOMPARE(item->description, QStringLiteral("Meet here"));
        QCOMPARE(item->color, QStringLiteral("#00ff00"));
        QVERIFY(item->symbol.isEmpty());
        auto unit = marker;
        unit.type = QStringLiteral("a-h-G-U-C-A");
        unit.uid = QStringLiteral("unit with spaces");
        unit.detail.clear();
        item = localObjectFromCot(unit);
        QVERIFY(item);
        QCOMPARE(item->symbol, QStringLiteral("a-h-G-U-C-A"));
        QCOMPARE(item->collection, MilitaryMarkerCollection);
        QVERIFY(item->id.startsWith(QStringLiteral("cot-")));
        QCOMPARE(item->id.size(), 36);
        QCOMPARE(item->color, QStringLiteral("#ffc857"));
        // Shapes and routes keep their geometry kind.
        auto sender = sampleEvent(QStringLiteral("SWAK-1"));
        sender.type = QStringLiteral("a-f-G-U-C");
        const auto now = QDateTime::currentDateTimeUtc();
        const auto roundTrip = [&](const UserObject &object) {
            return localObjectFromCot(*sharedObjectEvent(object, sender, {}, now));
        };
        const auto circle = localObject(QStringLiteral("c1"),
                                        QStringLiteral("range-circle"),
                                        vertices({{59, 18}, {59.01, 18}}));
        item = roundTrip(circle);
        QVERIFY(item);
        QCOMPARE(item->type, QStringLiteral("range-circle"));
        const auto points = polygonVertices(item->geometry);
        QVERIFY(std::abs(groundDistance(points.at(0), points.at(1)) - 1112) < 15);
        const auto area = localObject(QStringLiteral("a1"),
                                      QStringLiteral("polygon"),
                                      vertices({{59, 18}, {59.1, 18}, {59.1, 18.1}}));
        QCOMPARE(roundTrip(area)->type, QStringLiteral("polygon"));
        QCOMPARE(polygonVertices(roundTrip(area)->geometry).size(), 3);
        const auto line = localObject(
            QStringLiteral("l1"), QStringLiteral("line"), vertices({{59, 18}, {59.1, 18.1}}));
        QCOMPARE(roundTrip(line)->type, QStringLiteral("line"));
        const auto route = localObject(QStringLiteral("r1"),
                                       QStringLiteral("route"),
                                       vertices({{59, 18}, {59.1, 18}, {59.2, 18.1}}));
        QCOMPARE(roundTrip(route)->type, QStringLiteral("route"));
        QCOMPARE(roundTrip(route)->color, QStringLiteral("#ff0000"));
        auto open = *sharedObjectEvent(route, sender, {}, now);
        open.type = QStringLiteral("u-d-f");
        QCOMPARE(localObjectFromCot(open)->type, QStringLiteral("route"));
        // Unknown types and shapes beyond the local limits give no object.
        auto other = marker;
        other.type = QStringLiteral("t-x-c-t");
        QVERIFY(!localObjectFromCot(other));
        QString big;
        for (int i = 0; i < 70; ++i) {
            big += QStringLiteral("<link point=\"%1,%2\"/>")
                       .arg(59 + std::sin(i * 0.09) * 0.1)
                       .arg(18 + std::cos(i * 0.09) * 0.1);
        }
        auto large = marker;
        large.type = QStringLiteral("u-d-f");
        // A closed shape with 70 corners exceeds the local area limit of 64 corners.
        large.detail = big + big.left(big.indexOf(QStringLiteral("/>")) + 2);
        QVERIFY(!localObjectFromCot(large));
        auto unnamed = marker;
        unnamed.callsign.clear();
        unnamed.uid = QStringLiteral("named-by-uid");
        QCOMPARE(localObjectFromCot(unnamed)->title, QStringLiteral("named-by-uid"));
    }
    void packageImport()
    {
        QTemporaryDir folder;
        UserObjects objects;
        auto marker = sampleEvent(QStringLiteral("spot-1"));
        marker.type = QStringLiteral("b-m-p-s-m");
        marker.callsign = QStringLiteral("Rally");
        marker.latitude = 59.5;
        marker.longitude = 18.25;
        auto unknown = marker;
        unknown.type = QStringLiteral("t-x-c-t");
        DataPackage package;
        package.name = QStringLiteral("Patrol");
        package.entries = {{QStringLiteral("a/marker.cot"), encodeCot(marker)},
                           {QStringLiteral("b/marker.xml"), encodeCot(marker)},
                           {QStringLiteral("unknown.cot"), encodeCot(unknown)},
                           {QStringLiteral("broken.cot"), "<event"},
                           {QStringLiteral("notes.xml"), "<notes/>"},
                           {QStringLiteral("photo.png"), "png"},
                           {QStringLiteral("layer/roads.kml"), "<kml/>"},
                           {QStringLiteral("layer/roads.dbf"), "dbf"},
                           {QStringLiteral("map.tif"), "tif"}};
        auto result = importDataPackage(package,
                                        QStringLiteral("0123456789abcdef0123"),
                                        folder.filePath(QStringLiteral("pkg")),
                                        &objects,
                                        nullptr,
                                        nullptr);
        QCOMPARE(result.objects, 2);
        QCOMPARE(objects.items().size(), 1);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.skipped,
                 (QStringList{QStringLiteral("notes.xml"),
                              QStringLiteral("photo.png"),
                              QStringLiteral("unknown.cot"),
                              QStringLiteral("broken.cot"),
                              QStringLiteral("layer/roads.kml"),
                              QStringLiteral("map.tif")}));
        // Files stay in a private folder.
        const QFileInfo kml(folder.filePath(QStringLiteral("pkg/layer/roads.kml")));
        QVERIFY(kml.exists());
        QCOMPARE(kml.permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup),
                 QFileDevice::Permissions{});
        QVERIFY(QFileInfo::exists(folder.filePath(QStringLiteral("pkg/layer/roads.dbf"))));
        QVERIFY(!QFileInfo::exists(folder.filePath(QStringLiteral("pkg/photo.png"))));
        QVERIFY(result.summary().contains(QStringLiteral("2 objects")));
        QVERIFY(result.summary().contains(QStringLiteral("6 files skipped")));
        // A full object store reports errors.
        for (int i = 0; objects.items().size() < 256; ++i) {
            auto item = localObjectFromCot(marker);
            item->id = QStringLiteral("fill-%1").arg(i);
            if (!objects.put(*item).isEmpty()) {
                break;
            }
        }
        auto next = marker;
        next.uid = QStringLiteral("new-marker");
        package.entries = {{QStringLiteral("new.cot"), encodeCot(next)}};
        result = importDataPackage(package,
                                   QStringLiteral("k"),
                                   folder.filePath(QStringLiteral("pkg2")),
                                   &objects,
                                   nullptr,
                                   nullptr);
        QCOMPARE(result.objects, 0);
        QCOMPARE(result.errors.size(), 1);
        QVERIFY(result.summary().contains(QStringLiteral("Nothing imported.")));
        // A folder that cannot exist stops the import.
        QFile blocker(folder.filePath(QStringLiteral("file")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        package.entries = {{QStringLiteral("x.kml"), "<kml/>"}};
        result = importDataPackage(package,
                                   QStringLiteral("k"),
                                   folder.filePath(QStringLiteral("file/sub")),
                                   &objects,
                                   nullptr,
                                   nullptr);
        QCOMPARE(result.errors.size(), 1);
        // A file that cannot be written gets no feed. Its folder is blocked by a file here.
        DataSources feeds({});
        QVERIFY(QDir().mkpath(folder.filePath(QStringLiteral("pkg3"))));
        QFile layer(folder.filePath(QStringLiteral("pkg3/layer")));
        QVERIFY(layer.open(QIODevice::WriteOnly));
        layer.close();
        package.entries = {{QStringLiteral("layer/roads.kml"), "<kml/>"},
                           {QStringLiteral("ok.kml"), "<kml/>"}};
        result = importDataPackage(package,
                                   QStringLiteral("k"),
                                   folder.filePath(QStringLiteral("pkg3")),
                                   &objects,
                                   &feeds,
                                   nullptr);
        QCOMPARE(result.errors.size(), 1);
        QCOMPARE(result.feeds.size(), 1);
        QVERIFY(feeds.sources()
                    .value(result.feeds.first())
                    .toObject()
                    .value(QStringLiteral("endpoint"))
                    .toString()
                    .endsWith(QStringLiteral("ok.kml")));
    }
    void fileTransfers()
    {
        // Server file lists.
        const QByteArray hashA(64, 'a');
        const QByteArray list = "{\"resultCount\":3,\"results\":["
                                "{\"hash\":\"" +
                                hashA.toUpper() +
                                "\",\"name\":\"Patrol\",\"filename\":\"patrol.zip\","
                                "\"submitter\":\"alice\",\"mimeType\":\"application/"
                                "zip\",\"keywords\":[\"missionpackage\"],"
                                "\"size\":1234,\"submissionTime\":\"2026-10-08T10:00:00.000Z\"},"
                                "{\"hash\":\"short\"},{\"hash\":\"" +
                                QByteArray(64, 'b') + "\",\"filename\":\"x.kml\",\"size\":-5}]}";
        QList<ServerFile> files;
        QVERIFY(readServerFiles(list, &files));
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).hash, QString::fromLatin1(hashA));
        QCOMPARE(files.at(0).keywords, QStringList{QStringLiteral("missionpackage")});
        QCOMPARE(files.at(0).size, 1234);
        QVERIFY(files.at(0).time.isValid());
        QCOMPARE(files.at(1).name, QStringLiteral("x.kml"));
        QCOMPARE(files.at(1).size, -1);
        QVERIFY(!readServerFiles("[]", &files));
        QVERIFY(!readServerFiles("{\"results\":{}}", &files));
        // Offer events read back.
        auto sender = sampleEvent(QStringLiteral("SWAK-1"));
        sender.callsign = QStringLiteral("Swak");
        const auto offer = fileOfferEvent(
            sender,
            QStringLiteral("https://tak.example:8443/Marti/sync/content?hash=") + hashA,
            QStringLiteral("Patrol"),
            QStringLiteral("patrol.zip"),
            QString::fromLatin1(hashA),
            99,
            {{QStringLiteral("ANDROID-1"), QStringLiteral("Bravo")}},
            QDateTime::currentDateTimeUtc());
        QVERIFY(eventError(offer).isEmpty());
        QCOMPARE(fileShare(offer)->sha256, QString::fromLatin1(hashA));
        QCOMPARE(fileShare(offer)->sizeInBytes, 99);
        QCOMPARE(destinations(offer).first().callsign, QStringLiteral("Bravo"));
        QVERIFY(detailElement(offer, u"ackrequest"));
        // A package on the test server.
        auto marker = sampleEvent(QStringLiteral("spot-1"));
        marker.type = QStringLiteral("b-m-p-s-m");
        marker.callsign = QStringLiteral("Rally");
        marker.latitude = 59.5;
        marker.longitude = 18.25;
        QString error;
        const auto package = writeDataPackage(QStringLiteral("p"),
                                              QStringLiteral("Patrol"),
                                              {{QStringLiteral("m/m.cot"), encodeCot(marker)}},
                                              &error);
        const auto packageHash =
            QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex();
        const QByteArray kml = "<kml/>";
        const auto kmlHash = QCryptographicHash::hash(kml, QCryptographicHash::Sha256).toHex();
        const QByteArray broken = "PK broken";
        const auto brokenHash =
            QCryptographicHash::hash(broken, QCryptographicHash::Sha256).toHex();
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        api.respond = [&](const QByteArray &request) {
            QByteArray body;
            if (request.startsWith("GET /Marti/sync/content?hash=" + packageHash)) {
                body = package;
            } else if (request.startsWith("GET /Marti/sync/content?hash=" + kmlHash)) {
                body = kml;
            } else if (request.startsWith("GET /Marti/sync/content?hash=" + brokenHash)) {
                body = broken;
            } else if (request.startsWith("GET /Marti/sync/content?hash=" + QByteArray(64, 'c'))) {
                body = "wrong content";
            } else if (request.startsWith("GET /Marti/sync/search ")) {
                body = status == 200 ? list : "not json";
                return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") +
                       QByteArray::number(body.size()) + "\r\n\r\n" + body;
            } else if (!request.startsWith("DELETE ") && !request.startsWith("POST ")) {
                return QByteArray("HTTP/1.1 404 X\r\nContent-Length: 0\r\n\r\n");
            }
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        QTemporaryDir folder;
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakServerApi tak(servers, network);
        Identity identity;
        UserObjects objects;
        FileShares shares(store, &servers, &tak, &identity, &objects, nullptr, nullptr);
        shares.setDirectory(folder.path());
        QSignalSpy changed(&shares, &FileShares::changed);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QVERIFY(servers.put(QStringLiteral("udp"), source(9)).isEmpty());
        const auto offerFor = [&](const QByteArray &hash,
                                  const QString &fileName,
                                  qint64 size,
                                  const QString &origin) {
            auto event = fileOfferEvent(sender,
                                        {},
                                        QStringLiteral("File ") + fileName,
                                        fileName,
                                        QString::fromLatin1(hash),
                                        size,
                                        {},
                                        QDateTime::currentDateTimeUtc());
            event.origin = origin;
            return event;
        };
        // Offers from servers appear. Local offers and offers without a valid hash do not.
        QVERIFY(store
                    .accept(offerFor(packageHash,
                                     QStringLiteral("patrol.zip"),
                                     package.size(),
                                     QStringLiteral("tak")))
                    .isEmpty());
        QVERIFY(store
                    .accept(offerFor(packageHash,
                                     QStringLiteral("patrol.zip"),
                                     package.size(),
                                     QStringLiteral("tak")))
                    .isEmpty());
        QVERIFY(store.accept(offerFor(packageHash, QStringLiteral("local.zip"), 1, {})).isEmpty());
        QVERIFY(store.accept(offerFor("bad", QStringLiteral("bad.zip"), 1, QStringLiteral("tak")))
                    .isEmpty());
        QVERIFY(store
                    .accept(offerFor(QByteArray(64, 'd'),
                                     QStringLiteral("huge.zip"),
                                     MaxPackageBytes + 1,
                                     QStringLiteral("tak")))
                    .isEmpty());
        QVERIFY(store
                    .accept(offerFor(
                        QByteArray(64, 'e'), QStringLiteral("mesh.zip"), 1, QStringLiteral("udp")))
                    .isEmpty());
        QVERIFY(store.events().isEmpty());
        QCOMPARE(shares.offers().size(), 3);
        QCOMPARE(shares.offers().at(0).state, FileOffer::State::Offered);
        QCOMPARE(shares.offers().at(1).state, FileOffer::State::Failed);
        QCOMPARE(shares.offers().at(2).state, FileOffer::State::Failed);
        QVERIFY(!shares.download(QStringLiteral("tak"), QString(64, QLatin1Char('d'))).isEmpty());
        QVERIFY(!shares.download(QStringLiteral("tak"), QStringLiteral("missing")).isEmpty());
        // A download checks the hash and imports the package.
        QVERIFY(shares.download(QStringLiteral("tak"), QString::fromLatin1(packageHash)).isEmpty());
        QVERIFY(
            !shares.download(QStringLiteral("tak"), QString::fromLatin1(packageHash)).isEmpty());
        QCOMPARE(shares.offers().at(0).state, FileOffer::State::Downloading);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shares.offers().at(0).state == FileOffer::State::Imported;
            },
            10000));
        QVERIFY(objects.find(QStringLiteral("spot-1")));
        QVERIFY(shares.offers().at(0).status.contains(QStringLiteral("1 objects")));
        // A single shared file imports as a one-file package.
        QVERIFY(store
                    .accept(offerFor(
                        kmlHash, QStringLiteral("roads.kml"), kml.size(), QStringLiteral("tak")))
                    .isEmpty());
        QVERIFY(shares.download(QStringLiteral("tak"), QString::fromLatin1(kmlHash)).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shares.offers().last().state == FileOffer::State::Imported;
            },
            10000));
        QVERIFY(shares.offers().last().status.contains(QStringLiteral("1 files skipped")));
        // A damaged zip, a wrong hash, and a missing file fail.
        QVERIFY(
            store
                .accept(offerFor(
                    brokenHash, QStringLiteral("broken.zip"), broken.size(), QStringLiteral("tak")))
                .isEmpty());
        QVERIFY(shares.download(QStringLiteral("tak"), QString::fromLatin1(brokenHash)).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shares.offers().last().state == FileOffer::State::Failed;
            },
            10000));
        QVERIFY(
            store
                .accept(offerFor(
                    QByteArray(64, 'c'), QStringLiteral("wrong.zip"), 13, QStringLiteral("tak")))
                .isEmpty());
        QVERIFY(shares.download(QStringLiteral("tak"), QString(64, QLatin1Char('c'))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shares.offers().last().state == FileOffer::State::Failed;
            },
            10000));
        QVERIFY(shares.offers().last().status.contains(QStringLiteral("SHA-256")));
        QVERIFY(store
                    .accept(offerFor(
                        QByteArray(64, 'f'), QStringLiteral("gone.zip"), 13, QStringLiteral("tak")))
                    .isEmpty());
        QVERIFY(shares.download(QStringLiteral("tak"), QString(64, QLatin1Char('f'))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shares.offers().last().state == FileOffer::State::Failed;
            },
            10000));
        QVERIFY(shares.offers().last().status.contains(QStringLiteral("does not have")));
        const auto count = shares.offers().size();
        shares.dismiss(QStringLiteral("tak"), QString(64, QLatin1Char('f')));
        QCOMPARE(shares.offers().size(), count - 1);
        // The server file list, downloads from it, and deletes.
        QSignalSpy searched(&shares, &FileShares::searched);
        shares.search(QStringLiteral("tak"));
        QVERIFY(searched.wait(10000));
        QVERIFY(searched.last().at(2).toString().isEmpty());
        QCOMPARE(searched.last().at(1).value<QList<ServerFile>>().size(), 2);
        status = 500;
        shares.search(QStringLiteral("tak"));
        QVERIFY(searched.wait(10000));
        QVERIFY(!searched.last().at(2).toString().isEmpty());
        status = 200;
        ServerFile listed;
        listed.hash = QString::fromLatin1(kmlHash);
        listed.name = QStringLiteral("Roads");
        QVERIFY(!shares.downloadServerFile(QStringLiteral("tak"), ServerFile{}).isEmpty());
        listed.hash = QString(64, QLatin1Char('9'));
        QVERIFY(shares.downloadServerFile(QStringLiteral("tak"), listed).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shares.offers().last().state == FileOffer::State::Failed;
            },
            10000));
        QSignalSpy removed(&shares, &FileShares::removed);
        shares.remove(QStringLiteral("tak"), QString::fromLatin1(hashA));
        QVERIFY(removed.wait(10000));
        QVERIFY(removed.last().at(2).toString().isEmpty());
        QVERIFY(api.requests.last().startsWith("DELETE /Marti/sync/delete?hash=" + hashA));
        status = 403;
        shares.remove(QStringLiteral("tak"), QString::fromLatin1(hashA));
        QVERIFY(removed.wait(10000));
        QVERIFY(removed.last().at(2).toString().contains(QStringLiteral("does not allow")));
        shares.remove(QStringLiteral("tak"), QStringLiteral("bad"));
        QVERIFY(!removed.last().at(2).toString().isEmpty());
        status = 200;
        // Send uploads a package and offers it to the recipients.
        auto output = source(receiver.localPort());
        output.insert(QStringLiteral("protocol"), QStringLiteral("udpout"));
        QVERIFY(servers.put(QStringLiteral("out"), output).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("out")).startsWith(QStringLiteral("Sending"));
            },
            5000));
        const ShareRecipient bravo{QStringLiteral("ANDROID-1"), QStringLiteral("Bravo")};
        QVERIFY(
            !shares
                 .send(QStringLiteral("tak"), QStringLiteral("Out"), {QStringLiteral("spot-1")}, {})
                 .isEmpty());
        QVERIFY(!shares
                     .send(QStringLiteral("tak"),
                           QStringLiteral("  "),
                           {QStringLiteral("spot-1")},
                           {bravo})
                     .isEmpty());
        QVERIFY(!shares.send(QStringLiteral("tak"), QStringLiteral("Out"), {}, {bravo}).isEmpty());
        QVERIFY(!shares
                     .send(QStringLiteral("tak"),
                           QStringLiteral("Out"),
                           {QStringLiteral("missing")},
                           {bravo})
                     .isEmpty());
        QVERIFY(!shares
                     .send(QStringLiteral("udp"),
                           QStringLiteral("Out"),
                           {QStringLiteral("spot-1")},
                           {bravo})
                     .isEmpty());
        QSignalSpy sent(&shares, &FileShares::sent);
        QVERIFY(shares
                    .send(QStringLiteral("tak"),
                          QStringLiteral("Out/1"),
                          {QStringLiteral("spot-1")},
                          {bravo})
                    .isEmpty());
        QVERIFY(!shares
                     .send(QStringLiteral("tak"),
                           QStringLiteral("Out"),
                           {QStringLiteral("spot-1")},
                           {bravo})
                     .isEmpty());
        QVERIFY(sent.wait(10000));
        QVERIFY2(sent.last().at(0).toString().isEmpty(), qPrintable(sent.last().at(0).toString()));
        const auto upload = api.requests.last();
        QVERIFY(upload.startsWith("POST /Marti/sync/missionupload?hash="));
        QVERIFY(upload.contains("filename=Out_1.zip"));
        QVERIFY(upload.contains("multipart/form-data; boundary="));
        QVERIFY(upload.contains("name=\"assetfile\""));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return receiver.hasPendingDatagrams();
            },
            5000));
        QByteArray datagram(static_cast<qsizetype>(receiver.pendingDatagramSize()),
                            Qt::Uninitialized);
        receiver.readDatagram(datagram.data(), datagram.size());
        const auto announced = decodeCot(datagram).event;
        QVERIFY(announced);
        QCOMPARE(announced->type, QStringLiteral("b-f-t-r"));
        const auto share = fileShare(*announced);
        QVERIFY(
            share->senderUrl.contains(QStringLiteral("/Marti/sync/content?hash=") + share->sha256));
        QVERIFY(upload.contains(share->sha256.toLatin1()));
        QCOMPARE(destinations(*announced).first().callsign, QStringLiteral("Bravo"));
        status = 500;
        QVERIFY(shares
                    .send(QStringLiteral("tak"),
                          QStringLiteral("Out"),
                          {QStringLiteral("spot-1")},
                          {bravo})
                    .isEmpty());
        QVERIFY(sent.wait(10000));
        QVERIFY(!sent.last().at(0).toString().isEmpty());
        // The offer list stays bounded.
        for (int i = 0; i < FileShares::MaxOffers + 2; ++i) {
            const auto hash =
                QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha256).toHex();
            QVERIFY(store.accept(offerFor(hash, QStringLiteral("n.zip"), 1, QStringLiteral("tak")))
                        .isEmpty());
        }
        QCOMPARE(shares.offers().size(), FileShares::MaxOffers);
        // Without a REST API, nothing starts.
        FileShares offline(store, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        QSignalSpy offlineSearch(&offline, &FileShares::searched);
        offline.search(QStringLiteral("tak"));
        QCOMPARE(offlineSearch.size(), 1);
        QVERIFY(!offline.send(QStringLiteral("tak"), QStringLiteral("x"), {}, {}).isEmpty());
        QVERIFY(
            !offline.download(QStringLiteral("tak"), QString::fromLatin1(packageHash)).isEmpty());
    }
    void missionParsing()
    {
        QVERIFY(validMissionName(QStringLiteral("Ops 1")));
        for (const auto &bad : {QString{},
                                QStringLiteral("a/b"),
                                QStringLiteral(".."),
                                QStringLiteral("a?b"),
                                QStringLiteral("a%20"),
                                QString(129, QLatin1Char('a')),
                                QStringLiteral("a\nb")}) {
            QVERIFY(!validMissionName(bad));
        }
        const QByteArray list =
            "{\"version\":\"3\",\"type\":\"Mission\",\"data\":["
            "{\"name\":\"Ops\",\"description\":\"Operations\",\"creatorUid\":\"ANDROID-1\","
            "\"tool\":\"public\","
            "\"keywords\":[\"a\"],\"createTime\":\"2026-10-08T10:00:00.000Z\","
            "\"passwordProtected\":false,"
            "\"uids\":[{},{}],\"contents\":[{}]},"
            "{\"name\":\"Secret\",\"passwordProtected\":true,\"createTime\":1791000000000},"
            "{\"name\":\"bad/name\"}]}";
        QList<Mission> missions;
        QVERIFY(readMissions(list, &missions));
        QCOMPARE(missions.size(), 2);
        QCOMPARE(missions.at(0).description, QStringLiteral("Operations"));
        QCOMPARE(missions.at(0).objects, 2);
        QCOMPARE(missions.at(0).files, 1);
        QVERIFY(missions.at(0).created.isValid());
        QVERIFY(missions.at(1).passwordProtected);
        QVERIFY(missions.at(1).created.isValid());
        // A number that does not fit in milliseconds gives no time, not undefined behavior.
        QVERIFY(readMissions("{\"data\":[{\"name\":\"Far\",\"createTime\":1e300},"
                             "{\"name\":\"Past\",\"createTime\":-1e300}]}",
                             &missions));
        QCOMPARE(missions.size(), 2);
        QVERIFY(!missions.at(0).created.isValid());
        QVERIFY(!missions.at(1).created.isValid());
        QVERIFY(!readMissions("[]", &missions));
        QVERIFY(!readMissions("{\"data\":{}}", &missions));
        const auto files = readMissionFiles("{\"data\":[{\"contents\":[{\"data\":{\"hash\":\"" +
                                            QByteArray(64, 'A') +
                                            "\",\"name\":\"map.kmz\",\"submitter\":\"bob\","
                                            "\"size\":10}},{\"data\":{\"hash\":\"x\"}},"
                                            "{\"data\":{\"hash\":\"" +
                                            QByteArray(64, 'b') + "\"}}]}]}");
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).hash, QString(64, QLatin1Char('a')));
        QCOMPARE(files.at(0).size, 10);
        QCOMPARE(files.at(1).name, QString(12, QLatin1Char('b')));
        const auto logs = readMissionLogs(
            "{\"data\":[{\"id\":\"2\",\"content\":\"second\",\"dtg\":\"2026-10-08T11:00:00.000Z\"},"
            "{\"id\":\"1\",\"content\":\"first\",\"servertime\":\"2026-10-08T10:00:00.000Z\"},"
            "{\"id\":\"3\",\"content\":\"\"}]}");
        QCOMPARE(logs.size(), 2);
        QCOMPARE(logs.at(0).content, QStringLiteral("first"));
        MissionSubscription::Role role = MissionSubscription::Role::Owner;
        QString token;
        QVERIFY(readSubscription("{\"data\":{\"token\":\"abc.def\",\"role\":{\"type\":\"MISSION_"
                                 "READONLY_SUBSCRIBER\"}}}",
                                 &role,
                                 &token));
        QCOMPARE(role, MissionSubscription::Role::ReadOnly);
        QCOMPARE(token, QStringLiteral("abc.def"));
        QVERIFY(
            readSubscription("{\"data\":{\"role\":{\"type\":\"MISSION_OWNER\"}}}", &role, &token));
        QCOMPARE(role, MissionSubscription::Role::Owner);
        QVERIFY(token.isEmpty());
        QVERIFY(!readSubscription("{\"data\":{\"token\":\"a b\"}}", &role, &token));
        QVERIFY(!readSubscription("[]", &role, &token));
        const auto marker = sampleEvent(QStringLiteral("m-1"));
        const auto events =
            readMissionEvents("<?xml version=\"1.0\"?><events>" +
                              encodeCot(marker).mid(encodeCot(marker).indexOf("<event")) +
                              "<eventual/><event broken></event>" +
                              encodeCot(sampleEvent(QStringLiteral("m-2"))).mid(38) + "</events>");
        QCOMPARE(events.size(), 2);
        QCOMPARE(events.at(0).uid, QStringLiteral("m-1"));
        QVERIFY(readMissionEvents("<event never closed").isEmpty());
        QCOMPARE(missionOrigin(QStringLiteral("tak"), QStringLiteral("Ops")),
                 QStringLiteral("mission/tak/Ops"));
    }
    void missionService()
    {
        auto first = sampleEvent(QStringLiteral("mission-1"));
        first.time = first.start = first.time.addDays(-3);
        first.stale = first.time.addSecs(60);
        const auto second = sampleEvent(QStringLiteral("mission-2"));
        QByteArray cot = "<events>" + encodeCot(first) + encodeCot(second) + "</events>";
        HttpsHarness api;
        QVERIFY(api.load());
        api.respond = [&](const QByteArray &request) {
            const auto line = request.left(request.indexOf("\r\n"));
            QByteArray body = "{}";
            int status = 200;
            if (line.startsWith("GET /Marti/api/missions?passwordProtected=true ")) {
                body = "{\"data\":[{\"name\":\"Ops\"},{\"name\":\"Secret\",\"passwordProtected\":"
                       "true}]}";
            } else if (line.startsWith("PUT /Marti/api/missions/Ops/subscription?uid=SWAK-1 ")) {
                body = "{\"data\":{\"token\":\"tok\",\"role\":{\"type\":\"MISSION_SUBSCRIBER\"}}}";
            } else if (line.startsWith("PUT /Marti/api/missions/Watch/subscription")) {
                body = "{\"data\":{\"role\":{\"type\":\"MISSION_READONLY_SUBSCRIBER\"}}}";
            } else if (line.startsWith("PUT /Marti/api/missions/Secret/subscription")) {
                status = line.contains("password=pw") ? 200 : 403;
                body = "{\"data\":{\"token\":\"secret\"}}";
            } else if (line.startsWith("PUT /Marti/api/missions/Odd/subscription")) {
                body = "not json";
            } else if (line.contains("/cot ")) {
                body = cot;
            } else if (line.startsWith("GET /Marti/api/missions/Ops/log ")) {
                body = "{\"data\":[{\"id\":\"1\",\"content\":\"Started\",\"dtg\":\"2026-10-08T10:"
                       "00:00.000Z\"}]}";
            } else if (line.startsWith("GET /Marti/api/missions/Ops ")) {
                body = "{\"data\":[{\"name\":\"Ops\",\"contents\":[{\"data\":{\"hash\":\"" +
                       QByteArray(64, 'c') + "\",\"name\":\"map.kmz\"}}]}]}";
            } else if (line.startsWith("GET /Marti/api/missions/Ops/archive ")) {
                body = "PK archive";
            } else if (line.startsWith("GET /Marti/api/missions/Gone")) {
                status = 404;
            }
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        QUdpSocket receiver;
        QVERIFY(receiver.bind(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakServerApi tak(servers, network);
        Identity identity;
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("uid"), QStringLiteral("SWAK-1"));
        identity.restorePreferences(values);
        Missions missions(store, &servers, &tak, &identity);
        // A short wait before each reload keeps the test fast.
        missions.setReloadDelay(100);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QSignalSpy listed(&missions, &Missions::listed);
        QSignalSpy subscribed(&missions, &Missions::subscribed);
        missions.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QCOMPARE(listed.last().at(1).value<QList<Mission>>().size(), 2);
        // Subscribe loads CoT, files, and the log. Old mission objects stay current for a day.
        QVERIFY(!missions.subscribe(QStringLiteral("tak"), QStringLiteral("a/b")).isEmpty());
        QVERIFY(missions.subscribe(QStringLiteral("tak"), QStringLiteral("Ops")).isEmpty());
        QVERIFY(subscribed.wait(10000));
        QVERIFY(subscribed.last().at(2).toString().isEmpty());
        const auto origin = missionOrigin(QStringLiteral("tak"), QStringLiteral("Ops"));
        const auto inMission = [&store, &origin] {
            return std::count_if(
                store.events().cbegin(), store.events().cend(), [&origin](const Event &event) {
                    return event.origin == origin;
                });
        };
        QVERIFY(QTest::qWaitFor(
            [&] {
                return inMission() == 2;
            },
            10000));
        QVERIFY(store.events().first().stale > QDateTime::currentDateTimeUtc().addSecs(3600));
        const auto *ops = missions.subscription(QStringLiteral("tak"), QStringLiteral("Ops"));
        QVERIFY(ops);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return ops->files.size() == 1 && ops->logs.size() == 1;
            },
            10000));
        QCOMPARE(ops->role, MissionSubscription::Role::Subscriber);
        QCOMPARE(ops->objects, 2);
        QVERIFY(
            std::any_of(api.requests.cbegin(), api.requests.cend(), [](const QByteArray &request) {
                return request.contains("/cot ") && request.contains("Authorization: Bearer tok");
            }));
        // Visibility hides and shows the mission objects.
        missions.setVisible(QStringLiteral("tak"), QStringLiteral("Ops"), false);
        QCOMPARE(inMission(), 0);
        missions.setVisible(QStringLiteral("tak"), QStringLiteral("Ops"), true);
        QCOMPARE(inMission(), 2);
        // A change event reloads the mission. Removed objects leave the map.
        cot = "<events>" + encodeCot(second) + "</events>";
        auto change = sampleEvent(QStringLiteral("change-1"));
        change.type = QStringLiteral("t-x-m-c");
        change.origin = QStringLiteral("tak");
        change.detail = QStringLiteral("<mission type=\"CHANGE\" name=\"Ops\"/>");
        QVERIFY(store.accept(change).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return inMission() == 1;
            },
            10000));
        // A reply from an older reload is stale. Here the first reload waits, a second reload
        // brings both objects, and the late first reply holds none.
        api.hold = true;
        const auto heldBefore = api.held.size();
        change.time = QDateTime::currentDateTimeUtc();
        QVERIFY(store.accept(change).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return api.held.size() >= heldBefore + 3;
            },
            10000));
        api.hold = false;
        cot = "<events>" + encodeCot(first) + encodeCot(second) + "</events>";
        change.time = QDateTime::currentDateTimeUtc().addMSecs(1);
        QVERIFY(store.accept(change).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return inMission() == 2;
            },
            10000));
        cot = "<events></events>";
        api.releaseHeld();
        QTest::qWait(500);
        QCOMPARE(inMission(), 2);
        cot = "<events>" + encodeCot(second) + "</events>";
        change.time = QDateTime::currentDateTimeUtc().addMSecs(2);
        QVERIFY(store.accept(change).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return inMission() == 1;
            },
            10000));
        // A group of change events gives one reload, after the last event of the group.
        const auto cotReads = [&] {
            return std::count_if(api.requests.cbegin(), api.requests.cend(), [](const auto &r) {
                return r.startsWith("GET /Marti/api/missions/Ops/cot ");
            });
        };
        const auto readsBefore = cotReads();
        for (int i = 3; i < 6; ++i) {
            change.time = QDateTime::currentDateTimeUtc().addMSecs(i);
            QVERIFY(store.accept(change).isEmpty());
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return cotReads() > readsBefore;
            },
            10000));
        QTest::qWait(300);
        QCOMPARE(cotReads(), readsBefore + 1);
        auto other = change;
        other.detail = QStringLiteral("<mission name=\"Other\"/>");
        QVERIFY(store.accept(other).isEmpty());
        other.detail = QStringLiteral("<mission name=\"bad/name\"/>");
        QVERIFY(store.accept(other).isEmpty());
        // Invitations appear once.
        QSignalSpy invited(&missions, &Missions::invited);
        auto invitation = change;
        invitation.type = QStringLiteral("t-x-m-i");
        invitation.detail = QStringLiteral("<mission name=\"Plans\"/>");
        QVERIFY(store.accept(invitation).isEmpty());
        QVERIFY(store.accept(invitation).isEmpty());
        QCOMPARE(invited.size(), 1);
        QCOMPARE(missions.invitations().size(), 1);
        // Edits: add through the stream, remove and log through REST, and export.
        auto output = source(receiver.localPort());
        output.insert(QStringLiteral("protocol"), QStringLiteral("udpout"));
        QVERIFY(servers.put(QStringLiteral("out"), output).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("out")).startsWith(QStringLiteral("Sending"));
            },
            5000));
        auto added = sampleEvent(QStringLiteral("added-1"));
        added.detail =
            QStringLiteral("<marti><dest callsign=\"Old\"/></marti><remarks>keep</remarks>");
        QVERIFY(missions.addEvent(QStringLiteral("tak"), QStringLiteral("Ops"), added).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return receiver.hasPendingDatagrams();
            },
            5000));
        QByteArray datagram(static_cast<qsizetype>(receiver.pendingDatagramSize()),
                            Qt::Uninitialized);
        receiver.readDatagram(datagram.data(), datagram.size());
        const auto sent = decodeCot(datagram).event;
        QVERIFY(sent);
        QCOMPARE(destinations(*sent).size(), 1);
        QCOMPARE(destinations(*sent).first().mission, QStringLiteral("Ops"));
        QCOMPARE(remarks(*sent), QStringLiteral("keep"));
        QVERIFY(
            !missions.addEvent(QStringLiteral("tak"), QStringLiteral("Unknown"), added).isEmpty());
        QVERIFY(missions
                    .removeObject(
                        QStringLiteral("tak"), QStringLiteral("Ops"), QStringLiteral("mission-2"))
                    .isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(
                    api.requests.cbegin(), api.requests.cend(), [](const QByteArray &request) {
                        return request.startsWith(
                            "DELETE "
                            "/Marti/api/missions/Ops/contents?uid=mission-2&creatorUid=SWAK-1 ");
                    });
            },
            10000));
        QVERIFY(!missions.addLog(QStringLiteral("tak"), QStringLiteral("Ops"), QStringLiteral("  "))
                     .isEmpty());
        QVERIFY(missions
                    .addLog(QStringLiteral("tak"),
                            QStringLiteral("Ops"),
                            QStringLiteral("Checkpoint reached"))
                    .isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(
                    api.requests.cbegin(), api.requests.cend(), [](const QByteArray &request) {
                        return request.startsWith("POST /Marti/api/missions/logs/entries ") &&
                               request.contains("\"missionNames\":[\"Ops\"]") &&
                               request.contains("Checkpoint reached");
                    });
            },
            10000));
        QSignalSpy exported(&missions, &Missions::exported);
        QVERIFY(missions.exportArchive(QStringLiteral("tak"), QStringLiteral("Ops")).isEmpty());
        QVERIFY(exported.wait(10000));
        QCOMPARE(exported.last().at(2).toByteArray(), QByteArray("PK archive"));
        QVERIFY(
            !missions.exportArchive(QStringLiteral("tak"), QStringLiteral("Unknown")).isEmpty());
        // Read-only missions refuse edits. Password missions need the password.
        QVERIFY(missions.subscribe(QStringLiteral("tak"), QStringLiteral("Watch")).isEmpty());
        QVERIFY(subscribed.wait(10000));
        QCOMPARE(missions.subscription(QStringLiteral("tak"), QStringLiteral("Watch"))->role,
                 MissionSubscription::Role::ReadOnly);
        QVERIFY(
            !missions.addEvent(QStringLiteral("tak"), QStringLiteral("Watch"), added).isEmpty());
        QVERIFY(
            !missions
                 .removeObject(QStringLiteral("tak"), QStringLiteral("Watch"), QStringLiteral("x"))
                 .isEmpty());
        QVERIFY(
            !missions.addLog(QStringLiteral("tak"), QStringLiteral("Watch"), QStringLiteral("x"))
                 .isEmpty());
        QVERIFY(missions.subscribe(QStringLiteral("tak"), QStringLiteral("Secret")).isEmpty());
        QVERIFY(subscribed.wait(10000));
        QVERIFY(subscribed.last().at(2).toString().contains(QStringLiteral("password")));
        QVERIFY(
            missions
                .subscribe(QStringLiteral("tak"), QStringLiteral("Secret"), QStringLiteral("pw"))
                .isEmpty());
        QVERIFY(subscribed.wait(10000));
        QVERIFY(subscribed.last().at(2).toString().isEmpty());
        QVERIFY(missions.subscribe(QStringLiteral("tak"), QStringLiteral("Odd")).isEmpty());
        QVERIFY(subscribed.wait(10000));
        QVERIFY(!subscribed.last().at(2).toString().isEmpty());
        // Preferences keep names and visibility, never tokens. A restore subscribes again.
        const auto saved = missions.savePreferences();
        QVERIFY(!QJsonDocument(saved).toJson().contains("tok"));
        QCOMPARE(saved.value(QStringLiteral("subscriptions")).toArray().size(), 4);
        Store otherStore;
        Missions restored(otherStore, &servers, &tak, &identity);
        QSignalSpy restoredSubscribed(&restored, &Missions::subscribed);
        restored.restorePreferences(saved);
        QCOMPARE(restored.subscriptions().size(), 4);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return restoredSubscribed.size() == 4;
            },
            15000));
        restored.restorePreferences(
            {{QStringLiteral("subscriptions"),
              QJsonArray{QJsonObject{{QStringLiteral("server"), QStringLiteral("tak")},
                                     {QStringLiteral("name"), QStringLiteral("a/b")}}}}});
        QCOMPARE(restored.subscriptions().size(), 4);
        // A mission delete event ends the subscription and removes its objects.
        auto removed = change;
        removed.type = QStringLiteral("t-x-m-d");
        QVERIFY(store.accept(removed).isEmpty());
        QVERIFY(!missions.subscription(QStringLiteral("tak"), QStringLiteral("Ops")));
        QCOMPARE(inMission(), 0);
        // Unsubscribe tells the server.
        missions.unsubscribe(QStringLiteral("tak"), QStringLiteral("Watch"));
        QVERIFY(!missions.subscription(QStringLiteral("tak"), QStringLiteral("Watch")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(
                    api.requests.cbegin(), api.requests.cend(), [](const QByteArray &r) {
                        return r.startsWith(
                            "DELETE /Marti/api/missions/Watch/subscription?uid=SWAK-1 ");
                    });
            },
            10000));
        missions.unsubscribe(QStringLiteral("tak"), QStringLiteral("Unknown"));
        // A failed reload reports the error.
        QVERIFY(missions.subscribe(QStringLiteral("tak"), QStringLiteral("Gone")).isEmpty());
        QVERIFY(subscribed.wait(10000));
        // Without a REST API, nothing starts.
        Missions offline(store, nullptr, nullptr, nullptr);
        QSignalSpy offlineListed(&offline, &Missions::listed);
        offline.list(QStringLiteral("tak"));
        QCOMPARE(offlineListed.size(), 1);
        QVERIFY(!offline.subscribe(QStringLiteral("tak"), QStringLiteral("Ops")).isEmpty());
        QVERIFY(!offline.addLog(QStringLiteral("tak"), QStringLiteral("Ops"), QStringLiteral("x"))
                     .isEmpty());
        QVERIFY(
            !offline.removeObject(QStringLiteral("tak"), QStringLiteral("Ops"), QStringLiteral("x"))
                 .isEmpty());
    }
    void serverMapLayers()
    {
        const QByteArray json =
            "{\"data\":["
            "{\"uid\":\"osm 1\",\"name\":\"Streets\",\"type\":\"MOBAC\",\"url\":"
            "\"https://{$serverpart}.tiles.example/{$z}/{$x}/{$y}.png\",\"serverParts\":\"a b\","
            "\"minZoom\":2,\"maxZoom\":40,\"invertYCoordinate\":true},"
            "{\"uid\":\"wms\",\"name\":\"Weather\",\"type\":\"WMS\",\"url\":\"https://wms.example/"
            "ows\","
            "\"layers\":\"radar\",\"tileType\":\"image/jpeg\"},"
            "{\"name\":\"Matrix\",\"type\":\"WMTS\",\"url\":\"https://wmts.example\"},"
            "{\"name\":\"Plain\",\"url\":\"http://tiles.example/{z}/{x}/{y}.png\"},"
            "{\"name\":\"Quad\",\"url\":\"https://tiles.example/{$q}.png\"},"
            "{\"uid\":\"osm 1\",\"name\":\"Streets "
            "copy\",\"url\":\"https://t.example/{z}/{x}/{y}\"}]}";
        QList<ServerMapLayer> layers;
        QStringList skipped;
        QVERIFY(readServerMapLayers(
            json, QStringLiteral("tak_1"), QStringLiteral("TAK"), &layers, &skipped));
        QCOMPARE(layers.size(), 3);
        QCOMPARE(skipped.size(), 3);
        QCOMPARE(layers.at(0).id, QStringLiteral("tak.tak-1.osm-1"));
        QCOMPARE(layers.at(2).id, QStringLiteral("tak.tak-1.osm-1-2"));
        const auto streets = layers.at(0).source;
        QCOMPARE(streets.value(QStringLiteral("endpoint")).toString(),
                 QStringLiteral("https://a.tiles.example/{z}/{x}/{y}.png"));
        QCOMPARE(streets.value(QStringLiteral("service")).toString(), QStringLiteral("xyz"));
        QCOMPARE(streets.value(QStringLiteral("minZoom")).toInt(), 2);
        QCOMPARE(streets.value(QStringLiteral("maxZoom")).toInt(), 19);
        QVERIFY(streets.value(QStringLiteral("invertY")).toBool());
        QCOMPARE(streets.value(QStringLiteral("attribution")).toString(),
                 QStringLiteral("TAK Server TAK"));
        const auto weather = layers.at(1).source;
        QCOMPARE(weather.value(QStringLiteral("service")).toString(), QStringLiteral("wms"));
        QCOMPARE(weather.value(QStringLiteral("layer")).toString(), QStringLiteral("radar"));
        QCOMPARE(weather.value(QStringLiteral("imageFormat")).toString(),
                 QStringLiteral("image/jpeg"));
        QVERIFY(skipped.at(0).contains(QStringLiteral("WMTS")));
        QVERIFY(readServerMapLayers("[]", QStringLiteral("t"), {}, &layers, nullptr));
        QVERIFY(layers.isEmpty());
        QVERIFY(!readServerMapLayers("{\"data\":{}}", QStringLiteral("t"), {}, &layers, nullptr));
        QVERIFY(!readServerMapLayers("x", QStringLiteral("t"), {}, &layers, nullptr));
        // The server browser adds the checked layers as map providers.
        HttpsHarness api;
        QVERIFY(api.load());
        QByteArray reply = json;
        int status = 200;
        api.respond = [&](const QByteArray &) {
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: " + QByteArray::number(reply.size()) + "\r\n\r\n" + reply;
        };
        // The API is a child of Servers and uses the client, so the client must live longer.
        NetworkClient network;
        Store store;
        Servers servers(store);
        new TakServerApi(servers, network, &servers);
        MapSources maps(QList<QObject *>{});
        ServerBrowser browser(servers);
        auto *action = browser.findChild<QAction *>(QStringLiteral("serverMapLayers"));
        auto *message = browser.findChild<QLabel *>(QStringLiteral("enrollStatus"));
        QVERIFY(action && message);
        action->trigger();
        QCOMPARE(message->text(), QStringLiteral("This window has no map catalog."));
        browser.setMapSources(&maps);
        action->trigger();
        QVERIFY(message->text().startsWith(QStringLiteral("Select a TLS or QUIC entry")));
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        auto *list = browser.findChild<QTableWidget *>(QStringLiteral("serverList"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return list->rowCount() == 1;
            },
            2000));
        list->setCurrentCell(0, 1);
        bool shown = false;
        // The dialog opens after the server reply. Poll until it shows, then answer it.
        QTimer poll;
        poll.setInterval(20);
        bool acceptDialog = false;
        QObject::connect(&poll, &QTimer::timeout, &browser, [&] {
            auto *dialog = browser.findChild<QDialog *>(QStringLiteral("mapLayerDialog"));
            auto *rows =
                dialog ? dialog->findChild<QListWidget *>(QStringLiteral("mapLayerList")) : nullptr;
            if (!rows || !dialog->isVisible()) {
                return;
            }
            poll.stop();
            shown = rows->count() == 3 &&
                    dialog->findChild<QLabel *>(QStringLiteral("skippedMapLayers"));
            rows->item(0)->setCheckState(Qt::Checked);
            rows->item(1)->setCheckState(Qt::Checked);
            acceptDialog ? dialog->accept() : dialog->reject();
        });
        const auto choose = [&](bool accept) {
            acceptDialog = accept;
            poll.start();
        };
        choose(false);
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shown;
            },
            10000));
        QVERIFY(maps.sources().isEmpty() ||
                !maps.sources().contains(QStringLiteral("tak.tak.osm-1")));
        shown = false;
        choose(true);
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return shown;
            },
            10000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text().contains(QStringLiteral("2 map providers"));
            },
            5000));
        QVERIFY(maps.sources().contains(QStringLiteral("tak.tak.osm-1")));
        QVERIFY(maps.sources().contains(QStringLiteral("tak.tak.wms")));
        // Empty lists, invalid replies, and HTTP errors show in the status line.
        reply = "{\"data\":[]}";
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text() == QStringLiteral("The server has no map layers.");
            },
            10000));
        reply = "{\"data\":[{\"name\":\"Matrix\",\"type\":\"WMTS\",\"url\":\"https://w\"}]}";
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text().contains(QStringLiteral("WMTS"));
            },
            10000));
        reply = "x";
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text() == QStringLiteral("The map layer list is invalid.");
            },
            10000));
        status = 403;
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text().contains(QStringLiteral("403"));
            },
            10000));
    }
    void trackHistory()
    {
        const auto position = [](const QString &uid, double latitude, int minutesAgo) {
            auto event = sampleEvent(uid);
            event.latitude = latitude;
            event.longitude = 18;
            event.time = event.start = QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo);
            event.stale = event.time.addSecs(60);
            return encodeCot(event);
        };
        QByteArray xml = "<events>" + position(QStringLiteral("A"), 59.2, 1) +
                         position(QStringLiteral("B"), 50, 2) +
                         position(QStringLiteral("A"), 59.0, 3) +
                         position(QStringLiteral("A"), 59.1, 2) +
                         position(QStringLiteral("A"), 89, 4) + "<event broken></event></events>";
        const auto points = readHistoryPoints(xml, QStringLiteral("A"));
        QCOMPARE(points.size(), 3);
        QCOMPARE(points.first().world, project(59.0, 18));
        QCOMPARE(points.last().world, project(59.2, 18));
        QVERIFY(readHistoryPoints("<event never", QStringLiteral("A")).isEmpty());
        // The service loads from the REST API of the event server.
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        api.respond = [&](const QByteArray &) {
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: " + QByteArray::number(xml.size()) + "\r\n\r\n" + xml;
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        TakServerApi tak(servers, network);
        TrackHistory history(&servers, &tak);
        QSignalSpy loaded(&history, &TrackHistory::loaded);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QVERIFY(servers.put(QStringLiteral("udp"), source(9)).isEmpty());
        QVERIFY(history.available(QStringLiteral("tak")));
        QVERIFY(!history.available(QStringLiteral("udp")));
        const auto end = QDateTime::currentDateTimeUtc();
        QVERIFY(!history.load(QStringLiteral("udp"), QStringLiteral("A"), {}, end.addSecs(-60), end)
                     .isEmpty());
        QVERIFY(!history.load(QStringLiteral("tak"), QStringLiteral("A"), {}, end, end).isEmpty());
        QVERIFY(!history.load(QStringLiteral("tak"), QStringLiteral("A"), {}, end.addDays(-8), end)
                     .isEmpty());
        QVERIFY(!history.load(QStringLiteral("tak"), {}, {}, end.addSecs(-60), end).isEmpty());
        QVERIFY(history
                    .load(QStringLiteral("tak"),
                          QStringLiteral("A"),
                          QStringLiteral("Alpha"),
                          end.addSecs(-3600),
                          end)
                    .isEmpty());
        QVERIFY(loaded.wait(10000));
        QVERIFY(loaded.last().at(1).toString().isEmpty());
        QVERIFY(api.requests.last().startsWith("GET /Marti/api/cot/xml/A/all?start="));
        QCOMPARE(history.track(QStringLiteral("A"))->points.size(), 3);
        QCOMPARE(history.track(QStringLiteral("A"))->label, QStringLiteral("Alpha"));
        // No positions and HTTP errors report errors and keep the tracks.
        QVERIFY(
            history.load(QStringLiteral("tak"), QStringLiteral("C"), {}, end.addSecs(-3600), end)
                .isEmpty());
        QVERIFY(loaded.wait(10000));
        QVERIFY(!loaded.last().at(1).toString().isEmpty());
        status = 500;
        QVERIFY(
            history.load(QStringLiteral("tak"), QStringLiteral("A"), {}, end.addSecs(-3600), end)
                .isEmpty());
        QVERIFY(loaded.wait(10000));
        QVERIFY(loaded.last().at(1).toString().contains(QStringLiteral("500")));
        QCOMPARE(history.tracks().size(), 1);
        // The list keeps MaxTracks tracks and MaxPoints positions.
        for (int i = 0; i < TrackHistory::MaxTracks; ++i) {
            history.add({QStringLiteral("T%1").arg(i),
                         {},
                         QStringLiteral("tak"),
                         end,
                         end,
                         {{end, QPointF(0.5, 0.5)}}});
        }
        QCOMPARE(history.tracks().size(), TrackHistory::MaxTracks);
        QVERIFY(!history.track(QStringLiteral("A")));
        HistoryTrack large{QStringLiteral("L"), {}, {}, end, end, {}};
        for (int i = 0; i <= TrackHistory::MaxPoints; ++i) {
            large.points.append({end.addSecs(i), QPointF(0.5, 0.5)});
        }
        history.add(large);
        QCOMPARE(history.track(QStringLiteral("L"))->points.size(), TrackHistory::MaxPoints);
        history.add({QStringLiteral("E"), {}, {}, end, end, {}});
        QVERIFY(!history.track(QStringLiteral("E")));
        history.remove(QStringLiteral("L"));
        QVERIFY(!history.track(QStringLiteral("L")));
        history.remove(QStringLiteral("L"));
        history.clear();
        QVERIFY(history.tracks().isEmpty());
        history.clear();
    }
    void serverIcons()
    {
        QVERIFY(serverIconPath(
            QStringLiteral("6d781afb-89a6-4c07-b2b9-a89748b6a38f/Google/hiker.png")));
        for (const auto &bad : {QStringLiteral("COT_MAPPING_SPOTMAP/b-m-p-s-m/-1"),
                                QStringLiteral("a/b/c.gif"),
                                QStringLiteral("a/../c.png"),
                                QStringLiteral("a/b/c/d.png"),
                                QStringLiteral("/a/b.png")}) {
            QVERIFY2(!serverIconPath(bad), qPrintable(bad));
        }
        const auto png = [](int width, int height) {
            QImage image(width, height, QImage::Format_ARGB32);
            image.fill(Qt::red);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return bytes;
        };
        auto icon = decodeIcon(png(100, 50));
        QCOMPARE(icon.size(), QSize(64, 32));
        QCOMPARE(decodeIcon(png(32, 32)).size(), QSize(32, 32));
        QVERIFY(decodeIcon(png(600, 10)).isNull());
        QVERIFY(decodeIcon("not an image").isNull());
        QVERIFY(decodeIcon({}).isNull());
        QVERIFY(decodeIcon(QByteArray(IconCache::MaxIconBytes + 1, 'x')).isNull());
        QByteArray gif;
        {
            QBuffer buffer(&gif);
            buffer.open(QIODevice::WriteOnly);
            QImage(4, 4, QImage::Format_ARGB32).save(&buffer, "BMP");
        }
        QVERIFY(decodeIcon(gif).isNull());
        // The cache asks the server once per icon and remembers failures.
        HttpsHarness api;
        QVERIFY(api.load());
        api.respond = [&](const QByteArray &request) {
            const auto body = request.contains("missing") ? QByteArray{} : png(16, 16);
            const QByteArray status = body.isEmpty() ? "404" : "200";
            return "HTTP/1.1 " + status +
                   " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        TakServerApi tak(servers, network);
        IconCache cache(&servers, &tak);
        QSignalSpy iconLoaded(&cache, &IconCache::iconLoaded);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        const auto path = [](int i) {
            return QStringLiteral("set/group/icon%1.png").arg(i);
        };
        QVERIFY(cache.icon(QStringLiteral("tak"), path(0)).isNull());
        QVERIFY(cache.icon(QStringLiteral("tak"), path(0)).isNull());
        // More icons than request slots wait for a free slot.
        for (int i = 1; i < 7; ++i) {
            QVERIFY(cache.icon(QStringLiteral("tak"), path(i)).isNull());
        }
        QVERIFY(QTest::qWaitFor(
            [&] {
                return iconLoaded.size() == 7;
            },
            15000));
        QCOMPARE(cache.icon(QStringLiteral("tak"), path(6)).size(), QSize(16, 16));
        QCOMPARE(api.requests.size(), 7);
        QVERIFY(api.requests.first().startsWith("GET /Marti/api/icon/set/group/icon0.png "));
        QVERIFY(
            cache.icon(QStringLiteral("tak"), QStringLiteral("set/group/missing.png")).isNull());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return api.requests.size() == 8;
            },
            10000));
        QTest::qWait(200);
        QVERIFY(
            cache.icon(QStringLiteral("tak"), QStringLiteral("set/group/missing.png")).isNull());
        QTest::qWait(200);
        QCOMPARE(api.requests.size(), 8);
        // Other paths and servers without an API make no request.
        QVERIFY(cache.icon(QStringLiteral("tak"), QStringLiteral("COT_MAPPING_SPOTMAP/x/y.png"))
                    .isNull());
        QVERIFY(cache.icon(QStringLiteral("other"), path(9)).isNull());
        QTest::qWait(200);
        QCOMPARE(api.requests.size(), 8);
        // Imported icons replace a failure and are scaled down.
        cache.insert(QStringLiteral("tak"),
                     QStringLiteral("set/group/missing.png"),
                     QImage(200, 100, QImage::Format_ARGB32));
        QCOMPARE(cache.icon(QStringLiteral("tak"), QStringLiteral("set/group/missing.png")).size(),
                 QSize(64, 32));
        cache.insert(
            QStringLiteral("tak"), QStringLiteral("bad"), QImage(4, 4, QImage::Format_ARGB32));
        cache.insert(QStringLiteral("tak"), path(20), QImage());
        QVERIFY(cache.icon(QStringLiteral("tak"), path(20)).isNull());
        for (int i = 0; i <= IconCache::MaxIcons; ++i) {
            cache.insert(
                QStringLiteral("local"), path(100 + i), QImage(4, 4, QImage::Format_ARGB32));
        }
        QVERIFY(cache.icon(QStringLiteral("local"), path(100)).isNull());
        QVERIFY(!cache.icon(QStringLiteral("local"), path(101)).isNull());
        IconCache offline(nullptr, nullptr);
        QVERIFY(offline.icon(QStringLiteral("tak"), path(0)).isNull());
    }
    void videoSources()
    {
        const auto protocol = [](const QString &text) {
            return videoProtocol(QUrl(text, QUrl::StrictMode));
        };
        QCOMPARE(protocol(QStringLiteral("rtsp://cam.example:554/live")), VideoProtocol::Rtsp);
        QCOMPARE(protocol(QStringLiteral("rtsps://cam.example/live")), VideoProtocol::Rtsp);
        QCOMPARE(protocol(QStringLiteral("rtmp://cam.example/app/stream")), VideoProtocol::Rtmp);
        QCOMPARE(protocol(QStringLiteral("srt://cam.example:9000")), VideoProtocol::Srt);
        QCOMPARE(protocol(QStringLiteral("https://cam.example/live.m3u8")), VideoProtocol::Hls);
        QCOMPARE(protocol(QStringLiteral("http://10.0.0.5/live.m3u8")), VideoProtocol::Hls);
        QCOMPARE(protocol(QStringLiteral("udp://239.1.1.1:1234")), VideoProtocol::Udp);
        QCOMPARE(protocol(QStringLiteral("udp://@:1234")), VideoProtocol::Udp);
        QString error;
        QCOMPARE(videoProtocol(QUrl(QStringLiteral("file:///etc/passwd")), &error),
                 VideoProtocol::Unsupported);
        QVERIFY(error.contains(QStringLiteral("RTSP")));
        for (const auto &bad : {QStringLiteral("udp://239.1.1.1"),
                                QStringLiteral("ftp://cam.example/x"),
                                QStringLiteral("rtsp:///live"),
                                QStringLiteral("rtsp://cam.example:0/live"),
                                QStringLiteral("rtsp://cam.example/live#x"),
                                QStringLiteral("not a url")}) {
            QCOMPARE(protocol(bad), VideoProtocol::Unsupported);
        }
        QCOMPARE(videoProtocolName(VideoProtocol::Srt), QStringLiteral("SRT"));
        QCOMPARE(videoProtocolName(VideoProtocol::Hls), QStringLiteral("HLS"));
        QCOMPARE(videoProtocolName(VideoProtocol::Udp), QStringLiteral("UDP"));
        QCOMPARE(videoProtocolName(VideoProtocol::Rtmp), QStringLiteral("RTMP"));
        QCOMPARE(videoProtocolName(VideoProtocol::Rtsp), QStringLiteral("RTSP"));
        QVERIFY(!videoProtocolName(VideoProtocol::Unsupported).isEmpty());
        // TAK Server video lists.
        const QByteArray xml =
            "<videoConnections>"
            "<feed><uid>1</uid><alias>Gate</alias><url>rtsp://user:secret@cam.example/live</url></"
            "feed>"
            "<feed><alias>Drone</alias><protocol>srt</protocol><address>10.0.0.7</"
            "address><port>9000</port>"
            "<path>stream</path></feed>"
            "<feed><alias>Disk</alias><url>file:///tmp/x.ts</url></feed>"
            "<feed><protocol>udp</protocol><address>239.2.3.4</address><port>-1</port></feed>"
            "</videoConnections>";
        QList<VideoSource> list;
        QVERIFY(readVideoConnections(xml, QStringLiteral("tak"), &list));
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).name, QStringLiteral("Gate"));
        QCOMPARE(list.at(0).key, QStringLiteral("rtsp://cam.example/live"));
        QCOMPARE(list.at(0).url.password(), QStringLiteral("secret"));
        QCOMPARE(list.at(1).url.toString(), QStringLiteral("srt://10.0.0.7:9000/stream"));
        QCOMPARE(list.at(1).origin, QStringLiteral("tak"));
        QVERIFY(!readVideoConnections("<!DOCTYPE x><x/>", {}, &list));
        // TAK Server 5 answers JSON with connections and their feeds.
        const QByteArray json =
            "{\"videoConnections\":[{\"uid\":\"v1\",\"alias\":\"Gate\",\"feeds\":["
            "{\"alias\":\"North\",\"url\":\"rtsp://10.0.0.9:554/live\",\"active\":true},"
            "{\"url\":\"srt://10.0.0.7:9000\"},{\"alias\":\"Off\",\"url\":\"rtsp://10.0.0.8/"
            "x\",\"active\":false},"
            "{\"url\":\"file:///tmp/x\"}]}]}";
        QVERIFY(readVideoConnections(json, QStringLiteral("tak"), &list));
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).name, QStringLiteral("North"));
        QCOMPARE(list.at(1).name, QStringLiteral("Gate"));
        QCOMPARE(list.at(1).protocol, VideoProtocol::Srt);
        QVERIFY(readVideoConnections("{\"videoConnections\":[]}", {}, &list));
        QVERIFY(list.isEmpty());
        QVERIFY(!readVideoConnections("{\"other\":1}", {}, &list));
        QVERIFY(!readVideoConnections("<a><feed>", {}, &list));
        QByteArray many = "<v>";
        for (int i = 0; i <= 256; ++i) {
            many += "<feed/>";
        }
        QVERIFY(!readVideoConnections(many + "</v>", {}, &list));
        // Video events add and update sources.
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        api.respond = [&](const QByteArray &) {
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: " + QByteArray::number(xml.size()) + "\r\n\r\n" + xml;
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        TakServerApi tak(servers, network);
        VideoSources sources(store, &servers, &tak);
        QSignalSpy refreshed(&sources, &VideoSources::refreshed);
        sources.refresh();
        QVERIFY(!refreshed.last().at(0).toString().isEmpty());
        auto camera = sampleEvent(QStringLiteral("camera-1"));
        camera.type = QStringLiteral("b-i-v");
        camera.callsign = QStringLiteral("North gate");
        camera.detail = QStringLiteral("<__video url=\"rtsp://10.0.0.9:554/live\"/>");
        QVERIFY(store.accept(camera).isEmpty());
        QCOMPARE(sources.sources().size(), 1);
        QCOMPARE(sources.sources().first().name, QStringLiteral("North gate"));
        camera.callsign = QStringLiteral("Gate 2");
        camera.time = camera.start = camera.time.addSecs(1);
        QVERIFY(store.accept(camera).isEmpty());
        QCOMPARE(sources.sources().size(), 1);
        QCOMPARE(sources.sources().first().name, QStringLiteral("Gate 2"));
        auto entry = camera;
        entry.uid = QStringLiteral("camera-2");
        entry.detail = QStringLiteral(
            "<__video><ConnectionEntry protocol=\"rtmp\" address=\"10.0.0.8\" port=\"1935\" "
            "path=\"live/cam\" alias=\"x\"/></__video>");
        QVERIFY(store.accept(entry).isEmpty());
        QCOMPARE(sources.sources().size(), 2);
        QCOMPARE(sources.sources().at(1).protocol, VideoProtocol::Rtmp);
        auto unsafe = camera;
        unsafe.uid = QStringLiteral("camera-3");
        unsafe.detail = QStringLiteral("<__video url=\"file:///etc/passwd\"/>");
        QVERIFY(store.accept(unsafe).isEmpty());
        QCOMPARE(sources.sources().size(), 2);
        QVERIFY(!sources.add(QStringLiteral("x"), QUrl(QStringLiteral("gopher://x"))).isEmpty());
        QVERIFY(sources.add({}, QUrl(QStringLiteral("udp://@:5000"))).isEmpty());
        QCOMPARE(sources.sources().last().name, QStringLiteral("udp://:5000"));
        // Refresh reads the lists of enabled TAK Server entries.
        auto server = source(9);
        server.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        server.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        server.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), server).isEmpty());
        sources.refresh();
        QVERIFY(refreshed.wait(10000));
        QVERIFY(refreshed.last().at(0).toString().isEmpty());
        QCOMPARE(sources.sources().size(), 5);
        QVERIFY(api.requests.last().startsWith("GET /Marti/api/video "));
        status = 500;
        sources.refresh();
        QVERIFY(refreshed.wait(10000));
        QVERIFY(refreshed.last().at(0).toString().contains(QStringLiteral("unavailable")));
        sources.remove(QStringLiteral("rtsp://cam.example/live"));
        QCOMPARE(sources.sources().size(), 4);
        sources.remove(QStringLiteral("missing"));
        // An event does not change a source that the user entered.
        auto replace = sampleEvent(QStringLiteral("camera-9"));
        replace.detail = QStringLiteral("<__video url=\"udp://@:5000\" alias=\"Remote\"/>");
        QVERIFY(store.accept(replace).isEmpty());
        const auto manual = std::find_if(
            sources.sources().cbegin(), sources.sources().cend(), [](const VideoSource &item) {
                return item.key == u"udp://:5000";
            });
        QVERIFY(manual != sources.sources().cend());
        QCOMPARE(manual->name, QStringLiteral("udp://:5000"));
        int port = 6000;
        while (sources.sources().size() < MaxVideoSources) {
            QVERIFY(sources.add({}, QUrl(QStringLiteral("udp://@:%1").arg(port++))).isEmpty());
        }
        // A full list removes the oldest source from an event or a server to make room.
        const auto automatic = std::count_if(
            sources.sources().cbegin(), sources.sources().cend(), [](const VideoSource &item) {
                return !item.origin.isEmpty();
            });
        QVERIFY(automatic > 0);
        for (qsizetype i = 0; i < automatic; ++i) {
            QVERIFY(sources.add({}, QUrl(QStringLiteral("udp://@:%1").arg(port++))).isEmpty());
            QCOMPARE(sources.sources().size(), MaxVideoSources);
        }
        // Only manual sources remain. Neither the user nor an event can add more.
        QVERIFY(!sources.add({}, QUrl(QStringLiteral("udp://@:7999"))).isEmpty());
        auto late = sampleEvent(QStringLiteral("camera-10"));
        late.detail = QStringLiteral("<__video url=\"rtsp://late.example/live\"/>");
        QVERIFY(store.accept(late).isEmpty());
        QVERIFY(std::none_of(
            sources.sources().cbegin(), sources.sources().cend(), [](const VideoSource &item) {
                return item.key == u"rtsp://late.example/live";
            }));
        VideoSources offline(store, nullptr, nullptr);
        QSignalSpy offlineRefreshed(&offline, &VideoSources::refreshed);
        offline.refresh();
        QCOMPARE(offlineRefreshed.size(), 1);
    }
    void serverDataFeeds()
    {
        const QByteArray json = "{\"version\":\"3\",\"type\":\"DataFeed\",\"data\":["
                                "{\"uuid\":\"u1\",\"name\":\"Weather\",\"type\":\"Streaming\","
                                "\"protocol\":\"tls\",\"port\":8089,"
                                "\"active\":true,\"filterGroups\":[\"Cyan\"],\"tags\":[\"wx\"]},"
                                "{\"uuid\":\"u2\",\"name\":\"Ops\",\"type\":\"API\",\"active\":"
                                "true,\"feedGroup\":\"Ops\"},"
                                "{\"uuid\":\"u3\",\"type\":\"Plugin\",\"active\":true},"
                                "{\"name\":\"Old\",\"active\":false,\"port\":99999},"
                                "{\"type\":\"none\"}]}";
        QList<ServerFeed> feeds;
        QVERIFY(readServerFeeds(json, &feeds));
        QCOMPARE(feeds.size(), 4);
        QCOMPARE(feeds.at(0).groups, QStringList{QStringLiteral("Cyan")});
        QCOMPARE(feeds.at(0).tags, QStringList{QStringLiteral("wx")});
        QCOMPARE(feeds.at(0).port, 8089);
        QCOMPARE(feeds.at(1).groups, QStringList{QStringLiteral("Ops")});
        QCOMPARE(feeds.at(2).name, QStringLiteral("u3"));
        QCOMPARE(feeds.at(3).port, 65535);
        const QSet<QString> channels{QStringLiteral("Cyan")};
        QVERIFY(feedReceived(feeds.at(0), channels));
        QVERIFY(!feedReceived(feeds.at(1), channels));
        QVERIFY(feedReceived(feeds.at(2), channels));
        QVERIFY(!feedReceived(feeds.at(3), channels));
        QVERIFY(!readServerFeeds("[]", &feeds));
        QVERIFY(!readServerFeeds("{\"data\":{}}", &feeds));
        // The server browser lists the feeds and what this user receives.
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        QByteArray groups = "{\"data\":[{\"name\":\"Cyan\",\"direction\":\"IN\",\"active\":true}]}";
        api.respond = [&](const QByteArray &request) {
            const auto body = request.startsWith("GET /Marti/api/datafeeds ") ? json : groups;
            return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                   " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        new TakServerApi(servers, network, &servers);
        ServerBrowser browser(servers);
        auto *action = browser.findChild<QAction *>(QStringLiteral("serverDataFeeds"));
        auto *message = browser.findChild<QLabel *>(QStringLiteral("enrollStatus"));
        QVERIFY(action && message);
        action->trigger();
        QVERIFY(message->text().startsWith(QStringLiteral("Select a TLS or QUIC entry")));
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        auto *list = browser.findChild<QTableWidget *>(QStringLiteral("serverList"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return list->rowCount() == 1;
            },
            2000));
        list->setCurrentCell(0, 1);
        QStringList received;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, &browser, [&] {
            auto *dialog = browser.findChild<QDialog *>(QStringLiteral("dataFeedDialog"));
            auto *table = dialog
                              ? dialog->findChild<QTableWidget *>(QStringLiteral("dataFeedTable"))
                              : nullptr;
            if (!table || !dialog->isVisible()) {
                return;
            }
            poll.stop();
            received.clear();
            for (int row = 0; row < table->rowCount(); ++row) {
                received.append(table->item(row, 4)->text());
            }
            dialog->reject();
        });
        poll.start();
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !received.isEmpty();
            },
            10000));
        QCOMPARE(received,
                 (QStringList{QStringLiteral("Yes"),
                              QStringLiteral("No"),
                              QStringLiteral("Yes"),
                              QStringLiteral("No, inactive")}));
        // Without the group list, the column says Unknown.
        groups = "not json";
        received.clear();
        poll.start();
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !received.isEmpty();
            },
            10000));
        QCOMPARE(received.first(), QStringLiteral("Unknown"));
        // Errors and empty lists show in the status line.
        status = 500;
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text().contains(QStringLiteral("500"));
            },
            10000));
        status = 200;
        api.respond = [](const QByteArray &request) {
            const QByteArray body =
                request.startsWith("GET /Marti/api/datafeeds ") ? "{\"data\":[]}" : "x";
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") +
                   QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text() == QStringLiteral("The server has no data feeds.");
            },
            10000));
        api.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");
        };
        action->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return message->text() == QStringLiteral("The data feed list is invalid.");
            },
            10000));
    }
    void pluginData()
    {
        QVERIFY(TakServerApi::validPluginName(QStringLiteral("sensor-plugin.v2")));
        for (const auto &bad : {QString{},
                                QStringLiteral(".."),
                                QStringLiteral("a/b"),
                                QStringLiteral("a b"),
                                QString(65, QLatin1Char('a'))}) {
            QVERIFY(!TakServerApi::validPluginName(bad));
        }
        HttpsHarness api;
        QVERIFY(api.load());
        api.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        TakServerApi tak(servers, network);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QList<NetworkReply> replies;
        const auto collect = [&replies](const NetworkReply &reply) {
            replies.append(reply);
        };
        tak.submitPluginData(QStringLiteral("tak"),
                             QStringLiteral("sensor"),
                             "{\"a\":1}",
                             "application/json",
                             &api,
                             collect);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return replies.size() == 1;
            },
            10000));
        QVERIFY(replies.last().ok());
        QCOMPARE(replies.last().body, QByteArray("ok"));
        QVERIFY(api.requests.last().startsWith("POST /Marti/api/plugins/sensor/submit "));
        QVERIFY(api.requests.last().contains("Content-Type: application/json"));
        QVERIFY(api.requests.last().endsWith("{\"a\":1}"));
        tak.submitPluginData(
            QStringLiteral("tak"), QStringLiteral("sensor"), "raw", {}, &api, collect);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return replies.size() == 2;
            },
            10000));
        QVERIFY(api.requests.last().contains("Content-Type: application/octet-stream"));
        tak.requestPluginData(QStringLiteral("tak"), QStringLiteral("sensor"), &api, collect);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return replies.size() == 3;
            },
            10000));
        QVERIFY(api.requests.last().startsWith("GET /Marti/api/plugins/sensor/submit "));
        // Invalid names and large bodies fail through the callback without a request.
        const auto requests = api.requests.size();
        tak.submitPluginData(
            QStringLiteral("tak"), QStringLiteral("../admin"), "x", {}, &api, collect);
        tak.submitPluginData(QStringLiteral("tak"),
                             QStringLiteral("sensor"),
                             QByteArray(TakServerApi::MaxPluginDataBytes + 1, 'x'),
                             {},
                             &api,
                             collect);
        tak.requestPluginData(QStringLiteral("tak"), QStringLiteral("a b"), &api, collect);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return replies.size() == 6;
            },
            10000));
        QVERIFY(!replies.at(3).ok() && !replies.at(4).ok() && !replies.at(5).ok());
        QCOMPARE(api.requests.size(), requests);
    }
    void excheckChecklists()
    {
        const QByteArray checklist =
            "<?xml version=\"1.0\"?><checklist><checklistDetails><name>Patrol</name><uid>c-1</uid>"
            "<description>Night patrol</description><startTime>2026-10-09T08:00:00.000Z</startTime>"
            "<templateName>Base</templateName></checklistDetails>"
            "<checklistColumns><checklistColumn><columnName>Step</columnName></checklistColumn>"
            "<checklistColumn><columnName>Owner</columnName></checklistColumn></checklistColumns>"
            "<checklistTasks>"
            "<checklistTask><uid>t-1</uid><value>Radio check</value><value>Alpha</value>"
            "<status>Pending</status><notes keep=\"yes\">x &amp; y</notes></checklistTask>"
            "<checklistTask><uid>t-2</uid><value>Return</value><status>Complete</status>"
            "<completeDTG>2026-10-09T09:00:00.000Z</completeDTG></checklistTask>"
            "<checklistTask><uid>bad uid</uid></checklistTask>"
            "</checklistTasks></checklist>";
        QList<Checklist> list;
        QVERIFY(readChecklists(checklist, &list));
        QCOMPARE(list.size(), 1);
        const auto patrol = list.first();
        QCOMPARE(patrol.uid, QStringLiteral("c-1"));
        QCOMPARE(patrol.name, QStringLiteral("Patrol"));
        QCOMPARE(patrol.templateName, QStringLiteral("Base"));
        QVERIFY(patrol.started.isValid());
        QCOMPARE(patrol.columns, (QStringList{QStringLiteral("Step"), QStringLiteral("Owner")}));
        QCOMPARE(patrol.tasks.size(), 2);
        QCOMPARE(patrol.tasks.at(0).values,
                 (QStringList{QStringLiteral("Radio check"), QStringLiteral("Alpha")}));
        QVERIFY(!patrol.tasks.at(0).complete());
        QVERIFY(patrol.tasks.at(1).complete());
        QVERIFY(patrol.tasks.at(0).xml.startsWith("<checklistTask>"));
        QVERIFY(patrol.tasks.at(0).xml.endsWith("</checklistTask>"));
        // A list of active checklists.
        QVERIFY(readChecklists(
            "<checklists><checklist><checklistDetails><uid>a</uid></checklistDetails></checklist>"
            "<checklist><checklistDetails><uid>b/c</uid></checklistDetails></checklist></"
            "checklists>",
            &list));
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().name, QStringLiteral("a"));
        QVERIFY(!readChecklists("<!DOCTYPE x><x/>", &list));
        QVERIFY(!readChecklists("<checklists><checklist>", &list));
        // A status update keeps other fields and sets the completion time.
        const auto now = QDateTime::currentDateTimeUtc();
        auto updated = taskWithStatus(patrol.tasks.at(0).xml, QStringLiteral("Complete"), now);
        QVERIFY(updated.contains("<status>Complete</status>"));
        QVERIFY(updated.contains("<completeDTG>"));
        QVERIFY(updated.contains("<notes keep=\"yes\">x &amp; y</notes>"));
        QVERIFY(updated.contains("<value>Alpha</value>"));
        QCOMPARE(updated.count("<status>"), 1);
        updated = taskWithStatus(patrol.tasks.at(1).xml, QStringLiteral("Pending"), now);
        QVERIFY(updated.contains("<status>Pending</status>"));
        QVERIFY(!updated.contains("completeDTG"));
        updated = taskWithStatus(
            "<checklistTask><uid>x</uid></checklistTask>", QStringLiteral("Complete (late)"), now);
        QVERIFY(updated.contains("<status>Complete (late)</status>"));
        // The status and the completion time keep the schema order.
        updated = taskWithStatus(
            "<checklistTask><uid>y</uid><value>v</value><notes>n</notes></checklistTask>",
            QStringLiteral("Complete"),
            now);
        QVERIFY(updated.indexOf("<value>") < updated.indexOf("<status>Complete</status>"));
        QVERIFY(updated.indexOf("<status>") < updated.indexOf("<completeDTG>"));
        QVERIFY(updated.indexOf("<completeDTG>") < updated.indexOf("<notes>"));
        QVERIFY(taskWithStatus(patrol.tasks.at(0).xml, QStringLiteral("COMPLETE"), now).isEmpty());
        QVERIFY(taskWithStatus("<other/>", QStringLiteral("Pending"), now).isEmpty());
        QVERIFY(
            taskWithStatus(patrol.tasks.at(0).xml, QStringLiteral("bad status"), now).isEmpty());
        QVERIFY(taskWithStatus("<checklistTask>", QStringLiteral("Pending"), now).isEmpty());
        QVERIFY(taskWithStatus("<!DOCTYPE x><checklistTask/>", QStringLiteral("Pending"), now)
                    .isEmpty());
        QVERIFY(
            taskWithStatus(QByteArray(70 * 1024, ' '), QStringLiteral("Pending"), now).isEmpty());
        const auto templates =
            readChecklistTemplates("{\"data\":[{\"contents\":[{\"data\":{\"uid\":\"tpl-1\","
                                   "\"name\":\"Base\"}},{\"data\":{\"uid\":\"a b\"}},"
                                   "{\"data\":{\"uid\":\"tpl-2\"}}]}]}");
        QCOMPARE(templates.size(), 2);
        QCOMPARE(templates.at(1).name, QStringLiteral("tpl-2"));
        QVERIFY(validChecklistId(QStringLiteral("c-1")));
        QVERIFY(!validChecklistId(QStringLiteral("..")));
        // The service reads, starts, and updates checklists through the REST API.
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        bool templatesMissing = false;
        api.respond = [&](const QByteArray &request) {
            QByteArray body;
            int code = status;
            if (request.startsWith("GET /Marti/api/excheck/checklist/active?clientUid=SWAK-1 ")) {
                body = "<checklists>" + checklist.mid(checklist.indexOf("<checklist>")) +
                       "</checklists>";
            } else if (request.startsWith("GET /Marti/api/missions/exchecktemplates ")) {
                body = "{\"data\":[{\"contents\":[{\"data\":{\"uid\":\"tpl-1\",\"name\":\"Base\"}}]"
                       "}]}";
                code = templatesMissing ? 404 : 200;
            } else if (request.startsWith(
                           "GET /Marti/api/excheck/checklist/c-1?clientUid=SWAK-1 ")) {
                body = checklist;
            } else if (request.startsWith("GET /Marti/api/excheck/checklist/empty?clientUid=")) {
                body = "<checklists/>";
            }
            return QByteArray("HTTP/1.1 ") + QByteArray::number(code) +
                   " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        TakServerApi tak(servers, network);
        Identity identity;
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("uid"), QStringLiteral("SWAK-1"));
        identity.restorePreferences(values);
        ExCheck excheck(&tak, &identity);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QSignalSpy listed(&excheck, &ExCheck::listed);
        QSignalSpy loaded(&excheck, &ExCheck::loaded);
        QSignalSpy started(&excheck, &ExCheck::started);
        QSignalSpy changed(&excheck, &ExCheck::updated);
        excheck.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(3).toString().isEmpty());
        QCOMPARE(listed.last().at(1).value<QList<Checklist>>().size(), 1);
        QCOMPARE(listed.last().at(2).value<QList<ChecklistTemplate>>().size(), 1);
        templatesMissing = true;
        excheck.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(3).toString().isEmpty());
        QVERIFY(listed.last().at(2).value<QList<ChecklistTemplate>>().isEmpty());
        excheck.load(QStringLiteral("tak"), QStringLiteral("c-1"));
        QVERIFY(loaded.wait(10000));
        QCOMPARE(loaded.last().at(1).value<Checklist>().tasks.size(), 2);
        excheck.load(QStringLiteral("tak"), QStringLiteral("empty"));
        QVERIFY(loaded.wait(10000));
        QVERIFY(!loaded.last().at(2).toString().isEmpty());
        // A failed load names the checklist, so that a pane can ignore a stale reply.
        QCOMPARE(loaded.last().at(1).value<Checklist>().uid, QStringLiteral("empty"));
        excheck.load(QStringLiteral("tak"), QStringLiteral("a/b"));
        QVERIFY(!loaded.last().at(2).toString().isEmpty());
        QCOMPARE(loaded.last().at(1).value<Checklist>().uid, QStringLiteral("a/b"));
        QVERIFY(
            !excheck.start(QStringLiteral("tak"), QStringLiteral("tpl-1"), QStringLiteral("  "), {})
                 .isEmpty());
        QVERIFY(excheck
                    .start(QStringLiteral("tak"),
                           QStringLiteral("tpl-1"),
                           QStringLiteral("Night 1"),
                           QStringLiteral("Go"))
                    .isEmpty());
        QVERIFY(started.wait(10000));
        QVERIFY(started.last().at(1).toString().isEmpty());
        QVERIFY(api.requests.last().startsWith(
            "POST /Marti/api/excheck/tpl-1/start?name=Night%201&description=Go&callsign="));
        QVERIFY(api.requests.last().contains("clientUid=SWAK-1"));
        // TAK Server requires the start time.
        QVERIFY(api.requests.last().contains("&startTime=20"));
        QVERIFY(excheck
                    .setStatus(QStringLiteral("tak"),
                               QStringLiteral("c-1"),
                               patrol.tasks.at(0),
                               QStringLiteral("Complete"))
                    .isEmpty());
        QVERIFY(changed.wait(10000));
        QVERIFY(changed.last().at(2).toString().isEmpty());
        const auto put = api.requests.last();
        QVERIFY(put.startsWith("PUT /Marti/api/excheck/checklist/c-1/task/t-1?clientUid=SWAK-1 "));
        QVERIFY(put.contains("Content-Type: application/xml"));
        QVERIFY(put.contains("<status>Complete</status>"));
        QVERIFY(!excheck
                     .setStatus(QStringLiteral("tak"),
                                QStringLiteral("c-1"),
                                ChecklistTask{},
                                QStringLiteral("Complete"))
                     .isEmpty());
        auto broken = patrol.tasks.at(0);
        broken.xml = "<other/>";
        QVERIFY(!excheck
                     .setStatus(QStringLiteral("tak"),
                                QStringLiteral("c-1"),
                                broken,
                                QStringLiteral("Complete"))
                     .isEmpty());
        status = 403;
        excheck.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(3).toString().contains(QStringLiteral("does not allow")));
        QVERIFY(excheck
                    .setStatus(QStringLiteral("tak"),
                               QStringLiteral("c-1"),
                               patrol.tasks.at(0),
                               QStringLiteral("Pending"))
                    .isEmpty());
        QVERIFY(changed.wait(10000));
        QVERIFY(!changed.last().at(2).toString().isEmpty());
        QVERIFY(
            excheck.start(QStringLiteral("tak"), QStringLiteral("tpl-1"), QStringLiteral("x"), {})
                .isEmpty());
        QVERIFY(started.wait(10000));
        QVERIFY(!started.last().at(1).toString().isEmpty());
        excheck.load(QStringLiteral("tak"), QStringLiteral("c-1"));
        QVERIFY(loaded.wait(10000));
        QVERIFY(!loaded.last().at(2).toString().isEmpty());
        status = 200;
        api.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx");
        };
        excheck.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(3).toString().contains(QStringLiteral("invalid")));
        ExCheck offline(nullptr, nullptr);
        QSignalSpy offlineListed(&offline, &ExCheck::listed);
        offline.list(QStringLiteral("tak"));
        QCOMPARE(offlineListed.size(), 1);
        QVERIFY(!offline.start(QStringLiteral("tak"), QStringLiteral("t"), QStringLiteral("n"), {})
                     .isEmpty());
        QVERIFY(!offline
                     .setStatus(QStringLiteral("tak"),
                                QStringLiteral("c"),
                                patrol.tasks.at(0),
                                QStringLiteral("Pending"))
                     .isEmpty());
    }
    void citrapReports()
    {
        const QByteArray list =
            "<reports>"
            "<report id=\"r-1\" title=\"Bridge out\" type=\"Hazard\" status=\"Open\" "
            "importance=\"High\" "
            "userCallsign=\"Echo\" dateTime=\"2026-10-09T08:00:00.000Z\" location=\"59.5,18.25\">"
            "<userDescription>Water over road</userDescription></report>"
            "<report><id>r-2</id><title>Child elements</title><location>99,0</location></report>"
            "<report id=\"bad id\"/>"
            "</reports>";
        QList<CitrapReport> reports;
        QVERIFY(readCitrapReports(list, &reports));
        QCOMPARE(reports.size(), 2);
        QCOMPARE(reports.at(0).title, QStringLiteral("Bridge out"));
        QCOMPARE(reports.at(0).description, QStringLiteral("Water over road"));
        QCOMPARE(reports.at(0).latitude, 59.5);
        QVERIFY(reports.at(0).time.isValid());
        QCOMPARE(reports.at(1).id, QStringLiteral("r-2"));
        QCOMPARE(reports.at(1).latitude, 0.0);
        QVERIFY(!readCitrapReports("<!DOCTYPE x><x/>", &reports));
        QVERIFY(!readCitrapReports("<reports><report>", &reports));
        // A written report reads back.
        auto written = reports.first();
        written.status = QStringLiteral("Closed");
        QList<CitrapReport> again;
        QVERIFY(readCitrapReports(citrapReportXml(written), &again));
        QCOMPARE(again.size(), 1);
        QCOMPARE(again.first().status, QStringLiteral("Closed"));
        QCOMPARE(again.first().description, written.description);
        QVERIFY(std::abs(again.first().longitude - 18.25) < 1e-6);
        QVERIFY(validCitrapId(QStringLiteral("r-1")));
        QVERIFY(!validCitrapId(QStringLiteral("..")));
        // TAK Server writes WKT locations and answers a search with a JSON list.
        QVERIFY(citrapReportXml(written).contains("location=\"POINT(18.2500000 59.5000000)\""));
        QVERIFY(citrapReportXml(written).contains("userDescription=\"Water over road\""));
        QVERIFY(readCitrapReports(
            "[{\"id\":\"j-1\",\"title\":\"JSON\",\"location\":\"POINT (18.5 59.25)\","
            "\"userDescription\":\"From JSON\",\"importance\":3},{\"id\":\"bad id\"}]",
            &reports));
        QCOMPARE(reports.size(), 1);
        QCOMPARE(reports.first().latitude, 59.25);
        QCOMPARE(reports.first().longitude, 18.5);
        QCOMPARE(reports.first().description, QStringLiteral("From JSON"));
        QByteArray tooMany = "[";
        for (int i = 0; i <= MaxCitrapReports; ++i) {
            tooMany += i ? ",{}" : "{}";
        }
        QVERIFY(!readCitrapReports(tooMany + "]", &reports));
        // The service lists, creates with attachments, and updates reports.
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        QByteArray createReply = "{\"id\":\"server-7\"}";
        api.respond = [&](const QByteArray &request) {
            QByteArray body;
            int code = status;
            if (request.startsWith("GET /Marti/api/citrap?")) {
                body = list;
            } else if (request.startsWith("POST /Marti/api/citrap?")) {
                body = createReply;
            } else if (request.contains("/attachment?") &&
                       request.contains("filename=\"_bad.bin\"")) {
                code = 500;
            }
            return QByteArray("HTTP/1.1 ") + QByteArray::number(code) +
                   " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        NetworkClient network;
        Store store;
        Servers servers(store);
        TakServerApi tak(servers, network);
        Identity identity;
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("uid"), QStringLiteral("SWAK-1"));
        identity.restorePreferences(values);
        Citrap citrap(&tak, &identity);
        auto entry = source(9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), entry).isEmpty());
        QSignalSpy listed(&citrap, &Citrap::listed);
        QSignalSpy created(&citrap, &Citrap::created);
        QSignalSpy changed(&citrap, &Citrap::updated);
        citrap.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QCOMPARE(listed.last().at(1).value<QList<CitrapReport>>().size(), 2);
        CitrapReport report;
        QVERIFY(!citrap.create(QStringLiteral("tak"), report, {}).isEmpty());
        report.title = QStringLiteral("New hazard");
        QVERIFY(!citrap.create(QStringLiteral("tak"), report, {{QStringLiteral("empty"), {}}})
                     .isEmpty());
        QList<CitrapAttachment> many;
        for (int i = 0; i <= MaxCitrapAttachments; ++i) {
            many.append({QStringLiteral("f"), "x"});
        }
        QVERIFY(!citrap.create(QStringLiteral("tak"), report, many).isEmpty());
        QVERIFY(citrap
                    .create(QStringLiteral("tak"),
                            report,
                            {{QStringLiteral("photo 1.jpg"), "jpeg"},
                             {QStringLiteral("../bad.bin"), "bin"}})
                    .isEmpty());
        QVERIFY(created.wait(10000));
        QCOMPARE(created.last().at(1).toString(), QStringLiteral("server-7"));
        QVERIFY(created.last().at(2).toString().contains(QStringLiteral("bad.bin")));
        // The new report goes up as a zip package with the report XML.
        const auto post =
            std::find_if(api.requests.cbegin(), api.requests.cend(), [](const QByteArray &request) {
                return request.startsWith("POST /Marti/api/citrap?clientUid=SWAK-1 ");
            });
        QVERIFY(post != api.requests.cend());
        const auto package = readDataPackage(post->mid(post->indexOf("\r\n\r\n") + 4));
        QVERIFY2(package.error.isEmpty(), qPrintable(package.error));
        QCOMPARE(package.entries.size(), 1);
        QCOMPARE(package.entries.first().path, QStringLiteral("report.xml"));
        QVERIFY(package.entries.first().bytes.contains("title=\"New hazard\""));
        // Attachments are raw bytes with the file name in Content-Disposition.
        const auto uploads = std::count_if(
            api.requests.cbegin(), api.requests.cend(), [](const QByteArray &request) {
                return request.startsWith(
                    "POST /Marti/api/citrap/server-7/attachment?clientUid=SWAK-1 ");
            });
        QCOMPARE(uploads, 2);
        QVERIFY(
            std::any_of(api.requests.cbegin(), api.requests.cend(), [](const QByteArray &request) {
                return request.contains(
                           "Content-Disposition: attachment; filename=\"photo_1.jpg\"") &&
                       request.endsWith("\r\n\r\njpeg");
            }));
        // An XML reply names the new report too.
        createReply = "<report id=\"xml-3\"/>";
        QVERIFY(citrap.create(QStringLiteral("tak"), report, {}).isEmpty());
        QVERIFY(created.wait(10000));
        QCOMPARE(created.last().at(1).toString(), QStringLiteral("xml-3"));
        // A plain identifier reply also names the new report.
        createReply = "plain-9";
        QVERIFY(citrap.create(QStringLiteral("tak"), report, {}).isEmpty());
        QVERIFY(created.wait(10000));
        QCOMPARE(created.last().at(1).toString(), QStringLiteral("plain-9"));
        // A title with spaces at the ends or an emoji is cleaned, not refused by the package.
        auto spaced = report;
        spaced.title =
            QStringLiteral(" Fire ") + QString::fromUtf8("\xF0\x9F\x94\xA5") + QStringLiteral(" ");
        const auto spacedError = citrap.create(QStringLiteral("tak"), spaced, {});
        QVERIFY2(spacedError.isEmpty(), qPrintable(spacedError));
        QVERIFY(created.wait(10000));
        QVERIFY(created.last().at(2).toString().isEmpty());
        written.id = QStringLiteral("r-1");
        QVERIFY(citrap.update(QStringLiteral("tak"), written).isEmpty());
        QVERIFY(changed.wait(10000));
        QVERIFY(changed.last().at(2).toString().isEmpty());
        QVERIFY(api.requests.last().startsWith("PUT /Marti/api/citrap/r-1?clientUid=SWAK-1 "));
        QVERIFY(api.requests.last().contains("status=\"Closed\""));
        written.id = QStringLiteral("bad id");
        QVERIFY(!citrap.update(QStringLiteral("tak"), written).isEmpty());
        status = 403;
        citrap.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(2).toString().contains(QStringLiteral("does not allow")));
        QVERIFY(citrap.create(QStringLiteral("tak"), report, {}).isEmpty());
        QVERIFY(created.wait(10000));
        QVERIFY(!created.last().at(2).toString().isEmpty());
        written.id = QStringLiteral("r-1");
        QVERIFY(citrap.update(QStringLiteral("tak"), written).isEmpty());
        QVERIFY(changed.wait(10000));
        QVERIFY(!changed.last().at(2).toString().isEmpty());
        status = 200;
        api.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n<a>x");
        };
        citrap.list(QStringLiteral("tak"));
        QVERIFY(listed.wait(10000));
        QVERIFY(listed.last().at(2).toString().contains(QStringLiteral("invalid")));
        Citrap offline(nullptr, nullptr);
        QSignalSpy offlineListed(&offline, &Citrap::listed);
        offline.list(QStringLiteral("tak"));
        QCOMPARE(offlineListed.size(), 1);
        QVERIFY(!offline.create(QStringLiteral("tak"), report, {}).isEmpty());
        QVERIFY(!offline.update(QStringLiteral("tak"), written).isEmpty());
    }
    void takServerApi()
    {
        auto entry = serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("tak.example"));
        QCOMPARE(TakServerApi::apiUrl(entry, QStringLiteral("/version/config")),
                 QUrl(QStringLiteral("https://tak.example:8443/Marti/api/version/config")));
        QCOMPARE(TakServerApi::apiUrl(entry, QStringLiteral("/missions?tool=public"))
                     .toString(QUrl::FullyEncoded),
                 QStringLiteral("https://tak.example:8443/Marti/api/missions?tool=public"));
        QVERIFY(TakServerApi::apiUrl(entry, QStringLiteral("version")).isEmpty());
        QVERIFY(TakServerApi::apiUrl(entry, QStringLiteral("/../admin")).isEmpty());
        auto off = entry;
        off.insert(QStringLiteral("apiPort"), 0);
        QVERIFY(TakServerApi::apiUrl(off, QStringLiteral("/version")).isEmpty());
        off = entry;
        off.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        QVERIFY(TakServerApi::apiUrl(off, QStringLiteral("/version")).isEmpty());
        off = entry;
        off.insert(QStringLiteral("apiPort"), 70000);
        QVERIFY(!validateServer(off).isEmpty());
        off.remove(QStringLiteral("apiPort"));
        QVERIFY(validateServer(off).isEmpty());
        QCOMPARE(TakServerApi::apiUrl(off, QStringLiteral("/v")).port(), 8443);
        const auto good = readTakServerConfig(
            "{\"version\":\"3\",\"type\":\"ServerConfig\",\"data\":{\"version\":"
            "\"5.7.14-RELEASE\",\"api\":\"3\",\"hostname\":\"tak\"},\"nodeId\":\"n\"}");
        QVERIFY(good.available());
        QCOMPARE(good.version, QStringLiteral("5.7.14-RELEASE"));
        QCOMPARE(good.api, QStringLiteral("3"));
        QVERIFY(!readTakServerConfig("{\"type\":\"Other\",\"data\":{}}").available());
        QVERIFY(!readTakServerConfig("{\"type\":\"ServerConfig\",\"data\":{\"version\":"
                                     "\"<b>\",\"api\":\"3\"}}")
                     .available());
        QVERIFY(!readTakServerConfig("not json").available());
    }
    void takServerCapabilityCheck()
    {
        TlsHarness stream;
        QFile cert(QFINDTESTDATA("fixtures/map-test-cert.pem")),
            key(QFINDTESTDATA("fixtures/map-test-key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly) && key.open(QIODevice::ReadOnly));
        stream.certificate = QSslCertificate(cert.readAll());
        stream.key = QSslKey(key.readAll(), QSsl::Rsa);
        QVERIFY(stream.listen(QHostAddress::LocalHost, 0));
        HttpsHarness api;
        QVERIFY(api.load());
        api.respond = [](const QByteArray &) {
            const QByteArray body = "{\"version\":\"3\",\"type\":\"ServerConfig\",\"data\":"
                                    "{\"version\":\"5.7.14-RELEASE\",\"api\":\"3\"}}";
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Content-Length: ") +
                   QByteArray::number(body.size()) + "\r\n\r\n" + body;
        };
        Store store;
        Servers servers(store);
        NetworkClient network;
        TakServerApi tak(servers, network);
        auto v = source(stream.serverPort());
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        v.insert(QStringLiteral("ca"), cert.fileName());
        v.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers.put(QStringLiteral("tak"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return tak.info(QStringLiteral("tak")).available();
            },
            10000));
        QCOMPARE(tak.info(QStringLiteral("tak")).version, QStringLiteral("5.7.14-RELEASE"));
        QVERIFY(api.requests.first().startsWith("GET /Marti/api/version/config HTTP/"));
        QVERIFY(servers.status(QStringLiteral("tak"))
                    .contains(QStringLiteral("TAK Server 5.7.14-RELEASE, REST API 3.")));
        // A failed check shows the reason. Turning the API off removes the line.
        api.respond = [](const QByteArray &) {
            return QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        };
        v.insert(QStringLiteral("retrySeconds"), 2);
        QVERIFY(servers.put(QStringLiteral("tak"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tak"))
                    .contains(QStringLiteral("HTTP status 404"));
            },
            10000));
        v.insert(QStringLiteral("apiPort"), 0);
        QVERIFY(servers.put(QStringLiteral("tak"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !servers.status(QStringLiteral("tak")).contains(QStringLiteral("REST"));
            },
            5000));
        QVERIFY(!tak.info(QStringLiteral("tak")).checking);
    }
    void storeDeletesAndBudget()
    {
        Store store;
        auto first = sampleEvent();
        first.uid = QStringLiteral("from-one");
        first.origin = QStringLiteral("server-1");
        auto second = sampleEvent();
        second.uid = QStringLiteral("from-two");
        second.origin = QStringLiteral("server-2");
        QVERIFY(store.accept(first).isEmpty());
        QVERIFY(store.accept(second).isEmpty());
        store.select(first.uid);
        // A TAK delete removes only objects from its own origin, and is not stored itself.
        auto remove = sampleEvent();
        remove.uid = QStringLiteral("delete-1");
        remove.type = QStringLiteral("t-x-d-d");
        remove.origin = QStringLiteral("server-1");
        remove.detail = QStringLiteral("<link uid=\"from-one\" relation=\"none\" type=\"a-f-G\"/>"
                                       "<link uid=\"from-two\" relation=\"none\" type=\"a-f-G\"/>"
                                       "<__forcedelete/>");
        QSignalSpy changed(&store, &Store::changed);
        QVERIFY(store.accept(remove).isEmpty());
        QCOMPARE(store.rowCount(), 1);
        QCOMPARE(store.events().first().uid, QStringLiteral("from-two"));
        QVERIFY(store.selected().isEmpty());
        QVERIFY(changed.count() >= 1);
        QVERIFY(store.motion().snapshot().size() <= 1);
        remove.time = remove.time.addSecs(1);
        QVERIFY(store.accept(remove).isEmpty());
        QCOMPARE(store.rowCount(), 1);
        // An update from another origin does not take the object over. A later delete from that
        // origin therefore cannot remove a local object or another server's object.
        auto local = sampleEvent();
        local.uid = QStringLiteral("local-1");
        QVERIFY(store.accept(local).isEmpty());
        for (const auto &target : {local, second}) {
            auto takeover = target;
            takeover.origin = QStringLiteral("server-1");
            takeover.time = takeover.time.addSecs(2);
            takeover.callsign = QStringLiteral("Relayed");
            QVERIFY(store.accept(takeover).isEmpty());
            const auto row = std::find_if(
                store.events().cbegin(), store.events().cend(), [&](const Event &event) {
                    return event.uid == target.uid;
                });
            QVERIFY(row != store.events().cend());
            QCOMPARE(row->callsign, QStringLiteral("Relayed"));
            QCOMPARE(row->origin, target.origin);
            remove.time = remove.time.addSecs(1);
            remove.detail = QStringLiteral("<link uid=\"%1\" relation=\"none\" type=\"a-f-G\"/>")
                                .arg(target.uid);
            QVERIFY(store.accept(remove).isEmpty());
        }
        QCOMPARE(store.rowCount(), 2);
        // Retained detail has a total budget. Later events keep only their typed fields.
        store.clear();
        QCOMPARE(store.detailCharacters(), 0);
        const auto large =
            QStringLiteral("<remarks>") + QString(30000, u'r') + QStringLiteral("</remarks>");
        bool truncated = false;
        for (int i = 0; i < 300; ++i) {
            auto event = sampleEvent();
            event.uid = QStringLiteral("large-%1").arg(i);
            event.detail = large;
            QVERIFY(store.accept(event).isEmpty());
            truncated = truncated || store.events().last().detailTruncated;
        }
        QVERIFY(truncated);
        QVERIFY(store.detailCharacters() <= Store::DetailBudget);
        QVERIFY(store.detailCharacters() > Store::DetailBudget - large.size());
        // An update without detail frees budget for the same identity.
        auto lighter = store.events().first();
        lighter.detail.clear();
        lighter.time = lighter.time.addSecs(1);
        const auto before = store.detailCharacters();
        QVERIFY(store.accept(lighter).isEmpty());
        QCOMPARE(store.detailCharacters(), before - large.size());
        store.expire(QDateTime::currentDateTimeUtc().addDays(30));
        QCOMPARE(store.detailCharacters(), 0);
    }
    void emergencies()
    {
        auto sender = sampleEvent();
        sender.uid = QStringLiteral("ANDROID-7");
        sender.callsign = QStringLiteral("VIPER");
        sender.type = QStringLiteral("a-f-G-U-C");
        const auto alert = emergencyEvent(sender, QStringLiteral("Troops In Contact"));
        QVERIFY2(eventError(alert).isEmpty(), qPrintable(eventError(alert)));
        QCOMPARE(alert.uid, QStringLiteral("ANDROID-7-9-1-1"));
        QCOMPARE(alert.type, QStringLiteral("b-a-o-opn"));
        QCOMPARE(alert.callsign, QStringLiteral("VIPER-Alert"));
        QCOMPARE(alert.latitude, sender.latitude);
        QVERIFY(activeEmergency(alert));
        QCOMPARE(emergencyLabel(alert), QStringLiteral("Troops In Contact"));
        QCOMPARE(emergency(alert)->text, QStringLiteral("VIPER"));
        QCOMPARE(links(alert).first().uid, QStringLiteral("ANDROID-7"));
        QCOMPARE(links(alert).first().relation, QStringLiteral("p-p"));
        // The XML matches the TAK repeater tests on the emergency type text.
        QVERIFY(
            encodeCot(alert).contains("<emergency type=\"Troops In Contact\">VIPER</emergency>"));
        const auto cancel = emergencyCancelEvent(sender);
        QCOMPARE(cancel.uid, alert.uid);
        QCOMPARE(cancel.type, QStringLiteral("b-a-o-can"));
        QVERIFY(emergency(cancel)->cancel);
        QVERIFY(!activeEmergency(cancel));
        QVERIFY(eventError(emergencyEvent(sender, QStringLiteral("Unknown"))).size() > 0);
        auto custom = alert;
        custom.type = QStringLiteral("b-a-o-c");
        QVERIFY(activeEmergency(custom));
        auto cancelled = alert;
        cancelled.detail = QStringLiteral("<emergency cancel=\"true\">VIPER</emergency>");
        QVERIFY(!activeEmergency(cancelled));
        // A cancel removes the alert from the same origin only, and leaves no marker.
        Store store;
        auto remote = alert;
        remote.origin = QStringLiteral("server");
        QVERIFY(store.accept(remote).isEmpty());
        auto foreign = cancel;
        foreign.origin = QStringLiteral("other");
        QVERIFY(store.accept(foreign).isEmpty());
        QCOMPARE(store.rowCount(), 1);
        auto matching = cancel;
        matching.origin = QStringLiteral("server");
        QVERIFY(store.accept(matching).isEmpty());
        QCOMPARE(store.rowCount(), 0);
    }
    void cotDetailModel()
    {
        const auto event = [](const QByteArray &detail) {
            return QByteArray("<?xml version='1.0' encoding='UTF-8'?>"
                              "<event version='2.0' uid='ANDROID-1' type='a-f-G-U-C' how='m-g' "
                              "time='2026-10-07T10:00:00.000Z' start='2026-10-07T10:00:00.000Z' "
                              "stale='2026-10-07T10:05:00.000Z'>"
                              "<point lat='59.3' lon='18.1' hae='12' ce='5' le='9'/><detail>") +
                   detail + "</detail></event>";
        };
        const QByteArray detail =
            "<contact endpoint='192.168.1.5:4242:tcp' callsign='VIPER' phone='123'/>"
            "<__group name='Cyan' role='Team Lead'/>"
            "<status battery='87'/>"
            "<takv device='Pixel' platform='ATAK-CIV' os='34' version='5.4.0'/>"
            "<track course='90.5' speed='2.5'/>"
            "<remarks source='BAO.F.ATAK'>Hold &amp; observe</remarks>"
            "<link uid='RP-1' type='b-m-p-w' relation='c' point='59.3,18.1' "
            "parent_callsign='VIPER'/>"
            "<marti><dest callsign='HAWK'/><dest uid='ANDROID-2'/><dest mission='Ops'/></marti>"
            "<emergency type='911 Alert'>VIPER</emergency>"
            "<usericon iconsetpath='COT_MAPPING_SPOTMAP/b-m-p-s-m/-65536'/>"
            "<color argb='-1'/><strokeColor value='-16776961'/>"
            "<fileshare filename='plan.zip' name='Plan' senderUrl='https://tak:8443/Marti/sync/"
            "content?hash=ab' sizeInBytes='2048' sha256='" +
            QByteArray(64, 'a') +
            "' senderUid='ANDROID-1' senderCallsign='VIPER'/>"
            "<__chat parent='RootContactGroup' groupOwner='false' messageId='m-1' "
            "chatroom='All Chat Rooms' id='All Chat Rooms' senderCallsign='VIPER'/>"
            "<__video><ConnectionEntry protocol='rtsp' address='10.0.0.9' port='554' "
            "path='/live'/></__video>"
            "<x:extension xmlns:x='urn:example'><x:inner/></x:extension>";
        const auto decoded = decodeCot(event(detail));
        QVERIFY2(decoded.event, qPrintable(decoded.error));
        const auto &e = *decoded.event;
        QCOMPARE(e.callsign, QStringLiteral("VIPER"));
        QCOMPARE(e.team, QStringLiteral("Cyan"));
        QVERIFY(!e.detailTruncated);
        // Namespaced elements are left out, because their copy would not be valid XML.
        QVERIFY(!e.detail.contains(QStringLiteral("urn:example")));
        QVERIFY(!e.detail.contains(QStringLiteral("extension")));
        QCOMPARE(remarks(e), QStringLiteral("Hold & observe"));
        QCOMPARE(contactEndpoint(e), QStringLiteral("192.168.1.5:4242:tcp"));
        QCOMPARE(batteryPercent(e), std::optional<int>(87));
        const auto pointLinks = links(e);
        QCOMPARE(pointLinks.size(), 1);
        QCOMPARE(pointLinks.first().uid, QStringLiteral("RP-1"));
        QCOMPARE(pointLinks.first().callsign, QStringLiteral("VIPER"));
        const auto targets = destinations(e);
        QCOMPARE(targets.size(), 3);
        QCOMPARE(targets.at(0).callsign, QStringLiteral("HAWK"));
        QCOMPARE(targets.at(1).uid, QStringLiteral("ANDROID-2"));
        QCOMPARE(targets.at(2).mission, QStringLiteral("Ops"));
        const auto alert = emergency(e);
        QVERIFY(alert && !alert->cancel);
        QCOMPARE(alert->type, QStringLiteral("911 Alert"));
        QCOMPARE(iconsetPath(e), QStringLiteral("COT_MAPPING_SPOTMAP/b-m-p-s-m/-65536"));
        QCOMPARE(detailColor(e, u"color"), std::optional<QColor>(QColor::fromRgba(0xffffffff)));
        QCOMPARE(detailColor(e, u"strokeColor")->rgba(), 0xff0000ffu);
        QVERIFY(!detailColor(e, u"fillColor"));
        const auto share = fileShare(e);
        QVERIFY(share);
        QCOMPARE(share->sizeInBytes, 2048);
        QCOMPARE(share->sha256, QString(64, 'a'));
        QCOMPARE(share->senderCallsign, QStringLiteral("VIPER"));
        QCOMPARE(chat(e)->chatroom, QStringLiteral("All Chat Rooms"));
        QCOMPARE(videoUrl(e), QStringLiteral("rtsp://10.0.0.9:554/live"));
        // Information shows the remarks and selected detail values.
        const auto item = cotMapItem(e);
        QCOMPARE(item.description, QStringLiteral("Hold & observe"));
        QCOMPARE(item.properties.value(QStringLiteral("Endpoint")).toString(),
                 QStringLiteral("192.168.1.5:4242:tcp"));
        QCOMPARE(item.properties.value(QStringLiteral("Battery")).toString(),
                 QStringLiteral("87 %"));
        QCOMPARE(item.properties.value(QStringLiteral("Device")).toString(),
                 QStringLiteral("ATAK-CIV 5.4.0 Pixel"));
        QCOMPARE(item.properties.value(QStringLiteral("Links")).toInt(), 1);
        // Encoding keeps the detail. Typed fields replace their own attributes only.
        auto edited = e;
        edited.callsign = QStringLiteral("COBRA");
        edited.speed.reset();
        const auto again = decodeCot(encodeCot(edited));
        QVERIFY(again.event);
        QCOMPARE(again.event->callsign, QStringLiteral("COBRA"));
        QCOMPARE(contactEndpoint(*again.event), QStringLiteral("192.168.1.5:4242:tcp"));
        QCOMPARE(detailElement(*again.event, u"contact")->attribute(u"phone"),
                 QStringLiteral("123"));
        QCOMPARE(again.event->course, e.course);
        QVERIFY(!again.event->speed);
        QCOMPARE(remarks(*again.event), remarks(e));
        QCOMPARE(destinations(*again.event).size(), 3);
        QCOMPARE(videoUrl(*again.event), videoUrl(e));
        QCOMPARE(decodeCot(encodeCot(*again.event)).event->detail, again.event->detail);
        // Clearing typed fields removes their elements or attributes.
        edited = *again.event;
        edited.team.clear();
        edited.role.clear();
        edited.course.reset();
        edited.callsign.clear();
        const auto cleared = decodeCot(encodeCot(edited));
        QVERIFY(cleared.event);
        QVERIFY(!detailElement(*cleared.event, u"__group"));
        QVERIFY(!detailElement(*cleared.event, u"track"));
        QVERIFY(detailElement(*cleared.event, u"contact")->attribute(u"callsign").isEmpty());
        // Detail beyond the retained size keeps only the typed fields.
        const auto large =
            decodeCot(event("<contact callsign='BIG'/><remarks>" +
                            QByteArray(MaxDetailCharacters + 1, 'r') + "</remarks>"));
        QVERIFY(large.event);
        QVERIFY(large.event->detailTruncated);
        QVERIFY(large.event->detail.isEmpty());
        QCOMPARE(large.event->callsign, QStringLiteral("BIG"));
        // Readers reject bad values. Writers skip invalid names.
        const auto bad = decodeCot(event("<fileshare sha256='xyz' sizeInBytes='-4'/>"
                                         "<color argb='99999999999'/><status battery='300'/>"));
        QVERIFY(bad.event);
        QVERIFY(fileShare(*bad.event)->sha256.isEmpty());
        QCOMPARE(fileShare(*bad.event)->sizeInBytes, -1);
        QVERIFY(!detailColor(*bad.event, u"color"));
        QVERIFY(!batteryPercent(*bad.event));
        QVERIFY(detailElements(QStringLiteral("<a><b></a>")).empty());
        QVERIFY(detailElements(QStringLiteral("<!DOCTYPE x><a/>")).empty());
        QString deep;
        for (int i = 0; i < 12; ++i) {
            deep += QStringLiteral("<n>");
        }
        for (int i = 0; i < 12; ++i) {
            deep += QStringLiteral("</n>");
        }
        QVERIFY(detailElements(deep).empty());
        CotElement invalid;
        invalid.name = QStringLiteral("bad name");
        CotElement valid;
        valid.name = QStringLiteral("remarks");
        valid.text = QStringLiteral("ok");
        valid.setAttribute(QStringLiteral("source"), QStringLiteral("SWAK"));
        valid.setAttribute(QStringLiteral("source"), QStringLiteral("SWAK-2"));
        QCOMPARE(detailText({invalid, valid}),
                 QStringLiteral("<remarks source=\"SWAK-2\">ok</remarks>"));
        // A rejected value fails the whole write. Silent loss would send partial content.
        auto badValue = valid;
        badValue.setAttribute(QStringLiteral("to"), QStringLiteral("a") + QChar(0xDC00));
        QVERIFY(detailText({badValue}).isEmpty());
        auto badText = valid;
        badText.text = QStringLiteral("a") + QChar(0x0001);
        QVERIFY(detailText({badText}).isEmpty());
    }
    void identityRoundTrip()
    {
        Identity identity;
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("team"), QStringLiteral("Blue"));
        values.insert(QStringLiteral("role"), QStringLiteral("Medic"));
        QVERIFY(identity.validateSettings(values).isEmpty());
        identity.restorePreferences(values);
        auto e = sampleEvent();
        identity.apply(e);
        const auto result = decodeCot(encodeCot(e));
        QVERIFY(result.event);
        QCOMPARE(result.event->team, e.team);
        QCOMPARE(result.event->role, e.role);
        values.insert(QStringLiteral("how"), QStringLiteral("bad"));
        QVERIFY(!identity.validateSettings(values).isEmpty());
        identity.restorePreferences(values);
        QCOMPARE(identity.savePreferences().value(QStringLiteral("how")).toString(),
                 QStringLiteral("h-e"));
    }
    void lanDiscovery()
    {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto bytes = lanAnnouncement(id, 1234);
        const QHostAddress local(QStringLiteral("192.168.10.1")),
            remote(QStringLiteral("192.168.10.2"));
        const auto peer = parseLanAnnouncement(bytes, remote, local, 24);
        QVERIFY(peer);
        QCOMPARE(peer->id, id);
        QCOMPARE(peer->port, 1234);
        for (const auto &address : {QStringLiteral("192.168.11.2"),
                                    QStringLiteral("192.168.10.255"),
                                    QStringLiteral("192.168.10.0"),
                                    QStringLiteral("192.168.10.1"),
                                    QStringLiteral("224.0.0.1"),
                                    QStringLiteral("::1")}) {
            QVERIFY(!parseLanAnnouncement(bytes, QHostAddress(address), local, 24));
        }
        for (int prefix : {-1, 0, 31, 32}) {
            QVERIFY(!parseLanAnnouncement(bytes, remote, local, prefix));
        }
        for (const auto &invalid : {QByteArray{},
                                    QByteArray(513, 'x'),
                                    QByteArray("{}"),
                                    lanAnnouncement(QStringLiteral("bad"), 1234),
                                    lanAnnouncement(id, 0)}) {
            QVERIFY(!parseLanAnnouncement(invalid, remote, local, 24));
        }
        auto version = bytes;
        version.replace("\"version\":1", "\"version\":2");
        QVERIFY(!parseLanAnnouncement(version, remote, local, 24));
    }
    void lanPeers()
    {
        if (qEnvironmentVariable("SWAK_TEST_LAN") != QLatin1String("1")) {
            QSKIP("Run tools/test_lan.py for isolated LAN integration.");
        }
        Store firstStore, secondStore;
        Servers first(firstStore), second(secondStore);
        auto firstLan = serverDefaults();
        firstLan.insert("name", "First LAN");
        firstLan.insert("protocol", "lan");
        firstLan.insert("enabled", true);
        firstLan.insert("localAddress", "192.0.2.1");
        auto secondLan = firstLan;
        secondLan.insert("localAddress", "192.0.2.2");
        QVERIFY(first.put("lan", firstLan).isEmpty());
        QVERIFY(second.put("lan", secondLan).isEmpty());
        const auto connected = [](const Servers &servers) {
            const auto states = servers.statistics();
            return std::any_of(states.begin(), states.end(), [](const auto &state) {
                return state.toObject().value(QStringLiteral("state")) ==
                       QLatin1String("Connected");
            });
        };
        const bool ready = QTest::qWaitFor(
            [&] {
                return connected(first) && connected(second);
            },
            12000);
        if (!ready) {
            qInfo() << first.statistics() << second.statistics();
        }
        QVERIFY(ready);
        QVERIFY(first.send(sampleEvent(QStringLiteral("from-first"))).isEmpty());
        QVERIFY(second.send(sampleEvent(QStringLiteral("from-second"))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return firstStore.rowCount() == 1 && secondStore.rowCount() == 1;
            },
            3000));
        QCOMPARE(firstStore.events().first().uid, QStringLiteral("from-second"));
        QCOMPARE(secondStore.events().first().uid, QStringLiteral("from-first"));
        QTest::qWait(100);
        QCOMPARE(firstStore.rowCount(), 1);
        QCOMPARE(secondStore.rowCount(), 1);
        LocalLanStatus pane(first);
        auto *table = pane.findChild<QTableWidget *>("localLanPeers");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("192.0.2.2"));
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Connected"));
        QVERIFY(!(table->item(0, 0)->flags() & Qt::ItemIsEditable));
        table->setCurrentCell(0, 0);
        auto otherFirst = firstLan, otherSecond = secondLan;
        otherFirst.insert("localAddress", "198.51.100.1");
        otherSecond.insert("localAddress", "198.51.100.2");
        QVERIFY(first.put("other", otherFirst).isEmpty());
        QVERIFY(second.put("other", otherSecond).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return table->rowCount() == 2 &&
                       table->item(1, 4)->text() == QStringLiteral("Connected");
            },
            12000));
        QCOMPARE(table->item(table->currentRow(), 2)->text(), QStringLiteral("192.0.2.2"));
        first.remove("other");
        second.remove("other");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return table->rowCount() == 1;
            },
            3000));
        QVERIFY(connected(first) && connected(second));
        QSignalSpy statusChanges(&first, &Servers::statusChanged);
        first.reconnect();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return statusChanges.count() >= 2;
            },
            12000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return connected(first) && connected(second);
            },
            12000));
        QVERIFY(second.send(sampleEvent(QStringLiteral("after-reconnect"))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return firstStore.rowCount() == 2;
            },
            3000));
        auto disabled = secondLan;
        disabled.insert(QStringLiteral("enabled"), false);
        QVERIFY(second.put("lan", disabled).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !connected(first) && !connected(second);
            },
            3000));
    }
    void serverMigration()
    {
        Store store;
        Servers servers(store);
        servers.restorePreferences({{"address", "127.0.0.1"}, {"port", 6969}, {"running", true}});
        QVERIFY(servers.sources().isEmpty());
        servers.restorePreferences(
            {{"servers", QJsonObject{{"legacy-udp", source(6969)}, {"saved", source(8088)}}},
             {"lan", QJsonObject{{"enabled", true}, {"address", "192.0.2.1"}}}});
        QVERIFY(!servers.sources().contains("legacy-udp"));
        QCOMPARE(servers.sources().size(), 2);
        QCOMPARE(servers.sources().value("migrated-lan").toObject().value("protocol").toString(),
                 QStringLiteral("lan"));
        servers.remove("migrated-lan");
        auto saved = servers.savePreferences();
        saved.insert("lan", QJsonObject{{"enabled", true}, {"address", "192.0.2.1"}});
        servers.restorePreferences(saved);
        QCOMPARE(servers.sources().size(), 1);
        LocalLanStatus pane(servers);
        auto *summary = pane.findChild<QPlainTextEdit *>("localLanSummary");
        QVERIFY(summary->toPlainText().contains("No Local LAN servers"));
        auto lan = serverDefaults();
        lan.insert("name", "Unavailable LAN");
        lan.insert("protocol", "lan");
        lan.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put("lan", lan).isEmpty());
        QVERIFY(summary->toPlainText().contains("Disconnected"));
        lan.insert("enabled", true);
        QVERIFY(servers.put("lan", lan).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return summary->toPlainText().contains("active, non-loopback");
            },
            3000));
        servers.remove("lan");
        QVERIFY(summary->toPlainText().contains("No Local LAN servers"));
        const auto fields = serverFields(lan);
        const auto interface = std::find_if(fields.begin(), fields.end(), [](const auto &f) {
            return f.path == "localAddress";
        });
        QVERIFY(interface != fields.end());
    }
    void interfaceBinding()
    {
        Store store;
        Servers servers(store);
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        auto entry = source(listener.serverPort());
        entry.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put("bound", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("bound").contains("Cannot bind interface 203.0.113.254");
            },
            3000));
        QVERIFY(!listener.hasPendingConnections());
        entry.insert("localAddress", "127.0.0.1");
        QVERIFY(servers.put("bound", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return listener.hasPendingConnections();
            },
            3000));
        std::unique_ptr<QTcpSocket> peer(listener.nextPendingConnection());
        peer->setParent(nullptr);
        QCOMPARE(peer->peerAddress(), QHostAddress(QHostAddress::LocalHost));
        servers.remove("bound");
        QUdpSocket reservation;
        QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        entry.insert("port", reservation.localPort());
        reservation.close();
        entry.insert("protocol", "udp");
        entry.insert("host", "0.0.0.0");
        QVERIFY(servers.put("udp", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("udp").contains("Listening");
            },
            3000));
        QUdpSocket sender;
        sender.writeDatagram(encodeCot(sampleEvent()),
                             QHostAddress::LocalHost,
                             static_cast<quint16>(entry.value("port").toInt()));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return store.rowCount() == 1;
            },
            3000));
        entry.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put("udp", entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("udp").contains("Cannot bind UDP");
            },
            3000));
    }
    void validation()
    {
        auto v = source(8088);
        QVERIFY(validateServer(v).isEmpty());
        for (const auto &host : {QStringLiteral("https://example.com"),
                                 QStringLiteral("224.0.0.1"),
                                 QStringLiteral("255.255.255.255"),
                                 QStringLiteral("a\nb")}) {
            auto copy = v;
            copy.insert(QStringLiteral("host"), host);
            QVERIFY(!validateServer(copy).isEmpty());
        }
        for (const auto &local :
             QJsonArray{42, "invalid", "0.0.0.0", "::", "224.0.0.1", "255.255.255.255"}) {
            auto invalid = v;
            invalid.insert("localAddress", local);
            QVERIFY(!validateServer(invalid).isEmpty());
        }
        for (int port : {0, 65536, -1}) {
            auto copy = v;
            copy.insert(QStringLiteral("port"), port);
            QVERIFY(!validateServer(copy).isEmpty());
        }
        auto copy = v;
        copy.insert(QStringLiteral("passwordEnv"), QStringLiteral("HOME"));
        QVERIFY(!validateServer(copy).isEmpty());
        Store store;
        Servers servers(store);
        auto invalidLan = serverDefaults();
        invalidLan.insert("name", "Invalid LAN");
        invalidLan.insert("protocol", "lan");
        invalidLan.insert("localAddress", "127.0.0.1");
        QVERIFY(!servers.put("lan", invalidLan).isEmpty());
        for (int i = 0; i < 16; ++i) {
            copy = v;
            copy.insert(QStringLiteral("enabled"), false);
            QVERIFY(servers.put(QString::number(i), copy).isEmpty());
        }
        QVERIFY(!servers.put(QStringLiteral("overflow"), v).isEmpty());
        QVERIFY(!servers.send(sampleEvent()).isEmpty());
    }
    void multipleStreams()
    {
        QTcpServer a, b;
        QVERIFY(a.listen(QHostAddress::LocalHost, 0));
        QVERIFY(b.listen(QHostAddress::LocalHost, 0));
        Store store;
        QAbstractItemModelTester tester(&store,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("a"), source(a.serverPort())).isEmpty());
        QVERIFY(servers.put(QStringLiteral("b"), source(b.serverPort())).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return a.hasPendingConnections();
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return b.hasPendingConnections();
            },
            5000));
        // Configuration replacement can close an earlier pending connection.
        QTcpSocket *first = nullptr;
        while (a.hasPendingConnections()) {
            auto *candidate = a.nextPendingConnection();
            if (first) {
                first->deleteLater();
            }
            first = candidate;
        }
        auto *second = b.nextPendingConnection();
        QVERIFY(first && second);
        auto xml = encodeCot(sampleEvent(QStringLiteral("one")));
        first->write(xml.left(7));
        second->write(encodeCot(sampleEvent(QStringLiteral("two"))));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        first->write(xml.mid(7));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (2);
            },
            5000));
        auto update = sampleEvent(QStringLiteral("one"));
        update.latitude = 12;
        first->write(encodeCot(update));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(store.events().begin(), store.events().end(), [](const auto &e) {
                    return e.uid == QLatin1String("one") && e.latitude == 12;
                });
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("a")).contains(QStringLiteral("Connected"));
            },
            5000));
        QVERIFY(servers.send(sampleEvent(QStringLiteral("outgoing"))).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return first->bytesAvailable() > 0;
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return second->bytesAvailable() > 0;
            },
            5000));
        QVERIFY(first->readAll().contains("outgoing"));
        QVERIFY(second->readAll().contains("outgoing"));
        first->write("<event/>");
        first->write(encodeCot(sampleEvent(QStringLiteral("recovered"))));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (3);
            },
            5000));
        first->write(QByteArray(MaxMessageBytes + 1, 'x'));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("a")).contains(QStringLiteral("Invalid"));
            },
            5000));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return a.hasPendingConnections();
            },
            4000));
        auto *retry = a.nextPendingConnection();
        retry->write(encodeCot(sampleEvent(QStringLiteral("retry"))));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (4);
            },
            5000));
        retry->write("<event ");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("a")).contains(QStringLiteral("timed out"));
            },
            5000));
    }
    // A peer that stops reading fills the write backlog. SWAK drops and counts later events.
    void slowPeer()
    {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        auto entry = source(listener.serverPort());
        entry.insert(QStringLiteral("timeoutSeconds"), 120);
        QVERIFY(servers.put(QStringLiteral("slow"), entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return listener.hasPendingConnections() &&
                       servers.status(QStringLiteral("slow"))
                           .startsWith(QStringLiteral("Connected"));
            },
            5000));
        auto *peer = listener.nextPendingConnection();
        // Keep the data in the kernel, so the buffers of both sides fill.
        peer->setReadBufferSize(1);
        auto event = sampleEvent();
        event.detail =
            QStringLiteral("<remarks>%1</remarks>").arg(QString(40000, QLatin1Char('x')));
        QVERIFY2(QTest::qWaitFor(
                     [&] {
                         for (int i = 0; i < 16; ++i) {
                             static_cast<void>(servers.send(event));
                         }
                         return servers.status(QStringLiteral("slow"))
                                    .contains(QStringLiteral("Queue drops:")) &&
                                !servers.status(QStringLiteral("slow"))
                                     .contains(QStringLiteral("Queue drops: 0."));
                     },
                     30000),
                 qPrintable(servers.status(QStringLiteral("slow"))));
        // The stream stays open after the drops.
        QVERIFY(servers.status(QStringLiteral("slow")).startsWith(QStringLiteral("Connected")));
    }
    void flood()
    {
        QTcpServer listener;
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        QVERIFY(servers.put(QStringLiteral("load"), source(listener.serverPort())).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return listener.hasPendingConnections();
            },
            5000));
        auto *socket = listener.nextPendingConnection();
        QByteArray block;
        for (int i = 0; i < 12000; ++i) {
            auto e = sampleEvent(QString::number(i % 500));
            e.longitude = static_cast<double>(i % 180);
            block += encodeCot(e);
        }
        QElapsedTimer elapsed;
        elapsed.start();
        QSignalSpy updates(&store, &Store::changed);
        int beats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&beats] {
            ++beats;
        });
        heartbeat.start();
        QCOMPARE(socket->write(block), block.size());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.statistics()
                           .value(QStringLiteral("load"))
                           .toObject()
                           .value(QStringLiteral("received"))
                           .toInt() >= 12000;
            },
            20000));
        QCOMPARE(store.rowCount(), 500);
        QVERIFY(beats > 10);
        QVERIFY(updates.count() < 1200);
        qInfo("CoT load: 12000 messages in %lld ms, %d GUI heartbeat ticks",
              static_cast<long long>(elapsed.elapsed()),
              beats);
        servers.remove(QStringLiteral("load"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (socket->state()) == (QAbstractSocket::UnconnectedState);
            },
            5000));
    }
    void connectionDiagnostics()
    {
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        Store store;
        Servers servers(store);
        auto values = source(port);
        values.insert(QStringLiteral("retrySeconds"), 5);
        QVERIFY(servers.put(QStringLiteral("refused"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("refused"))
                    .contains(QStringLiteral("Connection refused"));
            },
            3000));
        const auto message = servers.status(QStringLiteral("refused"));
        QVERIFY(message.contains(QStringLiteral("TCP [127.0.0.1]:%1").arg(port)));
        QVERIFY(message.contains(QStringLiteral("Retry in 5 seconds")));
        QVERIFY(message.contains(QStringLiteral("Start the server")));
        QVERIFY(reservation.listen(QHostAddress::LocalHost, port));
        servers.reconnect();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("refused"))
                    .contains(QStringLiteral("Connected"));
            },
            3000));
        QVERIFY(!servers.status(QStringLiteral("refused"))
                     .contains(QStringLiteral("Connection refused")));
        QTcpServer mismatch;
        QVERIFY(mismatch.listen(QHostAddress::LocalHost, 0));
        values.insert(QStringLiteral("port"), mismatch.serverPort());
        values.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        QVERIFY(servers.put(QStringLiteral("tls-mismatch"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return mismatch.hasPendingConnections();
            },
            3000));
        auto *plain = mismatch.nextPendingConnection();
        plain->write("not TLS");
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls-mismatch"))
                    .contains(QStringLiteral("TLS handshake failed"));
            },
            3000));
        QVERIFY(
            servers.status(QStringLiteral("tls-mismatch")).contains(QStringLiteral("plain TCP")));
        // The reserved .invalid domain never resolves.
        values.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        values.insert(QStringLiteral("host"), QStringLiteral("swak-test.invalid"));
        QVERIFY(servers.put(QStringLiteral("unknown"), values).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("unknown"))
                    .contains(QStringLiteral("Host name not found"));
            },
            15000));
        // The entry check refuses a broadcast address.
        values.insert(QStringLiteral("host"), QStringLiteral("255.255.255.255"));
        QCOMPARE(servers.put(QStringLiteral("broadcast"), values),
                 QStringLiteral("Use a unicast address."));
    }
    void tlsCertificates()
    {
        qputenv("SWAK_COT_TEST_PASSWORD", "swak-test-only");
        qputenv("SWAK_COT_TEST_WRONG", "wrong");
        TlsHarness listener;
        QFile cert(QFINDTESTDATA("fixtures/map-test-cert.pem")),
            key(QFINDTESTDATA("fixtures/map-test-key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly));
        QVERIFY(key.open(QIODevice::ReadOnly));
        listener.certificate = QSslCertificate(cert.readAll());
        listener.key = QSslKey(key.readAll(), QSsl::Rsa);
        listener.payload = encodeCot(sampleEvent());
        QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
        Store store;
        Servers servers(store);
        auto v = source(listener.serverPort());
        v.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        v.insert("localAddress", "203.0.113.254");
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status("tls").contains("Cannot bind interface");
            },
            3000));
        v.insert("localAddress", "127.0.0.1");
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls"))
                    .contains(QStringLiteral("certificate validation failed"));
            },
            5000));
        QCOMPARE(store.rowCount(), 0);
        v.insert(QStringLiteral("ca"), cert.fileName());
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        listener.requireClient = true;
        store.clear();
        v.insert(QStringLiteral("certificate"), cert.fileName());
        v.insert(QStringLiteral("key"), key.fileName());
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        store.clear();
        v.insert(QStringLiteral("certificate"), QFINDTESTDATA("fixtures/cot-test-client.p12"));
        v.insert(QStringLiteral("key"), QString{});
        v.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_PASSWORD"));
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return (store.rowCount()) == (1);
            },
            5000));
        v.insert(QStringLiteral("passwordEnv"), QStringLiteral("SWAK_COT_TEST_WRONG"));
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls"))
                    .contains(QStringLiteral("credentials"));
            },
            5000));
        v.insert(QStringLiteral("certificate"), QStringLiteral("/missing/client.p12"));
        QVERIFY(servers.put(QStringLiteral("tls"), v).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers.status(QStringLiteral("tls"))
                    .contains(QStringLiteral("credentials"));
            },
            5000));
    }
    void pausedMotion()
    {
        TrackMotion motion;
        Event event;
        event.uid = QStringLiteral("paused");
        event.time = QDateTime::currentDateTimeUtc();
        motion.update(event);
        motion.setPaused(true);
        const auto frozen = motion.snapshot();
        QCOMPARE(frozen.size(), 1);
        event.longitude = 20;
        event.time = event.time.addSecs(1);
        motion.update(event);
        QCOMPARE(motion.snapshot().first().world, frozen.first().world);
        auto added = event;
        added.uid = QStringLiteral("new");
        motion.update(added);
        QCOMPARE(motion.snapshot().size(), 1);
        motion.setPaused(false);
        QCOMPARE(motion.snapshot().size(), 2);
        for (const auto &visual : motion.snapshot()) {
            QCOMPARE(visual.world, project(0, 20));
            QCOMPARE(visual.opacity, 1.0);
        }
        event.longitude = 21;
        event.time = event.time.addSecs(1);
        motion.update(event);
        QVERIFY(motion.snapshot().first().world.x() <= project(0, 21).x());
        motion.setPaused(true);
        motion.expire(event.uid);
        motion.setPaused(false);
        const auto expired = motion.snapshot();
        QVERIFY(std::any_of(expired.cbegin(), expired.cend(), [](const auto &item) {
            return item.expired;
        }));
        motion.setPaused(true);
        motion.clear();
        QVERIFY(motion.snapshot().isEmpty());
        motion.setPaused(false);
        QVERIFY(motion.snapshot().isEmpty());
    }
    void predictiveMotion()
    {
        TrackMotion motion;
        auto event = sampleEvent();
        motion.update(event, 1000);
        event.longitude = 0.001;
        event.time = event.time.addSecs(1);
        motion.update(event, 2000);
        const auto a = motion.snapshot(2600).first().world;
        const auto b = motion.snapshot(2900).first().world;
        QVERIFY(b.x() > a.x());
        const auto before = motion.snapshot(3000).first().world;
        event.longitude = 0.002;
        event.time = event.time.addSecs(1);
        motion.update(event, 3000);
        QCOMPARE(motion.snapshot(3000).first().world, before);
        QVERIFY(motion.snapshot(3600).first().world.x() > before.x());
        QCOMPARE(motion.snapshot(6000).first().world, motion.snapshot(7000).first().world);
        motion.expire(event.uid, 7000);
        QCOMPARE(motion.snapshot(7200).first().world, motion.snapshot(7300).first().world);
        auto values = motion.savePreferences();
        values.insert(QStringLiteral("predictionMs"), 0);
        motion.restorePreferences(values);
        QCOMPARE(motion.savePreferences().value(QStringLiteral("predictionMs")).toInt(), 0);
    }
    void motion()
    {
        TrackMotion motion;
        auto e = sampleEvent();
        motion.update(e, 1000);
        e.longitude = 0.5;
        motion.update(e, 2000);
        const auto start = motion.snapshot(2000).first();
        const auto halfway = motion.snapshot(2300).first();
        const auto end = motion.snapshot(2600).first();
        QCOMPARE(start.world.x(), 0.5);
        QVERIFY(halfway.world.x() > start.world.x());
        QVERIFY(halfway.world.x() < end.world.x());
        e.longitude = 0.6;
        motion.update(e, 2300);
        QCOMPARE(motion.snapshot(2300).first().world, halfway.world);
        e.longitude = 100;
        motion.update(e, 3000);
        QVERIFY(motion.snapshot(3300).isEmpty());
    }
    void dateLineMotion()
    {
        TrackMotion motion;
        auto e = sampleEvent();
        e.longitude = 179.9;
        motion.update(e, 1000);
        e.longitude = -179.9;
        motion.update(e, 2000);
        const auto half = motion.snapshot(2300).first();
        QVERIFY(half.world.x() < 0.01 || half.world.x() > 0.99);
        QCOMPARE(half.opacity, 1.0);
        e.longitude = 100;
        motion.update(e, 3000);
        e.longitude = 101;
        motion.update(e, 3100);
        QVERIFY(motion.snapshot(3300).isEmpty());
        QCOMPARE(motion.snapshot(3600).first().world, project(e.latitude, e.longitude));
        auto settings = motion.savePreferences();
        settings.insert(QStringLiteral("duration"), 0);
        motion.restorePreferences(settings);
        e.longitude = 101.1;
        motion.update(e, 4000);
        QCOMPARE(motion.snapshot(4000).first().world, project(e.latitude, e.longitude));
    }
    void staleMotion()
    {
        TrackMotion motion;
        auto e = sampleEvent();
        motion.update(e, 1000);
        motion.expire(e.uid, 2000);
        QVERIFY(motion.snapshot(2300).first().gray > 0);
        QCOMPARE(motion.snapshot(2600).first().gray, 1.0);
        QVERIFY(motion.snapshot(10000).first().expired);
        QVERIFY(motion.snapshot(14000).isEmpty());
        e.time = e.time.addSecs(1);
        motion.update(e, 10000);
        QVERIFY(!motion.snapshot(11000).first().expired);
        motion.clear();
        QVERIFY(motion.snapshot().isEmpty());
        motion.update(e, 1000);
        e.longitude = 179.9;
        motion.update(e, 2000);
        QVERIFY(motion.snapshot(2300).isEmpty());
        QCOMPARE(motion.snapshot(2600).first().opacity, 1.0);
    }
};
QTEST_MAIN(ServerTest)
#include "test_servers.moc"
