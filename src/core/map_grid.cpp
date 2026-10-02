#include "core/map_grid.h"
#include <QPainter>
#include <QLocale>
#include <algorithm>
#include <cmath>
namespace swak {
namespace {
double interval(double target)
{
    const double base = std::pow(10.0, std::floor(std::log10(target)));
    for (double multiplier : {1.0, 2.0, 5.0, 10.0}) if (base * multiplier >= target) return base * multiplier;
    return base * 10;
}
QString label(double value, double step, bool longitude)
{
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 8);
    const QString direction = value == 0 ? QString{} : longitude ? (value < 0 ? QStringLiteral("W") : QStringLiteral("E"))
                                                               : (value < 0 ? QStringLiteral("S") : QStringLiteral("N"));
    return QLocale().toString(std::abs(value), 'f', decimals) + QChar(0x00b0) + direction;
}
}
void paintMapGrid(QPainter &painter, const Viewport &view, double opacity, double spacing)
{
    if (!std::isfinite(view.zoom) || view.zoom < 0 || view.zoom > 30 || view.size.isEmpty()
        || !std::isfinite(view.center.x()) || !std::isfinite(view.center.y()) || !std::isfinite(spacing) || !std::isfinite(opacity)) return;
    spacing = std::clamp(spacing, 80.0, 400.0);
    const auto topLeft = view.coordinate({0, 0});
    const auto bottomRight = view.coordinate({view.size.width(), view.size.height()});
    const double west = std::max(-180.0, topLeft.x()), east = std::min(180.0, bottomRight.x());
    const double south = std::max(-85.051129, bottomRight.y()), north = std::min(85.051129, topLeft.y());
    if (west > east || south > north) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setClipRect(QRectF(QPointF{}, view.size));
    painter.setPen(QPen(QColor(0, 0, 0, qRound(std::clamp(opacity, 0.0, 1.0) * 255)), 1));
    const auto metrics = painter.fontMetrics();
    const double height = metrics.height();
    double lonStep = interval(spacing * 360.0 / view.scale());
    lonStep = interval(std::max(spacing, 2.0 * metrics.horizontalAdvance(label(-180, lonStep, true))) * 360.0 / view.scale());
    // Latitude lines have the smallest separation nearest the equator.
    const double nearest = south > 0 ? south : north < 0 ? north : 0;
    const double latStep = interval(spacing * 360.0 * std::cos(nearest * std::acos(-1.0) / 180.0) / view.scale());
    for (int i = 0; i < 512; ++i) {
        const double longitude = (std::ceil(west / lonStep) + i) * lonStep;
        if (longitude > east + lonStep * 1e-8) break;
        const double x = view.screen(0, longitude).x();
        painter.drawLine(QPointF(x, 0), QPointF(x, view.size.height()));
        const auto text = label(longitude, lonStep, true);
        const double width = metrics.horizontalAdvance(text) + 8;
        const double left = std::clamp(x - width / 2, 0.0, std::max(0.0, view.size.width() - width));
        painter.drawText(QRectF(left, 2, width, height), Qt::AlignCenter, text);
        painter.drawText(QRectF(left, view.size.height() - height - 2, width, height), Qt::AlignCenter, text);
    }
    double lastY = -1000;
    for (int i = 0; i < 512; ++i) {
        const double latitude = (std::floor(north / latStep) - i) * latStep;
        if (latitude < south - latStep * 1e-8) break;
        const double y = view.screen(latitude, 0).y();
        painter.drawLine(QPointF(0, y), QPointF(view.size.width(), y));
        if (y < height * 2 || y > view.size.height() - height * 2 || y - lastY < height * 2) continue;
        const auto text = label(latitude, latStep, false);
        const double width = metrics.horizontalAdvance(text) + 8;
        painter.drawText(QRectF(2, y - height / 2, width, height), Qt::AlignLeft | Qt::AlignVCenter, text);
        painter.drawText(QRectF(view.size.width() - width - 2, y - height / 2, width, height), Qt::AlignRight | Qt::AlignVCenter, text);
        lastY = y;
    }
    painter.restore();
}
}
