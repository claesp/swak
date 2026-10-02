#include "core/tiles.h"
#include "core/logging.h"
#include "swak/version.h"
#include <QBuffer>
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
    schedule_.setSingleShot(true);
    connect(&schedule_, &QTimer::timeout, this, &TileRenderer::pump);
}
TileRenderer::~TileRenderer() { cancel(); network_.setCache(nullptr); lock_.reset(); }
void TileRenderer::cancel()
{
    schedule_.stop();
    const auto pending = pending_;
    pending_.clear();
    for (auto *reply : pending) { reply->disconnect(this); reply->abort(); reply->deleteLater(); }
}
void TileRenderer::stop() { cancel(); visible_.clear(); }
void TileRenderer::setSource(MapProvider *provider, const QJsonObject &source)
{
    if (provider_ == provider && source_ == source && (ready_ || !provider)) return;
    cancel();
    images_.clear();
    visible_.clear();
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
void TileRenderer::paint(QPainter &painter, const Viewport &view, double opacity, bool smooth)
{
    if (!provider_ || !ready_) return;
    if (opacity <= 0) { stop(); return; }
    visible_ = visibleTiles(view, source_.value(QStringLiteral("minZoom")).toInt(), source_.value(QStringLiteral("maxZoom")).toInt());
    for (const auto &key : images_.keys()) {
        const auto *image = images_.object(key);
        if (image && image->transient && std::none_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == key; }))
            images_.remove(key);
    }
    painter.save();
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &tile : visible_) {
        if (const auto *image = images_.object(tile.key)) {
            if (image->transient || image->expires > now) painter.drawImage(tile.rectangle, image->pixels);
            else images_.remove(tile.key);
        }
    }
    painter.restore();
    if (!schedule_.isActive()) schedule_.start(150);
}
void TileRenderer::fail(const QString &message, int retrySeconds)
{
    status_ = message;
    retryAt_ = QDateTime::currentDateTimeUtc().addSecs(std::clamp(retrySeconds, 30, 3600));
    logMessage(Severity::Warning, Urgency::Attention, QStringLiteral("swak.maps"), message);
    emit changed();
}
void TileRenderer::pump()
{
    if (!provider_ || !ready_) return;
    const auto now = QDateTime::currentDateTimeUtc();
    if (retryAt_ > now) { schedule_.start(static_cast<int>(std::min<qint64>(now.msecsTo(retryAt_), 3600000))); return; }
    for (auto it = pending_.begin(); it != pending_.end();) {
        const bool needed = std::any_of(visible_.begin(), visible_.end(), [&](const TilePlacement &tile) { return tile.key == it.key(); });
        if (needed) { ++it; continue; }
        auto *reply = it.value();
        it = pending_.erase(it);
        reply->disconnect(this); reply->abort(); reply->deleteLater();
    }
    for (const auto &tile : visible_) {
        if (pending_.size() >= 4) break;
        if (images_.contains(tile.key) || pending_.contains(tile.key)) continue;
        const auto url = provider_->tileUrl(source_, tile.z, tile.x, tile.y);
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()) continue;
        QNetworkRequest request(url);
        request.setRawHeader("User-Agent", "SWAK/" SWAK_VERSION " (native map viewer)");
        request.setRawHeader("Accept", "image/png,image/jpeg,image/webp");
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
            const auto image = reply->error() == QNetworkReply::NoError && status == 200 ? decodeMapTile(*data) : QImage{};
            if (image.isNull()) {
                if (disk_) disk_->remove(reply->url());
                fail(tr("Map tile failed. Check the provider, HTTPS certificate, and service limits."), reply->rawHeader("Retry-After").toInt());
            } else {
                auto expires = disk_ ? disk_->metaData(reply->url()).expirationDate() : QDateTime{};
                const auto control = reply->rawHeader("Cache-Control").toLower();
                const bool transient = control.contains("no-store") || control.contains("no-cache") ||
                    (expires.isValid() && expires <= QDateTime::currentDateTimeUtc());
                if (!expires.isValid()) expires = QDateTime::currentDateTimeUtc().addDays(7);
                images_.insert(key, new Image{image, expires, transient}, static_cast<int>(image.sizeInBytes() / 1024));
                status_.clear();
                emit changed();
            }
            reply->deleteLater();
            if (!schedule_.isActive()) schedule_.start(150);
        });
    }
}
}
