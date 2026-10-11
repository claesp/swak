#pragma once
#include "core/projection.h"

#include <QLocale>
#include <QRegion>

#include <optional>
class QPainter;
namespace swak {
inline constexpr auto MapOcclusionProperty = "swakMapOcclusion";
// The map area that panes and the command dock leave free, as a QRect. The pane overlay sets
// it. Without it, the map estimates the area from MapOcclusionProperty.
inline constexpr auto MapFreeAreaProperty = "swakMapFreeArea";
struct ScaleLegend
{
    double pixels = 0;
    double meters = 0;
    QString text;
};
ScaleLegend
mapScaleLegend(const Viewport &view, const QLocale &locale = QLocale(), double bottom = -1);
QRectF mapZoomLegendRect(const Viewport &view);
std::optional<int>
mapZoomLevelAt(const Viewport &view, QPointF position, int minimumZoom, int maximumZoom);
void paintMapDecorations(QPainter &painter,
                         const Viewport &view,
                         bool scale,
                         bool zoom,
                         int minimumZoom,
                         int maximumZoom,
                         const QRegion &occlusion = {});
} // namespace swak
