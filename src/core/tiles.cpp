#include "core/tiles.h"

#include "core/logging.h"
#include "swak/version.h"

#include <QBuffer>
#include <QDir>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QLineF>
#include <QNetworkCacheMetaData>
#include <QPainter>
#include <QSslError>
#include <QStandardPaths>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <utility>
namespace swak {
namespace {
constexpr qsizetype MaxTileBytes = 2 * 1024 * 1024;
class TileDiskCache final : public QNetworkDiskCache
{
public:
    using QNetworkDiskCache::QNetworkDiskCache;
    QIODevice *prepare(const QNetworkCacheMetaData &metadata) override
    {
        auto bounded = metadata;
        if (!bounded.expirationDate().isValid()) {
            bounded.setExpirationDate(QDateTime::currentDateTimeUtc().addDays(7));
        }
        return QNetworkDiskCache::prepare(bounded);
    }
};
} // namespace
QImage decodeMapTile(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > MaxTileBytes) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto format = reader.format();
    if (format != "png" && format != "jpeg" && format != "webp") {
        return {};
    }
    const auto size = reader.size();
    if (size.width() != 256 || size.height() != 256) {
        return {};
    }
    return reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
QList<TilePlacement> visibleTiles(const Viewport &view, int minimumZoom, int maximumZoom)
{
    QList<TilePlacement> result;
    if (minimumZoom < 0 || maximumZoom > 19 || minimumZoom > maximumZoom ||
        !std::isfinite(view.zoom) || !std::isfinite(view.center.x()) ||
        !std::isfinite(view.center.y()) || !std::isfinite(view.size.width()) ||
        !std::isfinite(view.size.height()) || view.center.x() < 0 || view.center.x() > 1 ||
        view.center.y() < 0 || view.center.y() > 1 || view.zoom < 0 || view.zoom > 19 ||
        view.size.width() <= 0 || view.size.height() <= 0 || view.size.width() > 32768 ||
        view.size.height() > 32768) {
        return result;
    }
    if (view.zoom < minimumZoom) {
        return result;
    }
    int z = std::clamp(static_cast<int>(std::floor(view.zoom)), minimumZoom, maximumZoom);
    const QPointF origin =
        QPointF(view.size.width() / 2, view.size.height() / 2) - view.center * view.scale();
    while (z > minimumZoom && (view.size.width() / (view.scale() / (1 << z)) + 2) *
                                      (view.size.height() / (view.scale() / (1 << z)) + 2) >
                                  256) {
        --z;
    }
    const int count = 1 << z;
    const double side = view.scale() / count;
    const int left = static_cast<int>(
        std::clamp(std::floor(-origin.x() / side), 0.0, static_cast<double>(count - 1)));
    const int top = static_cast<int>(
        std::clamp(std::floor(-origin.y() / side), 0.0, static_cast<double>(count - 1)));
    const int right = static_cast<int>(std::clamp(
        std::floor((view.size.width() - origin.x()) / side), 0.0, static_cast<double>(count - 1)));
    const int bottom = static_cast<int>(std::clamp(
        std::floor((view.size.height() - origin.y()) / side), 0.0, static_cast<double>(count - 1)));
    const QRectF viewport(QPointF{}, view.size);
    for (int y = top; y <= bottom && result.size() < 256; ++y) {
        for (int x = left; x <= right && result.size() < 256; ++x) {
            const QRectF rectangle(origin + QPointF(x * side, y * side), QSizeF(side, side));
            if (rectangle.intersects(viewport)) {
                result.append(
                    {QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y), z, x, y, rectangle});
            }
        }
    }
    return result;
}
QList<TilePlacement> tileFetchArea(const QList<TilePlacement> &visible, int margin)
{
    auto result = visible;
    QSet<QString> keys;
    for (const auto &tile : visible) {
        keys.insert(tile.key);
    }
    // Visible tiles always precede at most 64 nearby tiles.
    for (int ring = 1; ring <= std::clamp(margin, 0, 2); ++ring) {
        for (const auto &tile : visible) {
            if (tile.z < 0 || tile.z > 19) {
                continue;
            }
            for (int dy = -ring; dy <= ring; ++dy) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != ring) {
                        continue;
                    }
                    const int x = tile.x + dx, y = tile.y + dy;
                    if (x < 0 || y < 0 || x >= (1 << tile.z) || y >= (1 << tile.z)) {
                        continue;
                    }
                    const auto key = QStringLiteral("%1/%2/%3").arg(tile.z).arg(x).arg(y);
                    if (keys.contains(key)) {
                        continue;
                    }
                    keys.insert(key);
                    result.append({key,
                                   tile.z,
                                   x,
                                   y,
                                   tile.rectangle.translated(dx * tile.rectangle.width(),
                                                             dy * tile.rectangle.height())});
                    if (result.size() >= visible.size() + 64) {
                        return result;
                    }
                }
            }
        }
    }
    return result;
}
void TileRenderer::setPrefetchMargin(int margin)
{
    const int bounded = std::clamp(margin, 0, 2);
    if (bounded == prefetchMargin_) {
        return;
    }
    prefetchMargin_ = bounded;
    prefetchSkipped_.clear();
    needed_ = tileFetchArea(visible_, authenticatedMap(source_) ? 0 : prefetchMargin_);
    if (ready_) {
        schedule_.start(0);
    }
}
TileRenderer::TileRenderer(QObject *parent) : QObject(parent)
{
    workers_.setMaxThreadCount(2);
    cacheStats_.setInterval(2000);
    connect(&cacheStats_, &QTimer::timeout, this, [this] {
        if (ready_ && contentVisible_ && coverage().rendered < coverage().total &&
            !schedule_.isActive()) {
            schedule_.start(0);
        }
        const qint64 bytes = disk_ ? disk_->cacheSize() : -1;
        if (bytes == diskBytes_) {
            return;
        }
        diskBytes_ = bytes;
        emit changed();
    });
    cacheStats_.start();
    schedule_.setSingleShot(true);
    connect(&schedule_, &QTimer::timeout, this, &TileRenderer::pump);
}
TileRenderer::Coverage TileRenderer::coverage() const
{
    Coverage result;
    if (!contentVisible_) {
        return result;
    }
    if (!visible_.isEmpty()) {
        const auto &first = visible_.first();
        const int count = 1 << first.z;
        const int columns =
            std::min(count - first.x,
                     static_cast<int>(std::ceil((viewSize_.width() - first.rectangle.left()) /
                                                first.rectangle.width())));
        const int rows =
            std::min(count - first.y,
                     static_cast<int>(std::ceil((viewSize_.height() - first.rectangle.top()) /
                                                first.rectangle.height())));
        result.total = columns * rows;
    }
    for (const auto &tile : visible_) {
        const auto *image = images_.object(tile.key);
        if (image && !image->pixels.isNull()) {
            ++result.rendered;
        }
    }
    return result;
}
QString TileRenderer::status() const
{
    const auto current = coverage();
    QString message = current.total > 0 && current.rendered == current.total ? QString{} : status_;
    if (current.rendered < current.total) {
        if (!message.isEmpty()) {
            message += QLatin1Char(' ');
        }
        message += tr("Map coverage: %1/%2 tiles.").arg(current.rendered).arg(current.total);
    } else if (current.total > 0 && compact_ && message.isEmpty()) {
        message = tr("Map complete. Object information is limited by the memory budget.");
    }
    return message;
}
void TileRenderer::compactImages()
{
    compact_ = true;
    for (const auto &key : images_.keys()) {
        if (std::none_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) {
                return tile.key == key;
            })) {
            images_.remove(key);
            continue;
        }
        std::unique_ptr<Image> image(images_.take(key));
        image->pixels =
            image->pixels.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image->vectors = {};
        const int cost = static_cast<int>(image->pixels.sizeInBytes() / 1024);
        images_.insert(key, image.release(), cost);
    }
}
void TileRenderer::setDiskLimit(int mebibytes)
{
    diskLimitMiB_ = std::clamp(mebibytes, 16, 4096);
    if (disk_) {
        disk_->setMaximumCacheSize(qint64(diskLimitMiB_) * 1024 * 1024);
    }
}
void TileRenderer::setMemoryLimit(int mebibytes)
{
    const int limit = std::clamp(mebibytes, 64, 512) * 1024;
    if (images_.maxCost() == limit) {
        return;
    }
    cancel();
    images_.clear();
    images_.setMaxCost(limit);
    visible_.clear();
    needed_.clear();
    prefetchSkipped_.clear();
    compact_ = false;
    pressureZoom_ = 19;
    emit changed();
}
TileRenderer::~TileRenderer()
{
    cancel();
    workers_.waitForDone();
    network_.setCache(nullptr);
    lock_.reset();
}
void TileRenderer::cancel()
{
    schedule_.stop();
    ++generation_;
    for (const auto &flag : decoding_) {
        flag->store(true, std::memory_order_relaxed);
    }
    decoding_.clear();
    const auto pending = pending_;
    pending_.clear();
    for (auto *reply : pending) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}
