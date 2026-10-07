#include "core/tls_files.h"

#include "core/files.h"

#include <QCoreApplication>

namespace swak {
namespace {
constexpr qsizetype MaxCredentialBytes = 64 * 1024;
QString tr(const char *text)
{
    return QCoreApplication::translate("TlsFiles", text);
}
} // namespace
TlsFiles::~TlsFiles()
{
    // Clear this copy of the password. Qt containers do not guarantee secure erasure of
    // every copy, as docs/security.md records.
    password.fill('\0');
}
TlsFiles readTlsFiles(const QJsonObject &source)
{
    TlsFiles files;
    const auto read = [&files](const QString &path, QByteArray &target) {
        if (path.isEmpty() || !files.error.isEmpty()) {
            return;
        }
        const auto input = readRegularFile(path, MaxCredentialBytes);
        if (!input.error.isEmpty()) {
            files.error = tr("A certificate or key file cannot be read.");
            return;
        }
        target = input.bytes;
    };
    read(source.value(QStringLiteral("ca")).toString(), files.ca);
    read(source.value(QStringLiteral("certificate")).toString(), files.certificate);
    read(source.value(QStringLiteral("key")).toString(), files.key);
    files.pkcs12 =
        !files.certificate.isEmpty() && source.value(QStringLiteral("key")).toString().isEmpty();
    const auto variable = source.value(QStringLiteral("passwordEnv")).toString().toLatin1();
    if (!variable.isEmpty()) {
        files.password = qgetenv(variable.constData());
        if (files.password.size() > 1024) {
            files.password.fill('\0');
            files.password.clear();
            files.error = tr("The certificate password is too long.");
        }
    }
    return files;
}
} // namespace swak
