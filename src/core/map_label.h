#pragma once
#include "core/modern_colors.h"

#include <QPainter>

#include <atomic>

namespace swak {
enum class MapLabelStyle
{
    // White text on a dark box.
    Standard,
    // Light text in a dark box with a thin cyan border and a cyan tick, as in film interfaces.
    Modern
};
// The style of every map label. DisplayStyle sets it. Workers can read it, so it is atomic.
inline std::atomic<MapLabelStyle> &mapLabelStyleValue()
{
    static std::atomic<MapLabelStyle> value{MapLabelStyle::Standard};
    return value;
}
inline MapLabelStyle mapLabelStyle()
{
    return mapLabelStyleValue().load(std::memory_order_relaxed);
}
inline void setMapLabelStyle(MapLabelStyle style)
{
    mapLabelStyleValue().store(style, std::memory_order_relaxed);
}
inline QRectF mapLabelBounds(const QFontMetrics &metrics, QPointF baseline, const QString &text)
{
    if (text.isEmpty()) {
        return {};
    }
    const auto bounds = QRectF(metrics.boundingRect(metrics.elidedText(text, Qt::ElideRight, 240)))
                            .translated(baseline);
    // The modern box leaves room for its tick on the left.
    return mapLabelStyle() == MapLabelStyle::Modern ? bounds.adjusted(-7, -3, 5, 3)
                                                    : bounds.adjusted(-4, -2, 4, 2);
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
    if (mapLabelStyle() == MapLabelStyle::Modern) {
        const QColor accent(modern::Accent);
        auto border = accent;
        border.setAlpha(selected ? 255 : 150);
        painter.setPen(QPen(border, 1));
        painter.setBrush(selected ? accent : QColor(6, 10, 16, 200));
        painter.drawRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5));
        if (!selected) {
            painter.fillRect(QRectF(bounds.left(), bounds.top(), 3, bounds.height()), accent);
        }
        painter.setPen(selected ? QColor(4, 12, 20) : QColor(214, 244, 255));
    } else {
        painter.fillRect(bounds, selected ? QColor(255, 255, 255, 170) : QColor(0, 0, 0, 170));
        painter.setPen(selected ? Qt::black : Qt::white);
    }
    painter.drawText(baseline, label);
    painter.restore();
}
} // namespace swak
