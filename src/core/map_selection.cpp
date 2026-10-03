#include "core/map_selection.h"
#include <QPainter>
#include "core/marker_symbols.h"
namespace swak {
void MapSelection::paint(QPainter &painter, const Viewport &view) const
{
    if (!item_) return;
    const auto &selected = *item_;
    const double scale = view.scale();
    const QPointF origin = QPointF(view.size.width() / 2, view.size.height() / 2) - view.center * scale;
    painter.save();
    painter.setClipRect(QRectF(origin, QSizeF(scale, scale)), Qt::IntersectClip);
    painter.setOpacity(1);
    for (int wrap = -1; wrap <= 1; ++wrap) {
        const auto offset = origin + QPointF(wrap * scale, 0);
        if (selected.kind == MapItem::Kind::Point) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Qt::white, 3));
            const auto center = selected.point * scale + offset;
            if (selected.markerOutline.isEmpty()) painter.drawPath(markerFrame(selected.symbol, center, selected.radius + 2));
            else {
                QTransform transform; transform.translate(center.x(), center.y()); transform.scale(selected.radius + 2, selected.radius + 2);
                painter.drawPath(transform.map(selected.markerOutline));
            }
        } else {
            const QColor inverse(255 - selected.color.red(), 255 - selected.color.green(), 255 - selected.color.blue());
            QPen pen(selected.kind == MapItem::Kind::Line ? inverse : QColor(Qt::white), selected.lineWidth + 2);
            pen.setCosmetic(true);
            if (selected.kind == MapItem::Kind::Polygon) pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(selected.kind == MapItem::Kind::Polygon ? QBrush(QColor(255, 255, 255, 35)) : Qt::NoBrush);
            painter.save(); painter.translate(offset); painter.scale(scale, scale);
            painter.drawPath(selected.path); painter.restore();
        }
    }
    painter.restore();
}
}
