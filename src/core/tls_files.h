#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace swak {
// The TLS identity of a server entry, read from its configured files.
// The CoT stream and the HTTPS client both use it, so they present the same identity.
struct TlsFiles
{
    TlsFiles() = default;
    TlsFiles(const TlsFiles &) = default;
    TlsFiles &operator=(const TlsFiles &) = default;
    TlsFiles(TlsFiles &&) = default;
    TlsFiles &operator=(TlsFiles &&) = default;
    ~TlsFiles();
    // PEM CA certificates. Empty means the system CA store.
    QByteArray ca;
    // PEM certificate chain or PKCS12 archive. Empty means no client certificate.
    QByteArray certificate;
    // PEM private key for a PEM certificate.
    QByteArray key;
    // Key or PKCS12 password from the configured SWAK_COT_ environment variable.
    QByteArray password;
    bool pkcs12 = false;
    QString error;
};
// Reads the ca, certificate, key, and passwordEnv fields of a server entry. Each file is a
// regular file of at most 64 KiB. The password never enters the profile.
TlsFiles readTlsFiles(const QJsonObject &source);
// Reads only the CA file of an entry, for a connection without a client certificate, such as
// enrollment. An unreadable CA file sets error.
TlsFiles readTlsAuthority(const QJsonObject &source);
} // namespace swak
