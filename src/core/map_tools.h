#pragma once
#include "core/projection.h"
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
    virtual void paintOverlay(QPainter &, const Viewport &) const {}
    virtual QString statusText() const { return {}; }
    virtual bool coordinateGridActive() const { return false; }
    virtual bool cancel() = 0;
    virtual bool deleteSelection() { return false; }
signals:
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
    void paintOverlay(QPainter &painter, const Viewport &view) const;
    QString statusText() const;
    bool coordinateGridActive() const;
    bool cancel();
    bool deleteSelection();
signals:
    void changed();
    void overlayChanged();
private:
    QList<QPointer<MapTool>> tools_;
};
}
