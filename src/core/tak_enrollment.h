#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

namespace swak {
class NetworkClient;
struct NetworkReply;
struct NetworkRequest;
class Servers;

// One required subject entry, such as O=TAK, from /Marti/api/tls/config.
using SubjectEntry = QPair<QString, QString>;

struct EnrollmentKey
{
    QByteArray pem;
    QString error;
};
// Creates a new RSA 2048-bit private key as PEM. A non-empty password encrypts the PEM with
// AES-256-CBC.
EnrollmentKey generateEnrollmentKey(const QByteArray &password);
// Reads an existing PEM private key and returns it as PEM with the same password.
EnrollmentKey readEnrollmentKey(const QByteArray &pem, const QByteArray &password);
// Builds a PKCS#10 certificate request, signed with SHA-256, as base64 DER. The subject is
// CN=commonName followed by the entries, which is exactly what TAK Server checks.
QByteArray enrollmentRequest(const QByteArray &keyPem,
                             const QByteArray &password,
                             const QString &commonName,
                             const QList<SubjectEntry> &entries,
                             QString *error);
// Reads the certificateConfig XML reply. Returns false for other content.
bool readCertificateConfig(const QByteArray &xml, QList<SubjectEntry> *entries);
struct EnrollmentResult
{
    // PEM certificates. The signed certificate comes first.
    QByteArray certificate;
    QByteArray authorities;
    QString error;
};
// Reads the JSON signClient/v2 reply and checks that the certificate matches the key.
EnrollmentResult
readEnrollmentReply(const QByteArray &json, const QByteArray &keyPem, const QByteArray &password);

// Enrolls a TLS server entry for a client certificate with TAK Server.
//
// The flow uses HTTP Basic authentication on the enrollment port, normally 8446:
// GET /Marti/api/tls/config, then POST /Marti/api/tls/signClient/v2 with the request.
// On success, it writes the key and certificates under the application data directory with
// owner-only permissions and updates the entry. It never stores the password.
class TakEnrollment final : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString serverId;
        QString username;
        QByteArray password;
        int port = 8446;
        // Empty generates a new key. Otherwise the existing PEM key file.
        QString keyFile;
        QString clientUid;
    };
    TakEnrollment(Servers &servers, NetworkClient &network, QObject *parent = nullptr);
    ~TakEnrollment() override;
    TakEnrollment(const TakEnrollment &) = delete;
    TakEnrollment &operator=(const TakEnrollment &) = delete;
    // Starts one enrollment. Returns an error when another one runs or the input is invalid.
    QString start(Options options);
    void cancel();
    bool running() const
    {
        return running_;
    }
    // The directory that holds enrolled credentials. Tests can redirect it.
    void setDirectory(const QString &directory);
signals:
    void progress(const QString &message);
    // An empty error means success.
    void finished(const QString &error);

private:
    void requestCertificate(const QList<SubjectEntry> &entries);
    void store(const EnrollmentResult &result);
    void finish(const QString &error);
    QByteArray authorization() const;
    // A request to the enrollment port of the selected entry, with its credentials and CA.
    NetworkRequest serviceRequest(const QString &path) const;
    // The error of an enrollment reply, or an empty string for HTTP 200. statusText names
    // other statuses with %1.
    static QString replyError(const NetworkReply &reply, const QString &statusText);
    Servers &servers_;
    NetworkClient &network_;
    Options options_;
    QByteArray keyPem_;
    QByteArray keyPassword_;
    QString directory_;
    quint64 request_ = 0;
    bool running_ = false;
};
} // namespace swak
