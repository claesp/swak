#pragma once
#include <QPointF>
#include <QSizeF>

namespace swak {
struct Viewport
{
    QSizeF size;
    QPointF center{0.5, 0.5};
    double zoom = 1;
    int markerScalePercent = 100;
    double markerSize(double baseSize) const;
    QPointF markerOffset(QPointF offset, double radius) const;
    QPointF screen(double latitude, double longitude) const;
    QPointF coordinate(QPointF screenPoint) const;
    // Converts between normalized world coordinates, where the world is the unit square, and
    // screen pixels of this view.
    QPointF toScreen(QPointF world) const;
    QPointF toWorld(QPointF screenPoint) const;
    double scale() const;
};
QPointF project(double latitude, double longitude);
QPointF unproject(QPointF point);
} // namespace swak
