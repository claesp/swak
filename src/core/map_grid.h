#pragma once
#include "core/projection.h"
class QPainter;
namespace swak {
void paintMapGrid(QPainter &painter, const Viewport &view, double opacity, double spacing);
}
