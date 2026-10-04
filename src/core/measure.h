#pragma once
#include <QJsonObject>
#include <QLocale>
#include <QPolygonF>
namespace swak {
double groundDistance(QPointF first, QPointF second);
QString formatDistance(double meters, const QLocale &locale = QLocale());
QPolygonF circleBoundary(QPointF center, QPointF edge);
QString validateTwoPointShape(const QString &type, const QJsonObject &geometry);
} // namespace swak
