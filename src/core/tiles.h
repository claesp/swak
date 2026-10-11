#pragma once
#include "core/map_selection.h"
#include "core/maps.h"
#include "core/projection.h"
#include "core/vector_tiles.h"

#include <QCache>
#include <QImage>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <memory>
class QPainter;
namespace swak {
inline constexpr QRgb MapBackgroundRgb = qRgb(48, 61, 43);
QImage decodeMapTile(const QByteArray &bytes);
struct TilePlacement
{
    QString key;
    int z = 0;
    int x = 0;
    int y = 0;
    QRectF rectangle;
};
QList<TilePlacement> visibleTiles(const Viewport &view, int minimumZoom, int maximumZoom);
QList<TilePlacement> tileFetchArea(const QList<TilePlacement> &visible, int margin);
class TileRenderer final : public QObject
{
    Q_OBJECT
public:
    explicit TileRenderer(QObject *parent = nullptr);
    ~TileRenderer() override;
    void
    setSource(MapProvider *provider, const QJsonObject &source, const QByteArray &credential = {});
    void paint(QPainter &painter,
               const Viewport &view,
               double opacity,
               bool smooth,
               double missingOpacity = 0.2);
    QString status() const;
    // Plain-text attribution that the service reported for the current view, or empty text.
    const QString &attribution() const
    {
        return attribution_;
    }
    struct Coverage
    {
        int total = 0;
        int rendered = 0;
    };
    Coverage coverage() const;
    void setMemoryLimit(int mebibytes);
    void setDiskLimit(int mebibytes);
    void setPrefetchMargin(int margin);
    // The fill behind tiles and of missing tiles. The default is MapBackgroundRgb.
    void setBackground(QRgb color)
    {
        background_ = color;
    }
    qint64 diskCacheBytes() const
    {
        return diskBytes_;
    }
    // Network tests inspect the request bound and cancellation.
    // cppcheck-suppress unusedFunction
    qsizetype pendingCount() const
    {
        return pending_.size();
    }
    // Regression tests enforce the decoded memory budget.
    // cppcheck-suppress unusedFunction
    qsizetype memoryCostKiB() const
    {
        return images_.totalCost();
    }
    void stop();
    QStringList layers() const;
    QList<QJsonObject> featuresAt(QPointF position) const;
    QList<MapItem> itemsAt(QPointF position) const;
signals:
    void changed();

private:
    QRgb background_ = MapBackgroundRgb;
    struct Image
    {
        QImage pixels;
        VectorTile vectors;
        bool vector = false;
        QDateTime expires;
        bool transient = false;
    };
    void pump();
    void compactImages();
    void cancel();
    void decodeVector(
        const QString &key, QByteArray bytes, const QUrl &url, QDateTime expires, bool transient);
    void renderRaster(const TilePlacement &tile);
    void fail(const QString &key, const QString &message, int retrySeconds, bool providerWide);
    QNetworkRequest serviceRequest(const QUrl &url) const;
    bool startSession(const MapServiceProvider &service);
    void requestAttribution(const Viewport &view);
    void fetchAttribution();
    void stopServiceRequests();
    QNetworkAccessManager network_;
    QNetworkDiskCache *disk_ = nullptr;
    std::unique_ptr<QLockFile> lock_;
    MapProvider *provider_ = nullptr;
    QJsonObject source_;
    QByteArray credential_;
    QString rasterCache_;
    bool rasterDirty_ = false;
    QCache<QString, Image> images_{256 * 1024};
    QHash<QString, QNetworkReply *> pending_;
    QList<TilePlacement> visible_;
    QSizeF viewSize_;
    QList<TilePlacement> needed_;
    QSet<QString> prefetchSkipped_;
    int prefetchMargin_ = 1;
    int pressureZoom_ = 19;
    double requestedZoom_ = -1;
    QTimer schedule_;
    QTimer cacheStats_;
    qint64 diskBytes_ = -1;
    int diskLimitMiB_ = 128;
    QDateTime retryAt_;
    struct Failure
    {
        QDateTime retryAt;
        int delay = 0;
    };
    QCache<QString, Failure> failures_{256};
    QString status_;
    bool ready_ = false;
    bool antialias_ = false;
    bool contentVisible_ = false;
    QThreadPool workers_;
    QHash<QString, std::shared_ptr<std::atomic_bool>> decoding_;
    quint64 generation_ = 0;
    int activeJobs_ = 0;
    bool compact_ = false;
    MapSession session_;
    QDateTime sessionStarted_;
    QPointer<QNetworkReply> sessionReply_;
    QPointer<QNetworkReply> attributionReply_;
    QString attribution_;
    QString attributionKey_;
    int attributionZoom_ = 0;
    MapBounds attributionBounds_;
    QTimer attributionTimer_;
};
} // namespace swak
