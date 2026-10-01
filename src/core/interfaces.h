#pragma once

#include "core/event.h"
#include "core/settings.h"
#include <QtPlugin>
#include <QList>

class QWidget;
class QPainter;
namespace swak {
class Store;
struct Viewport;

class Codec {
public:
    virtual ~Codec() = default;
    virtual QString name() const = 0;
    virtual DecodeResult decode(const QByteArray &data) const = 0;
    virtual QByteArray encode(const Event &event) const = 0;
};
class MapLayer {
public:
    virtual ~MapLayer() = default;
    virtual QString name() const = 0;
    virtual int order() const = 0;
    virtual void paint(QPainter &painter, const Viewport &view, const Store &store) const = 0;
};
class ViewPlugin {
public:
    virtual ~ViewPlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *create(Store &store, const QList<MapLayer *> &layers, QWidget *parent) = 0;
};
class ToolPlugin {
public:
    virtual ~ToolPlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *create(Store &store, const Codec &codec, QWidget *parent) = 0;
};
class SourcePlugin {
public:
    virtual ~SourcePlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *create(Store &store, const Codec &codec, QWidget *parent) = 0;
};
}
Q_DECLARE_INTERFACE(swak::Codec, "org.swak.Codec/1.0")
Q_DECLARE_INTERFACE(swak::MapLayer, "org.swak.MapLayer/1.0")
Q_DECLARE_INTERFACE(swak::ViewPlugin, "org.swak.View/1.0")
Q_DECLARE_INTERFACE(swak::ToolPlugin, "org.swak.Tool/1.0")
Q_DECLARE_INTERFACE(swak::SourcePlugin, "org.swak.Source/1.0")
