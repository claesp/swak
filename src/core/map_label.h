#pragma once
#include <QPainter>
namespace swak {
inline QRectF mapLabelBounds(const QFontMetrics &metrics, QPointF baseline, const QString &text)
{
    if (text.isEmpty()) {
        return {};
    }
    return QRectF(metrics.boundingRect(metrics.elidedText(text, Qt::ElideRight, 240)))
        .translated(baseline)
        .adjusted(-4, -2, 4, 2);
}
inline void
paintMapLabel(QPainter &painter, QPointF baseline, const QString &text, bool selected = false)
{
    if (text.isEmpty()) {
        return;
    }
    painter.save();
    const auto label = painter.fontMetrics().elidedText(text, Qt::ElideRight, 240);
    const auto bounds = mapLabelBounds(painter.fontMetrics(), baseline, text);
    painter.fillRect(bounds, selected ? QColor(255, 255, 255, 170) : QColor(0, 0, 0, 170));
    painter.setPen(selected ? Qt::black : Qt::white);
    painter.drawText(baseline, label);
    painter.restore();
}
} // namespace swak
