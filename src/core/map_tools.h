#pragma once
#include "core/map_selection.h"
#include "core/projection.h"

#include <QObject>
#include <QPointer>
#include <QWidget>

#include <optional>
class QPainter;
class QMenu;
namespace swak {
class MapTool : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool handle(QEvent &event, const Viewport &view, QWidget &map) = 0;
    virtual void paint(QPainter &painter, const Viewport &view) const = 0;
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
    virtual bool cancel() = 0;
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
