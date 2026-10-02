#pragma once
#include "core/maps.h"
#include "core/projection.h"
#include "core/vector_tiles.h"
#include <QThreadPool>
#include <QSet>
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
inline constexpr QRgb MapBackgroundRgb = qRgb(48, 61, 43);
QImage decodeMapTile(const QByteArray &bytes);
struct TilePlacement { QString key; int z; int x; int y; QRectF rectangle; };
QList<TilePlacement> visibleTiles(const Viewport &view, int minimumZoom, int maximumZoom);
class TileRenderer final : public QObject {
    Q_OBJECT
public:
    explicit TileRenderer(QObject *parent = nullptr);
    ~TileRenderer() override;
    void setSource(MapProvider *provider, const QJsonObject &source);
    void paint(QPainter &painter, const Viewport &view, double opacity, bool smooth, double missingOpacity = 0.2);
    QString status() const;
    void setMemoryLimit(int mebibytes);
    void setDiskLimit(int mebibytes);
    qint64 diskCacheBytes() const { return diskBytes_; }
    // Network tests inspect the request bound and cancellation.
    // cppcheck-suppress unusedFunction
    qsizetype pendingCount() const { return pending_.size(); }
    // Regression tests enforce the decoded memory budget.
    // cppcheck-suppress unusedFunction
    qsizetype memoryCostKiB() const { return images_.totalCost(); }
    void stop();
    QStringList layers() const;
    QList<QJsonObject> featuresAt(QPointF position) const;
signals:
    void changed();
private:
    struct Image { QImage pixels; VectorTile vectors; bool vector = false; QDateTime expires; bool transient = false; };
    void pump();
    void cancel();
    void decodeVector(const QString &key, QByteArray bytes, const QUrl &url, QDateTime expires, bool transient);
    void fail(const QString &key, const QString &message, int retrySeconds, bool providerWide);
    QNetworkAccessManager network_;
    QNetworkDiskCache *disk_ = nullptr;
    std::unique_ptr<QLockFile> lock_;
    MapProvider *provider_ = nullptr;
    QJsonObject source_;
    QCache<QString, Image> images_{256 * 1024};
    QHash<QString, QNetworkReply *> pending_;
    QList<TilePlacement> visible_;
    int pressureZoom_ = 19;
    double requestedZoom_ = -1;
    QTimer schedule_;
    QTimer cacheStats_;
    qint64 diskBytes_ = -1;
    int diskLimitMiB_ = 128;
    QDateTime retryAt_;
    struct Failure { QDateTime retryAt; int delay; };
    QCache<QString, Failure> failures_{256};
    QString status_;
    bool ready_ = false;
    bool antialias_ = false;
    bool contentVisible_ = false;
    QThreadPool workers_;
    QHash<QString, std::shared_ptr<std::atomic_bool>> decoding_;
    quint64 generation_ = 0;
    int activeJobs_ = 0;
    QSet<QString> deferred_;
};
}
