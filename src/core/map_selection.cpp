#include "core/map_selection.h"

#include "core/marker_symbols.h"

#include <QPainter>
#include <QTextDocument>
#include <QTextDocumentFragment>

#include <algorithm>
#include <cmath>
#include <optional>
namespace swak {
QString displayDescription(const MapItem &item)
{
    if (!item.owner.startsWith(QStringLiteral("org.swak.data/")) ||
        !Qt::mightBeRichText(item.description)) {
        return item.description;
    }
    // Rich text conversion only reads markup here. It loads no images or links.
    auto text = QTextDocumentFragment::fromHtml(item.description).toPlainText();
    text.replace(QChar::LineSeparator, QLatin1Char('\n'))
        .replace(QChar::ParagraphSeparator, QLatin1Char('\n'))
        .replace(QChar::Nbsp, QLatin1Char(' '));
    return text.trimmed();
}
MapItem cotMapItem(const Event &message)
{
    MapItem item;
    item.owner = QStringLiteral("org.swak.events");
    item.label = message.callsign.isEmpty() ? message.uid : message.callsign;
    item.type = message.type;
    item.point = project(message.latitude, message.longitude);
    item.color = Qt::white;
    item.symbol = markerSymbolForCot(message.type);
    item.properties = {{QStringLiteral("UID"), message.uid},
                       {QStringLiteral("How"), message.how},
                       {QStringLiteral("Latitude"), message.latitude},
                       {QStringLiteral("Longitude"), message.longitude},
                       {QStringLiteral("Altitude"), message.altitude},
                       {QStringLiteral("Circular error"), message.circularError},
                       {QStringLiteral("Linear error"), message.linearError},
                       {QStringLiteral("Time"), message.time.toString(Qt::ISODateWithMs)},
                       {QStringLiteral("Start"), message.start.toString(Qt::ISODateWithMs)},
                       {QStringLiteral("Stale"), message.stale.toString(Qt::ISODateWithMs)}};
    item.id = message.uid;
    return item;
}

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
            const double radius =
                (selected.scaleIcon ? view.markerSize(selected.radius) : selected.radius) + 2;
            if (selected.markerOutline.isEmpty()) {
                painter.drawPath(markerFrame(selected.symbol, center, radius));
            } else {
                QTransform transform;
                transform.translate(center.x(), center.y());
                transform.scale(radius, radius);
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
            // Clamp before the conversion. A deep zoom can exceed the int range.
            const double pixels = selected.cachedLabelFontHeight * scale;
            font.setPixelSize(std::isfinite(pixels) ? qRound(std::clamp(pixels, 1.0, 256.0)) : 1);
            painter.setFont(font);
            painter.drawText(screenBounds, Qt::AlignCenter, selected.cachedLabelText);
            painter.restore();
        }
    }
    painter.restore();
}
} // namespace swak
