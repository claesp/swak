#include "core/network_client.h"

#include "swak/version.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <curl/curl.h>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace swak {
namespace {
constexpr int MaxActiveRequests = 256;
constexpr long MaxConnections = 8;
constexpr long MaxHostConnections = 4;
constexpr qsizetype MaxRequestBody = 64 * 1024 * 1024;
constexpr qsizetype MaxReplyBytes = 256 * 1024 * 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("NetworkClient", text);
}
bool curlReady()
{
    // curl_global_init is thread-safe in libcurl 7.84 and later. GDAL also initializes it,
    // and libcurl counts the calls. SWAK does not call curl_global_cleanup while GDAL can
    // still use libcurl.
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ready;
}
QString transferError(CURLcode code, bool overflow)
{
    if (overflow || code == CURLE_FILESIZE_EXCEEDED) {
        return tr("The server reply exceeds its size limit.");
    }
    switch (code) {
    case CURLE_OK:
        return {};
    case CURLE_OPERATION_TIMEDOUT:
        return tr("The server did not answer in time.");
    case CURLE_COULDNT_RESOLVE_HOST:
        return tr("The server host name cannot be resolved.");
    case CURLE_COULDNT_CONNECT:
        return tr("The server refused the connection or cannot be reached.");
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_ENGINE_INITFAILED:
        return tr("TLS certificate validation or the client credentials failed.");
    default:
        // libcurl messages describe the failure class and contain no URL or credential.
        return tr("The HTTPS request failed: %1.").arg(QString::fromUtf8(curl_easy_strerror(code)));
    }
}
} // namespace

struct NetworkClient::Shared
{
    struct Pending
    {
        quint64 id = 0;
        NetworkRequest request;
        QPointer<QObject> context;
        Callback done;
    };
    std::mutex mutex;
    std::deque<Pending> queued;
    std::unordered_set<quint64> cancelled;
    bool stop = false;
    CURLM *multi = nullptr;
    std::atomic_int active = 0;
    quint64 next = 0;
};

namespace {
struct Transfer
{
    Transfer() = default;
    Transfer(const Transfer &) = delete;
    Transfer &operator=(const Transfer &) = delete;
    ~Transfer()
    {
        if (easy) {
            curl_easy_cleanup(easy);
        }
        curl_slist_free_all(headers);
    }
    quint64 id = 0;
    CURL *easy = nullptr;
    curl_slist *headers = nullptr;
    QByteArray body;
    qsizetype limit = 0;
    bool overflow = false;
    QPointer<QObject> context;
    NetworkClient::Callback done;
};
size_t writeReply(char *data, size_t size, size_t count, void *user)
{
    auto *transfer = static_cast<Transfer *>(user);
    if (size != 1 || count > static_cast<size_t>(transfer->limit - transfer->body.size())) {
        transfer->overflow = true;
        return 0;
    }
    transfer->body.append(data, static_cast<qsizetype>(count));
    return count;
}
// Sets a binary option from memory. libcurl copies the bytes, so the request can end.
bool setBlob(CURL *easy, CURLoption option, const QByteArray &bytes)
{
    curl_blob blob{
        const_cast<char *>(bytes.constData()), static_cast<size_t>(bytes.size()), CURL_BLOB_COPY};
    return curl_easy_setopt(easy, option, &blob) == CURLE_OK;
}
std::unique_ptr<Transfer> makeTransfer(const NetworkRequest &request)
{
    auto transfer = std::make_unique<Transfer>();
    transfer->easy = curl_easy_init();
    if (!transfer->easy) {
        return nullptr;
    }
    auto *easy = transfer->easy;
    transfer->limit = request.maxBytes;
    const auto url = request.url.toString(QUrl::FullyEncoded).toUtf8();
    bool ok =
        curl_easy_setopt(easy, CURLOPT_URL, url.constData()) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https") == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_SSLVERSION, long{CURL_SSLVERSION_TLSv1_2}) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, long{request.timeoutMs}) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, long{request.connectTimeoutMs}) ==
            CURLE_OK &&
        curl_easy_setopt(easy,
                         CURLOPT_MAXFILESIZE_LARGE,
                         static_cast<curl_off_t>(request.maxBytes)) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeReply) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get()) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get()) == CURLE_OK &&
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "SWAK/" SWAK_VERSION) == CURLE_OK;
    // Send the body at once instead of waiting for a 100 Continue reply.
    transfer->headers = curl_slist_append(transfer->headers, "Expect:");
    for (const auto &[name, value] : request.headers) {
        transfer->headers = curl_slist_append(transfer->headers, (name + ": " + value).constData());
    }
    ok = ok && transfer->headers &&
         curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headers) == CURLE_OK;
    const auto &tls = request.tls;
    if (ok && !tls.ca.isEmpty()) {
        ok = setBlob(easy, CURLOPT_CAINFO_BLOB, tls.ca);
    }
    if (ok && !tls.certificate.isEmpty()) {
        ok = setBlob(easy, CURLOPT_SSLCERT_BLOB, tls.certificate) &&
             curl_easy_setopt(easy, CURLOPT_SSLCERTTYPE, tls.pkcs12 ? "P12" : "PEM") == CURLE_OK;
        if (ok && !tls.pkcs12) {
            ok = setBlob(easy, CURLOPT_SSLKEY_BLOB, tls.key) &&
                 curl_easy_setopt(easy, CURLOPT_SSLKEYTYPE, "PEM") == CURLE_OK;
        }
        if (ok && !tls.password.isEmpty()) {
            // libcurl copies the string.
            ok = curl_easy_setopt(easy, CURLOPT_KEYPASSWD, tls.password.constData()) == CURLE_OK;
        }
    }
    if (ok && request.method == "HEAD") {
        ok = curl_easy_setopt(easy, CURLOPT_NOBODY, 1L) == CURLE_OK;
    } else if (ok && request.method != "GET") {
        ok = curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, request.method.constData()) == CURLE_OK;
        if (ok && (request.method == "POST" || !request.body.isEmpty())) {
            ok = curl_easy_setopt(easy,
                                  CURLOPT_POSTFIELDSIZE_LARGE,
                                  static_cast<curl_off_t>(request.body.size())) == CURLE_OK &&
                 curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, request.body.constData()) ==
                     CURLE_OK;
        }
    }
    return ok ? std::move(transfer) : nullptr;
}
} // namespace

