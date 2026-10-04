#include "core/measure.h"

#include "core/polygon.h"
#include "core/projection.h"

#include <QObject>

#include <algorithm>
#include <cmath>
#include <numbers>
namespace swak {
double groundDistance(QPointF first, QPointF second)
{
    const auto a = unproject(first), b = unproject(second);
    constexpr double radians = std::numbers::pi / 180;
    const double lat = (b.y() - a.y()) * radians, lon = (b.x() - a.x()) * radians;
    const double h = std::pow(std::sin(lat / 2), 2) + std::cos(a.y() * radians) *
                                                          std::cos(b.y() * radians) *
                                                          std::pow(std::sin(lon / 2), 2);
    return 12742017.6 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
}
QString formatSpeed(double metersPerSecond, const QLocale &locale)
{
    if (!std::isfinite(metersPerSecond) || metersPerSecond < 0) {
        return {};
    }
    const bool metric = locale.measurementSystem() == QLocale::MetricSystem;
    return locale.toString(metersPerSecond * (metric ? 3.6 : 2.2369362921), 'f', 1) +
           QLatin1Char(' ') + (metric ? QObject::tr("km/h") : QObject::tr("mph"));
}
QString formatDistance(double meters, const QLocale &locale)
{
    if (!std::isfinite(meters) || meters < 0) {
        return {};
    }
    double value = meters;
    QString unit = QObject::tr("m");
    if (locale.measurementSystem() == QLocale::MetricSystem) {
        if (value >= 1000) {
            value /= 1000;
            unit = QObject::tr("km");
        }
    } else {
        value /= 0.3048;
        unit = QObject::tr("ft");
        if (value >= 5280) {
            value /= 5280;
            unit = QObject::tr("mi");
        }
    }
    return locale.toString(value, 'f', 2) + QLatin1Char(' ') + unit;
}
QPolygonF circleBoundary(QPointF center, QPointF edge)
{
    const double radius = std::hypot(edge.x() - center.x(), edge.y() - center.y());
    QPolygonF points;
    points.reserve(128);
    for (int i = 0; i < 128; ++i) {
        const double angle = i * 2 * std::numbers::pi / 128;
        points.append(center + QPointF(radius * std::cos(angle), radius * std::sin(angle)));
    }
    return points;
}
QString validateTwoPointShape(const QString &type, const QJsonObject &geometry)
{
    const auto points = polygonVertices(geometry);
    if (points.size() != 2) {
        return QObject::tr("Choose two points inside the map.");
    }
    if (std::any_of(points.cbegin(), points.cend(), [](QPointF p) {
            return !std::isfinite(p.x()) || !std::isfinite(p.y()) ||
                   !QRectF(0, 0, 1, 1).contains(p);
        })) {
        return QObject::tr("Choose two points inside the map.");
    }
    const double radius = std::hypot(points[1].x() - points[0].x(), points[1].y() - points[0].y());
    if (radius < 1e-10) {
        return QObject::tr("Choose two different points.");
    }
    if (type == QStringLiteral("range-circle") &&
        !QRectF(0, 0, 1, 1)
             .contains(
                 QRectF(points[0] - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius)))) {
        return QObject::tr("Keep the complete circle inside the map.");
    }
    return {};
}
} // namespace swak
