#include "core/map_grid.h"

#include "core/grid_types.h"
#include "core/measure.h"

#include <QLocale>
#include <QPainter>
#include <QPainterPath>

#include <GeographicLib/MGRS.hpp>
#include <GeographicLib/UTMUPS.hpp>
#include <algorithm>
#include <cmath>
namespace swak {
namespace {
double interval(double target)
{
    const double base = std::pow(10.0, std::floor(std::log10(target)));
    for (double multiplier : {1.0, 2.0, 5.0, 10.0}) {
        if (base * multiplier >= target) {
            return base * multiplier;
        }
    }
    return base * 10;
}
QString label(double value, double step, bool longitude, int format = 0)
{
    const int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 8);
    const QString direction = value == 0  ? QString{}
                              : longitude ? (value < 0 ? QStringLiteral("W") : QStringLiteral("E"))
                                          : (value < 0 ? QStringLiteral("S") : QStringLiteral("N"));
    if (format == 1 || format == 2) {
        const double factor = format == 1 ? 60 : 3600;
        const int places =
            std::clamp(static_cast<int>(std::ceil(-std::log10(step * factor))), 0, 8);
        const double rounding = std::pow(10.0, places);
        const double units = std::round(std::abs(value) * factor * rounding) / rounding;
        const auto degrees = static_cast<qint64>(std::floor(units / factor));
        const double remainder = units - static_cast<double>(degrees) * factor;
        auto text = QLocale().toString(degrees) + QChar(0x00b0);
        if (format == 1) {
            text += QLocale().toString(remainder, 'f', places) + QChar(0x2032);
        } else {
            const auto minutes = static_cast<qint64>(std::floor(remainder / 60));
            text += QLocale().toString(minutes) + QChar(0x2032);
            text += QLocale().toString(remainder - static_cast<double>(minutes) * 60, 'f', places) +
                    QChar(0x2033);
        }
        return text + direction;
    }
    return QLocale().toString(std::abs(value), 'f', decimals) + QChar(0x00b0) + direction;
}
} // namespace
void paintMapGrid(QPainter &painter,
                  const Viewport &view,
                  double opacity,
                  double spacing,
                  QColor color,
                  double lineWidth,
                  int format)
{
    if (!std::isfinite(view.zoom) || view.zoom < 0 || view.zoom > 30 || view.size.isEmpty() ||
        !std::isfinite(view.center.x()) || !std::isfinite(view.center.y()) ||
        !std::isfinite(spacing) || !std::isfinite(opacity) || !std::isfinite(lineWidth)) {
        return;
    }
    spacing = std::clamp(spacing, 80.0, 400.0);
    const auto topLeft = view.coordinate({0, 0});
    const auto bottomRight = view.coordinate({view.size.width(), view.size.height()});
    const double west = std::max(-180.0, topLeft.x()), east = std::min(180.0, bottomRight.x());
    const double south = std::max(-85.051129, bottomRight.y()),
                 north = std::min(85.051129, topLeft.y());
    if (west > east || south > north) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(QRectF(QPointF{}, view.size));
    color.setAlphaF(static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
    painter.setPen(QPen(color, std::clamp(lineWidth, .5, 3.0)));
    const auto metrics = painter.fontMetrics();
    const double height = metrics.height();
    double lonStep = interval(spacing * 360.0 / view.scale());
    lonStep =
        interval(std::max(spacing, 2.0 * metrics.horizontalAdvance(label(-180, lonStep, true))) *
                 360.0 / view.scale());
    // Latitude lines have the smallest separation nearest the equator.
    const double nearest = south > 0 ? south : north < 0 ? north : 0;
    const double latStep =
        interval(spacing * 360.0 * std::cos(nearest * std::acos(-1.0) / 180.0) / view.scale());
    for (int i = 0; i < 512; ++i) {
        const double longitude = (std::ceil(west / lonStep) + i) * lonStep;
        if (longitude > east + lonStep * 1e-8) {
            break;
        }
        const double x = view.screen(0, longitude).x();
        painter.drawLine(QPointF(x, 0), QPointF(x, view.size.height()));
        const auto text = label(longitude, lonStep, true, format);
        const double width = metrics.horizontalAdvance(text) + 8;
        const double left =
            std::clamp(x - width / 2, 0.0, std::max(0.0, view.size.width() - width));
        painter.drawText(QRectF(left, 2, width, height), Qt::AlignCenter, text);
        painter.drawText(
            QRectF(left, view.size.height() - height - 2, width, height), Qt::AlignCenter, text);
    }
    double lastY = -1000;
    for (int i = 0; i < 512; ++i) {
        const double latitude = (std::floor(north / latStep) - i) * latStep;
        if (latitude < south - latStep * 1e-8) {
            break;
        }
        const double y = view.screen(latitude, 0).y();
        painter.drawLine(QPointF(0, y), QPointF(view.size.width(), y));
        if (y < height * 2 || y > view.size.height() - height * 2 || y - lastY < height * 2) {
            continue;
        }
        const auto text = label(latitude, latStep, false, format);
        const double width = metrics.horizontalAdvance(text) + 8;
        painter.drawText(
            QRectF(2, y - height / 2, width, height), Qt::AlignLeft | Qt::AlignVCenter, text);
        painter.drawText(QRectF(view.size.width() - width - 2, y - height / 2, width, height),
                         Qt::AlignRight | Qt::AlignVCenter,
                         text);
        lastY = y;
    }
    painter.restore();
}
} // namespace swak

