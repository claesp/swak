#pragma once
#include "core/map_selection.h"
#include "core/projection.h"

#include <QObject>
#include <QPointer>
#include <QPolygonF>
#include <QWidget>

#include <optional>
class QPainter;
class QMenu;
namespace swak {
// Returns the shortest distance from a point to an open polyline, or infinity without a segment.
double lineDistance(const QPolygonF &line, QPointF point);
// Paints the black horizontal and vertical guide lines through the pointer of a drawing tool.
void paintGuides(QPainter &painter, const Viewport &view, QPointF pointer, double opacity);
class MapTool : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    // A tool that only adds menu entries or paints nothing keeps these defaults.
    virtual bool handle(QEvent &, const Viewport &, QWidget &)
    {
        return false;
    }
    virtual void paint(QPainter &, const Viewport &) const {}
    virtual void paintOverlay(QPainter &, const Viewport &, QPointF) const {}
    virtual QString statusText() const
    {
        return {};
    }
    virtual std::optional<double> areaFillOpacity() const
    {
        return {};
    }
    // Returns plain-text credits that the map shows with the map provider attribution.
    virtual QStringList attributions() const
    {
        return {};
    }
    virtual bool guidesActive() const
    {
        return false;
    }
    virtual void activate(QWidget &) {}
    virtual bool interactionActive() const
    {
        return false;
    }
    virtual QList<MapItem> itemsAt(const Viewport &, QPointF) const
    {
        return {};
    }
    virtual bool interact(const MapItem &, QEvent &, QWidget &)
    {
        return false;
    }
    virtual void populateContextMenu(const MapItem &, QMenu &) {}
    virtual void populateInformationMenu(const MapItem &, QMenu &) {}
    // Returns true when the tool ended an operation.
    virtual bool cancel()
    {
        return false;
    }
    virtual bool dismiss()
    {
        return cancel();
    }
    virtual bool deleteSelection()
    {
        return false;
    }
signals:
    void activationRequested();
    void changed();
    void itemsChanged();
    void actionsChanged();
    void overlayChanged();
};
class MapTools final : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    void add(MapTool *tool);
    bool handle(QEvent &event, const Viewport &view, QWidget &map);
    void paint(QPainter &painter, const Viewport &view) const;
    void paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const;
    QString statusText() const;
    double areaFillOpacity() const;
    QStringList attributions() const;
    bool guidesActive() const;
    // True when a tool paints an overlay: during an interaction or with active guides.
    bool overlayActive() const;
    QList<MapItem> itemsAt(const Viewport &view, QPointF position) const;
    bool interact(const MapItem &item, QEvent &event, QWidget &map);
    void populateContextMenu(const MapItem &item, QMenu &menu);
    void populateInformationMenu(const MapItem &item, QMenu &menu);
    bool cancel();
    bool dismiss();
    bool deleteSelection();
signals:
    void centerRequested(QPointF world);
    void activationRequested(swak::MapTool *tool);
    void changed();
    void itemsChanged();
    void actionsChanged();
    void overlayChanged();

private:
    QList<QPointer<MapTool>> tools_;
};
} // namespace swak
