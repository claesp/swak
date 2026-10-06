#pragma once
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>

#include <functional>
#include <memory>
#include <utility>

// This server and its private key are test fixtures only.
class TileServer final : public QTcpServer
{
public:
    QSslCertificate certificate;
    QSslKey key;
    QByteArray body;
    QByteArray headers =
        "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: max-age=604800\r\n";
    QList<QByteArray> requests;
    bool hold = false;
    int failFirst = 0;
    QSet<int> holdRequests;
    QByteArray failureHeaders = "HTTP/1.1 404 Not Found\r\n";
    // Optional routing. It receives the request head and returns status headers and a body.
    std::function<std::pair<QByteArray, QByteArray>(const QByteArray &)> respond;
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
        if (!cert.open(QIODevice::ReadOnly) || !secret.open(QIODevice::ReadOnly)) {
            return false;
        }
        certificate = QSslCertificate(cert.readAll());
        key = QSslKey(secret.readAll(), QSsl::Rsa);
        return !certificate.isNull() && !key.isNull();
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) {
            socket->deleteLater();
            return;
        }
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::errorOccurred, socket, [socket] {
            socket->disconnectFromHost();
        });
        auto input = std::make_shared<QByteArray>();
        auto answered = std::make_shared<bool>(false);
        connect(socket, &QSslSocket::readyRead, this, [this, socket, input, answered] {
            input->append(socket->readAll());
            if (*answered || !input->contains("\r\n\r\n")) {
                return;
            }
            *answered = true;
            requests.append(*input);
            if (hold || holdRequests.contains(static_cast<int>(requests.size()))) {
                return;
            }
            auto [response, content] =
                respond ? respond(*input)
                        : std::pair{requests.size() <= failFirst ? failureHeaders : headers, body};
            socket->write(response + "Content-Length: " + QByteArray::number(content.size()) +
                          "\r\nConnection: close\r\n\r\n" + content);
            socket->disconnectFromHost();
        });
        socket->startServerEncryption();
    }
};
