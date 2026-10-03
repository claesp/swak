#pragma once
#include <QColor>
#include <QList>
#include <QPainterPath>
#include <QString>
class QPainter;
namespace swak {
struct MarkerSymbol {
    QString code;
    QString name;
    QColor color;
};
const QList<MarkerSymbol> &markerSymbols();
QString markerSymbolForCot(const QString &type);
bool validMarkerSymbol(const QString &code);
QPainterPath markerFrame(const QString &code, QPointF center, double radius);
void paintMarkerSymbol(QPainter &painter, const QString &code, QPointF center, double radius,
                       const QColor &color, bool selected = false);
}
