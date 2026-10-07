#include "core/quic_stream.h"

#include <QCoreApplication>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace swak {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("QuicStream", text);
}
// ALPN protocol list in wire format: one length byte, then the name.
constexpr std::array<unsigned char, 10> TakAlpn{9, 't', 'a', 'k', 's', 't', 'r', 'e', 'a', 'm'};
constexpr qsizetype MaxPending = 256 * 1024;

// OpenSSL fixes this callback signature, so the user pointer cannot be const.
// cppcheck-suppress constParameterCallback
int passwordCallback(char *buffer, int size, int, void *user)
{
    const auto *value = static_cast<const QByteArray *>(user);
    if (value->isEmpty() || value->size() >= size) {
        return 0;
    }
    std::copy(value->cbegin(), value->cend(), buffer);
    return static_cast<int>(value->size());
}
QString openSslError()
{
    const auto code = ERR_get_error();
    ERR_clear_error();
    if (!code) {
        return {};
    }
    std::array<char, 256> text{};
    ERR_error_string_n(code, text.data(), text.size());
    return QString::fromLatin1(text.data());
}
} // namespace

struct QuicStream::State
{
    State() = default;
    State(const State &) = delete;
    State &operator=(const State &) = delete;
    ~State()
    {
        // SSL_free also frees the BIO, which closes the socket.
        SSL_free(ssl);
        SSL_CTX_free(context);
    }
    SSL_CTX *context = nullptr;
    SSL *ssl = nullptr;
    QByteArray pending;
    QString error;
    bool connected = false;
    bool written = false;
    bool failed = false;
    void fail(const QString &message)
    {
        failed = true;
        error = message;
    }
    // Explains a failed handshake or transfer without addresses or secrets.
    QString describe(int code) const
    {
        if (ssl) {
            const long verify = SSL_get_verify_result(ssl);
            if (verify != X509_V_OK) {
                return tr("TLS certificate validation failed: %1.")
                    .arg(QString::fromLatin1(X509_verify_cert_error_string(verify)));
            }
        }
        if (code == SSL_ERROR_ZERO_RETURN) {
            return tr("The server closed the QUIC stream.");
        }
        const auto detail = openSslError();
        return detail.isEmpty() ? tr("The QUIC connection failed.")
                                : tr("The QUIC connection failed: %1.").arg(detail.left(200));
    }
};

