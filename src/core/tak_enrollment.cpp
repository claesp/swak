#include "core/tak_enrollment.h"

#include "core/files.h"
#include "core/network_client.h"
#include "core/servers.h"
#include "core/tls_files.h"
#include "swak/version.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <memory>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace swak {
namespace {
constexpr qsizetype MaxKeyBytes = 64 * 1024;
constexpr qsizetype MaxReplyBytes = 256 * 1024;
constexpr int MaxSubjectEntries = 16;
constexpr int MaxAuthorities = 8;

QString tr(const char *text)
{
    return QCoreApplication::translate("TakEnrollment", text);
}
struct BioDelete
{
    void operator()(BIO *bio) const
    {
        BIO_free(bio);
    }
};
struct KeyDelete
{
    void operator()(EVP_PKEY *key) const
    {
        EVP_PKEY_free(key);
    }
};
struct RequestDelete
{
    void operator()(X509_REQ *request) const
    {
        X509_REQ_free(request);
    }
};
struct CertificateDelete
{
    void operator()(X509 *certificate) const
    {
        X509_free(certificate);
    }
};
using Bio = std::unique_ptr<BIO, BioDelete>;
using Key = std::unique_ptr<EVP_PKEY, KeyDelete>;

Bio memoryBio(const QByteArray &bytes)
{
    return Bio(BIO_new_mem_buf(bytes.constData(), static_cast<int>(bytes.size())));
}
QByteArray bioBytes(BIO *bio)
{
    char *data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return size > 0 && data ? QByteArray(data, static_cast<qsizetype>(size)) : QByteArray{};
}
// The password callback reads a null-terminated password. An empty password fails at once,
// so OpenSSL never asks a terminal.
Key readKey(const QByteArray &pem, const QByteArray &password)
{
    if (pem.isEmpty() || pem.size() > MaxKeyBytes) {
        return nullptr;
    }
    auto bio = memoryBio(pem);
    QByteArray secret = password;
    Key key(PEM_read_bio_PrivateKey(
        bio.get(),
        nullptr,
        [](char *buffer, int size, int, void *user) -> int {
            const auto *value = static_cast<const QByteArray *>(user);
            if (value->isEmpty() || value->size() >= size) {
                return 0;
            }
            std::copy(value->cbegin(), value->cend(), buffer);
            return static_cast<int>(value->size());
        },
        &secret));
    secret.fill('\0');
    return key;
}
QByteArray writeKey(EVP_PKEY *key, const QByteArray &password)
{
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio) {
        return {};
    }
    const bool encrypted = !password.isEmpty();
    QByteArray secret = password;
    const int written = PEM_write_bio_PrivateKey(
        bio.get(),
        key,
        encrypted ? EVP_aes_256_cbc() : nullptr,
        encrypted ? reinterpret_cast<unsigned char *>(secret.data()) : nullptr,
        encrypted ? static_cast<int>(secret.size()) : 0,
        nullptr,
        nullptr);
    secret.fill('\0');
    return written == 1 ? bioBytes(bio.get()) : QByteArray{};
}
// TAK returns base64 DER without PEM lines. Rebuild PEM with 64-character lines.
QByteArray pemCertificate(const QString &base64)
{
    // TAK Server wraps the base64 text at 64 characters. Remove the line breaks first.
    auto text = base64.toLatin1();
    text.removeIf([](char ch) {
        return ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t';
    });
    const auto der = QByteArray::fromBase64(
        text, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (der.isEmpty() || der.size() > MaxKeyBytes) {
        return {};
    }
    const auto *cursor = reinterpret_cast<const unsigned char *>(der.constData());
    std::unique_ptr<X509, CertificateDelete> certificate(
        d2i_X509(nullptr, &cursor, static_cast<long>(der.size())));
    if (!certificate) {
        return {};
    }
    Bio bio(BIO_new(BIO_s_mem()));
    if (!bio || PEM_write_bio_X509(bio.get(), certificate.get()) != 1) {
        return {};
    }
    return bioBytes(bio.get());
}
bool writePrivateFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return false;
    }
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
} // namespace

