#pragma once

#include "core/event.h"
#include "core/settings.h"

#include <QList>
#include <QtPlugin>

class QWidget;
class QPainter;
class QMenu;
class QAction;
class QToolBar;
namespace swak {
class Store;
class ActivityModel;
class MapSources;
class DataSources;
class MapSelection;
class MapTools;
class UserObjects;
class MarkerCollections;
class Identity;
class Servers;
struct PluginServices
{
    Store &store;
    ActivityModel &activity;
    MapSources &maps;
    DataSources *data = nullptr;
    MapSelection *selection = nullptr;
    MapTools *mapTools = nullptr;
    UserObjects *objects = nullptr;
    MarkerCollections *collections = nullptr;
    Identity *identity = nullptr;
    Servers *servers = nullptr;
    QAction *navigationLock = nullptr;
};
class Plugin
{
public:
    virtual ~Plugin() = default;
    virtual int apiVersion() const
    {
        return 2;
    }
};
struct Viewport;

class Codec
{
public:
    virtual ~Codec() = default;
    virtual QString name() const = 0;
    virtual DecodeResult decode(const QByteArray &data) const = 0;
    virtual QByteArray encode(const Event &event) const = 0;
};
class MapLayer
{
public:
    virtual ~MapLayer() = default;
    virtual QString name() const = 0;
    virtual int order() const = 0;
    virtual void paint(QPainter &painter, const Viewport &view, const Store &store) const = 0;
};
class ViewPlugin
{
public:
    virtual ~ViewPlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *
    create(PluginServices &services, const QList<MapLayer *> &layers, QWidget *parent) = 0;
};
class StatusProvider
{
public:
    virtual ~StatusProvider() = default;
    virtual QWidget *createStatusWidget(QWidget *parent) = 0;
};
class ToolPlugin
{
public:
    virtual ~ToolPlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *create(PluginServices &services, const Codec &codec, QWidget *parent) = 0;
};
class PanePlugin
{
public:
    virtual ~PanePlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *create(PluginServices &services, QWidget *parent) = 0;
    virtual QAction *primaryAction(QWidget *)
    {
        return nullptr;
    }
};
class MenuPlugin
{
public:
    virtual bool rootActions() const
    {
        return false;
    }
    virtual ~MenuPlugin() = default;
    virtual QString name() const = 0;
    virtual void populateMenu(PluginServices &services, QMenu &menu) = 0;
};
class ToolbarPlugin
{
public:
    virtual ~ToolbarPlugin() = default;
    virtual QString name() const = 0;
    virtual void
    populateToolbar(PluginServices &services, QToolBar &toolbar, QMenu *pluginMenu) = 0;
};
class SourcePlugin
{
public:
    virtual QString paneId() const
    {
        return {};
    }
    virtual ~SourcePlugin() = default;
    virtual QString name() const = 0;
    virtual QWidget *create(PluginServices &services, const Codec &codec, QWidget *parent) = 0;
};
} // namespace swak
Q_DECLARE_INTERFACE(swak::Codec, "org.swak.Codec/1.0")
Q_DECLARE_INTERFACE(swak::MapLayer, "org.swak.MapLayer/1.0")
Q_DECLARE_INTERFACE(swak::ViewPlugin, "org.swak.View/2.0")
Q_DECLARE_INTERFACE(swak::ToolPlugin, "org.swak.Tool/2.0")
Q_DECLARE_INTERFACE(swak::SourcePlugin, "org.swak.Source/2.0")
Q_DECLARE_INTERFACE(swak::MenuPlugin, "org.swak.Menu/2.0")
Q_DECLARE_INTERFACE(swak::ToolbarPlugin, "org.swak.Toolbar/3.0")

Q_DECLARE_INTERFACE(swak::PanePlugin, "org.swak.Pane/2.0")
Q_DECLARE_INTERFACE(swak::Plugin, "org.swak.Plugin/2.0")
