#pragma once
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <deque>

namespace swak {
class Servers;
class TakServerApi;

// Images of TAK iconset icons for received objects (usericon iconsetpath).
//
// The cache asks the TAK Server of an object for an icon once, at
// /Marti/api/iconimage?iconsetpath=. It accepts PNG and JPEG images of at most 256 KiB and
// 512 by 512 pixels and keeps them at most 64 pixels wide. It keeps at most MaxIcons images
// and remembers failed paths, so a missing icon is not requested again.
class IconCache final : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxIcons = 512;
    static constexpr int MaxPending = 4;
    static constexpr qsizetype MaxIconBytes = 256 * 1024;
    static constexpr int MaxDecodedSide = 512;
    static constexpr int IconSide = 64;
    IconCache(Servers *servers, TakServerApi *api, QObject *parent = nullptr);
    // Returns the icon, or a null image while it loads or when it is not available.
    QImage icon(const QString &serverId, const QString &path);
    // Adds an icon, for example from an imported iconset. Images beyond the size limit are
    // scaled down.
    void insert(const QString &serverId, const QString &path, const QImage &image);
signals:
    void iconLoaded();

private:
    void request(const QString &serverId, const QString &path);
    void store(const QString &key, const QImage &image);
    QPointer<Servers> servers_;
    QPointer<TakServerApi> api_;
    QHash<QString, QImage> icons_;
    std::deque<QString> order_;
    QSet<QString> failed_;
    QSet<QString> pending_;
    QList<std::pair<QString, QString>> waiting_;
};
// True for an iconset path of a server icon: iconset UID, group, and an image file name.
// Built-in ATAK paths, such as COT_MAPPING_SPOTMAP, are not server icons.
bool serverIconPath(const QString &path);
// Decodes an icon with the size limits of the cache. Returns a null image for other data.
QImage decodeIcon(const QByteArray &bytes);
} // namespace swak
