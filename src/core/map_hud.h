#pragma once
#include <QDateTime>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QString>

namespace swak {
// What the Modern map HUD shows. The map fills it for each frame.
struct MapHudState
{
    // Longitude and latitude of the map center, in degrees.
    QPointF center;
    double zoom = 0;
    QString source;
    QDateTime utc;
    // Darkens the map outside a central circle, as a sensor scope does.
    bool scope = true;
};
// Formats a latitude and longitude as degrees, minutes, and seconds, such as
// 59°19'48.0"N 018°04'12.0"E. Invalid values give an empty string.
QString hudDms(double latitude, double longitude);
// Groups a compact MGRS reference, such as 33VXG1234567890, as 33V XG 12345 67890.
QString hudMgrs(const QString &compact);
// The radius of the scope circle for a visible map area.
double hudScopeRadius(QSize size);
// The part of the map that overlay panes leave free. Panes that cover an edge of the map shrink
// the area from that edge. A result below 200 by 150 pixels gives the whole map.
QRect visibleMapRect(const QRect &bounds, const QRegion &occlusion);
// Paints the HUD of the Modern display mode over the visible map area. It keeps the bottom edge
// free for the scale and the attribution.
void paintMapHud(QPainter &painter, const QRect &area, const MapHudState &state);
} // namespace swak
