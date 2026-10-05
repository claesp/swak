#pragma once
#include <QJsonObject>
#include <QLocale>
#include <QPolygonF>
namespace swak {
constexpr int MaxPolygonVertices = 64;
// A route is an open line with several points. Planned routes are simplified to this limit.
constexpr int MaxRoutePoints = 250;
QPolygonF polygonVertices(const QJsonObject &geometry);
QPolygonF routePoints(const QJsonObject &geometry);
QString validateRoute(const QPolygonF &points);
// Moves each point by whole world widths so that each segment takes the short way across the
// date line.
QPolygonF unwrappedRoute(const QPolygonF &points);
double routeLength(const QPolygonF &points);
// Removes points until at most the limit remain. Visible shape changes stay small.
QPolygonF simplifiedRoute(const QPolygonF &points, int limit = MaxRoutePoints);
QJsonObject polygonGeometry(const QPolygonF &points);
QString validatePolygon(const QPolygonF &points);
QPointF polygonLabelPoint(const QPolygonF &points);
double polygonArea(const QPolygonF &points);
QString formatArea(double squareMeters, const QLocale &locale = QLocale());
} // namespace swak
