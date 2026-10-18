#pragma once
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTest>

#include <functional>
#include <memory>
#include <utility>

// This server and its private key are test fixtures only.
// An HTTPS server for client tests. It reads one request and answers through respond.
class HttpsHarness final : public QTcpServer
{
public:
    QSslCertificate certificate;
    QSslKey key;
    bool requireClient = false;
    bool hold = false;
    QList<QByteArray> requests;
    // Requests that arrived while hold was set. releaseHeld answers them later.
    QList<std::pair<QPointer<QSslSocket>, QByteArray>> held;
    int clientCertificates = 0;
    std::function<QByteArray(const QByteArray &)> respond = [](const QByteArray &) {
        const QByteArray body = "{\"ok\":true}";
        return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: ") +
               QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    };
    bool load()
    {
        QFile cert(QFINDTESTDATA("fixtures/map-test-cert.pem")),
            secret(QFINDTESTDATA("fixtures/map-test-key.pem"));
        if (!cert.open(QIODevice::ReadOnly) || !secret.open(QIODevice::ReadOnly)) {
            return false;
        }
        certificate = QSslCertificate(cert.readAll());
        key = QSslKey(secret.readAll(), QSsl::Rsa);
        return !certificate.isNull() && !key.isNull() && listen(QHostAddress::LocalHost, 0);
    }
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
        auto input = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, input] {
            input->append(socket->readAll());
            const auto end = input->indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            static const QRegularExpression length(QStringLiteral("(?i)content-length: *(\\d+)"));
            const auto match = length.match(QString::fromLatin1(input->left(end)));
            const auto expected = end + 4 + (match.hasMatch() ? match.captured(1).toInt() : 0);
            if (input->size() < expected) {
                return;
            }
            requests.append(*input);
            if (!socket->peerCertificate().isNull()) {
                ++clientCertificates;
            }
            if (!hold) {
                socket->write(respond(*input));
                socket->disconnectFromHost();
            } else {
                held.append({QPointer<QSslSocket>(socket), *input});
            }
        });
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }
    // Answers the held requests with the current respond function.
    void releaseHeld()
    {
        for (const auto &[socket, input] : std::exchange(held, {})) {
            if (socket) {
                socket->write(respond(input));
                socket->disconnectFromHost();
            }
        }
    }
};
// Makes an HTTP reply with a status code and a body.
inline QByteArray httpReply(int status, const QByteArray &body)
{
    return QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
           " X\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}
