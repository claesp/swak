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
class RouteSources;
// Per-window services for plugin factories and callbacks.
//
// The struct itself is a local value in the Window constructor. It exists only during the
// call that receives it. Do not keep a pointer or reference to the struct. Copy the
// service references or pointers that you need.
//
// The Window owns every service. All services live on the interface thread. A null pointer
// means that the service is not available in this window.
//
// The Window destroys objects in this order:
// 1. Dock panes and the central map view. Factories in ViewPlugin, ToolPlugin,
//    SourcePlugin, and PanePlugin create these widgets.
// 2. Servers.
// 3. Store.
// 4. All other services, in creation order: activity, maps, data, selection, mapTools,
//    objects, collections, identity, and routes.
// 5. Menus, toolbars, and their children, which include objects that MenuPlugin and
//    ToolbarPlugin create.
//
// So every service outlives the widgets from step 1. Objects from step 5 outlive all
// services. Their destructors must not use a service. Their connections to services
// must use a QObject context, so that Qt removes them when a service dies.
struct PluginServices
{
    // Window member. Lives until step 3.
    Store &store;
    // Window child. Lives until step 4.
    ActivityModel &activity;
    // Window child. Lives until step 4.
    MapSources &maps;
    // Window child. Lives until step 4. Null in hosts without data feeds.
    DataSources *data = nullptr;
    // Window child. Lives until step 4.
    MapSelection *selection = nullptr;
    // Window child. Lives until step 4. It holds guarded tool pointers and does not own tools.
    MapTools *mapTools = nullptr;
    // Window child. Lives until step 4.
    UserObjects *objects = nullptr;
    // Window child. Lives until step 4.
    MarkerCollections *collections = nullptr;
    // Window child. Lives until step 4.
    Identity *identity = nullptr;
    // Window child. Lives until step 2. Null when no codec or source plugin exists.
    Servers *servers = nullptr;
    // Owned by the map view, so it dies in step 1. ViewPlugin::create sets it. It is null
    // before that call and when no view plugin exists. Menu and toolbar callbacks run later.
    QAction *navigationLock = nullptr;
    // Owned by the map view, like navigationLock. QAction removes itself from menus and
    // toolbars when it dies.
    QList<QAction *> viewActions = {};
    // Window child. Lives until step 4.
    RouteSources *routes = nullptr;
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
struct PluginPane
{
    QString id;
    QString title;
    QWidget *widget = nullptr;
};
class PanePlugin
{
public:
    virtual QList<PluginPane> additionalPanes(PluginServices &, QWidget *)
    {
        return {};
    }
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
    virtual QList<PluginPane> additionalPanes(PluginServices &, QWidget *)
    {
        return {};
    }
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
