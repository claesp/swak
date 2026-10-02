#include "core/vector_tiles.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <QJsonArray>
#include <QFontMetricsF>
#include <algorithm>
#include <cmath>
namespace swak {
namespace {
QTransform transform(const QRectF &rectangle)
{
    QTransform result;
    result.translate(rectangle.x(), rectangle.y());
    result.scale(rectangle.width(), rectangle.height());
    return result;
}

}
bool vectorLayerVisible(const QJsonObject &source, const QString &layer)
{
    return source.value(QStringLiteral("visibleLayers")).toObject().value(layer).toBool(true);
}
void paintVectorTile(QPainter &painter, const VectorTile &tile, const QRectF &rectangle, const QJsonObject &source,
                     QList<QRectF> &labels, const std::atomic_bool *cancelled)
{
    painter.save();
    painter.setClipRect(rectangle, Qt::IntersectClip);
    painter.fillRect(rectangle, QColor("#e9e6dc"));
    const auto mapping = transform(rectangle);
    const double width = source.value(QStringLiteral("lineWidth")).toDouble(1.5);
    const double radius = source.value(QStringLiteral("pointRadius")).toDouble(3);
    auto font = painter.font();
    font.setPixelSize(source.value(QStringLiteral("labelSize")).toInt(12));
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    for (const auto &feature : tile.features) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) break;
        if (!vectorLayerVisible(source, feature.layer)) continue;
        const auto geometry = mapping.map(feature.path);
        const auto ink = feature.color;
        if (!feature.labelOnly && feature.kind == VectorFeature::Kind::Point) {
            painter.setPen(QPen(QColor("#faf8f1"), 1)); painter.setBrush(ink);
            for (const auto &point : feature.points) painter.drawEllipse(mapping.map(point), radius, radius);
        } else if (!feature.labelOnly) {
            painter.setBrush(feature.kind == VectorFeature::Kind::Polygon ? QBrush(ink) : Qt::NoBrush);
            painter.setPen(feature.kind == VectorFeature::Kind::Polygon ? QPen(Qt::NoPen) : QPen(ink, width));
            painter.drawPath(geometry);
        }
        if (!source.value(QStringLiteral("labels")).toBool(true) || labels.size() >= 200) continue;
        const auto language = source.value(QStringLiteral("language")).toString();
        QString name = language.isEmpty() ? QString{} : feature.properties.value(QStringLiteral("name:") + language).toString();
        if (name.isEmpty()) name = feature.properties.value(QStringLiteral("name")).toString();
        if (name.isEmpty() && feature.layer == QLatin1String("housenumber")) name = feature.properties.value(QStringLiteral("housenumber")).toString();
        if (name.isEmpty()) continue;
        const QPointF anchor = feature.kind == VectorFeature::Kind::Point ? mapping.map(feature.points.first()) : geometry.boundingRect().center();
        if (!rectangle.contains(anchor)) continue;
        name = metrics.elidedText(name, Qt::ElideRight, 180);
        const QRectF bounds(anchor + QPointF(radius + 3, -metrics.height() / 2), QSizeF(metrics.horizontalAdvance(name) + 4, metrics.height()));
        if (std::any_of(labels.cbegin(), labels.cend(), [&](const QRectF &other) { return other.intersects(bounds); })) continue;
        labels.append(bounds);
        painter.fillRect(bounds, QColor(255, 255, 255, 195));
        painter.setPen(QColor("#30352f"));
        painter.drawText(bounds, Qt::AlignCenter, name);
    }
    painter.restore();
}
QList<QJsonObject> vectorFeaturesAt(const VectorTile &tile, const QRectF &rectangle, QPointF position, const QJsonObject &source)
{
    QList<QJsonObject> result;
    if (!rectangle.contains(position)) return result;
    const auto mapping = transform(rectangle);
    QPainterPathStroker stroke;
    stroke.setWidth(std::max(12.0, source.value(QStringLiteral("lineWidth")).toDouble(1.5) + 4));
    for (auto it = tile.features.crbegin(); it != tile.features.crend() && result.size() < 12; ++it) {
        if (!vectorLayerVisible(source, it->layer)) continue;
        bool hit = false;
        if (it->kind == VectorFeature::Kind::Point) {
            for (const auto &point : it->points) {
                const auto distance = mapping.map(point) - position;
                if (std::hypot(distance.x(), distance.y()) <= std::max(8.0, source.value(QStringLiteral("pointRadius")).toDouble(3) + 3)) { hit = true; break; }
            }
        } else {
            const auto path = mapping.map(it->path);
            if (path.boundingRect().adjusted(-6, -6, 6, 6).contains(position))
                hit = it->kind == VectorFeature::Kind::Polygon ? path.contains(position) : stroke.createStroke(path).contains(position);
        }
        if (hit) result.append({{QStringLiteral("layer"), it->layer}, {QStringLiteral("id"), it->id}, {QStringLiteral("properties"), it->properties}});
    }
    return result;
}
}
