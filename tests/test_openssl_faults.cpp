// Makes OpenSSL allocations fail one at a time and checks each enrollment function. OpenSSL lets
// a program replace its memory functions only before the first allocation, so this test has its
// own executable and installs them in main.
//
// The test also replaces the random source with a seeded generator. Key generation and
// signatures then make the same allocations and results in each run, so every successful run
// must give exactly the reference result.
#define OPENSSL_SUPPRESS_DEPRECATED
#include "core/tak_enrollment.h"

#include <QCoreApplication>
#include <QSet>
#include <QStringList>
#include <QTest>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

using namespace swak;
namespace {
std::atomic<long> allocations{0};
// The allocation that fails, counted from the start of a run. Zero means none.
std::atomic<long> failAt{0};
// Allocations at this source file and line stay successful, because OpenSSL crashes when they
// fail.
std::atomic<const char *> sparedFile{nullptr};
std::atomic<int> sparedLine{0};
std::atomic<long> spared{0};
std::uint64_t randomState = 0;

bool failNow(const char *file, int line)
{
    const auto count = ++allocations;
    if (count != failAt.load()) {
        return false;
    }
    const char *spare = sparedFile.load();
    if (spare && file && std::strstr(file, spare) && line == sparedLine.load()) {
        ++spared;
        return false;
    }
    return true;
}
void *testMalloc(size_t size, const char *file, int line)
{
    return failNow(file, line) ? nullptr : std::malloc(size);
}
void *testRealloc(void *pointer, size_t size, const char *file, int line)
{
    return size > 0 && failNow(file, line) ? nullptr : std::realloc(pointer, size);
}
void testFree(void *pointer, const char *, int)
{
    std::free(pointer);
}
// SplitMix64. Tests only need repeatable bytes, not secure ones.
int seededBytes(unsigned char *out, int count)
{
    for (int i = 0; i < count; ++i) {
        randomState += 0x9E3779B97F4A7C15ULL;
        auto value = randomState;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        out[i] = static_cast<unsigned char>(value ^ (value >> 31));
    }
    return 1;
}
int seededStatus()
{
    return 1;
}
RAND_METHOD seededRandom{nullptr, seededBytes, nullptr, nullptr, seededBytes, seededStatus};

// One run of a function under test: an output on success, or an error.
struct Outcome
{
    QByteArray output;
    QString error;
};
// Starts a run with a fixed random state and an optional failing allocation.
void begin(long failure)
{
    ERR_clear_error();
    randomState = 1;
    allocations = 0;
    failAt = failure;
}
long end()
{
    failAt = 0;
    ERR_clear_error();
    return allocations.load();
}
// The results of a sweep.
struct Sweep
{
    QSet<QString> errors;
    QStringList changed;
    QStringList unexpected;
};
// Fails each allocation in turn, or each allocation at the given stride. Each run must fail
// with one of the expected errors or return the reference output. A leak on a failure path
// shows in the leak check of a sanitizer build when the test ends.
Sweep sweep(const std::function<Outcome()> &run,
            const QByteArray &reference,
            const QSet<QString> &expected,
            long first = 1,
            long stride = 1)
{
    Sweep result;
    for (long failure = first;; failure += stride) {
        begin(failure);
        const auto outcome = run();
        const auto count = end();
        const auto name = QString::number(failure);
        if (outcome.error.isEmpty()) {
            if (outcome.output != reference) {
                result.changed.append(name);
            }
        } else {
            result.errors.insert(outcome.error);
            if (!expected.contains(outcome.error)) {
                result.unexpected.append(name + QStringLiteral(": ") + outcome.error);
            }
        }
        // The run made fewer allocations than the failure number, so no fault happened.
        if (count < failure) {
            return result;
        }
    }
}
// Makes a self-signed certificate for a key, as base64 DER.
QByteArray certificateFor(const QByteArray &keyPem, const QByteArray &password)
{
    BIO *bio = BIO_new_mem_buf(keyPem.constData(), static_cast<int>(keyPem.size()));
    EVP_PKEY *key =
        PEM_read_bio_PrivateKey(bio, nullptr, nullptr, const_cast<char *>(password.constData()));
    BIO_free(bio);
    X509 *certificate = X509_new();
    QByteArray result;
    if (key && certificate) {
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 7);
        X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate),
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("alice"),
                                   -1,
                                   -1,
                                   0);
        X509_set_issuer_name(certificate, X509_get_subject_name(certificate));
        X509_gmtime_adj(X509_getm_notBefore(certificate), -60);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
        X509_set_pubkey(certificate, key);
        if (X509_sign(certificate, key, EVP_sha256()) > 0) {
            const int size = i2d_X509(certificate, nullptr);
            QByteArray der(size, '\0');
            auto *cursor = reinterpret_cast<unsigned char *>(der.data());
            i2d_X509(certificate, &cursor);
            result = der.toBase64();
        }
    }
    X509_free(certificate);
    EVP_PKEY_free(key);
    return result;
}
} // namespace

class OpenSslFaultTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // Warm up OpenSSL, so that one-time initialization does not take part in the sweeps.
        begin(0);
        QVERIFY(generateEnrollmentKey("warm-up").error.isEmpty());
        end();
        key_ = generateEnrollmentKey(Password).pem;
        QVERIFY(!key_.isEmpty());
    }
    void generateKey()
    {
        // The reference run. The same seed gives the same key.
        begin(0);
        const auto reference = generateEnrollmentKey(Password);
        const auto total = end();
        QVERIFY(reference.error.isEmpty());
        begin(0);
        QCOMPARE(generateEnrollmentKey(Password).pem, reference.pem);
        end();
        // Count the allocations of key generation alone. Writing the PEM makes the rest.
        begin(0);
        EVP_PKEY_free(EVP_RSA_gen(2048));
        const auto generation = end();
        QVERIFY(generation > 0 && generation < total);
        const auto run = [] {
            const auto key = generateEnrollmentKey(Password);
            return Outcome{key.pem, key.error};
        };
        const QString created = QStringLiteral("A new private key cannot be created.");
        const QString written = QStringLiteral("The new private key cannot be written.");
        // Every run needs a whole key, so the sweeps take a sample of each phase.
        const auto making = sweep(run, reference.pem, {created, written}, 1, 61);
        check(making);
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY(making.errors.contains(created));
        const auto writing = sweep(run, reference.pem, {written}, generation + 1, 7);
        check(writing);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(writing.errors, QSet<QString>{written});
    }
    void readKey()
    {
        const auto run = [this] {
            const auto key = readEnrollmentKey(key_, Password);
            return Outcome{key.pem, key.error};
        };
        const QString unreadable =
            QStringLiteral("The private key cannot be read. Check the file and its password "
                           "variable.");
        const auto result = sweep(run, key_, {unreadable});
        check(result);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(result.errors, QSet<QString>{unreadable});
        // Empty and oversized keys fail before OpenSSL reads them.
        QCOMPARE(readEnrollmentKey({}, Password).error, unreadable);
        QCOMPARE(readEnrollmentKey(QByteArray(64 * 1024 + 1, 'x'), Password).error, unreadable);
    }
    void certificateRequest()
    {
        const QList<SubjectEntry> entries{{QStringLiteral("O"), QStringLiteral("TAK")}};
        QString error;
        begin(0);
        const auto reference =
            enrollmentRequest(key_, Password, QStringLiteral("alice"), entries, &error);
        end();
        QVERIFY2(!reference.isEmpty(), qPrintable(error));
        const auto run = [&] {
            QString failure;
            const auto request =
                enrollmentRequest(key_, Password, QStringLiteral("alice"), entries, &failure);
            return Outcome{request, request.isEmpty() ? failure : QString{}};
        };
        const QSet<QString> errors{
            QStringLiteral("The private key cannot be read."),
            QStringLiteral("The certificate request cannot be created."),
            QStringLiteral("The certificate request cannot be signed. Check the subject entries."),
            QStringLiteral("The certificate request cannot be encoded.")};
        // OpenSSL 3.5.7 frees an embedded ASN.1 field when the allocation at tasn_new.c:299
        // fails during X509_REQ_sign, and the process aborts. The crash is in OpenSSL, after
        // SWAK passed valid arguments. That allocation stays successful in this sweep.
        sparedFile = "crypto/asn1/tasn_new.c";
        sparedLine = 299;
        spared = 0;
        const auto result = sweep(run, reference, errors);
        sparedFile = nullptr;
        qInfo() << "Allocations at tasn_new.c:299 that the sweep did not fail:" << spared.load();
        check(result);
        if (QTest::currentTestFailed()) {
            return;
        }
        // An allocation failure cannot cause the encoding error. A request above 64 KiB does.
        auto oversized = errors;
        oversized.remove(QStringLiteral("The certificate request cannot be encoded."));
        QCOMPARE(result.errors, oversized);
        QList<SubjectEntry> large;
        for (int index = 0; index < 16; ++index) {
            large.append({QStringLiteral("street"), QString(5000, QLatin1Char('a'))});
        }
        QVERIFY(
            enrollmentRequest(key_, Password, QStringLiteral("alice"), large, &error).isEmpty());
        QCOMPARE(error, QStringLiteral("The certificate request cannot be encoded."));
        // OpenSSL refuses an unknown subject field.
        QVERIFY(enrollmentRequest(key_,
                                  Password,
                                  QStringLiteral("alice"),
                                  {{QStringLiteral("ZZ"), QStringLiteral("x")}},
                                  &error)
                    .isEmpty());
        QCOMPARE(
            error,
            QStringLiteral("The certificate request cannot be signed. Check the subject entries."));
    }
    void enrollmentReply()
    {
        const auto certificate = certificateFor(key_, Password);
        QVERIFY(!certificate.isEmpty());
        const auto reply =
            "{\"signedCert\":\"" + certificate + "\",\"ca0\":\"" + certificate + "\"}";
        begin(0);
        const auto reference = readEnrollmentReply(reply, key_, Password);
        end();
        QVERIFY2(reference.error.isEmpty(), qPrintable(reference.error));
        const auto run = [&] {
            const auto result = readEnrollmentReply(reply, key_, Password);
            return Outcome{result.certificate + result.authorities, result.error};
        };
        const QSet<QString> errors{
            QStringLiteral("The server did not return a valid signed certificate."),
            QStringLiteral("The server returned an invalid CA certificate."),
            QStringLiteral("The signed certificate does not match the private key.")};
        const auto result = sweep(run, reference.certificate + reference.authorities, errors);
        check(result);
        if (QTest::currentTestFailed()) {
            return;
        }
        QCOMPARE(result.errors, errors);
    }

private:
    // Fails the test for a changed result or an unexpected message.
    static void check(const Sweep &result)
    {
        QVERIFY2(result.changed.isEmpty(),
                 qPrintable(QStringLiteral("Changed result after allocation ") +
                            result.changed.join(QStringLiteral(", "))));
        QVERIFY2(result.unexpected.isEmpty(),
                 qPrintable(result.unexpected.join(QStringLiteral("; "))));
    }
    static constexpr const char *Password = "fault-test";
    QByteArray key_;
};

int main(int argc, char **argv)
{
    // These calls must come before any OpenSSL allocation.
    if (CRYPTO_set_mem_functions(testMalloc, testRealloc, testFree) != 1 ||
        RAND_set_rand_method(&seededRandom) != 1) {
        return 2;
    }
    QCoreApplication application(argc, argv);
    OpenSslFaultTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_openssl_faults.moc"
