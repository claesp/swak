#pragma once
#include <QPointF>
#include <QSizeF>

namespace swak {
struct Viewport {
    QSizeF size;
    QPointF center{0.5, 0.5};
    double zoom = 1;
    QPointF screen(double latitude, double longitude) const;
    QPointF coordinate(QPointF screenPoint) const;
    double scale() const;
};
QPointF project(double latitude, double longitude);
QPointF unproject(QPointF point);
}
