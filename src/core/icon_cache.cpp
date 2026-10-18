#include "core/icon_cache.h"

#include "core/servers.h"
#include "core/tak_server_api.h"

#include <QBuffer>
#include <QImageReader>
#include <QRegularExpression>
#include <QUrl>

namespace swak {
bool serverIconPath(const QString &path)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9-]{1,64}/[A-Za-z0-9 ._()-]{1,64}/[A-Za-z0-9 "
                       "._()-]{1,128}\\.(?:png|PNG|jpg|JPG|jpeg)\\z"));
    return pattern.match(path).hasMatch() && !path.startsWith(QStringLiteral("COT_MAPPING_")) &&
           !path.contains(QStringLiteral(".."));
}
namespace {
// TAK Server lets every user read /api/icon/{uid}/{group}/{name}. The /api/iconimage
// request needs the administrator role.
QString iconRequestPath(const QString &path)
{
    QString result = QStringLiteral("/api/icon");
    for (const auto &part : path.split(QLatin1Char('/'))) {
        result += QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(part));
    }
    return result;
}
} // namespace
QImage decodeIcon(const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > IconCache::MaxIconBytes) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer);
    const auto format = reader.format();
    // Check the size before decoding, so a small file cannot expand to a huge image.
    const auto size = reader.size();
    if ((format != "png" && format != "jpeg" && format != "jpg") || !size.isValid() ||
        size.width() > IconCache::MaxDecodedSide || size.height() > IconCache::MaxDecodedSide) {
        return {};
    }
    auto image = reader.read();
    if (image.isNull()) {
        return {};
    }
    if (image.width() > IconCache::IconSide || image.height() > IconCache::IconSide) {
        image = image.scaled(IconCache::IconSide,
                             IconCache::IconSide,
                             Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
IconCache::IconCache(Servers *servers, TakServerApi *api, QObject *parent)
    : QObject(parent), servers_(servers), api_(api)
{
    setObjectName(QStringLiteral("iconCache"));
}
QImage IconCache::icon(const QString &serverId, const QString &path)
{
    const auto key = serverId + QLatin1Char('\n') + path;
    if (const auto found = icons_.constFind(key); found != icons_.cend()) {
        return *found;
    }
    if (failed_.contains(key) || pending_.contains(key) || !serverIconPath(path) || !api_ ||
        !servers_ ||
        TakServerApi::apiUrl(servers_->sources().value(serverId).toObject(),
                             QStringLiteral("/icon"))
            .isEmpty()) {
        return {};
    }
    if (pending_.size() >= MaxPending) {
        // Wait for a free request slot. Keep the waiting list as small as the cache.
        if (waiting_.size() < MaxIcons && !waiting_.contains(std::pair{serverId, path})) {
            waiting_.append({serverId, path});
        }
        return {};
    }
    request(serverId, path);
    return {};
}
void IconCache::insert(const QString &serverId, const QString &path, const QImage &image)
{
    if (image.isNull() || !serverIconPath(path)) {
        return;
    }
    auto scaled = image;
    if (scaled.width() > IconSide || scaled.height() > IconSide) {
        scaled = scaled.scaled(IconSide, IconSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const auto key = serverId + QLatin1Char('\n') + path;
    failed_.remove(key);
    store(key, scaled.convertToFormat(QImage::Format_ARGB32_Premultiplied));
}
void IconCache::store(const QString &key, const QImage &image)
{
    if (!icons_.contains(key)) {
        if (icons_.size() >= MaxIcons) {
            icons_.remove(order_.front());
            order_.pop_front();
        }
        order_.push_back(key);
    }
    icons_.insert(key, image);
    emit iconLoaded();
}
void IconCache::request(const QString &serverId, const QString &path)
{
    const auto key = serverId + QLatin1Char('\n') + path;
    pending_.insert(key);
    api_->martiRequest(
        serverId,
        "GET",
        iconRequestPath(path),
        {},
        {},
        this,
        [this, key](const NetworkReply &reply) {
            pending_.remove(key);
            const auto image = reply.ok() ? decodeIcon(reply.body) : QImage{};
            if (image.isNull()) {
                if (failed_.size() < 4 * MaxIcons) {
                    failed_.insert(key);
                }
            } else {
                store(key, image);
            }
            while (!waiting_.isEmpty() && pending_.size() < MaxPending && api_) {
                const auto [nextServer, nextPath] = waiting_.takeFirst();
                const auto next = nextServer + QLatin1Char('\n') + nextPath;
                if (!icons_.contains(next) && !failed_.contains(next) && !pending_.contains(next)) {
                    request(nextServer, nextPath);
                }
            }
        },
        MaxIconBytes,
        30000);
}
} // namespace swak
