#include "core/map_decorations.h"

#include "core/measure.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
namespace swak {
ScaleLegend mapScaleLegend(const Viewport &view, const QLocale &locale, double bottom)
{
    if (view.size.width() < 80 || view.size.height() < 60 || !std::isfinite(view.zoom) ||
        view.zoom < 0 || view.zoom > 30 || !std::isfinite(view.center.y()) ||
        !std::isfinite(view.center.x()) || !std::isfinite(bottom) || view.center.x() < 0 ||
        view.center.x() > 1 || view.center.y() < 0 || view.center.y() > 1) {
        return {};
    }
    const QPointF pixel(16, (bottom < 0 ? view.size.height() : bottom) - 20);
    const auto a = view.coordinate(pixel), b = view.coordinate(pixel + QPointF(1, 0));
    const double perPixel = groundDistance(project(a.y(), a.x()), project(b.y(), b.x()));
    if (!std::isfinite(perPixel) || perPixel <= 0) {
        return {};
    }
    double unit = 1;
    QString suffix = QObject::tr("m");
    const double maximum = std::min(160.0, view.size.width() / 3) * perPixel;
    if (locale.measurementSystem() == QLocale::MetricSystem) {
        if (maximum >= 1000) {
            unit = 1000;
            suffix = QObject::tr("km");
        }
    } else {
        unit = .3048;
        suffix = QObject::tr("ft");
        if (maximum >= 1609.344) {
            unit = 1609.344;
            suffix = QObject::tr("mi");
        }
    }
    const double target = maximum / unit;
    const double base = std::pow(10.0, std::floor(std::log10(target)));
    const double nice = base * (target >= 5 * base ? 5 : target >= 2 * base ? 2 : 1);
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(nice))), 0, 5);
    return {nice * unit / perPixel,
            nice * unit,
            locale.toString(nice, 'f', decimals) + QLatin1Char(' ') + suffix};
}
void paintMapDecorations(QPainter &painter,
                         const Viewport &view,
                         bool scale,
                         bool zoom,
                         int minimumZoom,
                         int maximumZoom,
                         const QRegion &occlusion)
{
    if (view.size.isEmpty() || !std::isfinite(view.zoom) || view.zoom < 0 || view.zoom > 30) {
        return;
    }
    double bottom = view.size.height();
    for (int pass = 0; pass < 64; ++pass) {
        const double before = bottom;
        for (const auto &rectangle : occlusion) {
            if (rectangle.left() < 190 && rectangle.right() > 8 && rectangle.top() < bottom &&
                rectangle.bottom() > bottom - 50) {
                bottom = std::min(bottom, static_cast<double>(rectangle.top()) - 4);
            }
        }
        if (before == bottom || bottom < 60) {
            break;
        }
    }
    if (bottom < 60) {
        return;
    }
    painter.save();
    painter.setClipRect(QRectF(QPointF{}, view.size));
    if (scale) {
        const auto legend = mapScaleLegend(view, QLocale(), bottom);
        if (legend.pixels > 0) {
            const double y = bottom - 18;
            const auto width = std::max(
                legend.pixels + 12,
                static_cast<double>(painter.fontMetrics().horizontalAdvance(legend.text) + 12));
            painter.fillRect(QRectF(10,
                                    y - painter.fontMetrics().height() - 9,
                                    width,
                                    painter.fontMetrics().height() + 20),
                             QColor(255, 255, 255, 150));
            painter.setPen(QPen(Qt::black, 1.5));
            painter.drawLine(QPointF(16, y), QPointF(16 + legend.pixels, y));
            for (const double x : {16.0, 16 + legend.pixels}) {
                painter.drawLine(QPointF(x, y - 4), QPointF(x, y + 4));
            }
            painter.drawText(QPointF(16, y - 6), legend.text);
        }
    }
    if (zoom) {
        minimumZoom = std::clamp(minimumZoom, 0, 30);
        maximumZoom = std::clamp(maximumZoom, minimumZoom, 30);
        const double height = std::min(220.0, bottom * .4);
        const double top = (bottom - height) / 2;
        const int span = std::max(1, maximumZoom - minimumZoom);
        painter.setOpacity(.4);
        painter.fillRect(QRectF(7, top - 12, 43, height + 24), Qt::white);
        painter.setPen(QPen(Qt::black, 1));
        painter.drawLine(QPointF(15, top), QPointF(15, top + height));
        for (int level = minimumZoom; level <= maximumZoom; ++level) {
            const double y = top + (maximumZoom - level) * height / span;
            const bool current = level == qRound(view.zoom);
            painter.setPen(QPen(Qt::black, current ? 2 : 1));
            painter.drawLine(QPointF(15, y), QPointF(current ? 34 : 25, y));
            if (current || level == minimumZoom || level == maximumZoom) {
                painter.drawText(QPointF(35, y + 4), QLocale().toString(level));
            }
        }
    }
    painter.restore();
}
} // namespace swak
