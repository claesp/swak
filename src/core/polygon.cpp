#include "core/polygon.h"

#include "core/measure.h"

#include <QJsonArray>
#include <QLineF>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>
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
QPolygonF readVertices(const QJsonObject &geometry, int limit)
{
    const auto array = geometry.value(QStringLiteral("vertices")).toArray();
    if (array.size() > limit) {
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
} // namespace
QPolygonF polygonVertices(const QJsonObject &geometry)
{
    return readVertices(geometry, MaxPolygonVertices);
}
QPolygonF routePoints(const QJsonObject &geometry)
{
    return readVertices(geometry, MaxRoutePoints);
}
QString validateRoute(const QPolygonF &points)
{
    if (points.size() < 2 || points.size() > MaxRoutePoints) {
        return QObject::tr("Use 2 to %1 route points.").arg(MaxRoutePoints);
    }
    for (qsizetype i = 0; i < points.size(); ++i) {
        const auto point = points.at(i);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || point.x() < 0 ||
            point.x() > 1 || point.y() < 0 || point.y() > 1) {
            return QObject::tr("The route position is outside the map.");
        }
        if (i > 0 && points.at(i - 1) == point) {
            return QObject::tr("Neighboring route points must be distinct.");
        }
    }
    return {};
}
QPolygonF unwrappedRoute(const QPolygonF &points)
{
    QPolygonF result;
    for (auto point : points) {
        if (!result.isEmpty()) {
            point.rx() += std::round(result.last().x() - point.x());
        }
        result.append(point);
    }
    return result;
}
double routeLength(const QPolygonF &points)
{
    const auto line = unwrappedRoute(points);
    double total = 0;
    for (qsizetype i = 1; i < line.size(); ++i) {
        total += groundDistance(line.at(i - 1), line.at(i));
    }
    return total;
}
QPolygonF simplifiedRoute(const QPolygonF &points, int limit)
{
    if (points.size() <= limit || limit < 2) {
        return points;
    }
    const auto line = unwrappedRoute(points);
    // Douglas-Peucker with a growing tolerance in world units. The loop stops after 64 steps.
    double tolerance = 1e-9;
    QPolygonF result = line;
    for (int step = 0; step < 64 && result.size() > limit; ++step, tolerance *= 2) {
        std::vector<bool> keep(static_cast<size_t>(line.size()), false);
        keep.front() = keep.back() = true;
        std::vector<std::pair<qsizetype, qsizetype>> spans{{0, line.size() - 1}};
        while (!spans.empty()) {
            const auto [first, last] = spans.back();
            spans.pop_back();
            const QLineF chord(line.at(first), line.at(last));
            double farthest = -1;
            qsizetype index = -1;
            for (qsizetype i = first + 1; i < last; ++i) {
                const auto point = line.at(i);
                const auto d = chord.p2() - chord.p1();
                const double length = QPointF::dotProduct(d, d);
                const double t =
                    length > 0
                        ? std::clamp(QPointF::dotProduct(point - chord.p1(), d) / length, 0.0, 1.0)
                        : 0;
                const double distance = QLineF(point, chord.p1() + t * d).length();
                if (distance > farthest) {
                    farthest = distance;
                    index = i;
                }
            }
            if (index > 0 && farthest > tolerance) {
                keep[static_cast<size_t>(index)] = true;
                spans.emplace_back(first, index);
                spans.emplace_back(index, last);
            }
        }
        result.clear();
        for (qsizetype i = 0; i < line.size(); ++i) {
            if (keep[static_cast<size_t>(i)]) {
                result.append(line.at(i));
            }
        }
    }
    if (result.size() > limit) {
        // A very dense line can stay above the limit. Keep evenly spaced points.
        QPolygonF sparse;
        for (int i = 0; i < limit; ++i) {
            sparse.append(result.at(i * (result.size() - 1) / (limit - 1)));
        }
        result = sparse;
    }
    for (auto &point : result) {
        point.rx() -= std::floor(point.x());
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