void TileRenderer::stop()
{
    contentVisible_ = false;
    cancel();
    visible_.clear();
    needed_.clear();
    prefetchSkipped_.clear();
    for (const auto &key : images_.keys()) {
        if (images_.object(key)->transient) {
            images_.remove(key);
        }
    }
}
void TileRenderer::setSource(MapProvider *provider,
                             const QJsonObject &source,
                             const QByteArray &credential)
{
    if (provider_ == provider && source_ == source && credential_ == credential &&
        (ready_ || !provider)) {
        return;
    }
    cancel();
    images_.clear();
    compact_ = false;
    failures_.clear();
    visible_.clear();
    needed_.clear();
    prefetchSkipped_.clear();
    pressureZoom_ = 19;
    network_.clearAccessCache();
    network_.setCache(nullptr);
    disk_ = nullptr;
    diskBytes_ = -1;
    lock_.reset();
    ready_ = false;
    provider_ = provider;
    source_ = source;
    credential_ = credential;
    status_.clear();
    retryAt_ = {};
    if (!provider_ || !provider_->validate(source_).isEmpty()) {
        provider_ = nullptr;
        emit changed();
        return;
    }
    if (authenticatedMap(source_)) {
        QNetworkRequest probe(QUrl(source_.value(QStringLiteral("endpoint")).toString()));
        status_ = authorizeMapRequest(probe, source_, credential_);
        ready_ = status_.isEmpty();
        emit changed();
        return;
    }
    const auto root =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/maps/") +
        (source_.value(QStringLiteral("ignoreTls")).toBool(false) ? QStringLiteral("unchecked")
                                                                  : QStringLiteral("verified"));
    if (!QDir().mkpath(root)) {
        status_ = tr("Map cache cannot be created. Online maps stopped.");
        emit changed();
        return;
    }
    QFile::setPermissions(root,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    lock_ = std::make_unique<QLockFile>(root + QStringLiteral("/cache.lock"));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        status_ = tr("Another window uses this map cache. Online maps stopped.");
        emit changed();
        return;
    }
    disk_ = new TileDiskCache(&network_);
    disk_->setCacheDirectory(root + QStringLiteral("/tiles"));
    disk_->setMaximumCacheSize(qint64(diskLimitMiB_) * 1024 * 1024);
    diskBytes_ = disk_->cacheSize();
    network_.setCache(disk_);
    ready_ = true;
    emit changed();
}
void TileRenderer::paint(
    QPainter &painter, const Viewport &view, double opacity, bool smooth, double missingOpacity)
{
    const bool fetching = provider_ && ready_ && opacity > 0;
    if (!fetching) {
        stop();
    }
    contentVisible_ = fetching;
    const bool antialias = painter.testRenderHint(QPainter::Antialiasing);
    if (antialias_ != antialias && dynamic_cast<VectorTileProvider *>(provider_)) {
        cancel();
        images_.clear();
        compact_ = false;
    }
    antialias_ = antialias;
    const int minimum = provider_ ? source_.value(QStringLiteral("minZoom")).toInt() : 0;
    const int maximum = provider_ ? source_.value(QStringLiteral("maxZoom")).toInt() : 19;
    const auto requested = visibleTiles(view, minimum, maximum);
    // Keep the reduced detail level while panning to avoid repeated cache resets.
    if (view.zoom != requestedZoom_) {
        pressureZoom_ = maximum;
    }
    requestedZoom_ = view.zoom;
    const auto next =
        pressureZoom_ < maximum ? visibleTiles(view, minimum, pressureZoom_) : requested;
    const bool moved = next.size() != visible_.size() ||
                       !std::equal(next.begin(),
                                   next.end(),
                                   visible_.begin(),
                                   [](const TilePlacement &a, const TilePlacement &b) {
                                       return a.key == b.key;
                                   });
    visible_ = next;
    viewSize_ = view.size;
    needed_ = tileFetchArea(visible_, authenticatedMap(source_) ? 0 : prefetchMargin_);
    if (fetching && moved) {
        prefetchSkipped_.clear();
        schedule_.start(0);
    }
    for (const auto &key : images_.keys()) {
        const auto *image = images_.object(key);
        if (image && image->transient &&
            std::none_of(needed_.begin(), needed_.end(), [&](const TilePlacement &tile) {
                return tile.key == key;
            })) {
            images_.remove(key);
        }
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    for (const auto &tile : visible_) {
        painter.setOpacity(1);
        painter.fillRect(tile.rectangle, QColor(MapBackgroundRgb));
        if (const auto *image = fetching ? images_.object(tile.key) : nullptr) {
            painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
            painter.drawImage(tile.rectangle, image->pixels);
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
            painter.drawText(QRectF(-tile.rectangle.width() / 2,
                                    -tile.rectangle.height() / 2,
                                    tile.rectangle.width(),
                                    tile.rectangle.height()),
                             Qt::AlignCenter,
                             tr("MISSING TILE"));
            painter.restore();
        }
    }
    painter.restore();
    if (fetching && !schedule_.isActive()) {
        schedule_.start(150);
    }
}
void TileRenderer::fail(const QString &key,
                        const QString &message,
                        int retrySeconds,
                        bool providerWide)
{
    status_ = message;
    const auto *previous = failures_.object(key);
    const int delay =
        std::clamp(std::max(retrySeconds, previous ? previous->delay * 2 : 30), 30, 3600);
    const auto retry = QDateTime::currentDateTimeUtc().addSecs(delay);
    failures_.insert(key, new Failure{retry, delay});
    if (providerWide) {
        retryAt_ = retry;
    }
    logMessage(Severity::Warning, Urgency::Attention, QStringLiteral("swak.maps"), message);
    emit changed();
}
void TileRenderer::pump()
{
    if (!provider_ || !ready_) {
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool needed =
            std::any_of(needed_.begin(), needed_.end(), [&](const TilePlacement &tile) {
                return tile.key == it.key();
            });
        if (needed) {
            ++it;
            continue;
        }
        auto *reply = it.value();
        it = pending_.erase(it);
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    for (auto it = decoding_.begin(); it != decoding_.end(); ++it) {
        if (std::none_of(needed_.begin(), needed_.end(), [&](const TilePlacement &tile) {
                return tile.key == it.key();
            })) {
            it.value()->store(true, std::memory_order_relaxed);
        }
    }
    if (retryAt_ > now) {
        schedule_.start(static_cast<int>(std::min<qint64>(now.msecsTo(retryAt_), 3600000)));
        return;
    }
    qint64 nextRetry = 3600000;
    bool waiting = false;
    const bool visibleReady =
        std::all_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) {
            const auto *image = images_.object(tile.key);
            const auto *failure = failures_.object(tile.key);
            return (image && (image->transient || image->expires > now)) ||
                   (failure && failure->retryAt > now);
        });
    for (const auto &tile : needed_) {
        const bool visible =
            std::any_of(visible_.begin(), visible_.end(), [&](const TilePlacement &item) {
                return item.key == tile.key;
            });
        if (!visible && (!visibleReady || compact_)) {
            break;
        }
        if (prefetchSkipped_.contains(tile.key)) {
            continue;
        }
        if (const auto *failure = failures_.object(tile.key); failure && failure->retryAt > now) {
            waiting = true;
            nextRetry = std::min(nextRetry, now.msecsTo(failure->retryAt));
            continue;
        }
        if (pending_.size() + activeJobs_ >= 4) {
            continue;
        }
        const auto *cached = images_.object(tile.key);
        if ((cached && (cached->transient || cached->expires > now)) ||
            pending_.contains(tile.key) || decoding_.contains(tile.key)) {
            continue;
        }
        const auto url = provider_->tileUrl(source_, tile.z, tile.x, tile.y);
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) {
            continue;
        }
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "SWAK/" SWAK_VERSION " (native map viewer)");
        request.setRawHeader("Accept",
                             dynamic_cast<VectorTileProvider *>(provider_)
                                 ? "application/vnd.mapbox-vector-tile,application/x-protobuf"
                                 : "image/png,image/jpeg,image/webp");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setTransferTimeout(10000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        const auto authenticationError = authorizeMapRequest(request, source_, credential_);
        if (!authenticationError.isEmpty()) {
            ready_ = false;
            status_ = authenticationError;
            emit changed();
            return;
        }
        const qsizetype limit =
            dynamic_cast<VectorTileProvider *>(provider_) ? VectorDownloadLimit : MaxTileBytes;
        auto *reply = network_.get(request);
        reply->setReadBufferSize(limit + 1);
        pending_.insert(tile.key, reply);
        auto data = std::make_shared<QByteArray>();
        connect(reply, &QNetworkReply::readyRead, this, [reply, data, limit] {
            data->append(reply->read(limit + 1 - data->size()));
            if (data->size() > limit) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [reply, limit] {
            if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError> &) {
            if (source_.value(QStringLiteral("ignoreTls")).toBool(false)) {
                reply->ignoreSslErrors();
            }
        });
        QTimer::singleShot(15000, reply, [reply] {
            if (!reply->isFinished()) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, data, limit, key = tile.key] {
            pending_.remove(key);
            data->append(reply->read(limit + 1 - data->size()));
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool vector = dynamic_cast<VectorTileProvider *>(provider_) != nullptr;
            const bool responseOk =
                reply->error() == QNetworkReply::NoError && status == 200 && data->size() <= limit;
            const auto image = !vector && reply->error() == QNetworkReply::NoError && status == 200
                                   ? decodeMapTile(*data)
                                   : QImage{};
            if (!responseOk || (!vector && image.isNull())) {
                if (disk_) {
                    disk_->remove(reply->url());
                }
                fail(key,
                     tr("Map tile failed. Check the provider, HTTPS certificate, and service "
                        "limits."),
                     reply->rawHeader("Retry-After").toInt(),
                     status == 401 || status == 403 || status == 429 || status == 503 ||
                         reply->error() == QNetworkReply::SslHandshakeFailedError);
            } else {
                failures_.remove(key);
                const auto metadata =
                    disk_ ? disk_->metaData(reply->url()) : QNetworkCacheMetaData{};
                auto expires = metadata.expirationDate();
                const auto control = reply->rawHeader("Cache-Control").toLower();
                const bool transient =
                    !metadata.isValid() || control.contains("no-store") ||
                    control.contains("no-cache") ||
                    (expires.isValid() && expires <= QDateTime::currentDateTimeUtc());
                if (!expires.isValid()) {
                    expires = QDateTime::currentDateTimeUtc().addDays(7);
                }
                if (vector) {
                    decodeVector(key, std::move(*data), reply->url(), expires, transient);
                } else {
                    const int cost = static_cast<int>(image.sizeInBytes() / 1024);
                    const bool onScreen = std::any_of(
                        visible_.begin(), visible_.end(), [&](const TilePlacement &item) {
                            return item.key == key;
                        });
                    if (!onScreen && images_.totalCost() + cost > images_.maxCost()) {
                        prefetchSkipped_.insert(key);
                    } else {
                        images_.insert(key, new Image{image, {}, false, expires, transient}, cost);
                    }
                    if (failures_.isEmpty()) {
                        status_.clear();
                    }
                    emit changed();
                }
            }
            reply->deleteLater();
            schedule_.start(150);
        });
    }
    if (waiting && !schedule_.isActive()) {
        schedule_.start(static_cast<int>(std::max<qint64>(1, nextRetry)));
    }
}
void TileRenderer::decodeVector(
    const QString &key, QByteArray bytes, const QUrl &url, QDateTime expires, bool transient)
{
    const auto *decoder = dynamic_cast<VectorTileProvider *>(provider_);
    const auto generation = generation_;
    auto flag = std::make_shared<std::atomic_bool>(false);
    decoding_.insert(key, flag);
    ++activeJobs_;
    auto *watcher = new QFutureWatcher<Image>(this);
    connect(
        watcher,
        &QFutureWatcher<Image>::finished,
        this,
        [this, watcher, key, url, flag, generation, expires, transient] {
            --activeJobs_;
            auto prepared = watcher->result();
            const auto &result = prepared.vectors;
            watcher->deleteLater();
            if (generation == generation_) {
                decoding_.remove(key);
                if (!flag->load(std::memory_order_relaxed) &&
                    std::any_of(needed_.begin(), needed_.end(), [&](const TilePlacement &tile) {
                        return tile.key == key;
                    })) {
                    if (!result.error.isEmpty()) {
                        const auto tile = std::find_if(
                            visible_.begin(), visible_.end(), [&](const TilePlacement &item) {
                                return item.key == key;
                            });
                        if (result.resourceLimit && tile != visible_.end() &&
                            tile->z > source_.value(QStringLiteral("minZoom")).toInt()) {
                            pressureZoom_ = tile->z - 1;
                            cancel();
                            visible_.clear();
                            needed_.clear();
                            prefetchSkipped_.clear();
                            emit changed();
                            return;
                        }
                        if (disk_) {
                            disk_->remove(url);
                        }
                        fail(key, result.error, 30, false);
                    } else {
                        failures_.remove(key);
                        const auto *previous = images_.object(key);
                        const int previousCost =
                            previous ? (compact_ ? 256 : previous->vectors.costKiB) : 0;
                        const bool visible = std::any_of(
                            visible_.begin(), visible_.end(), [&](const TilePlacement &tile) {
                                return tile.key == key;
                            });
                        int cost = result.costKiB;
                        if (!visible &&
                            images_.totalCost() - previousCost + cost > images_.maxCost()) {
                            prefetchSkipped_.insert(key);
                            schedule_.start(150);
                            return;
                        }
                        if (!compact_ &&
                            images_.totalCost() - previousCost + cost > images_.maxCost()) {
                            for (const auto &cached : images_.keys()) {
                                if (std::none_of(visible_.begin(),
                                                 visible_.end(),
                                                 [&](const TilePlacement &tile) {
                                                     return tile.key == cached;
                                                 })) {
                                    images_.remove(cached);
                                }
                            }
                        }
                        if (compact_ ||
                            images_.totalCost() - previousCost + cost > images_.maxCost()) {
                            if (!compact_ ||
                                images_.totalCost() - previousCost + 256 > images_.maxCost()) {
                                compactImages();
                            }
                            prepared.pixels = prepared.pixels.scaled(
                                256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                            prepared.vectors = {};
                            cost = static_cast<int>(prepared.pixels.sizeInBytes() / 1024);
                        }
                        images_.insert(
                            key,
                            new Image{prepared.pixels, prepared.vectors, true, expires, transient},
                            cost);
                        if (failures_.isEmpty()) {
                            status_.clear();
                        }
                        emit changed();
                    }
                }
            }
            if (ready_ && !visible_.isEmpty()) {
                schedule_.start(150);
            }
        });
    const auto source = source_;
    const bool antialias = antialias_;
    watcher->setFuture(
        QtConcurrent::run(&workers_, [decoder, bytes = std::move(bytes), flag, source, antialias] {
            Image prepared;
            try {
                prepared.vectors = decoder->decodeTile(bytes, *flag);
                if (!prepared.vectors.error.isEmpty() || flag->load(std::memory_order_relaxed)) {
                    return prepared;
                }
                // Two pixels per logical pixel keep ordinary high-DPI views sharp.
                prepared.pixels = QImage(512, 512, QImage::Format_ARGB32_Premultiplied);
                if (prepared.pixels.isNull()) {
                    prepared.vectors.error = tr("Vector tile image allocation failed.");
                    return prepared;
                }
                QPainter painter(&prepared.pixels);
                painter.scale(2, 2);
                painter.setRenderHint(QPainter::Antialiasing, antialias);
                QList<QRectF> labels;
                paintVectorTile(
                    painter, prepared.vectors, QRectF(0, 0, 256, 256), source, labels, flag.get());
                prepared.vectors.costKiB += static_cast<int>(prepared.pixels.sizeInBytes() / 1024);
            } catch (const std::exception &) {
                prepared.vectors.error = tr("Vector tile preparation failed.");
            }
            return prepared;
        }));
}
QStringList TileRenderer::layers() const
{
    QStringList result;
    if (const auto *decoder = dynamic_cast<VectorTileProvider *>(provider_)) {
        result = decoder->layerNames();
    }
    for (const auto &tile : visible_) {
        if (const auto *image = images_.object(tile.key); image && image->vector) {
            for (const auto &layer : image->vectors.layers) {
                if (!result.contains(layer) && result.size() < 64) {
                    result.append(layer);
                }
            }
        }
    }
    return result;
}
QList<QJsonObject> TileRenderer::featuresAt(QPointF position) const
{
    if (!contentVisible_) {
        return {};
    }
    for (const auto &tile : visible_) {
        if (tile.rectangle.contains(position)) {
            if (const auto *image = images_.object(tile.key); image && image->vector) {
                return vectorFeaturesAt(image->vectors, tile.rectangle, position, source_);
            }
        }
    }
    return {};
}

QList<MapItem> TileRenderer::itemsAt(QPointF position) const
{
    QList<MapItem> result;
    if (!contentVisible_) {
        return {};
    }
    for (const auto &tile : visible_) {
        if (!tile.rectangle.contains(position)) {
            continue;
        }
        const auto *image = images_.object(tile.key);
        if (!image || !image->vector) {
            return {};
        }
        const auto hits = vectorFeaturesAt(image->vectors, tile.rectangle, position, source_);
        const auto labels = vectorLabels(image->vectors, source_);
        if (hits.isEmpty()) {
            return {};
        }
        for (const auto &hit : hits) {
            const auto &feature =
                image->vectors.features.at(hit.value(QStringLiteral("featureIndex")).toInt());
            MapItem item;
            item.kind = feature.kind == VectorFeature::Kind::Point  ? MapItem::Kind::Point
                        : feature.kind == VectorFeature::Kind::Line ? MapItem::Kind::Line
                                                                    : MapItem::Kind::Polygon;
            item.properties = feature.properties;
            item.label = item.properties.take(QStringLiteral("name")).toString();
            item.description = item.properties.take(QStringLiteral("description")).toString();
            item.type = feature.layer;
            if (item.label.isEmpty()) {
                item.label = feature.layer;
            }
            item.properties.insert(QStringLiteral("Feature ID"), feature.id);
            item.properties.insert(QStringLiteral("Map provider"),
                                   source_.value(QStringLiteral("name")));
            QTransform world;
            const double count = 1 << tile.z;
            world.translate(tile.x / count, tile.y / count);
            world.scale(1 / count, 1 / count);
            for (const auto &label : labels) {
                if (label.index == hit.value(QStringLiteral("featureIndex")).toInt()) {
                    item.cachedLabelBounds = world.mapRect(
                        QRectF(label.bounds.topLeft() / 256, label.bounds.size() / 256));
                    item.cachedLabelText = label.text;
                    item.cachedLabelFontHeight =
                        source_.value(QStringLiteral("labelSize")).toInt(12) / (256 * count);
                }
            }
            if (!feature.points.isEmpty()) {
                const auto nearest =
                    std::min_element(feature.points.begin(),
                                     feature.points.end(),
                                     [&](const auto &a, const auto &b) {
                                         const auto screen = [&](QPointF point) {
                                             return tile.rectangle.topLeft() +
                                                    QPointF(point.x() * tile.rectangle.width(),
                                                            point.y() * tile.rectangle.height());
                                         };
                                         return QLineF(screen(a), position).length() <
                                                QLineF(screen(b), position).length();
                                     });
                item.point = world.map(*nearest);
                const auto coordinate = unproject(item.point);
                item.properties.insert(QStringLiteral("Longitude"), coordinate.x());
                item.properties.insert(QStringLiteral("Latitude"), coordinate.y());
            }
            item.path = world.map(feature.path);
            item.color = feature.color;
            item.radius = source_.value(QStringLiteral("pointRadius")).toDouble(3);
            item.lineWidth = source_.value(QStringLiteral("lineWidth")).toDouble(1.5);
            result.append(std::move(item));
        }
        return result;
    }
    return result;
}

} // namespace swak
