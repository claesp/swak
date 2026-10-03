#pragma once
#include "core/projection.h"
#include "core/map_selection.h"
#include <QObject>
#include <QPointer>
#include <QWidget>
class QPainter;
namespace swak {
class MapTool : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool handle(QEvent &event, const Viewport &view, QWidget &map) = 0;
    virtual void paint(QPainter &painter, const Viewport &view) const = 0;
    virtual void paintOverlay(QPainter &, const Viewport &, QPointF) const {}
    virtual QString statusText() const { return {}; }
    virtual bool coordinateGridActive() const { return false; }
    virtual void activate(QWidget &) {}
    virtual bool interactionActive() const { return false; }
    virtual QList<MapItem> itemsAt(const Viewport &, QPointF) const { return {}; }
    virtual bool interact(const MapItem &, QEvent &, QWidget &) { return false; }
    virtual bool cancel() = 0;
    virtual bool dismiss() { return cancel(); }
    virtual bool deleteSelection() { return false; }
signals:
    void activationRequested();
    void changed();
    void overlayChanged();
};
class MapTools final : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void add(MapTool *tool);
    bool handle(QEvent &event, const Viewport &view, QWidget &map);
    void paint(QPainter &painter, const Viewport &view) const;
    void paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const;
    QString statusText() const;
    bool coordinateGridActive() const;
    QList<MapItem> itemsAt(const Viewport &view, QPointF position) const;
    bool interact(const MapItem &item, QEvent &event, QWidget &map);
    bool cancel();
    bool dismiss();
    bool deleteSelection();
signals:
    void activationRequested(swak::MapTool *tool);
    void changed();
    void overlayChanged();
private:
    QList<QPointer<MapTool>> tools_;
};
}
