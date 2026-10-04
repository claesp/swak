#pragma once
#include <QColor>
#include <QJsonObject>
#include <QPainterPath>
#include <QStringList>
#include <QtPlugin>

#include <atomic>

class QPainter;
namespace swak {
inline constexpr qsizetype VectorDownloadLimit = 8 * 1024 * 1024;
struct VectorFeature
{
    enum class Kind
    {
        Point = 1,
        Line = 2,
        Polygon = 3
    };
    Kind kind = Kind::Point;
    QString layer;
    QString id;
    QJsonObject properties;
    QColor color = QColor("#a2ada5");
    bool labelOnly = false;
    QPainterPath path;
    QList<QPointF> points;
};
struct VectorTile
{
    QList<VectorFeature> features;
    QStringList layers;
    QString error;
    int costKiB = 1;
    bool resourceLimit = false;
};
class VectorTileProvider
{
public:
    virtual ~VectorTileProvider() = default;
    // The host calls this method on a worker. The result owns all its data.
    virtual VectorTile decodeTile(const QByteArray &bytes,
                                  const std::atomic_bool &cancelled) const = 0;
    virtual QStringList layerNames() const = 0;
};
struct VectorLabel
{
    int index;
    QRectF bounds;
    QString text;
};
QList<VectorLabel> vectorLabels(const VectorTile &tile,
                                const QJsonObject &source,
                                const std::atomic_bool *cancelled = nullptr);
bool vectorLayerVisible(const QJsonObject &source, const QString &layer);
void paintVectorTile(QPainter &painter,
                     const VectorTile &tile,
                     const QRectF &rectangle,
                     const QJsonObject &source,
                     QList<QRectF> &labels,
                     const std::atomic_bool *cancelled = nullptr);
QList<QJsonObject> vectorFeaturesAt(const VectorTile &tile,
                                    const QRectF &rectangle,
                                    QPointF position,
                                    const QJsonObject &source);
} // namespace swak
Q_DECLARE_INTERFACE(swak::VectorTileProvider, "org.swak.VectorTiles/1.0")
