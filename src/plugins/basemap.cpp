#include "core/interfaces.h"
#include "core/tiles.h"
#include <QObject>
#include <QPainter>

namespace swak {
class BasemapPlugin final : public QObject, public Plugin, public MapLayer {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "basemap/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapLayer)
public:
    QString name() const override { return tr("Map background"); }
    int order() const override { return 0; }
    void paint(QPainter &painter, const Viewport &view, const Store &) const override
    {
        painter.fillRect(QRectF(QPointF{}, view.size), QColor(MapBackgroundRgb));
    }
};
}
#include "basemap.moc"
