#include "core/map_hud.h"

#include "core/display_style.h"
#include "core/grid_types.h"

#include <QPainterPath>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace swak {
namespace {
QColor accent(int alpha)
{
    QColor color(modern::Accent);
    color.setAlpha(alpha);
    return color;
}
QString dms(double value, bool latitude)
{
    const auto hemisphere = latitude ? (value < 0 ? QLatin1Char('S') : QLatin1Char('N'))
                                     : (value < 0 ? QLatin1Char('W') : QLatin1Char('E'));
    // Round to tenths of a second first, so that 59.99999 does not show 60 seconds.
    const auto tenths = std::llround(std::abs(value) * 36000);
    const auto degrees = tenths / 36000;
    const auto minutes = (tenths / 600) % 60;
    const auto seconds = static_cast<double>(tenths % 600) / 10;
    return QStringLiteral("%1°%2'%3\"%4")
        .arg(degrees, latitude ? 2 : 3, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 4, 'f', 1, QLatin1Char('0'))
        .arg(hemisphere);
}
// Draws text in the data face with a dark halo, so it stays readable over imagery.
void hudText(QPainter &painter, QPointF baseline, const QString &text, const QColor &color)
{
    painter.setPen(QColor(0, 0, 0, 200));
    for (const auto &offset : {QPointF(1, 1), QPointF(-1, 1), QPointF(1, -1), QPointF(-1, -1)}) {
        painter.drawText(baseline + offset, text);
    }
    painter.setPen(color);
    painter.drawText(baseline, text);
}
void paintScope(QPainter &painter, const QRect &area)
{
    const QPointF center = QRectF(area).center();
    const double radius = hudScopeRadius(area.size());
    QPainterPath outside;
    outside.addRect(QRectF(area));
    QPainterPath circle;
    circle.addEllipse(center, radius, radius);
    outside = outside.subtracted(circle);
    painter.fillPath(outside, QColor(10, 10, 15, 232));
    // A faint grid outside the scope, as on an analyst's light table.
    painter.save();
    painter.setClipPath(outside);
    painter.setPen(QPen(accent(18), 1));
    for (double x = area.left() + std::fmod(center.x() - area.left(), 48.0); x < area.right();
         x += 48) {
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    for (double y = area.top() + std::fmod(center.y() - area.top(), 48.0); y < area.bottom();
         y += 48) {
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    painter.restore();
    painter.setPen(QPen(accent(150), 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, radius, radius);
    // Bearing ticks every 10 degrees, longer every 30 degrees.
    for (int bearing = 0; bearing < 360; bearing += 10) {
        const double angle = (bearing - 90) * M_PI / 180;
        const QPointF direction(std::cos(angle), std::sin(angle));
        const double length = bearing % 30 == 0 ? 12 : 6;
        painter.setPen(QPen(accent(bearing % 30 == 0 ? 200 : 110), 1));
        painter.drawLine(center + direction * radius, center + direction * (radius + length));
    }
    painter.setFont(DisplayStyle::dataFont(11));
    const auto metrics = painter.fontMetrics();
    for (const auto &[bearing, letter] : {std::pair{0, QStringLiteral("N")},
                                          std::pair{90, QStringLiteral("E")},
                                          std::pair{180, QStringLiteral("S")},
                                          std::pair{270, QStringLiteral("W")}}) {
        const double angle = (bearing - 90) * M_PI / 180;
        const auto point = center + QPointF(std::cos(angle), std::sin(angle)) * (radius + 24);
        hudText(painter,
                point + QPointF(-metrics.horizontalAdvance(letter) / 2.0, metrics.ascent() / 2.0),
                letter,
                accent(220));
    }
}
void paintFrame(QPainter &painter, const QRect &area)
{
    const QRectF frame = QRectF(area).adjusted(8, 8, -8, -8);
    painter.setPen(QPen(accent(55), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, 14, 14);
    // Corner brackets.
    painter.setPen(QPen(accent(210), 2));
    const double length = 26;
    const auto inner = frame.adjusted(6, 6, -6, -6);
    for (const auto &[corner, dx, dy] : {std::tuple{inner.topLeft(), 1.0, 1.0},
                                         std::tuple{inner.topRight(), -1.0, 1.0},
                                         std::tuple{inner.bottomLeft(), 1.0, -1.0},
                                         std::tuple{inner.bottomRight(), -1.0, -1.0}}) {
        painter.drawLine(corner, corner + QPointF(dx * length, 0));
        painter.drawLine(corner, corner + QPointF(0, dy * length));
    }
    // A fine crosshair at the center, with a gap for the target.
    const QPointF center = QRectF(area).center();
    painter.setPen(QPen(accent(170), 1));
    for (const auto &direction : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) {
        painter.drawLine(center + direction * 6, center + direction * 18);
    }
}
} // namespace

QString hudDms(double latitude, double longitude)
{
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || std::abs(latitude) > 90 ||
        std::abs(longitude) > 180) {
        return {};
    }
    return dms(latitude, true) + QLatin1Char(' ') + dms(longitude, false);
}
QString hudMgrs(const QString &compact)
{
    // Zone digits and band letter, square letters, then easting and northing of equal length.
    static const QRegularExpression pattern(
        QStringLiteral("\\A(\\d{1,2}[C-X])([A-Z]{2})(\\d*)\\z"));
    const auto match = pattern.match(compact);
    if (!match.hasMatch() || match.captured(3).size() % 2 != 0) {
        return compact;
    }
    const auto digits = match.captured(3);
    const auto half = digits.size() / 2;
    auto result = match.captured(1) + QLatin1Char(' ') + match.captured(2);
    if (half > 0) {
        result += QLatin1Char(' ') + digits.left(half) + QLatin1Char(' ') + digits.mid(half);
    }
    return result;
}
double hudScopeRadius(QSize size)
{
    return std::max(0.0, std::min(size.width(), size.height()) * 0.41);
}
QRect visibleMapRect(const QRect &bounds, const QRegion &occlusion)
{
    int left = bounds.left(), right = bounds.right(), top = bounds.top(), bottom = bounds.bottom();
    for (const auto &rect : occlusion) {
        const auto cover = rect & bounds;
        if (cover.isEmpty()) {
            continue;
        }
        const bool tall = cover.height() * 3 > bounds.height();
        const bool wide = cover.width() * 3 > bounds.width();
        if (tall && cover.right() >= bounds.right() - 8) {
            right = std::min(right, cover.left() - 1);
        } else if (tall && cover.left() <= bounds.left() + 8) {
            left = std::max(left, cover.right() + 1);
        } else if (wide && cover.bottom() >= bounds.bottom() - 8) {
            bottom = std::min(bottom, cover.top() - 1);
        } else if (wide && cover.top() <= bounds.top() + 8) {
            top = std::max(top, cover.bottom() + 1);
        }
    }
    const QRect result(QPoint(left, top), QPoint(right, bottom));
    return result.width() < 200 || result.height() < 150 ? bounds : result;
}
void paintMapHud(QPainter &painter, const QRect &area, const MapHudState &state)
{
    if (area.isEmpty()) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (state.scope) {
        paintScope(painter, area);
    }
    paintFrame(painter, area);
    const double left = area.left() + 26.0, right = area.right() - 26.0;
    double top = area.top() + 40.0;
    // Title block.
    painter.setFont(DisplayStyle::titleFont(18));
    hudText(painter, QPointF(left, top), QStringLiteral("SWAK"), QColor(modern::Text));
    painter.setFont(DisplayStyle::dataFont(11));
    const auto line = painter.fontMetrics().height() + 2;
    top += line + 4;
    hudText(painter,
            QPointF(left, top),
            QStringLiteral("COMMON OPERATIONAL PICTURE // %1 TRACKS").arg(state.tracks),
            QColor(modern::Dim));
    top += line * 1.6;
    const auto latitude = state.center.y(), longitude = state.center.x();
    const auto mgrs = gridCoordinate(latitude, longitude, QStringLiteral("mgrs"), 5);
    if (!mgrs.isEmpty()) {
        hudText(painter, QPointF(left, top), QStringLiteral("MGRS  ") + hudMgrs(mgrs), accent(230));
        top += line;
    }
    if (const auto text = hudDms(latitude, longitude); !text.isEmpty()) {
        hudText(painter, QPointF(left, top), text, accent(230));
        top += line;
    }
    hudText(painter,
            QPointF(left, top),
            QStringLiteral("ZOOM  %1").arg(state.zoom, 0, 'f', 1),
            QColor(modern::Dim));
    // Time block, right aligned.
    const auto metrics = painter.fontMetrics();
    const auto alignRight = [&](double y, const QString &text, const QColor &color) {
        hudText(painter, QPointF(right - metrics.horizontalAdvance(text), y), text, color);
    };
    double y = area.top() + 40.0;
    const auto time = state.utc.isValid()
                          ? state.utc.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss'Z'"))
                          : QStringLiteral("----------");
    alignRight(y, QStringLiteral("UTC  ") + time, QColor(modern::Text));
    y += line * 1.6;
    if (!state.source.isEmpty()) {
        alignRight(y,
                   QStringLiteral("SOURCE  ") +
                       metrics.elidedText(state.source.toUpper(), Qt::ElideRight, 260),
                   QColor(modern::Dim));
        y += line;
    }
    alignRight(y,
               QStringLiteral("SENSOR  ") +
                   (state.sensor.isEmpty() ? QStringLiteral("NORMAL") : state.sensor.toUpper()),
               accent(230));
    painter.restore();
}
} // namespace swak
