#pragma once
#include <QPainter>
namespace swak {
inline void paintMapLabel(QPainter &painter, QPointF baseline, const QString &text)
{
    if (text.isEmpty()) return;
    painter.save();
    const auto label = painter.fontMetrics().elidedText(text, Qt::ElideRight, 240);
    const auto bounds = QRectF(painter.fontMetrics().boundingRect(label)).translated(baseline).adjusted(-4, -2, 4, 2);
    painter.fillRect(bounds, QColor(0, 0, 0, 170));
    painter.setPen(Qt::white); painter.drawText(baseline, label);
    painter.restore();
}
}
