#include "core/projection.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace swak {
QPointF project(double latitude, double longitude)
{
    const double radians =
        std::clamp(latitude, -85.05112878, 85.05112878) * std::numbers::pi / 180.0;
    return {(longitude + 180) / 360, (1 - std::asinh(std::tan(radians)) / std::numbers::pi) / 2};
}
QPointF unproject(QPointF point)
{
    return {point.x() * 360 - 180,
            std::atan(std::sinh(std::numbers::pi * (1 - 2 * point.y()))) * 180 / std::numbers::pi};
}
double Viewport::markerSize(double baseSize) const
{
    switch (markerScalePercent) {
    case 50:
    case 75:
    case 100:
    case 125:
    case 150:
    case 200:
        return baseSize * markerScalePercent / 100.0;
    default:
        return baseSize;
    }
}
QPointF Viewport::markerOffset(QPointF offset, double radius) const
{
    return offset + QPointF(markerSize(radius) - radius, 0);
}
double Viewport::scale() const
{
    return 256 * std::exp2(zoom);
}
QPointF Viewport::screen(double latitude, double longitude) const
{
    return (project(latitude, longitude) - center) * scale() +
           QPointF(size.width() / 2, size.height() / 2);
}
QPointF Viewport::toScreen(QPointF world) const
{
    return (world - center) * scale() + QPointF(size.width() / 2, size.height() / 2);
}
QPointF Viewport::toWorld(QPointF screenPoint) const
{
    return center + (screenPoint - QPointF(size.width() / 2, size.height() / 2)) / scale();
}
QPointF Viewport::coordinate(QPointF screenPoint) const
{
    return unproject((screenPoint - QPointF(size.width() / 2, size.height() / 2)) / scale() +
                     center);
}
} // namespace swak
