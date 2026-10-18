#pragma once
#include "core/projection.h"

#include <QColor>
#include <QJsonObject>
#include <QString>

#include <optional>
class QPainter;
namespace swak {
// The drawing values of a grid type, read once from its validated JSON record.
struct GridStyle
{
    QString system;
    double opacity = 1;
    double spacing = 140;
    double width = 1;
    QColor color;
    int format = 0;
};
// Reads a grid type record. Returns no value when GridTypes::validate rejects it.
std::optional<GridStyle> gridStyle(const QJsonObject &type);
// Formats a position as a UTM or MGRS reference, for system "utm" or "mgrs". Invalid input
// gives an empty string. precision is the number of digits for each axis in MGRS.
QString gridCoordinate(double latitude, double longitude, const QString &system, int precision = 3);
void paintMapGrid(QPainter &painter,
                  const Viewport &view,
                  double opacity,
                  double spacing,
                  QColor color = Qt::black,
                  double width = 1,
                  int format = 0);
void paintGridType(QPainter &painter, const Viewport &view, const GridStyle &style);
// Validates the record and paints it. Frame code keeps a GridStyle instead.
void paintGridType(QPainter &painter, const Viewport &view, const QJsonObject &type);
} // namespace swak
