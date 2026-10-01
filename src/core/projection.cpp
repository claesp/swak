#include "core/projection.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace swak {
QPointF project(double latitude, double longitude)
{
    const double radians = std::clamp(latitude, -85.05112878, 85.05112878) * std::numbers::pi / 180.0;
    return {(longitude + 180) / 360, (1 - std::asinh(std::tan(radians)) / std::numbers::pi) / 2};
}
QPointF unproject(QPointF point)
{
    return {point.x() * 360 - 180, std::atan(std::sinh(std::numbers::pi * (1 - 2 * point.y()))) * 180 / std::numbers::pi};
}
double Viewport::scale() const { return 256 * std::exp2(zoom); }
QPointF Viewport::screen(double latitude, double longitude) const
{
    return (project(latitude, longitude) - center) * scale() + QPointF(size.width() / 2, size.height() / 2);
}
QPointF Viewport::coordinate(QPointF screenPoint) const
{
    return unproject((screenPoint - QPointF(size.width() / 2, size.height() / 2)) / scale() + center);
}
}
