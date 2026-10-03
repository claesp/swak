#include "core/marker_symbols.h"
#include <QPainter>
#include <QObject>
#include <algorithm>
namespace swak {
const QList<MarkerSymbol> &markerSymbols()
{
    static const auto symbols = [] {
        QList<MarkerSymbol> result;
        const QStringList identities{QStringLiteral("f"), QStringLiteral("h"), QStringLiteral("n"), QStringLiteral("u")};
        const QStringList names{QObject::tr("Friendly"), QObject::tr("Hostile"), QObject::tr("Neutral"), QObject::tr("Unknown")};
        const QList<QColor> colors{QColor("#80e0ff"), QColor("#ff8080"), QColor("#aaffaa"), QColor("#ffff80")};
        const QStringList suffixes{QStringLiteral("G-U"), QStringLiteral("G-U-C-I"), QStringLiteral("G-U-C-A"), QStringLiteral("G-U-S-M")};
        const QStringList roles{QObject::tr("ground unit"), QObject::tr("infantry"), QObject::tr("armor"), QObject::tr("medical unit")};
        for (int identity = 0; identity < 4; ++identity) for (int role = 0; role < 4; ++role)
            result.append({QStringLiteral("a-%1-%2").arg(identities.at(identity), suffixes.at(role)),
                QObject::tr("%1 %2").arg(names.at(identity), roles.at(role)), colors.at(identity)});
        return result;
    }();
    return symbols;
}
bool validMarkerSymbol(const QString &code)
{
    return code.isEmpty() || std::any_of(markerSymbols().cbegin(), markerSymbols().cend(),
        [&code](const auto &symbol) { return symbol.code == code; });
}
QString markerSymbolForCot(const QString &type)
{
    if (type.size() > 128) return {};
    // Use an exact supported type. Do not guess the meaning of a subtype.
    const auto &symbols = markerSymbols();
    const auto found = std::find_if(symbols.cbegin(), symbols.cend(), [&type](const auto &symbol) { return type == symbol.code; });
    return found == symbols.cend() ? QString{} : found->code;
}
QPainterPath markerFrame(const QString &code, QPointF center, double radius)
{
    QPainterPath path;
    if (code.isEmpty() || !validMarkerSymbol(code)) path.addEllipse(QRectF(-1, -1, 2, 2));
    else if (code.at(2) == QLatin1Char('f')) path.addRect(QRectF(-1, -0.7, 2, 1.4));
    else if (code.at(2) == QLatin1Char('n')) path.addRect(QRectF(-0.85, -0.85, 1.7, 1.7));
    else if (code.at(2) == QLatin1Char('h')) {
        path.moveTo(0, -1); path.lineTo(1, 0); path.lineTo(0, 1); path.lineTo(-1, 0); path.closeSubpath();
    } else {
        path.moveTo(-0.5, -0.5);
        path.cubicTo(-0.6, -1.16, 0.6, -1.16, 0.5, -0.5);
        path.cubicTo(1.16, -0.6, 1.16, 0.6, 0.5, 0.5);
        path.cubicTo(0.6, 1.16, -0.6, 1.16, -0.5, 0.5);
        path.cubicTo(-1.16, 0.6, -1.16, -0.6, -0.5, -0.5);
        path.closeSubpath();
    }
    QTransform transform; transform.translate(center.x(), center.y()); transform.scale(radius, radius);
    return transform.map(path);
}
void paintMarkerSymbol(QPainter &painter, const QString &code, QPointF center, double radius, const QColor &color, bool selected)
{
    painter.save();
    painter.setPen(QPen(selected ? QColor(Qt::white) : QColor("#152028"), selected ? 3 : 2));
    painter.setBrush(color);
    painter.drawPath(markerFrame(code, center, radius));
    painter.setPen(QPen(QColor("#152028"), 1)); painter.setBrush(Qt::NoBrush);
    const double r = radius * 0.42;
    if (!validMarkerSymbol(code)) { painter.restore(); return; }
    if (code.endsWith(QLatin1String("-C-I"))) {
        painter.drawLine(center + QPointF(-r, -r), center + QPointF(r, r));
        painter.drawLine(center + QPointF(-r, r), center + QPointF(r, -r));
    } else if (code.endsWith(QLatin1String("-C-A"))) painter.drawEllipse(center, r, r * 0.6);
    else if (code.endsWith(QLatin1String("-S-M"))) {
        painter.drawLine(center + QPointF(-r, 0), center + QPointF(r, 0));
        painter.drawLine(center + QPointF(0, -r), center + QPointF(0, r));
    }
    painter.restore();
}
}
