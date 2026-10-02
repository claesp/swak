#include "core/interfaces.h"
#include "core/projection.h"
#include <QObject>
#include <QPainter>
#include <QPolygonF>
#include <cmath>
#include <algorithm>

namespace swak {
class BasemapPlugin final : public QObject, public Plugin, public MapLayer {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "basemap/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapLayer)
public:
    QString name() const override { return tr("Offline reference map"); }
    int order() const override { return 0; }
    void paint(QPainter &painter, const Viewport &view, const Store &) const override
    {
        const auto world = QRectF(view.screen(85.05112878, -180), view.screen(-85.05112878, 180));
        painter.fillRect(world, QColor("#102a3a"));
        painter.setPen(QPen(QColor("#4c7381"), 1));
        painter.setBrush(QColor("#284751"));
        static const QList<QPolygonF> land{
            {{-168,72},{-140,70},{-125,60},{-130,50},{-124,42},{-117,32},{-106,23},{-97,16},{-83,9},{-77,8},{-87,21},{-81,25},{-80,32},{-66,45},{-55,52},{-62,61},{-82,63},{-95,73},{-130,72}},
            {{-73,60},{-48,60},{-20,76},{-28,83},{-55,82},{-65,72}},
            {{-81,12},{-70,11},{-60,5},{-50,0},{-35,-6},{-40,-22},{-53,-34},{-68,-55},{-76,-42},{-70,-18},{-81,-4}},
            {{-17,36},{-5,36},{10,37},{25,32},{34,31},{43,12},{51,12},{42,-1},{40,-15},{31,-30},{18,-35},{11,-18},{8,4},{-5,5},{-17,15}},
            {{-10,36},{-9,44},{-1,49},{8,54},{5,60},{20,71},{31,70},{40,60},{61,68},{90,76},{130,70},{160,65},{179,66},{179,53},{150,46},{141,35},{123,39},{120,25},{110,20},{105,2},{99,8},{91,22},{80,8},{70,24},{57,24},{44,12},{36,30},{28,41},{15,38},{10,44},{2,42}},
            {{113,-22},{122,-15},{131,-12},{137,-17},{143,-11},{153,-28},{147,-39},{132,-33},{115,-35}},
            {{47,-13},{51,-16},{47,-25},{44,-24}},
            {{130,32},{140,36},{145,44},{142,45},{136,35}},
            {{166,-34},{179,-38},{174,-42},{167,-47},{165,-44},{173,-39}},
            {{-8,50},{2,51},{-3,59},{-7,58}},
            {{-180,-72},{-130,-74},{-70,-66},{-60,-75},{0,-70},{60,-68},{120,-73},{180,-72},{180,-85},{-180,-85}}
        };
        for (const auto &outline : land) {
            QPolygonF shape;
            shape.reserve(outline.size());
            for (const auto &point : outline) shape.append(view.screen(point.y(), point.x()));
            painter.drawPolygon(shape);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor("#335460"), 1));
        const double step = view.zoom < 3 ? 30 : view.zoom < 6 ? 10 : view.zoom < 9 ? 1 : view.zoom < 12 ? 0.1 : 0.01;
        const QPointF topLeft = view.coordinate({0, 0});
        const QPointF bottomRight = view.coordinate({view.size.width(), view.size.height()});
        const double west = std::max(-180.0, std::floor(topLeft.x() / step) * step);
        const double east = std::min(180.0, bottomRight.x());
        const double south = std::max(-85.0, std::floor(bottomRight.y() / step) * step);
        const double north = std::min(85.0, topLeft.y());
        for (double lon = west; lon <= east; lon += step) {
            const double x = view.screen(0, lon).x();
            painter.drawLine(QPointF(x, world.top()), QPointF(x, world.bottom()));
        }
        for (double lat = south; lat <= north; lat += step) {
            const double y = view.screen(lat, 0).y();
            painter.drawLine(QPointF(world.left(), y), QPointF(world.right(), y));
        }
    }
};
}
#include "basemap.moc"
