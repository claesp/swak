#pragma once
#include <QJsonObject>
#include <QPolygonF>
#include <QLocale>
namespace swak {
constexpr int MaxPolygonVertices = 64;
QPolygonF polygonVertices(const QJsonObject &geometry);
QJsonObject polygonGeometry(const QPolygonF &points);
QString validatePolygon(const QPolygonF &points);
QPointF polygonLabelPoint(const QPolygonF &points);
double polygonArea(const QPolygonF &points);
QString formatArea(double squareMeters, const QLocale &locale = QLocale());
}
