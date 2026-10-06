#include "core/map_decorations.h"

#include "core/measure.h"

#include <QLineF>
#include <QList>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <utility>
namespace swak {
namespace {
// A white halo keeps black marks readable on any map without a background plate.
// All halos come first so that no halo covers the black line of a joined mark.
void drawHaloLines(QPainter &painter, const QList<std::pair<QLineF, double>> &lines)
{
    for (const auto &[line, width] : lines) {
        painter.setPen(QPen(QColor(255, 255, 255, 200), width + 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(line);
    }
    for (const auto &[line, width] : lines) {
        painter.setPen(QPen(Qt::black, width, Qt::SolidLine, Qt::FlatCap));
        painter.drawLine(line);
    }
}
void drawHaloText(QPainter &painter, QPointF baseline, const QString &text)
{
    QPainterPath path;
    path.addText(baseline, painter.font(), text);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.strokePath(
        path, QPen(QColor(255, 255, 255, 200), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.fillPath(path, Qt::black);
    painter.restore();
}
} // namespace
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
QRectF mapZoomLegendRect(const Viewport &view)
{
    if (view.size.width() < 80 || view.size.height() < 60) {
        return {};
    }
    const double height = std::min(220.0, view.size.height() * .4);
    return {view.size.width() - 68, (view.size.height() - height) / 2, 58, height};
}
std::optional<int>
mapZoomLevelAt(const Viewport &view, QPointF position, int minimumZoom, int maximumZoom)
{
    const auto rectangle = mapZoomLegendRect(view);
    if (rectangle.isEmpty() || !rectangle.adjusted(0, -10, 0, 10).contains(position)) {
        return {};
    }
    minimumZoom = std::clamp(minimumZoom, 0, 30);
    maximumZoom = std::clamp(maximumZoom, minimumZoom, 30);
    return std::clamp(qRound(maximumZoom - (position.y() - rectangle.top()) *
                                               (maximumZoom - minimumZoom) / rectangle.height()),
                      minimumZoom,
                      maximumZoom);
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
    painter.save();
    painter.setClipRect(QRectF(QPointF{}, view.size));
    if (scale && bottom >= 60) {
        const auto legend = mapScaleLegend(view, QLocale(), bottom);
        if (legend.pixels > 0) {
            const double y = bottom - 18;
            drawHaloLines(painter,
                          {{QLineF(16, y, 16 + legend.pixels, y), 1.5},
                           {QLineF(16, y - 4, 16, y + 4), 1.5},
                           {QLineF(16 + legend.pixels, y - 4, 16 + legend.pixels, y + 4), 1.5}});
            drawHaloText(painter, QPointF(16, y - 6), legend.text);
        }
    }
    if (zoom) {
        minimumZoom = std::clamp(minimumZoom, 0, 30);
        maximumZoom = std::clamp(maximumZoom, minimumZoom, 30);
        const auto rectangle = mapZoomLegendRect(view);
        if (!rectangle.isEmpty()) {
            const double height = rectangle.height(), top = rectangle.top();
            const double x = rectangle.left() + 7;
            const int span = std::max(1, maximumZoom - minimumZoom);
            painter.setOpacity(.6);
            QList<std::pair<QLineF, double>> marks{{QLineF(x, top, x, top + height), 1}};
            QList<std::pair<QPointF, QString>> labels;
            const auto metrics = painter.fontMetrics();
            for (int level = minimumZoom; level <= maximumZoom; ++level) {
                const double y = top + (maximumZoom - level) * height / span;
                const bool current = level == qRound(view.zoom);
                marks.append({QLineF(x, y, x + (current ? 19 : 10), y), current ? 2 : 1});
                if (current || level == minimumZoom || level == maximumZoom) {
                    const auto text = QLocale().toString(level);
                    labels.append({QPointF(rectangle.right() - 4 - metrics.horizontalAdvance(text),
                                           y + (metrics.ascent() - metrics.descent()) / 2.0),
                                   text});
                }
            }
            drawHaloLines(painter, marks);
            for (const auto &[baseline, text] : labels) {
                drawHaloText(painter, baseline, text);
            }
        }
    }
    painter.restore();
}
} // namespace swak
