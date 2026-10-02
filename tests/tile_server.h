#pragma once
#include <QTcpServer>
#include <memory>
#include <QSslSocket>
#include <QSslKey>
#include <QFile>
#include <QImage>
#include <QBuffer>
#include <QSet>

// This server and its private key are test fixtures only.
class TileServer final : public QTcpServer {
public:
    QSslCertificate certificate;
    QSslKey key;
    QByteArray body;
    QByteArray headers = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: max-age=604800\r\n";
    QList<QByteArray> requests;
    bool hold = false;
    int failFirst = 0;
    QSet<int> holdRequests;
    QByteArray failureHeaders = "HTTP/1.1 404 Not Found\r\n";
    TileServer()
    {
        QImage image(256, 256, QImage::Format_RGB32);
        image.fill(QColor("#eb426a"));
        QBuffer buffer(&body);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    bool load(const QString &path)
    {
        QFile cert(path + QStringLiteral("/map-test-cert.pem"));
        QFile secret(path + QStringLiteral("/map-test-key.pem"));
        if (!cert.open(QIODevice::ReadOnly) || !secret.open(QIODevice::ReadOnly)) return false;
        certificate = QSslCertificate(cert.readAll());
        key = QSslKey(secret.readAll(), QSsl::Rsa);
        return !certificate.isNull() && !key.isNull();
    }
protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::errorOccurred, socket, [socket] { socket->disconnectFromHost(); });
        auto input = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::readyRead, this, [this, socket, input] {
            input->append(socket->readAll());
            if (!input->contains("\r\n\r\n")) return;
            requests.append(*input);
            if (hold || holdRequests.contains(static_cast<int>(requests.size()))) return;
            const auto response = requests.size() <= failFirst ? failureHeaders : headers;
            socket->write(response + "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
        socket->startServerEncryption();
    }
};
