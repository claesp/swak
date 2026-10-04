#include "core/map_selection.h"

#include "core/marker_symbols.h"

#include <QPainter>

#include <algorithm>
#include <optional>
namespace swak {
void MapSelection::paint(QPainter &painter,
                         const Viewport &view,
                         std::optional<QPointF> point,
                         double opacity) const
{
    if (!item_) {
        return;
    }
    const auto &selected = *item_;
    const double scale = view.scale();
    const QPointF origin =
        QPointF(view.size.width() / 2, view.size.height() / 2) - view.center * scale;
    painter.save();
    painter.setClipRect(QRectF(origin, QSizeF(scale, scale)), Qt::IntersectClip);
    painter.setOpacity(opacity);
    for (int wrap = -1; wrap <= 1; ++wrap) {
        const auto offset = origin + QPointF(wrap * scale, 0);
        if (selected.kind == MapItem::Kind::Point) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Qt::white, 3));
            const auto center = point.value_or(selected.point) * scale + offset;
            if (selected.markerOutline.isEmpty()) {
                painter.drawPath(markerFrame(selected.symbol, center, selected.radius + 2));
            } else {
                QTransform transform;
                transform.translate(center.x(), center.y());
                transform.scale(selected.radius + 2, selected.radius + 2);
                painter.drawPath(transform.map(selected.markerOutline));
            }
        } else {
            const QColor inverse(255 - selected.color.red(),
                                 255 - selected.color.green(),
                                 255 - selected.color.blue());
            QPen pen(selected.kind == MapItem::Kind::Line ? inverse : QColor(Qt::white),
                     selected.kind == MapItem::Kind::Polygon ? selected.lineWidth
                                                             : selected.lineWidth + 2);
            if (selected.kind == MapItem::Kind::Polygon) {
                pen.setStyle(selected.outlineStyle);
            }
            painter.setPen(pen);
            painter.setBrush(selected.kind == MapItem::Kind::Polygon
                                 ? QBrush(QColor(255, 255, 255, 35))
                                 : Qt::NoBrush);
            // Stroke in screen coordinates. Large painter scales distort cosmetic pens in OpenGL.
            QTransform transform;
            transform.translate(offset.x(), offset.y());
            transform.scale(scale, scale);
            const auto bounds =
                transform.mapRect(selected.path.boundingRect())
                    .adjusted(-pen.widthF(), -pen.widthF(), pen.widthF(), pen.widthF());
            if (QRectF(QPointF{}, view.size).intersects(bounds)) {
                painter.drawPath(transform.map(selected.path));
            }
        }
        if (!selected.cachedLabelBounds.isEmpty()) {
            painter.save();
            const auto bounds = selected.cachedLabelBounds;
            const QRectF screenBounds(bounds.topLeft() * scale + offset, bounds.size() * scale);
            painter.fillRect(screenBounds, Qt::black);
            painter.setPen(Qt::white);
            auto font = painter.font();
            font.setPixelSize(std::clamp(qRound(selected.cachedLabelFontHeight * scale), 1, 256));
            painter.setFont(font);
            painter.drawText(screenBounds, Qt::AlignCenter, selected.cachedLabelText);
            painter.restore();
        }
    }
    painter.restore();
}
} // namespace swak
