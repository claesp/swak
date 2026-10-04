#include "core/polygon.h"

#include <QJsonArray>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <numbers>
namespace swak {
namespace {
double cross(QPointF a, QPointF b)
{
    return a.x() * b.y() - a.y() * b.x();
}
bool intersects(QPointF a, QPointF b, QPointF c, QPointF d)
{
    const double abC = cross(b - a, c - a), abD = cross(b - a, d - a);
    const double cdA = cross(d - c, a - c), cdB = cross(d - c, b - c);
    if (std::max(a.x(), b.x()) < std::min(c.x(), d.x()) ||
        std::max(c.x(), d.x()) < std::min(a.x(), b.x()) ||
        std::max(a.y(), b.y()) < std::min(c.y(), d.y()) ||
        std::max(c.y(), d.y()) < std::min(a.y(), b.y())) {
        return false;
    }
    return abC * abD <= 0 && cdA * cdB <= 0;
}
} // namespace
QPolygonF polygonVertices(const QJsonObject &geometry)
{
    const auto array = geometry.value(QStringLiteral("vertices")).toArray();
    if (array.size() > MaxPolygonVertices) {
        return {};
    }
    QPolygonF result;
    for (const auto &value : array) {
        const auto point = value.toArray();
        if (point.size() != 2 || !point.at(0).isDouble() || !point.at(1).isDouble()) {
            return {};
        }
        result.append({point.at(0).toDouble(), point.at(1).toDouble()});
    }
    return result;
}
QJsonObject polygonGeometry(const QPolygonF &points)
{
    QJsonArray array;
    for (const auto &point : points) {
        array.append(QJsonArray{point.x(), point.y()});
    }
    return {{QStringLiteral("vertices"), array}};
}
QString validatePolygon(const QPolygonF &points)
{
    const auto count = points.size();
    if (count < 3 || count > MaxPolygonVertices) {
        return QObject::tr("Use 3 to 64 distinct polygon corners.");
    }
    for (qsizetype i = 0; i < count; ++i) {
        const auto a = points.at(i);
        if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || a.x() < 0 || a.x() > 1 || a.y() < 0 ||
            a.y() > 1) {
            return QObject::tr("The polygon position is outside the map.");
        }
        for (qsizetype j = 0; j < i; ++j) {
            if (points.at(j) == a) {
                return QObject::tr("Polygon corners must be distinct.");
            }
        }
    }
    double twiceArea = 0;
    for (qsizetype i = 0; i < count; ++i) {
        const auto a = points.at(i), b = points.at((i + 1) % count), c = points.at((i + 2) % count);
        if (cross(b - a, c - b) == 0 && QPointF::dotProduct(b - a, c - b) < 0) {
            return QObject::tr("Polygon edges cannot overlap.");
        }
        twiceArea += cross(a - points.first(), b - points.first());
        for (qsizetype j = i + 2; j < count; ++j) {
            if (i == 0 && j == count - 1) {
                continue;
            }
            if (intersects(a, b, points.at(j), points.at((j + 1) % count))) {
                return QObject::tr("Polygon edges cannot cross or touch.");
            }
        }
    }
    if (std::abs(twiceArea) < 1e-24) {
        return QObject::tr("The polygon needs a nonzero area.");
    }
    return {};
}
QPointF polygonLabelPoint(const QPolygonF &points)
{
    if (points.size() < 3) {
        return {};
    }
    QPointF weighted;
    double area = 0;
    const auto origin = points.first();
    for (qsizetype i = 0; i < points.size(); ++i) {
        const auto a = points.at(i) - origin, b = points.at((i + 1) % points.size()) - origin;
        const double weight = cross(a, b);
        area += weight;
        weighted += (a + b) * weight;
    }
    const auto center =
        std::abs(area) > 1e-24 ? origin + weighted / (3 * area) : points.boundingRect().center();
    if (points.containsPoint(center, Qt::OddEvenFill)) {
        return center;
    }
    // A concave outline can place its centroid outside the area. Use an interior span.
    const double y = center.y();
    QList<double> crossings;
    for (qsizetype i = 0; i < points.size(); ++i) {
        const auto a = points.at(i), b = points.at((i + 1) % points.size());
        if ((a.y() > y) != (b.y() > y)) {
            crossings.append(a.x() + (y - a.y()) * (b.x() - a.x()) / (b.y() - a.y()));
        }
    }
    std::sort(crossings.begin(), crossings.end());
    double width = -1;
    QPointF result = center;
    for (qsizetype i = 0; i + 1 < crossings.size(); i += 2) {
        if (crossings.at(i + 1) - crossings.at(i) > width) {
            width = crossings.at(i + 1) - crossings.at(i);
            result = {(crossings.at(i) + crossings.at(i + 1)) / 2, y};
        }
    }
    return result;
}
double polygonArea(const QPolygonF &points)
{
    if (points.size() < 3) {
        return 0;
    }
    // Integrate spherical surface area along the straight Mercator edges that the map draws.
    constexpr double radius = 6371008.8;
    constexpr double tau = 2 * std::numbers::pi;
    double integral = 0;
    for (qsizetype i = 0; i < points.size(); ++i) {
        const auto a = points.at(i), b = points.at((i + 1) % points.size());
        double mean = 0;
        for (int step = 0; step <= 32; ++step) {
            const double y = a.y() + (b.y() - a.y()) * step / 32;
            mean += (step == 0 || step == 32 ? 1
                     : step % 2              ? 4
                                             : 2) *
                    std::tanh(std::numbers::pi * (1 - 2 * y));
        }
        integral += (b.x() - a.x()) * tau * mean / 96;
    }
    return std::abs(integral) * radius * radius;
}
QString formatArea(double squareMeters, const QLocale &locale)
{
    if (!std::isfinite(squareMeters) || squareMeters < 0) {
        return {};
    }
    double value = squareMeters;
    QString unit;
    if (locale.measurementSystem() == QLocale::MetricSystem) {
        unit = QObject::tr("m²");
        if (value >= 1000) {
            value /= 1000000;
            unit = QObject::tr("km²");
        }
    } else {
        value /= 0.09290304;
        unit = QObject::tr("ft²");
        if (value >= 1000) {
            value /= 43560;
            unit = QObject::tr("acres");
        }
        if (value >= 640 && unit == QObject::tr("acres")) {
            value /= 640;
            unit = QObject::tr("mi²");
        }
    }
    return locale.toString(value, 'f', value < 1 ? 4 : 2) + QLatin1Char(' ') + unit;
}
} // namespace swak