// Tests and later diagnostics check QUIC support before they start a stream.
// cppcheck-suppress unusedFunction
bool quicAvailable()
{
    SSL_CTX *context = SSL_CTX_new(OSSL_QUIC_client_method());
    SSL_CTX_free(context);
    ERR_clear_error();
    return context != nullptr;
}
QuicStream::QuicStream() : state_(std::make_unique<State>()) {}
QuicStream::~QuicStream() = default;
QString QuicStream::start(const QHostAddress &address,
                          quint16 port,
                          const QString &serverName,
                          const TlsFiles &files,
                          const QHostAddress &local)
{
    auto &s = *state_;
    ERR_clear_error();
    s.context = SSL_CTX_new(OSSL_QUIC_client_method());
    if (!s.context) {
        return tr("This OpenSSL library has no QUIC client. QUIC needs OpenSSL 3.2 or later.");
    }
    SSL_CTX_set_verify(s.context, SSL_VERIFY_PEER, nullptr);
    if (!files.ca.isEmpty()) {
        BIO *bio = BIO_new_mem_buf(files.ca.constData(), static_cast<int>(files.ca.size()));
        STACK_OF(X509_INFO) *items = PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        int added = 0;
        auto *store = SSL_CTX_get_cert_store(s.context);
        for (int i = 0; items && i < sk_X509_INFO_num(items); ++i) {
            auto *item = sk_X509_INFO_value(items, i);
            if (item->x509 && X509_STORE_add_cert(store, item->x509) == 1) {
                ++added;
            }
        }
        sk_X509_INFO_pop_free(items, X509_INFO_free);
        if (!added) {
            return tr("The CA certificate file contains no certificate.");
        }
    } else if (SSL_CTX_set_default_verify_paths(s.context) != 1) {
        return tr("The system CA certificates cannot be loaded.");
    }
    if (!files.certificate.isEmpty()) {
        BIO *bio = BIO_new_mem_buf(files.certificate.constData(),
                                   static_cast<int>(files.certificate.size()));
        bool ok = false;
        if (files.pkcs12) {
            PKCS12 *archive = d2i_PKCS12_bio(bio, nullptr);
            EVP_PKEY *key = nullptr;
            X509 *certificate = nullptr;
            STACK_OF(X509) *chain = nullptr;
            ok = archive &&
                 PKCS12_parse(archive, files.password.constData(), &key, &certificate, &chain) ==
                     1 &&
                 SSL_CTX_use_certificate(s.context, certificate) == 1 &&
                 SSL_CTX_use_PrivateKey(s.context, key) == 1;
            while (ok && chain && sk_X509_num(chain) > 0) {
                // The context takes ownership of each chain certificate.
                ok = SSL_CTX_add_extra_chain_cert(s.context, sk_X509_shift(chain)) == 1;
            }
            sk_X509_pop_free(chain, X509_free);
            X509_free(certificate);
            EVP_PKEY_free(key);
            PKCS12_free(archive);
        } else {
            X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            ok = certificate && SSL_CTX_use_certificate(s.context, certificate) == 1;
            X509_free(certificate);
            while (ok) {
                X509 *extra = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
                if (!extra) {
                    break;
                }
                ok = SSL_CTX_add_extra_chain_cert(s.context, extra) == 1;
            }
            ERR_clear_error();
            BIO *keyBio =
                BIO_new_mem_buf(files.key.constData(), static_cast<int>(files.key.size()));
            QByteArray password = files.password;
            EVP_PKEY *key =
                ok ? PEM_read_bio_PrivateKey(keyBio, nullptr, passwordCallback, &password)
                   : nullptr;
            password.fill('\0');
            ok = key && SSL_CTX_use_PrivateKey(s.context, key) == 1;
            EVP_PKEY_free(key);
            BIO_free(keyBio);
        }
        BIO_free(bio);
        if (!ok || SSL_CTX_check_private_key(s.context) != 1) {
            ERR_clear_error();
            return tr("The client certificate or private key cannot be used.");
        }
    }
    s.ssl = SSL_new(s.context);
    if (!s.ssl || SSL_set_alpn_protos(s.ssl, TakAlpn.data(), TakAlpn.size()) != 0) {
        return tr("The QUIC connection cannot be prepared.");
    }
    const auto name = serverName.toUtf8();
    if (!QHostAddress(serverName).isNull()) {
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(s.ssl), name.constData()) != 1) {
            return tr("The server address cannot be checked.");
        }
    } else if (SSL_set_tlsext_host_name(s.ssl, name.constData()) != 1 ||
               SSL_set1_host(s.ssl, name.constData()) != 1) {
        return tr("The server name cannot be checked.");
    }
    const bool ipv6 = address.protocol() == QAbstractSocket::IPv6Protocol;
    const int family = ipv6 ? AF_INET6 : AF_INET;
    const int socket = BIO_socket(family, SOCK_DGRAM, IPPROTO_UDP, 0);
    if (socket < 0) {
        return tr("A UDP socket for QUIC cannot be created.");
    }
    const auto makeAddress = [ipv6, family](const QHostAddress &host, quint16 portNumber) {
        BIO_ADDR *result = BIO_ADDR_new();
        if (ipv6) {
            const auto raw = host.toIPv6Address();
            BIO_ADDR_rawmake(result, family, raw.c, sizeof(raw.c), htons(portNumber));
        } else {
            const quint32 raw = htonl(host.toIPv4Address());
            BIO_ADDR_rawmake(result, family, &raw, sizeof(raw), htons(portNumber));
        }
        return result;
    };
    BIO *bio = BIO_new(BIO_s_datagram());
    if (!bio) {
        BIO_closesocket(socket);
        return tr("The QUIC connection cannot be prepared.");
    }
    BIO_set_fd(bio, socket, BIO_CLOSE);
    SSL_set_bio(s.ssl, bio, bio);
    bool ok = true;
    if (!local.isNull()) {
        BIO_ADDR *bind = makeAddress(local, 0);
        ok = BIO_bind(socket, bind, 0) == 1;
        BIO_ADDR_free(bind);
    }
    BIO_ADDR *peer = makeAddress(address, port);
    ok = ok && BIO_connect(socket, peer, 0) == 1 && BIO_socket_nbio(socket, 1) == 1 &&
         SSL_set1_initial_peer_addr(s.ssl, peer) == 1 && SSL_set_blocking_mode(s.ssl, 0) == 1;
    BIO_ADDR_free(peer);
    if (!ok) {
        ERR_clear_error();
        return tr("The QUIC socket cannot reach the configured address.");
    }
    SSL_set_mode(s.ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_connect(s.ssl);
    ERR_clear_error();
    return {};
}
bool QuicStream::poll(QByteArray &received, qsizetype limit)
{
    auto &s = *state_;
    if (s.failed || !s.ssl) {
        return false;
    }
    SSL_handle_events(s.ssl);
    if (!s.connected) {
        const int result = SSL_connect(s.ssl);
        if (result != 1) {
            const int code = SSL_get_error(s.ssl, result);
            if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
                return true;
            }
            s.fail(s.describe(code));
            return false;
        }
        s.connected = true;
    }
    while (!s.pending.isEmpty()) {
        size_t written = 0;
        if (SSL_write_ex(
                s.ssl, s.pending.constData(), static_cast<size_t>(s.pending.size()), &written) ==
            1) {
            s.pending.remove(0, static_cast<qsizetype>(written));
            s.written = true;
            continue;
        }
        const int code = SSL_get_error(s.ssl, 0);
        if (code == SSL_ERROR_WANT_WRITE || code == SSL_ERROR_WANT_READ) {
            break;
        }
        s.fail(s.describe(code));
        return false;
    }
    // The first write opens the stream. Reading earlier would wait for a server stream.
    std::array<char, 16 * 1024> buffer{};
    while (s.written && received.size() < limit) {
        size_t count = 0;
        const auto wanted =
            std::min<size_t>(buffer.size(), static_cast<size_t>(limit - received.size()));
        if (SSL_read_ex(s.ssl, buffer.data(), wanted, &count) == 1) {
            received.append(buffer.data(), static_cast<qsizetype>(count));
            continue;
        }
        const int code = SSL_get_error(s.ssl, 0);
        if (code == SSL_ERROR_WANT_READ || code == SSL_ERROR_WANT_WRITE) {
            break;
        }
        s.fail(s.describe(code));
        return false;
    }
    return true;
}
bool QuicStream::write(const QByteArray &bytes)
{
    auto &s = *state_;
    if (s.failed || s.pending.size() + bytes.size() > MaxPending) {
        return false;
    }
    s.pending.append(bytes);
    return true;
}
bool QuicStream::connected() const
{
    return state_->connected;
}
qsizetype QuicStream::bytesToWrite() const
{
    return state_->pending.size();
}
const QString &QuicStream::error() const
{
    return state_->error;
}
} // namespace swak
