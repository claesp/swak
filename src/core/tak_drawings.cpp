#include "core/tak_drawings.h"

#include "core/cot_detail.h"
#include "core/projection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace swak {
namespace {
constexpr int EllipseSegments = 72;
constexpr double MaxRadiusMeters = 1000000;
constexpr double MetersPerDegree = 111320;

std::optional<QPointF> readPoint(const QString &text)
{
    const auto parts = QStringView(text).split(u',');
    if (parts.size() < 2 || parts.size() > 3) {
        return {};
    }
    bool latitudeOk = false;
    bool longitudeOk = false;
    const double latitude = parts.at(0).trimmed().toDouble(&latitudeOk);
    const double longitude = parts.at(1).trimmed().toDouble(&longitudeOk);
    // Mercator ends near 85 degrees. Shapes beyond it cannot show on the map.
    if (!latitudeOk || !longitudeOk || !std::isfinite(latitude) || !std::isfinite(longitude) ||
        std::abs(latitude) > 85.05 || std::abs(longitude) > 180) {
        return {};
    }
    return project(latitude, longitude);
}
QPolygonF linkPoints(const Event &event)
{
    const auto all = links(event);
    if (all.size() > MaxDrawingPoints) {
        return {};
    }
    QPolygonF result;
    for (const auto &link : all) {
        if (link.point.isEmpty()) {
            continue;
        }
        const auto point = readPoint(link.point);
        if (!point) {
            return {};
        }
        result.append(*point);
    }
    return result;
}
std::optional<double> readMeters(const CotElement &element, QStringView name)
{
    bool ok = false;
    const double value = element.attribute(name).toDouble(&ok);
    if (!ok || !std::isfinite(value) || value <= 0 || value > MaxRadiusMeters) {
        return {};
    }
    return value;
}
// Builds the ellipse boundary from radii in meters and a rotation in degrees from north.
QPolygonF ellipse(double latitude, double longitude, double major, double minor, double angle)
{
    QPolygonF result;
    const double radians = std::numbers::pi / 180;
    const double rotation = angle * radians;
    const double east = std::max(std::cos(latitude * radians), 0.01) * MetersPerDegree;
    for (int i = 0; i < EllipseSegments; ++i) {
        const double t = 2 * std::numbers::pi * i / EllipseSegments;
        const double x = minor * std::cos(t);
        const double y = major * std::sin(t);
        const double eastMeters = x * std::cos(rotation) + y * std::sin(rotation);
        const double northMeters = -x * std::sin(rotation) + y * std::cos(rotation);
        const double pointLatitude =
            std::clamp(latitude + northMeters / MetersPerDegree, -85.0, 85.0);
        result.append(project(pointLatitude, longitude + eastMeters / east));
    }
    return result;
}
QColor attributeColor(const std::optional<CotElement> &element, QStringView name)
{
    if (!element) {
        return {};
    }
    bool ok = false;
    const qint64 value = element->attribute(name).toLongLong(&ok);
    if (!ok || value < std::numeric_limits<qint32>::min() ||
        value > std::numeric_limits<quint32>::max()) {
        return {};
    }
    return QColor::fromRgba(static_cast<QRgb>(static_cast<quint32>(value)));
}
} // namespace

bool takDrawingType(const QString &type)
{
    return type == u"u-d-c-c" || type == u"u-d-c-e" || type == u"u-d-f" || type == u"u-d-f-m" ||
           type == u"u-d-r" || type == u"b-m-r";
}
std::optional<TakDrawing> readTakDrawing(const Event &event)
{
    if (!takDrawingType(event.type) || event.detailTruncated) {
        return {};
    }
    TakDrawing drawing;
    drawing.stroke = detailColor(event, u"strokeColor").value_or(QColor(Qt::white));
    drawing.fill = detailColor(event, u"fillColor").value_or(QColor());
    if (const auto weight = detailElement(event, u"strokeWeight")) {
        bool ok = false;
        const double value = weight->attribute(u"value").toDouble(&ok);
        if (ok && std::isfinite(value)) {
            drawing.strokeWidth = std::clamp(value, 1.0, 10.0);
        }
    }
    if (event.type.startsWith(u"u-d-c-")) {
        const auto shape = detailElement(event, u"shape");
        const auto *element = shape ? shape->child(u"ellipse") : nullptr;
        if (!element || std::abs(event.latitude) > 85.05) {
            return {};
        }
        const auto major = readMeters(*element, u"major");
        const auto minor = readMeters(*element, u"minor");
        if (!major || !minor) {
            return {};
        }
        bool angleOk = false;
        double angle = element->attribute(u"angle").toDouble(&angleOk);
        if (!angleOk || !std::isfinite(angle)) {
            angle = 0;
        }
        drawing.kind = TakDrawing::Kind::Circle;
        drawing.points = ellipse(event.latitude, event.longitude, *major, *minor, angle);
        drawing.anchor = project(event.latitude, event.longitude);
        return drawing;
    }
    drawing.points = linkPoints(event);
    if (drawing.points.size() < 2) {
        return {};
    }
    drawing.anchor = drawing.points.boundingRect().center();
    if (event.type == u"b-m-r") {
        drawing.kind = TakDrawing::Kind::Route;
        const auto color = attributeColor(detailElement(event, u"link_attr"), u"color");
        if (color.isValid()) {
            drawing.stroke = color;
        }
        drawing.fill = QColor();
        drawing.anchor = drawing.points.first();
        return drawing;
    }
    // TAK closes an area with the first point again. A rectangle has four corners.
    const bool closed =
        drawing.points.size() >= 4 && drawing.points.first() == drawing.points.last();
    if (closed) {
        drawing.points.removeLast();
    }
    if (closed || (event.type == u"u-d-r" && drawing.points.size() >= 3)) {
        drawing.kind = TakDrawing::Kind::Area;
    } else {
        drawing.kind = TakDrawing::Kind::Line;
        drawing.fill = QColor();
    }
    return drawing;
}
} // namespace swak