NetworkClient::NetworkClient(QObject *parent) : QObject(parent), shared_(std::make_shared<Shared>())
{
    if (curlReady()) {
        shared_->multi = curl_multi_init();
    }
    if (shared_->multi) {
        curl_multi_setopt(shared_->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, MaxConnections);
        curl_multi_setopt(shared_->multi, CURLMOPT_MAX_HOST_CONNECTIONS, MaxHostConnections);
        worker_ = std::thread([this] {
            run();
        });
    }
}
NetworkClient::~NetworkClient()
{
    {
        const std::lock_guard lock(shared_->mutex);
        shared_->stop = true;
    }
    if (worker_.joinable()) {
        curl_multi_wakeup(shared_->multi);
        worker_.join();
    }
    if (shared_->multi) {
        curl_multi_cleanup(shared_->multi);
    }
}
quint64 NetworkClient::send(NetworkRequest request, QObject *context, Callback done)
{
    const auto reject = [&](const QString &error) {
        NetworkReply reply;
        reply.error = error;
        deliver(context, std::move(done), std::move(reply));
        return quint64{0};
    };
    if (!shared_->multi) {
        return reject(tr("The HTTPS client is not available."));
    }
    const auto &url = request.url;
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasFragment() ||
        url.host().compare(request.expectedHost, Qt::CaseInsensitive) != 0 ||
        url.port(443) != request.expectedPort) {
        return reject(tr("The request does not match the configured HTTPS server."));
    }
    static const QList<QByteArray> methods{"GET", "HEAD", "POST", "PUT", "DELETE"};
    if (!methods.contains(request.method) || request.body.size() > MaxRequestBody ||
        request.maxBytes < 0 || request.maxBytes > MaxReplyBytes || request.timeoutMs < 100 ||
        request.timeoutMs > 600000 || request.connectTimeoutMs < 100 ||
        request.connectTimeoutMs > request.timeoutMs) {
        return reject(tr("The HTTPS request is outside its supported limits."));
    }
    if (std::any_of(request.headers.cbegin(), request.headers.cend(), [](const auto &header) {
            return header.first.isEmpty() || header.first.contains(':') ||
                   header.first.contains('\r') || header.first.contains('\n') ||
                   header.second.contains('\r') || header.second.contains('\n');
        })) {
        return reject(tr("An HTTPS request header is invalid."));
    }
    if (!request.tls.error.isEmpty()) {
        return reject(request.tls.error);
    }
    if (shared_->active.load() >= MaxActiveRequests) {
        return reject(tr("Too many HTTPS requests are waiting. Try again later."));
    }
    quint64 id = 0;
    {
        const std::lock_guard lock(shared_->mutex);
        id = ++shared_->next;
        shared_->queued.push_back({id, std::move(request), context, std::move(done)});
        ++shared_->active;
    }
    curl_multi_wakeup(shared_->multi);
    return id;
}
void NetworkClient::cancel(quint64 id)
{
    if (!id || !shared_->multi) {
        return;
    }
    {
        const std::lock_guard lock(shared_->mutex);
        shared_->cancelled.insert(id);
    }
    curl_multi_wakeup(shared_->multi);
}
// Network tests inspect the queue and cancellation.
// cppcheck-suppress unusedFunction
int NetworkClient::activeCount() const
{
    return shared_->active.load();
}
void NetworkClient::deliver(QPointer<QObject> context, Callback done, NetworkReply reply)
{
    // This object lives on the interface thread. The destructor joins the worker before
    // QObject removes pending events, so a queued call never reaches a destroyed client.
    QMetaObject::invokeMethod(
        this,
        [context, done = std::move(done), reply = std::move(reply)] {
            if (context && done) {
                done(reply);
            }
        },
        Qt::QueuedConnection);
}
void NetworkClient::run()
{
    auto &shared = *shared_;
    std::unordered_map<quint64, std::unique_ptr<Transfer>> transfers;
    const auto finish = [&](std::unordered_map<quint64, std::unique_ptr<Transfer>>::iterator it) {
        curl_multi_remove_handle(shared.multi, it->second->easy);
        transfers.erase(it);
        --shared.active;
    };
    for (;;) {
        std::deque<Shared::Pending> incoming;
        std::unordered_set<quint64> cancels;
        {
            const std::lock_guard lock(shared.mutex);
            if (shared.stop) {
                break;
            }
            incoming.swap(shared.queued);
            cancels.swap(shared.cancelled);
        }
        for (auto &pending : incoming) {
            if (cancels.erase(pending.id)) {
                --shared.active;
                continue;
            }
            auto transfer = makeTransfer(pending.request);
            if (!transfer || curl_multi_add_handle(shared.multi, transfer->easy) != CURLM_OK) {
                NetworkReply reply;
                reply.error = tr("The HTTPS request cannot be prepared.");
                deliver(pending.context, std::move(pending.done), std::move(reply));
                --shared.active;
                continue;
            }
            transfer->id = pending.id;
            transfer->context = pending.context;
            transfer->done = std::move(pending.done);
            transfers.emplace(pending.id, std::move(transfer));
        }
        for (const auto id : cancels) {
            if (const auto it = transfers.find(id); it != transfers.end()) {
                finish(it);
            }
        }
        int running = 0;
        curl_multi_perform(shared.multi, &running);
        int left = 0;
        while (CURLMsg *message = curl_multi_info_read(shared.multi, &left)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            Transfer *transfer = nullptr;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &transfer);
            const auto it = transfer ? transfers.find(transfer->id) : transfers.end();
            if (it == transfers.end()) {
                continue;
            }
            NetworkReply reply;
            long status = 0;
            curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &status);
            const char *type = nullptr;
            curl_easy_getinfo(transfer->easy, CURLINFO_CONTENT_TYPE, &type);
            reply.status = static_cast<int>(status);
            reply.contentType = QByteArray(type ? type : "").left(256);
            reply.error = transferError(message->data.result, transfer->overflow);
            if (reply.error.isEmpty()) {
                reply.body = std::move(transfer->body);
            }
            deliver(transfer->context, std::move(transfer->done), std::move(reply));
            finish(it);
        }
        curl_multi_poll(shared.multi, nullptr, 0, 1000, nullptr);
    }
    for (auto &[id, transfer] : transfers) {
        curl_multi_remove_handle(shared.multi, transfer->easy);
    }
    transfers.clear();
}
} // namespace swak
