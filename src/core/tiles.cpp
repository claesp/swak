#include "core/tiles.h"
#include "core/logging.h"
#include "swak/version.h"
#include <QBuffer>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QDir>
#include <QImageReader>
#include <QNetworkCacheMetaData>
#include <QPainter>
#include <QStandardPaths>
#include <QSslError>
#include <algorithm>
#include <cmath>
namespace swak {
namespace {
constexpr qsizetype MaxTileBytes = 2 * 1024 * 1024;
class TileDiskCache final : public QNetworkDiskCache {
public:
    using QNetworkDiskCache::QNetworkDiskCache;
    QIODevice *prepare(const QNetworkCacheMetaData &metadata) override
    {
        auto bounded = metadata;
        if (!bounded.expirationDate().isValid()) bounded.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(7));
        return QNetworkDiskCache::prepare(bounded);
    }
};
}
QImage decodeMapTile(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > MaxTileBytes) return {};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto format = reader.format();
    if (format != "png" && format != "jpeg" && format != "webp") return {};
    const auto size = reader.size();
    if (size.width() != 256 || size.height() != 256) return {};
    return reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
QList<TilePlacement> visibleTiles(const Viewport &view, int minimumZoom, int maximumZoom)
{
    QList<TilePlacement> result;
    if (minimumZoom < 0 || maximumZoom > 19 || minimumZoom > maximumZoom ||
        !std::isfinite(view.zoom) || !std::isfinite(view.center.x()) || !std::isfinite(view.center.y()) ||
        !std::isfinite(view.size.width()) || !std::isfinite(view.size.height()) ||
        view.center.x() < 0 || view.center.x() > 1 || view.center.y() < 0 || view.center.y() > 1 ||
        view.zoom < 0 || view.zoom > 19 || view.size.width() <= 0 || view.size.height() <= 0 ||
        view.size.width() > 32768 || view.size.height() > 32768) return result;
    if (view.zoom < minimumZoom) return result;
    int z = std::clamp(static_cast<int>(std::floor(view.zoom)), minimumZoom, maximumZoom);
    const QPointF origin = QPointF(view.size.width() / 2, view.size.height() / 2) - view.center * view.scale();
    while (z > minimumZoom && (view.size.width() / (view.scale() / (1 << z)) + 2) *
           (view.size.height() / (view.scale() / (1 << z)) + 2) > 256) --z;
    const int count = 1 << z;
    const double side = view.scale() / count;
    const int left = static_cast<int>(std::clamp(std::floor(-origin.x() / side), 0.0, static_cast<double>(count - 1)));
    const int top = static_cast<int>(std::clamp(std::floor(-origin.y() / side), 0.0, static_cast<double>(count - 1)));
    const int right = static_cast<int>(std::clamp(std::floor((view.size.width() - origin.x()) / side), 0.0, static_cast<double>(count - 1)));
    const int bottom = static_cast<int>(std::clamp(std::floor((view.size.height() - origin.y()) / side), 0.0, static_cast<double>(count - 1)));
    const QRectF viewport(QPointF{}, view.size);
    for (int y = top; y <= bottom && result.size() < 256; ++y) for (int x = left; x <= right && result.size() < 256; ++x) {
        const QRectF rectangle(origin + QPointF(x * side, y * side), QSizeF(side, side));
        if (rectangle.intersects(viewport)) result.append({QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y), z, x, y, rectangle});
    }
    return result;
}
TileRenderer::TileRenderer(QObject *parent) : QObject(parent)
{
    workers_.setMaxThreadCount(2);
    schedule_.setSingleShot(true);
    connect(&schedule_, &QTimer::timeout, this, &TileRenderer::pump);
}
QString TileRenderer::status() const
{
    if (!deferred_.isEmpty()) return tr("Map memory limit reached. Increase Tile memory cache in Settings.");
    return detailLimited_ ? tr("Map detail limit reached. Zoom in or hide layers.") : status_;
}
void TileRenderer::setMemoryLimit(int mebibytes)
{
    const int limit = std::clamp(mebibytes, 64, 512) * 1024;
    if (images_.maxCost() == limit) return;
    cancel();
    images_.clear();
    images_.setMaxCost(limit);
    requested_.clear();
    visible_.clear();
    deferred_.clear();
    pressureZoom_ = 19;
    emit changed();
}
TileRenderer::~TileRenderer() { cancel(); workers_.waitForDone(); network_.setCache(nullptr); lock_.reset(); }
void TileRenderer::cancel()
{
    schedule_.stop();
    ++generation_;
    for (const auto &flag : decoding_) flag->store(true, std::memory_order_relaxed);
    decoding_.clear();
    const auto pending = pending_;
    pending_.clear();
    for (auto *reply : pending) { reply->disconnect(this); reply->abort(); reply->deleteLater(); }
}
void TileRenderer::stop()
{
    contentVisible_ = false;
    cancel();
    visible_.clear();
    for (const auto &key : images_.keys())
        if (images_.object(key)->transient) images_.remove(key);
}
void TileRenderer::setSource(MapProvider *provider, const QJsonObject &source)
{
    if (provider_ == provider && source_ == source && (ready_ || !provider)) return;
    cancel();
    images_.clear();
    deferred_.clear();
    failures_.clear();
    visible_.clear();
    requested_.clear();
    pressureZoom_ = 19;
    network_.clearAccessCache();
    network_.setCache(nullptr);
    disk_ = nullptr;
    lock_.reset();
    ready_ = false;
    provider_ = provider;
    source_ = source;
    status_.clear();
    retryAt_ = {};
    if (!provider_ || !provider_->validate(source_).isEmpty()) { provider_ = nullptr; emit changed(); return; }
    const auto root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/maps/") +
        (source_.value(QStringLiteral("ignoreTls")).toBool(false) ? QStringLiteral("unchecked") : QStringLiteral("verified"));
    if (!QDir().mkpath(root)) { status_ = tr("Map cache cannot be created. Online maps stopped."); emit changed(); return; }
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    lock_ = std::make_unique<QLockFile>(root + QStringLiteral("/cache.lock"));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) { status_ = tr("Another window uses this map cache. Online maps stopped."); emit changed(); return; }
    disk_ = new TileDiskCache(&network_);
    disk_->setCacheDirectory(root + QStringLiteral("/tiles"));
    disk_->setMaximumCacheSize(128 * 1024 * 1024);
    network_.setCache(disk_);
    ready_ = true;
    emit changed();
}
void TileRenderer::paint(QPainter &painter, const Viewport &view, double opacity, bool smooth, double missingOpacity)
{
    const bool fetching = provider_ && ready_ && opacity > 0;
    if (!fetching) stop();
    contentVisible_ = fetching;
    const int minimum = provider_ ? source_.value(QStringLiteral("minZoom")).toInt() : 0;
    const int maximum = provider_ ? source_.value(QStringLiteral("maxZoom")).toInt() : 19;
    const auto requested = visibleTiles(view, minimum, maximum);
    const bool newView = view.zoom != requestedZoom_ || requested.size() != requested_.size()
        || !std::equal(requested.begin(), requested.end(), requested_.begin(),
            [](const TilePlacement &a, const TilePlacement &b) { return a.key == b.key; });
    if (newView) pressureZoom_ = maximum;
    requestedZoom_ = view.zoom;
    requested_ = requested;
    const auto next = pressureZoom_ < maximum ? visibleTiles(view, minimum, pressureZoom_) : requested;
    const bool moved = next.size() != visible_.size() || !std::equal(next.begin(), next.end(), visible_.begin(),
        [](const TilePlacement &a, const TilePlacement &b) { return a.key == b.key; });
    visible_ = next;
    if (fetching && moved) { deferred_.clear(); schedule_.start(150); }
    for (const auto &key : images_.keys()) {
        const auto *image = images_.object(key);
        if (image && image->transient && std::none_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == key; }))
            images_.remove(key);
    }
    QList<QRectF> labels;
    int featureBudget = 200000;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    for (const auto &tile : visible_) {
        painter.setOpacity(1);
        painter.fillRect(tile.rectangle, QColor(MapBackgroundRgb));
        if (const auto *image = fetching ? images_.object(tile.key) : nullptr) {
            painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
            if (image->vector) paintVectorTile(painter, image->vectors, tile.rectangle, source_, labels, featureBudget);
            else painter.drawImage(tile.rectangle, image->pixels);
        } else {
            painter.setPen(QColor("#d0d7cb"));
            painter.setOpacity(std::clamp(missingOpacity, 0.0, 1.0) * 0.35);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(tile.rectangle);
            painter.setOpacity(std::clamp(missingOpacity, 0.0, 1.0));
            painter.save();
            painter.setClipRect(tile.rectangle, Qt::IntersectClip);
            painter.translate(tile.rectangle.center());
            painter.rotate(-45);
            painter.drawText(QRectF(-tile.rectangle.width() / 2, -tile.rectangle.height() / 2,
                tile.rectangle.width(), tile.rectangle.height()), Qt::AlignCenter, tr("MISSING TILE"));
            painter.restore();
        }
    }
    painter.restore();
    detailLimited_ = featureBudget <= 0;
    if (fetching && !schedule_.isActive()) schedule_.start(150);
}
void TileRenderer::fail(const QString &key, const QString &message, int retrySeconds, bool providerWide)
{
    status_ = message;
    const auto *previous = failures_.object(key);
    const int delay = std::clamp(std::max(retrySeconds, previous ? previous->delay * 2 : 30), 30, 3600);
    const auto retry = QDateTime::currentDateTimeUtc().addSecs(delay);
    failures_.insert(key, new Failure{retry, delay});
    if (providerWide) retryAt_ = retry;
    logMessage(Severity::Warning, Urgency::Attention, QStringLiteral("swak.maps"), message);
    emit changed();
}
void TileRenderer::pump()
{
    if (!provider_ || !ready_) return;
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool needed = std::any_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == it.key(); });
        if (needed) { ++it; continue; }
        auto *reply = it.value();
        it = pending_.erase(it);
        reply->disconnect(this); reply->abort(); reply->deleteLater();
    }
    for (auto it = decoding_.begin(); it != decoding_.end(); ++it)
        if (std::none_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == it.key(); }))
            it.value()->store(true, std::memory_order_relaxed);
    if (retryAt_ > now) { schedule_.start(static_cast<int>(std::min<qint64>(now.msecsTo(retryAt_), 3600000))); return; }
    qint64 nextRetry = 3600000;
    bool waiting = false;
    for (const auto &tile : visible_) {
        if (const auto *failure = failures_.object(tile.key); failure && failure->retryAt > now) {
            waiting = true;
            nextRetry = std::min(nextRetry, now.msecsTo(failure->retryAt));
            continue;
        }
        if (pending_.size() + activeJobs_ >= 4 || deferred_.contains(tile.key)) continue;
        const auto *cached = images_.object(tile.key);
        if ((cached && (cached->transient || cached->expires > now)) || pending_.contains(tile.key) || decoding_.contains(tile.key)) continue;
        const auto url = provider_->tileUrl(source_, tile.z, tile.x, tile.y);
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) continue;
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "SWAK/" SWAK_VERSION " (native map viewer)");
        request.setRawHeader("Accept", dynamic_cast<VectorTileProvider *>(provider_) ? "application/vnd.mapbox-vector-tile,application/x-protobuf" : "image/png,image/jpeg,image/webp");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setTransferTimeout(10000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        auto *reply = network_.get(request);
        reply->setReadBufferSize(MaxTileBytes + 1);
        pending_.insert(tile.key, reply);
        auto data = std::make_shared<QByteArray>();
        connect(reply, &QNetworkReply::readyRead, this, [reply, data] {
            data->append(reply->read(MaxTileBytes + 1 - data->size()));
            if (data->size() > MaxTileBytes) reply->abort();
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [reply] {
            if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > MaxTileBytes) reply->abort();
        });
        connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError> &) {
            if (source_.value(QStringLiteral("ignoreTls")).toBool(false)) reply->ignoreSslErrors();
        });
        QTimer::singleShot(15000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
        connect(reply, &QNetworkReply::finished, this, [this, reply, data, key = tile.key] {
            pending_.remove(key);
            data->append(reply->read(MaxTileBytes + 1 - data->size()));
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool vector = dynamic_cast<VectorTileProvider *>(provider_) != nullptr;
            const bool responseOk = reply->error() == QNetworkReply::NoError && status == 200 && data->size() <= MaxTileBytes;
            const auto image = !vector && reply->error() == QNetworkReply::NoError && status == 200 ? decodeMapTile(*data) : QImage{};
            if (!responseOk || (!vector && image.isNull())) {
                if (disk_) disk_->remove(reply->url());
                fail(key, tr("Map tile failed. Check the provider, HTTPS certificate, and service limits."),
                    reply->rawHeader("Retry-After").toInt(), status == 429 || status == 503
                        || reply->error() == QNetworkReply::SslHandshakeFailedError);
            } else {
                failures_.remove(key);
                const auto metadata = disk_ ? disk_->metaData(reply->url()) : QNetworkCacheMetaData{};
                auto expires = metadata.expirationDate();
                const auto control = reply->rawHeader("Cache-Control").toLower();
                const bool transient = !metadata.isValid() || control.contains("no-store") || control.contains("no-cache") ||
                    (expires.isValid() && expires <= QDateTime::currentDateTimeUtc());
                if (!expires.isValid()) expires = QDateTime::currentDateTimeUtc().addDays(7);
                if (vector) decodeVector(key, std::move(*data), reply->url(), expires, transient);
                else {
                    images_.insert(key, new Image{image, {}, false, expires, transient}, static_cast<int>(image.sizeInBytes() / 1024));
                    if (failures_.isEmpty()) status_.clear();
                    emit changed();
                }
            }
            reply->deleteLater();
            schedule_.start(150);
        });
    }
    if (waiting && !schedule_.isActive()) schedule_.start(static_cast<int>(std::max<qint64>(1, nextRetry)));
}
void TileRenderer::decodeVector(const QString &key, QByteArray bytes, const QUrl &url, QDateTime expires, bool transient)
{
    const auto *decoder = dynamic_cast<VectorTileProvider *>(provider_);
    const auto generation = generation_;
    auto flag = std::make_shared<std::atomic_bool>(false);
    decoding_.insert(key, flag);
    ++activeJobs_;
    auto *watcher = new QFutureWatcher<VectorTile>(this);
    connect(watcher, &QFutureWatcher<VectorTile>::finished, this, [this, watcher, key, url, flag, generation, expires, transient] {
        --activeJobs_;
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation == generation_) {
            decoding_.remove(key);
            if (!flag->load(std::memory_order_relaxed)
                && std::any_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == key; })) {
                if (!result.error.isEmpty()) {
                    if (disk_) disk_->remove(url);
                    fail(key, result.error, 30, false);
                } else {
                    failures_.remove(key);
                    const auto *previous = images_.object(key);
                    const int previousCost = previous ? previous->vectors.costKiB : 0;
                    if (images_.totalCost() - previousCost + result.costKiB > images_.maxCost()) {
                        for (const auto &cached : images_.keys())
                            if (std::none_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == cached; }))
                                images_.remove(cached);
                    }
                    if (images_.totalCost() - previousCost + result.costKiB > images_.maxCost()) {
                        const auto tile = std::find_if(visible_.begin(), visible_.end(), [&](const TilePlacement &item) { return item.key == key; });
                        const int minimum = source_.value(QStringLiteral("minZoom")).toInt();
                        if (tile != visible_.end() && tile->z > minimum) {
                            pressureZoom_ = std::min(pressureZoom_, tile->z - 1);
                            cancel();
                            images_.clear();
                            visible_.clear();
                            deferred_.clear();
                        } else {
                            const bool first = deferred_.isEmpty();
                            deferred_.insert(key);
                            if (first) logMessage(Severity::Warning, Urgency::Attention, QStringLiteral("swak.maps"), status());
                        }
                        emit changed();
                        return;
                    }
                    images_.insert(key, new Image{{}, result, true, expires, transient}, result.costKiB);
                    if (failures_.isEmpty() && deferred_.isEmpty()) status_.clear();
                    emit changed();
                }
            }
        }
        if (ready_ && !visible_.isEmpty()) schedule_.start(150);
    });
    watcher->setFuture(QtConcurrent::run(&workers_, [decoder, bytes = std::move(bytes), flag] {
        try { return decoder->decodeTile(bytes, *flag); }
        catch (const std::exception &) { VectorTile result; result.error = tr("Vector tile decoder failed."); return result; }
    }));
}
QStringList TileRenderer::layers() const
{
    QStringList result;
    if (const auto *decoder = dynamic_cast<VectorTileProvider *>(provider_)) result = decoder->layerNames();
    for (const auto &tile : visible_) if (const auto *image = images_.object(tile.key); image && image->vector)
        for (const auto &layer : image->vectors.layers) if (!result.contains(layer) && result.size() < 64) result.append(layer);
    return result;
}
QList<QJsonObject> TileRenderer::featuresAt(QPointF position) const
{
    if (!contentVisible_) return {};
    for (const auto &tile : visible_) if (tile.rectangle.contains(position))
        if (const auto *image = images_.object(tile.key); image && image->vector)
            return vectorFeaturesAt(image->vectors, tile.rectangle, position, source_);
    return {};
}

}