namespace swak {
QString gridCoordinate(double latitude, double longitude, const QString &system, int precision)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || std::abs(latitude) > 90 ||
        std::abs(longitude) > 180 ||
        (system != QLatin1String("utm") && system != QLatin1String("mgrs"))) {
        return {};
    }
    try {
        int zone;
        bool north;
        double x, y;
        GeographicLib::UTMUPS::Forward(latitude, longitude, zone, north, x, y);
        if (system == QLatin1String("mgrs")) {
            std::string text;
            GeographicLib::MGRS::Forward(
                zone, north, x, y, latitude, std::clamp(precision, -1, 5), text);
            return QString::fromStdString(text);
        }
        const auto name = zone == 0 ? QStringLiteral("UPS") : QStringLiteral("UTM %1").arg(zone);
        return QStringLiteral("%1%2 %3 E %4 N")
            .arg(name,
                 north ? QStringLiteral("N") : QStringLiteral("S"),
                 QLocale().toString(x, 'f', 0),
                 QLocale().toString(y, 'f', 0));
    } catch (const GeographicLib::GeographicErr &) {
        return {};
    }
}
namespace {
void metricCell(QPainter &painter,
                const Viewport &view,
                QRectF bounds,
                int zone,
                bool north,
                const QJsonObject &type)
{
    const auto a = view.screen(bounds.bottom(), bounds.left()),
               b = view.screen(bounds.top(), bounds.right());
    const QRectF cell = QRectF(a, b).normalized();
    const auto visible = cell.intersected(QRectF(QPointF{}, view.size));
    if (visible.width() < 1 || visible.height() < 1) {
        return;
    }
    painter.save();
    painter.setClipRect(visible, Qt::IntersectClip);
    painter.drawRect(cell);
    const auto center = view.coordinate(visible.center());
    const bool mgrs = type.value("system").toString() == QLatin1String("mgrs");
    const auto zoneName =
        mgrs ? gridCoordinate(center.y(), center.x(), "mgrs", -1)
             : (zone == 0 ? QStringLiteral("UPS") : QStringLiteral("UTM %1").arg(zone)) +
                   (north ? QStringLiteral("N") : QStringLiteral("S"));
    if (visible.width() > 60 && visible.height() > 30) {
        painter.drawText(visible.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, zoneName);
    }
    if (view.zoom < 6) {
        painter.restore();
        return;
    }
    try {
        double minX = 1e20, maxX = -1e20, minY = 1e20, maxY = -1e20;
        for (int row = 0; row <= 4; ++row) {
            for (int col = 0; col <= 4; ++col) {
                const auto coordinate =
                    view.coordinate(visible.topLeft() +
                                    QPointF(visible.width() * col / 4, visible.height() * row / 4));
                int actualZone;
                bool hemisphere;
                double x, y;
                GeographicLib::UTMUPS::Forward(
                    coordinate.y(), coordinate.x(), actualZone, hemisphere, x, y, zone);
                if (hemisphere != north && zone != 0) {
                    y += north ? -10000000 : 10000000;
                }
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        const double spacing = type.value("spacing").toDouble(140);
        const double metersPerPixel =
            groundDistance(project(center.y(), center.x()),
                           project(view.coordinate(visible.center() + QPointF(1, 0)).y(),
                                   view.coordinate(visible.center() + QPointF(1, 0)).x()));
        const double target =
            std::max({1.0, spacing * metersPerPixel, (maxX - minX) / 48, (maxY - minY) / 48});
        const double step =
            mgrs ? std::max(std::pow(10.0, std::round(std::log10(target))),
                            std::pow(10.0,
                                     std::ceil(std::log10(
                                         std::max(1.0, std::max(maxX - minX, maxY - minY) / 48)))))
                 : interval(target);
        const auto point = [&](double x, double y) {
            double lat, lon;
            GeographicLib::UTMUPS::Reverse(zone, north, x, y, lat, lon);
            return view.screen(lat, lon);
        };
        QList<QRectF> labelBounds;
        for (const bool vertical : {true, false}) {
            const double start = vertical ? minX : minY, end = vertical ? maxX : maxY;
            for (int line = 0; line < 50; ++line) {
                const double fixed = (std::ceil(start / step) + line) * step;
                if (fixed > end) {
                    break;
                }
                QPainterPath path;
                QPointF labelPoint;
                QPointF labelCoordinate;
                bool labelled = false;
                for (int sample = 0; sample <= 32; ++sample) {
                    const double variable = (vertical ? minY : minX) +
                                            (vertical ? maxY - minY : maxX - minX) * sample / 32;
                    const auto p = point(vertical ? fixed : variable, vertical ? variable : fixed);
                    if (sample == 0) {
                        path.moveTo(p);
                    } else {
                        path.lineTo(p);
                    }
                    if (!labelled && visible.adjusted(4, 24, -4, -16).contains(p)) {
                        labelPoint = p;
                        labelCoordinate = {vertical ? fixed : variable,
                                           vertical ? variable : fixed};
                        labelled = true;
                    }
                }
                painter.drawPath(path);
                if (labelled) {
                    QString text;
                    if (mgrs) {
                        const auto ll = view.coordinate(labelPoint);
                        std::string reference;
                        GeographicLib::MGRS::Forward(
                            zone,
                            north,
                            labelCoordinate.x(),
                            labelCoordinate.y(),
                            ll.y(),
                            std::clamp(5 - static_cast<int>(std::log10(step)), 0, 5),
                            reference);
                        text = QString::fromStdString(reference);
                    } else {
                        text = QLocale().toString(fixed, 'f', 0) +
                               (vertical ? QStringLiteral(" E") : QStringLiteral(" N"));
                    }
                    const auto baseline = labelPoint + QPointF(3, -3);
                    const QRectF textBounds(baseline.x(),
                                            baseline.y() - painter.fontMetrics().ascent(),
                                            painter.fontMetrics().horizontalAdvance(text),
                                            painter.fontMetrics().height());
                    if (visible.contains(textBounds) &&
                        std::none_of(labelBounds.begin(), labelBounds.end(), [&](const auto &used) {
                            return used.intersects(textBounds.adjusted(-3, -3, 3, 3));
                        })) {
                        painter.drawText(baseline, text);
                        labelBounds.append(textBounds);
                    }
                }
            }
        }
    } catch (const GeographicLib::GeographicErr &) {
        // A clipped boundary can lie outside the conversion domain.
    }
    painter.restore();
}
} // namespace
void paintGridType(QPainter &painter, const Viewport &view, const QJsonObject &type)
{
    if (!GridTypes::validate(type).isEmpty() || !std::isfinite(view.zoom) || view.zoom < 0 ||
        view.zoom > 30 || view.size.isEmpty() || !std::isfinite(view.center.x()) ||
        !std::isfinite(view.center.y())) {
        return;
    }
    const auto system = type.value("system").toString();
    if (system == QLatin1String("geographic")) {
        paintMapGrid(painter,
                     view,
                     type.value("opacity").toDouble(),
                     type.value("spacing").toDouble(),
                     QColor(type.value("color").toString()),
                     type.value("width").toDouble(),
                     type.value("format").toInt());
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto color = QColor(type.value("color").toString());
    color.setAlphaF(static_cast<float>(type.value("opacity").toDouble()));
    painter.setPen(QPen(color, type.value("width").toDouble()));
    painter.setBrush(Qt::NoBrush);
    painter.setClipRect(QRectF(QPointF{}, view.size));
    // Latitude bands also delimit the Norway and Svalbard zone exceptions.
    for (int band = 0; band < 20; ++band) {
        const double south = -80 + band * 8, north = band == 19 ? 84 : south + 8;
        for (int zone = 1; zone <= 60; ++zone) {
            double west = -180 + (zone - 1) * 6, east = west + 6;
            if (band == 17 && zone == 31) {
                east = 3;
            }
            if (band == 17 && zone == 32) {
                west = 3;
            }
            if (band == 19 && zone >= 31 && zone <= 37) {
                if (zone % 2 == 0) {
                    continue;
                }
                west = zone == 31 ? 0 : (zone - 31) * 6 - 3;
                east = zone == 37 ? 42 : (zone - 31) * 6 + 9;
            }
            metricCell(painter,
                       view,
                       QRectF(west, south, east - west, north - south),
                       zone,
                       south >= 0,
                       type);
        }
    }
    metricCell(painter, view, QRectF(-180, 84, 360, 1.05112878), 0, true, type);
    metricCell(painter, view, QRectF(-180, -85.05112878, 360, 5.05112878), 0, false, type);
    painter.restore();
}
} // namespace swak
