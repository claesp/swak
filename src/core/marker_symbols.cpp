#include "core/marker_symbols.h"

#include <QObject>
#include <QPainter>

#include <algorithm>
namespace swak {
bool validMarkerSymbol(const QString &code)
{
    return code.isEmpty() || std::any_of(markerSymbols().cbegin(),
                                         markerSymbols().cend(),
                                         [&code](const auto &symbol) {
                                             return symbol.code == code;
                                         });
}
QString markerSymbolForCot(const QString &type)
{
    if (type.size() > 128) {
        return {};
    }
    // Use an exact supported type. Do not guess the meaning of a subtype.
    const auto &symbols = markerSymbols();
    const auto found = std::find_if(symbols.cbegin(), symbols.cend(), [&type](const auto &symbol) {
        return type == symbol.code;
    });
    return found == symbols.cend() ? QString{} : found->code;
}
QPainterPath markerFrame(const QString &code, QPointF center, double radius)
{
    QPainterPath path;
    if (code.isEmpty() || !validMarkerSymbol(code)) {
        path.addEllipse(QRectF(-1, -1, 2, 2));
    } else if (code.at(2) == QLatin1Char('f')) {
        path.addRect(QRectF(-1, -0.7, 2, 1.4));
    } else if (code.at(2) == QLatin1Char('n')) {
        path.addRect(QRectF(-0.85, -0.85, 1.7, 1.7));
    } else if (code.at(2) == QLatin1Char('h')) {
        path.moveTo(0, -1);
        path.lineTo(1, 0);
        path.lineTo(0, 1);
        path.lineTo(-1, 0);
        path.closeSubpath();
    } else {
        path.moveTo(-0.5, -0.5);
        path.cubicTo(-0.6, -1.16, 0.6, -1.16, 0.5, -0.5);
        path.cubicTo(1.16, -0.6, 1.16, 0.6, 0.5, 0.5);
        path.cubicTo(0.6, 1.16, -0.6, 1.16, -0.5, 0.5);
        path.cubicTo(-1.16, 0.6, -1.16, -0.6, -0.5, -0.5);
        path.closeSubpath();
    }
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.scale(radius, radius);
    return transform.map(path);
}
QPainterPath markerGlyph(const QString &code)
{
    QPainterPath path;
    if (!validMarkerSymbol(code)) {
        return path;
    }
    constexpr double r = 0.42;
    if (code.endsWith(QLatin1String("-C-I"))) {
        path.moveTo(-r, -r);
        path.lineTo(r, r);
        path.moveTo(-r, r);
        path.lineTo(r, -r);
    } else if (code.endsWith(QLatin1String("-C-A"))) {
        path.addEllipse(QPointF{}, r, r * 0.6);
    } else if (code.endsWith(QLatin1String("-S-M"))) {
        path.moveTo(-r, 0);
        path.lineTo(r, 0);
        path.moveTo(0, -r);
        path.lineTo(0, r);
    }
    return path;
}
void paintMarkerSymbol(QPainter &painter,
                       const QString &code,
                       QPointF center,
                       double radius,
                       const QColor &color,
                       bool selected)
{
    painter.save();
    painter.setPen(QPen(selected ? QColor(Qt::white) : QColor("#152028"), selected ? 3 : 2));
    painter.setBrush(color);
    painter.drawPath(markerFrame(code, center, radius));
    painter.setPen(QPen(QColor("#152028"), 1));
    painter.setBrush(Qt::NoBrush);
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.scale(radius, radius);
    painter.drawPath(transform.map(markerGlyph(code)));
    painter.restore();
}
} // namespace swak
