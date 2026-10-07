#pragma once
#include "core/tls_files.h"

#include <QByteArray>
#include <QHostAddress>
#include <QString>

#include <memory>

namespace swak {
// A CoT stream over QUIC to the TAK Server QUIC input, normally port 8090.
//
// TAK Server uses the ALPN name takstream and one client-initiated bidirectional stream.
// The class uses the OpenSSL 3.5 QUIC client in non-blocking mode. The owner calls poll()
// often, for example every few milliseconds, to drive timers, reads, and writes.
class QuicStream final
{
public:
    QuicStream();
    ~QuicStream();
    QuicStream(const QuicStream &) = delete;
    QuicStream &operator=(const QuicStream &) = delete;
    // Starts the handshake. serverName is the host name or address that the certificate must
    // match. Returns an error when the start fails at once.
    QString start(const QHostAddress &address,
                  quint16 port,
                  const QString &serverName,
                  const TlsFiles &files,
                  const QHostAddress &local = {});
    // Advances the connection and appends at most limit received bytes to received.
    // Returns false after an error or a close. error() then gives the reason.
    bool poll(QByteArray &received, qsizetype limit);
    // Queues bytes for the stream. The write starts after the handshake.
    bool write(const QByteArray &bytes);
    bool connected() const;
    qsizetype bytesToWrite() const;
    const QString &error() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
// True when the OpenSSL library at run time supports a QUIC client.
bool quicAvailable();
} // namespace swak
