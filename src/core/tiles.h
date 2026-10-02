#pragma once
#include "core/maps.h"
#include "core/projection.h"
#include <QCache>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkDiskCache>
#include <QLockFile>
#include <QTimer>
#include <memory>
class QPainter;
namespace swak {
QImage decodeMapTile(const QByteArray &bytes);
struct TilePlacement { QString key; int z; int x; int y; QRectF rectangle; };
QList<TilePlacement> visibleTiles(const Viewport &view, int minimumZoom, int maximumZoom);
class TileRenderer final : public QObject {
    Q_OBJECT
public:
    explicit TileRenderer(QObject *parent = nullptr);
    ~TileRenderer() override;
    void setSource(MapProvider *provider, const QJsonObject &source);
    void paint(QPainter &painter, const Viewport &view, double opacity, bool smooth);
    const QString &status() const { return status_; }
    // Network tests inspect the request bound and cancellation.
    // cppcheck-suppress unusedFunction
    qsizetype pendingCount() const { return pending_.size(); }
    void stop();
signals:
    void changed();
private:
    struct Image { QImage pixels; QDateTime expires; bool transient; };
    void pump();
    void cancel();
    void fail(const QString &key, const QString &message, int retrySeconds, bool providerWide);
    QNetworkAccessManager network_;
    QNetworkDiskCache *disk_ = nullptr;
    std::unique_ptr<QLockFile> lock_;
    MapProvider *provider_ = nullptr;
    QJsonObject source_;
    QCache<QString, Image> images_{64 * 1024};
    QHash<QString, QNetworkReply *> pending_;
    QList<TilePlacement> visible_;
    QTimer schedule_;
    QDateTime retryAt_;
    struct Failure { QDateTime retryAt; int delay; };
    QCache<QString, Failure> failures_{256};
    QString status_;
    bool ready_ = false;
};
}
