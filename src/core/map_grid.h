#pragma once
#include "core/projection.h"

#include <QColor>
#include <QJsonObject>
class QPainter;
namespace swak {
void paintMapGrid(QPainter &painter,
                  const Viewport &view,
                  double opacity,
                  double spacing,
                  QColor color = Qt::black,
                  double width = 1,
                  int format = 0);
void paintGridType(QPainter &painter, const Viewport &view, const QJsonObject &type);
} // namespace swak
