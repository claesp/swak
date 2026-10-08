#pragma once
#include "core/event.h"

#include <QColor>
#include <QPolygonF>

#include <optional>

namespace swak {
// A shape or route that a TAK client drew. Points use normalized Mercator coordinates.
struct TakDrawing
{
    enum class Kind
    {
        Circle,
        Line,
        Area,
        Route
    };
    Kind kind = Kind::Line;
    QPolygonF points;
    QColor stroke;
    // Invalid when the shape has no fill.
    QColor fill;
    double strokeWidth = 3;
    // The label position.
    QPointF anchor;
};
// The most points in one drawing. It is below the link reader limit of 1024, so a larger
// shape is rejected and never drawn incomplete.
inline constexpr int MaxDrawingPoints = 1000;

// True for CoT types that TAK clients use for drawings and routes: u-d-c-c, u-d-c-e, u-d-f,
// u-d-f-m, u-d-r, and b-m-r. Readers can skip their point symbols.
bool takDrawingType(const QString &type);
// Reads the shape of a drawing event. Returns no value for other types and invalid content.
std::optional<TakDrawing> readTakDrawing(const Event &event);
} // namespace swak
