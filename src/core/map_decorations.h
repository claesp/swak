#pragma once
#include "core/projection.h"

#include <QLocale>
#include <QRegion>
class QPainter;
namespace swak {
inline constexpr auto MapOcclusionProperty = "swakMapOcclusion";
struct ScaleLegend
{
    double pixels = 0;
    double meters = 0;
    QString text;
};
ScaleLegend
mapScaleLegend(const Viewport &view, const QLocale &locale = QLocale(), double bottom = -1);
void paintMapDecorations(QPainter &painter,
                         const Viewport &view,
                         bool scale,
                         bool zoom,
                         int minimumZoom,
                         int maximumZoom,
                         const QRegion &occlusion = {});
} // namespace swak