EnrollmentKey generateEnrollmentKey(const QByteArray &password)
{
    Key key(EVP_RSA_gen(2048));
    if (!key) {
        return {{}, tr("A new private key cannot be created.")};
    }
    auto pem = writeKey(key.get(), password);
    if (pem.isEmpty()) {
        return {{}, tr("The new private key cannot be written.")};
    }
    return {pem, {}};
}
EnrollmentKey readEnrollmentKey(const QByteArray &pem, const QByteArray &password)
{
    const auto key = readKey(pem, password);
    if (!key) {
        return {{},
                tr("The private key cannot be read. Check the file and its password variable.")};
    }
    if (EVP_PKEY_get_bits(key.get()) < 2048 && EVP_PKEY_get_base_id(key.get()) == EVP_PKEY_RSA) {
        return {{}, tr("Use an RSA key with at least 2048 bits, or an EC key.")};
    }
    return {pem, {}};
}
QByteArray enrollmentRequest(const QByteArray &keyPem,
                             const QByteArray &password,
                             const QString &commonName,
                             const QList<SubjectEntry> &entries,
                             QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return QByteArray{};
    };
    const auto key = readKey(keyPem, password);
    if (!key) {
        return fail(tr("The private key cannot be read."));
    }
    std::unique_ptr<X509_REQ, RequestDelete> request(X509_REQ_new());
    if (!request || X509_REQ_set_version(request.get(), 0) != 1 ||
        X509_REQ_set_pubkey(request.get(), key.get()) != 1) {
        return fail(tr("The certificate request cannot be created."));
    }
    auto *name = X509_REQ_get_subject_name(request.get());
    const auto add = [name](const QString &field, const QString &value) {
        const auto field8 = field.toUtf8();
        const auto value8 = value.toUtf8();
        return X509_NAME_add_entry_by_txt(
                   name,
                   field8.constData(),
                   MBSTRING_UTF8,
                   reinterpret_cast<const unsigned char *>(value8.constData()),
                   static_cast<int>(value8.size()),
                   -1,
                   0) == 1;
    };
    bool ok = add(QStringLiteral("CN"), commonName);
    for (const auto &[field, value] : entries) {
        ok = ok && add(field, value);
    }
    if (!ok || X509_REQ_sign(request.get(), key.get(), EVP_sha256()) <= 0) {
        return fail(tr("The certificate request cannot be signed. Check the subject entries."));
    }
    const int size = i2d_X509_REQ(request.get(), nullptr);
    if (size <= 0 || size > MaxKeyBytes) {
        return fail(tr("The certificate request cannot be encoded."));
    }
    QByteArray der(size, Qt::Uninitialized);
    auto *cursor = reinterpret_cast<unsigned char *>(der.data());
    i2d_X509_REQ(request.get(), &cursor);
    return der.toBase64();
}
bool readCertificateConfig(const QByteArray &xml, QList<SubjectEntry> *entries)
{
    QXmlStreamReader reader(xml);
    reader.setEntityExpansionLimit(0);
    bool root = false;
    QList<SubjectEntry> result;
    static const QRegularExpression field(QStringLiteral("\\A[A-Za-z]{1,16}\\z"));
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            return false;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        if (reader.name() == u"certificateConfig") {
            root = true;
        } else if (reader.name() == u"nameEntry") {
            const auto name = reader.attributes().value(u"name").toString();
            const auto value = reader.attributes().value(u"value").toString();
            if (!field.match(name).hasMatch() || value.isEmpty() || value.size() > 64 ||
                result.size() >= MaxSubjectEntries ||
                name.compare(u"CN", Qt::CaseInsensitive) == 0) {
                return false;
            }
            result.append({name, value});
        }
    }
    if (reader.hasError() || !root) {
        return false;
    }
    *entries = result;
    return true;
}
EnrollmentResult
readEnrollmentReply(const QByteArray &json, const QByteArray &keyPem, const QByteArray &password)
{
    EnrollmentResult result;
    const auto object = QJsonDocument::fromJson(json).object();
    result.certificate = pemCertificate(object.value(QStringLiteral("signedCert")).toString());
    if (result.certificate.isEmpty()) {
        result.error = tr("The server did not return a valid signed certificate.");
        return result;
    }
    for (int i = 0; i < MaxAuthorities; ++i) {
        const auto value = object.value(QStringLiteral("ca%1").arg(i));
        if (value.isUndefined()) {
            break;
        }
        const auto pem = pemCertificate(value.toString());
        if (pem.isEmpty()) {
            result.error = tr("The server returned an invalid CA certificate.");
            return result;
        }
        result.authorities += pem;
    }
    // The signed certificate must carry the public part of the private key.
    auto bio = memoryBio(result.certificate);
    std::unique_ptr<X509, CertificateDelete> certificate(
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    const auto key = readKey(keyPem, password);
    if (!certificate || !key || X509_check_private_key(certificate.get(), key.get()) != 1) {
        result.error = tr("The signed certificate does not match the private key.");
    }
    return result;
}

TakEnrollment::TakEnrollment(Servers &servers, NetworkClient &network, QObject *parent)
    : QObject(parent), servers_(servers), network_(network),
      directory_(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                 QStringLiteral("/certificates"))
{
}
TakEnrollment::~TakEnrollment()
{
    // Stop the request without signals. Receivers can already be gone.
    network_.cancel(request_);
    options_.password.fill('\0');
    keyPassword_.fill('\0');
    keyPem_.fill('\0');
}
void TakEnrollment::setDirectory(const QString &directory)
{
    directory_ = directory;
}
QByteArray TakEnrollment::authorization() const
{
    return "Basic " + (options_.username.toUtf8() + ':' + options_.password).toBase64();
}
QString TakEnrollment::start(Options options)
{
    if (running_) {
        return tr("An enrollment is already running.");
    }
    const auto entry = servers_.sources().value(options.serverId).toObject();
    if (entry.value(QStringLiteral("protocol")).toString() != u"tls" ||
        !validateServer(entry).isEmpty()) {
        return tr("Select a valid TLS server entry.");
    }
    if (options.username.isEmpty() || options.username.size() > 128 ||
        options.username.contains(QLatin1Char(':')) ||
        std::any_of(options.username.cbegin(), options.username.cend(), [](QChar ch) {
            return ch.category() == QChar::Other_Control;
        })) {
        return tr("Enter a username without a colon, up to 128 characters.");
    }
    if (options.password.isEmpty() || options.password.size() > 1024 ||
        options.password.contains('\r') || options.password.contains('\n')) {
        return tr("Enter the password of the TAK account.");
    }
    if (options.port < 1 || options.port > 65535) {
        return tr("Use an enrollment port from 1 through 65535.");
    }
    // The key password comes from the entry's password variable, like the CoT stream.
    const auto variable = entry.value(QStringLiteral("passwordEnv")).toString().toLatin1();
    keyPassword_ = variable.isEmpty() ? QByteArray{} : qgetenv(variable.constData());
    EnrollmentKey key;
    if (options.keyFile.isEmpty()) {
        key = generateEnrollmentKey(keyPassword_);
    } else {
        const auto input = readRegularFile(options.keyFile, MaxKeyBytes);
        key = input.error.isEmpty() ? readEnrollmentKey(input.bytes, keyPassword_)
                                    : EnrollmentKey{{}, tr("The private key file cannot be read.")};
    }
    if (!key.error.isEmpty()) {
        keyPassword_.fill('\0');
        return key.error;
    }
    keyPem_ = key.pem;
    options_ = std::move(options);
    running_ = true;
    emit progress(tr("Reading the certificate settings of the server…"));
    NetworkRequest request;
    const auto host = entry.value(QStringLiteral("host")).toString();
    request.url.setScheme(QStringLiteral("https"));
    request.url.setHost(host);
    request.url.setPort(options_.port);
    request.url.setPath(QStringLiteral("/Marti/api/tls/config"));
    request.expectedHost = host;
    request.expectedPort = options_.port;
    request.maxBytes = MaxReplyBytes;
    request.headers = {{"Authorization", authorization()}, {"Accept", "application/xml"}};
    // The enrollment port needs no client certificate. It uses only the CA of the entry.
    auto files = readTlsFiles(entry);
    request.tls.ca = files.ca;
    request_ = network_.send(std::move(request), this, [this](const NetworkReply &reply) {
        request_ = 0;
        if (!reply.error.isEmpty()) {
            finish(reply.error);
        } else if (reply.status == 401 || reply.status == 403) {
            finish(tr("The server rejected the username or password."));
        } else if (reply.status != 200) {
            finish(tr("The enrollment service answered with HTTP status %1.").arg(reply.status));
        } else {
            QList<SubjectEntry> entries;
            if (!readCertificateConfig(reply.body, &entries)) {
                finish(tr("The server sent invalid certificate settings."));
            } else {
                requestCertificate(entries);
            }
        }
    });
    return {};
}
void TakEnrollment::requestCertificate(const QList<SubjectEntry> &entries)
{
    QString error;
    const auto csr = enrollmentRequest(keyPem_, keyPassword_, options_.username, entries, &error);
    if (csr.isEmpty()) {
        finish(error);
        return;
    }
    emit progress(tr("Asking the server to sign the certificate…"));
    const auto entry = servers_.sources().value(options_.serverId).toObject();
    const auto host = entry.value(QStringLiteral("host")).toString();
    NetworkRequest request;
    request.url.setScheme(QStringLiteral("https"));
    request.url.setHost(host);
    request.url.setPort(options_.port);
    request.url.setPath(QStringLiteral("/Marti/api/tls/signClient/v2"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("clientUid"), options_.clientUid.left(128));
    query.addQueryItem(QStringLiteral("version"), QStringLiteral("SWAK " SWAK_VERSION));
    request.url.setQuery(query);
    request.method = "POST";
    request.body = csr;
    request.expectedHost = host;
    request.expectedPort = options_.port;
    request.maxBytes = MaxReplyBytes;
    request.headers = {{"Authorization", authorization()},
                       {"Accept", "application/json"},
                       {"Content-Type", "text/plain"}};
    request.tls.ca = readTlsFiles(entry).ca;
    request_ = network_.send(std::move(request), this, [this](const NetworkReply &reply) {
        request_ = 0;
        if (!reply.error.isEmpty()) {
            finish(reply.error);
        } else if (reply.status == 401 || reply.status == 403) {
            finish(tr("The server rejected the username or password."));
        } else if (reply.status != 200) {
            finish(tr("The server did not sign the certificate. HTTP status %1. Check the "
                      "username and the required certificate subject.")
                       .arg(reply.status));
        } else {
            store(readEnrollmentReply(reply.body, keyPem_, keyPassword_));
        }
    });
}
void TakEnrollment::store(const EnrollmentResult &result)
{
    if (!result.error.isEmpty()) {
        finish(result.error);
        return;
    }
    // A private directory protects the files while they are written.
    const auto folder = QDir(directory_).filePath(QString(options_.serverId).replace('/', '_'));
    if (!QDir().mkpath(folder) ||
        !QFile::setPermissions(
            folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        finish(tr("The certificate directory cannot be created."));
        return;
    }
    auto entry = servers_.sources().value(options_.serverId).toObject();
    const auto certificatePath = QDir(folder).filePath(QStringLiteral("client.pem"));
    const auto caPath = QDir(folder).filePath(QStringLiteral("ca.pem"));
    auto keyPath = options_.keyFile;
    if (keyPath.isEmpty()) {
        keyPath = QDir(folder).filePath(QStringLiteral("client.key"));
        if (!writePrivateFile(keyPath, keyPem_)) {
            finish(tr("The private key cannot be saved."));
            return;
        }
    }
    if (!writePrivateFile(certificatePath, result.certificate + result.authorities) ||
        (!result.authorities.isEmpty() && !writePrivateFile(caPath, result.authorities))) {
        finish(tr("The certificates cannot be saved."));
        return;
    }
    entry.insert(QStringLiteral("certificate"), certificatePath);
    entry.insert(QStringLiteral("key"), keyPath);
    if (entry.value(QStringLiteral("ca")).toString().isEmpty() && !result.authorities.isEmpty()) {
        entry.insert(QStringLiteral("ca"), caPath);
    }
    const auto error = servers_.put(options_.serverId, entry);
    if (!error.isEmpty()) {
        finish(error);
        return;
    }
    finish({});
}
void TakEnrollment::cancel()
{
    network_.cancel(request_);
    request_ = 0;
    if (running_) {
        finish(tr("Enrollment cancelled."));
    }
}
void TakEnrollment::finish(const QString &error)
{
    running_ = false;
    options_.password.fill('\0');
    options_.password.clear();
    keyPassword_.fill('\0');
    keyPassword_.clear();
    keyPem_.fill('\0');
    keyPem_.clear();
    emit finished(error);
}
} // namespace swak
