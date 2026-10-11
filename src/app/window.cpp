#include "app/window.h"

#include "app/data_manager.h"
#include "app/map_manager.h"
#include "app/map_panels.h"
#include "app/marker_manager.h"
#include "app/plugin_panel.h"
#include "core/activity.h"
#include "core/chat.h"
#include "core/citrap.h"
#include "core/contacts.h"
#include "core/credits.h"
#include "core/delete_confirmation.h"
#include "core/display_style.h"
#include "core/excheck.h"
#include "core/file_shares.h"
#include "core/icon_cache.h"
#include "core/identity.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_selection.h"
#include "core/map_tools.h"
#include "core/menu_order.h"
#include "core/missions.h"
#include "core/modal.h"
#include "core/network_client.h"
#include "core/object_sharing.h"
#include "core/provider_toolbar.h"
#include "core/routing.h"
#include "core/server_browser.h"
#include "core/servers.h"
#include "core/settings_dialog.h"
#include "core/tak_server_api.h"
#include "core/toolbar.h"
#include "core/track_history.h"
#include "core/ui_behavior.h"
#include "core/ui_icons.h"
#include "core/user_objects.h"
#include "core/video_sources.h"
#include "swak/version.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizeGrip>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace swak {
namespace {
class PluginMenu final : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton &&
            activatePluginAction(actionAt(event->position().toPoint()))) {
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
             event->key() == Qt::Key_Space) &&
            activatePluginAction(activeAction())) {
            return;
        }
        QMenu::keyPressEvent(event);
    }

private:
    bool activatePluginAction(QAction *action)
    {
        if (!action || !action->isEnabled() ||
            !action->property("activatesPluginAction").toBool()) {
            return false;
        }
        if (action->menu()) {
            action->menu()->close();
        }
        for (auto *menu = static_cast<QMenu *>(this); menu;
             menu = qobject_cast<QMenu *>(menu->parentWidget())) {
            menu->close();
        }
        action->trigger();
        return true;
    }
};
void addPaneAction(QMenu &menu, QDockWidget &dock, QAction *action = nullptr)
{
    if (!action) {
        action = menu.addAction(dock.windowTitle());
    }
    action->setProperty("activatesPluginAction", true);
    action->setCheckable(true);
    const auto sync = [action, &dock] {
        action->setChecked(!dock.isHidden());
    };
    sync();
    QObject::connect(&dock, &QDockWidget::visibilityChanged, action, sync);
    QObject::connect(&menu, &QMenu::aboutToShow, action, sync);
    QObject::connect(action, &QAction::triggered, &dock, [&dock](bool visible) {
        dock.setVisible(visible);
        if (!visible) {
            return;
        }
        dock.raise();
        dock.activateWindow();
        auto *focus = dock.focusWidget();
        if (!focus || focus == &dock || focus->focusPolicy() == Qt::NoFocus) {
            focus = &dock;
            for (auto *child : dock.findChildren<QWidget *>()) {
                if (child->isVisible() && child->isEnabled() &&
                    (child->focusPolicy() & Qt::TabFocus)) {
                    focus = child;
                    break;
                }
            }
        }
        focus->setFocus(Qt::MenuBarFocusReason);
    });
}
constexpr int LayoutVersion = 1;
// Pane content in a scroll area. Its size hint is the full content size, so default pane sizes
// stay the same. Its small minimum height lets a tall pane scroll instead of covering others,
// for example with the larger fonts of the Modern display mode.
class PaneScroll final : public QScrollArea
{
public:
    explicit PaneScroll(QWidget *parent) : QScrollArea(parent)
    {
        setObjectName(QStringLiteral("paneScroll"));
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QSize sizeHint() const override
    {
        return widget() ? widget()->sizeHint() : QScrollArea::sizeHint();
    }
    QSize minimumSizeHint() const override
    {
        if (!widget()) {
            return QScrollArea::minimumSizeHint();
        }
        const auto content = widget()->minimumSizeHint();
        return {content.width(), std::min(content.height(), 120)};
    }

protected:
    // The size hints follow the content, so a content layout change must reach the pane
    // layout. A scroll area does not pass it on by itself.
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == widget() && event->type() == QEvent::LayoutRequest) {
            updateGeometry();
        }
        return QScrollArea::eventFilter(object, event);
    }
};
QByteArray geometryBytes(const QJsonObject &values, const QString &key)
{
    const QString encoded = preferenceText(values, key, {}, 128 * 1024);
    return QByteArray::fromBase64(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
}
void mergeValues(QJsonObject &target, const QJsonObject &source)
{
    for (auto it = source.begin(); it != source.end(); ++it) {
        target.insert(it.key(), it.value());
    }
}
} // namespace
Window::Window(const QString &preferencesPath, const QList<QObject *> &plugins, QWidget *parent)
    : QMainWindow(parent), plugins_(plugins), store_(this), preferences_(preferencesPath)
{
    auto *activity = new ActivityModel(this);
    configureLogging(preferences_.document().value(QStringLiteral("logging")).toObject());
    setWindowTitle(tr("SWAK"));
    resize(1280, 800);
    setMinimumSize(850, 600);
    Codec *codec = nullptr;
    ViewPlugin *view = nullptr;
    QList<MapLayer *> layers;
    for (auto *plugin : plugins) {
        if (auto *candidate = qobject_cast<Codec *>(plugin)) {
            codec = candidate;
        }
        if (auto *candidate = qobject_cast<ViewPlugin *>(plugin)) {
            view = candidate;
        }
        if (auto *layer = qobject_cast<MapLayer *>(plugin)) {
            layers.append(layer);
        }
    }
    auto *mapSources = new MapSources(plugins, this);
    auto *dataSources = new DataSources(plugins, this);
    dataSources->setObjectName(QStringLiteral("dataSources"));
    auto *selection = new MapSelection(this);
    auto *mapTools = new MapTools(this);
    auto *objects = new UserObjects(this);
    auto *collections = new MarkerCollections(plugins, this);
    auto *identity = new Identity(this);
    Servers *servers = nullptr;
    if (codec && std::any_of(plugins.begin(), plugins.end(), [](QObject *plugin) {
            return qobject_cast<SourcePlugin *>(plugin);
        })) {
        servers = new Servers(store_, this);
    }
    PluginServices services{store_,
                            *activity,
                            *mapSources,
                            dataSources,
                            selection,
                            mapTools,
                            objects,
                            collections,
                            identity,
                            servers};
    displayStyle_ = new DisplayStyle(this);
    displayStyle_->setObjectName(QStringLiteral("displayStyle"));
    services.display = displayStyle_;
    services.routes = new RouteSources(plugins, this);
    services.network = new NetworkClient(this);
    services.network->setObjectName(QStringLiteral("networkClient"));
    if (servers) {
        const auto sendIdentity = [servers, identity] {
            Event sender;
            identity->apply(sender);
            servers->setSenderIdentity(sender.uid, sender.callsign);
        };
        connect(identity, &Identity::changed, servers, sendIdentity);
        sendIdentity();
        services.tak = new TakServerApi(*servers, *services.network, servers);
        services.tak->setObjectName(QStringLiteral("takServerApi"));
    }
    services.contacts = new Contacts(store_, servers, services.tak, this);
    services.sharing = new ObjectSharing(*objects, servers, identity, this);
    services.chat = new Chat(store_, servers, identity, this);
    services.files = new FileShares(
        store_, servers, services.tak, identity, objects, dataSources, mapSources, this);
    services.missions = new Missions(store_, servers, services.tak, identity, this);
    services.history = new TrackHistory(servers, services.tak, this);
    services.icons = new IconCache(servers, services.tak, this);
    services.video = new VideoSources(store_, servers, services.tak, this);
    services.excheck = new ExCheck(services.tak, identity, this);
    services.citrap = new Citrap(services.tak, identity, this);
    auto *mainViews = new QTabWidget(this);
    mainViews->setObjectName(QStringLiteral("mainViews"));
    mainViews->setDocumentMode(true);
    setCentralWidget(mainViews);
    QWidget *mainView = nullptr;
    if (view) {
        mainView = view->create(services, layers, mainViews);
    } else {
        auto *empty = new QLabel(tr("No map view plugin is available."), this);
        empty->setObjectName(QStringLiteral("missingMap"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        mainView = empty;
    }
    mainViews->addTab(mainView, tr("Map"));
    mapSources->setObjectName(QStringLiteral("mapSources"));
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    auto *settings = fileMenu->addAction(tr("&Settings"));
    settings->setObjectName(QStringLiteral("settingsAction"));
    settings->setShortcut(QKeySequence::Preferences);
    connect(settings, &QAction::triggered, this, &Window::openSettings);
    auto *identityAction = fileMenu->addAction(tr("Identity"));
    identityAction->setObjectName(QStringLiteral("identityAction"));
    connect(identityAction, &QAction::triggered, this, [this, identity] {
        Modal<SettingsDialog> dialog(
            QList<SettingsPage>{{identity->preferenceId(),
                                 identity->settingsTitle(),
                                 identity->settingsFields(),
                                 identity->savePreferences(),
                                 [identity](const QJsonObject &v) {
                                     return identity->validateSettings(v);
                                 },
                                 [identity](const QJsonObject &v) {
                                     identity->restorePreferences(v);
                                 }}},
            [this] {
                return savePreferences() ? QString{} : preferences_.error();
            },
            this,
            true);
        dialog->setObjectName(QStringLiteral("identityDialog"));
        dialog->setWindowTitle(tr("Identity"));
        if (!identityGeometry_.isEmpty()) {
            dialog->restoreGeometry(identityGeometry_);
        }
        dialog->exec();
        if (!dialog) {
            return;
        }
        identityGeometry_ = dialog->saveGeometry();
        savePreferences();
    });
    fileMenu->addSeparator();
    auto *quitSeparator = new QAction(fileMenu);
    quitSeparator->setSeparator(true);
    quitSeparator->setProperty("menuTailOrder", 1);
    fileMenu->addAction(quitSeparator);
    auto *quit = new QAction(tr("&Quit"), fileMenu);
    quit->setProperty("menuTailOrder", 2);
    fileMenu->addAction(quit);
    quit->setObjectName(QStringLiteral("quitAction"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->setObjectName(QStringLiteral("editMenu"));
    auto *undo = objects->undoStack().createUndoAction(editMenu, tr("Undo"));
    undo->setObjectName(QStringLiteral("undoAction"));
    undo->setShortcut(QKeySequence::Undo);
    editMenu->addAction(undo);
    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));
    for (auto *action : services.viewActions) {
        viewMenu->addAction(action);
    }
    auto *displayMenu = viewMenu->addMenu(tr("Display Mode"));
    displayMenu->setObjectName(QStringLiteral("displayModeMenu"));
    auto *displayModes = new QActionGroup(displayMenu);
    for (const auto &[mode, title] : {std::pair{DisplayMode::Standard, tr("Standard")},
                                      std::pair{DisplayMode::Modern, tr("Modern")}}) {
        auto *action = displayMenu->addAction(title);
        action->setObjectName(QStringLiteral("displayMode/") + displayModeId(mode));
        action->setCheckable(true);
        action->setChecked(mode == DisplayMode::Standard);
        displayModes->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            setDisplayMode(mode);
        });
    }
    connect(displayStyle_, &DisplayStyle::changed, displayMenu, [displayMenu](DisplayMode mode) {
        for (auto *action : displayMenu->actions()) {
            action->setChecked(action->objectName() ==
                               QStringLiteral("displayMode/") + displayModeId(mode));
        }
    });
    panels_ = new MapPanels(mainView);
    QHash<QObject *, QDockWidget *> pluginDocks;
    QHash<QObject *, QList<QDockWidget *>> additionalDocks;
    const auto createDock =
        [this](const QString &title, const QString &id, QWidget *content, Qt::DockWidgetArea area) {
            auto *dock = new QDockWidget(title, panels_);
            dock->setAutoFillBackground(true);
            const auto *participant = dynamic_cast<const PreferenceParticipant *>(content);
            dock->setObjectName(participant ? participant->preferenceId() + QStringLiteral(".pane")
                                            : id);
            auto *container = new QWidget(dock);
            container->setAutoFillBackground(true);
            container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            auto *layout = new QVBoxLayout(container);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);
            auto *scroll = new PaneScroll(container);
            scroll->setWidget(content);
            layout->addWidget(scroll, 1);
            auto *grip = new QSizeGrip(container);
            grip->setAccessibleName(tr("Resize pane"));
            layout->addWidget(grip, 0, Qt::AlignRight);
            grip->hide();
            connect(dock, &QDockWidget::topLevelChanged, grip, &QWidget::setVisible);
            dock->setWidget(container);
            addDockWidget(area, dock);
            panels_->track(dock);
            if (auto *pane = dynamic_cast<PaneContent *>(content)) {
                pane->watchPane(dock);
                connect(pane, &PaneContent::visibilityRequested, dock, [dock](bool visible) {
                    dock->setVisible(visible);
                    if (visible) {
                        dock->raise();
                    }
                });
            }
            return dock;
        };
    QList<QDockWidget *> sideDocks;
    QDockWidget *bottomDock = nullptr;
    for (auto *plugin : plugins) {
        if (auto *tool = qobject_cast<ToolPlugin *>(plugin); tool && codec) {
            const bool isLayer = qobject_cast<MapLayer *>(plugin) != nullptr;
            const QString id = isLayer ? QStringLiteral("eventsDock")
                                       : QString::fromLatin1(plugin->metaObject()->className()) +
                                             QStringLiteral("Dock");
            auto *dock = createDock(tool->name(),
                                    id,
                                    tool->create(services, *codec, this),
                                    isLayer ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);
            pluginDocks.insert(plugin, dock);
            if (isLayer) {
                bottomDock = dock;
                resizeDocks({dock}, {170}, Qt::Vertical);
            } else {
                sideDocks.append(dock);
            }
        }
    }
    for (auto *plugin : plugins) {
        if (auto *source = qobject_cast<SourcePlugin *>(plugin); source && codec) {
            auto *dock = createDock(source->name(),
                                    source->paneId().isEmpty()
                                        ? QString::fromLatin1(plugin->metaObject()->className()) +
                                              QStringLiteral("Dock")
                                        : source->paneId(),
                                    source->create(services, *codec, this),
                                    Qt::RightDockWidgetArea);
            sideDocks.append(dock);
            pluginDocks.insert(plugin, dock);
            for (const auto &extra : source->additionalPanes(services, this)) {
                if (!extra.widget || extra.id.isEmpty()) {
                    continue;
                }
                auto *extraDock =
                    createDock(extra.title, extra.id, extra.widget, Qt::RightDockWidgetArea);
                additionalDocks[plugin].append(extraDock);
                sideDocks.append(extraDock);
                extraDock->hide();
            }
        }
    }
    for (auto *plugin : plugins) {
        if (auto *pane = qobject_cast<PanePlugin *>(plugin)) {
            auto *content = pane->create(services, this);
            const auto *requests = dynamic_cast<PaneContent *>(content);
            const auto area = requests ? requests->preferredDockArea() : Qt::BottomDockWidgetArea;
            auto *dock = createDock(pane->name(),
                                    QString::fromLatin1(plugin->metaObject()->className()) +
                                        QStringLiteral("Dock"),
                                    content,
                                    area);
            pluginDocks.insert(plugin, dock);
            for (const auto &extra : pane->additionalPanes(services, this)) {
                if (!extra.widget || extra.id.isEmpty()) {
                    continue;
                }
                auto *extraDock =
                    createDock(extra.title, extra.id, extra.widget, Qt::RightDockWidgetArea);
                additionalDocks[plugin].append(extraDock);
                sideDocks.append(extraDock);
                extraDock->hide();
            }
            if (area == Qt::RightDockWidgetArea) {
                sideDocks.append(dock);
            } else if (area == Qt::BottomDockWidgetArea && bottomDock) {
                tabifyDockWidget(bottomDock, dock);
            }
            dock->hide();
        }
    }
    auto *pluginPanel = createPluginPanel(plugins, this);
    if (!codec) {
        auto *missing = new QLabel(
            tr("No data format plugin is available. Event tools are unavailable."), pluginPanel);
        missing->setWordWrap(true);
        pluginPanel->layout()->addWidget(missing);
    }
    sideDocks.append(createDock(
        tr("Plugins"), QStringLiteral("pluginsDock"), pluginPanel, Qt::RightDockWidgetArea));
    for (qsizetype index = 1; index < sideDocks.size(); ++index) {
        tabifyDockWidget(sideDocks.first(), sideDocks.at(index));
    }
    resizeDocks(sideDocks, QList<int>(sideDocks.size(), 380), Qt::Horizontal);
    sideDocks.first()->raise();
    auto *resetLayout = addMenuFooterAction(*viewMenu, tr("Reset Layout ..."));
    resetLayout->setObjectName(QStringLiteral("resetLayoutAction"));
    connect(resetLayout, &QAction::triggered, this, [this] {
        if (!confirmAction(this,
                           tr("Reset Layout"),
                           tr("Reset the layout to its default arrangement?"),
                           tr("Reset"))) {
            return;
        }
        panels_->restoreState(defaultLayout_, LayoutVersion);
        QMainWindow::restoreState(defaultWindowLayout_, LayoutVersion);
        fitWindowsToScreens();
        savePreferences();
    });
    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("toolsMenu"));
    if (servers) {
        auto *manageServers = toolsMenu->addAction(tr("Servers"));
        manageServers->setObjectName(QStringLiteral("serversAction"));
        connect(manageServers, &QAction::triggered, this, [this, servers] {
            Modal<SettingsDialog> dialog(
                QList<SettingsPage>{{servers->preferenceId(),
                                     servers->settingsTitle(),
                                     {},
                                     {},
                                     {},
                                     {},
                                     false,
                                     [servers](QWidget *owner) {
                                         return servers->createSettingsWidget(owner);
                                     }}},
                [this] {
                    return savePreferences() ? QString{} : preferences_.error();
                },
                this,
                true);
            dialog->setWindowTitle(tr("Servers"));
            if (!serversGeometry_.isEmpty()) {
                dialog->restoreGeometry(serversGeometry_);
            }
            dialog->exec();
            if (!dialog) {
                return;
            }
            serversGeometry_ = dialog->saveGeometry();
            savePreferences();
        });
        connect(servers, &Servers::changed, this, [this] {
            QTimer::singleShot(0, this, [this] {
                savePreferences();
            });
        });
    }
    auto *manageMaps = toolsMenu->addAction(tr("Map Providers"));
    manageMaps->setObjectName(QStringLiteral("mapProvidersAction"));
    connect(manageMaps, &QAction::triggered, this, [this, mapSources] {
        if (auto *existing = findChild<MapManager *>()) {
            existing->show();
            existing->raise();
            return;
        }
        auto *dialog = new MapManager(*mapSources, this, [this] {
            return savePreferences() ? QString{} : preferences_.error();
        });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (!mapManagerGeometry_.isEmpty()) {
            dialog->restoreGeometry(mapManagerGeometry_);
        }
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            mapManagerGeometry_ = dialog->saveGeometry();
            savePreferences();
        });
        dialog->show();
    });
    connect(mapSources, &MapSources::managementRequested, manageMaps, &QAction::trigger);
    auto *manageData = toolsMenu->addAction(tr("Data Providers"));
    manageData->setObjectName(QStringLiteral("dataProvidersAction"));
    connect(manageData, &QAction::triggered, this, [this, dataSources] {
        if (auto *existing = findChild<DataManager *>()) {
            existing->show();
            existing->raise();
            return;
        }
        auto *dialog = new DataManager(*dataSources, this, [this] {
            return savePreferences() ? QString{} : preferences_.error();
        });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (!dataManagerGeometry_.isEmpty()) {
            dialog->restoreGeometry(dataManagerGeometry_);
        }
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            dataManagerGeometry_ = dialog->saveGeometry();
            savePreferences();
        });
        dialog->show();
    });
    auto *manageMarkers = toolsMenu->addAction(tr("Marker Collections"));
    manageMarkers->setObjectName(QStringLiteral("markerCollectionsAction"));
    connect(manageMarkers, &QAction::triggered, this, [this, collections] {
        if (auto *existing = findChild<MarkerManager *>(); existing && existing->isWindow()) {
            existing->show();
            existing->raise();
            return;
        }
        auto *dialog = new MarkerManager(*collections, this, [this] {
            return savePreferences() ? QString{} : preferences_.error();
        });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (!markerManagerGeometry_.isEmpty()) {
            dialog->restoreGeometry(markerManagerGeometry_);
        }
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            markerManagerGeometry_ = dialog->saveGeometry();
            savePreferences();
        });
        dialog->show();
    });
    auto *pluginsMenu = new PluginMenu(tr("&Plugins"), toolsMenu);
    toolsMenu->addMenu(pluginsMenu);
    pluginsMenu->setObjectName(QStringLiteral("pluginsMenu"));
    auto *toolbar = new QToolBar(tr("Main toolbar"), this);
    toolbar_ = toolbar;
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(24, 24));
    toolbar->setFloatable(false);
    toolbar->setAllowedAreas(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, toolbar);
    toolbar->toggleViewAction()->setText(tr("Toolbar"));
    viewMenu->insertAction(resetLayout, toolbar->toggleViewAction());
    for (auto *plugin : plugins) {
        QMenu *menu = nullptr;
        if (auto *provider = qobject_cast<MenuPlugin *>(plugin)) {
            menu = provider->rootActions() ? pluginsMenu : pluginsMenu->addMenu(provider->name());
            provider->populateMenu(services, *menu);
        }
        if (auto *provider = qobject_cast<ToolbarPlugin *>(plugin)) {
            if (!toolbar->actions().isEmpty()) {
                toolbar->addSeparator();
            }
            provider->populateToolbar(services, *toolbar, menu);
        }
        if (auto *dock = pluginDocks.value(plugin)) {
            auto *pane = qobject_cast<PanePlugin *>(plugin);
            auto *primary = pane ? pane->primaryAction(dock->widget()) : nullptr;
            if (primary) {
                primary->setProperty("activatesPluginAction", true);
                pluginsMenu->addAction(primary);
                if (!toolbar->actions().isEmpty()) {
                    toolbar->addSeparator();
                }
                toolbar->addAction(primary);
            } else if (menu == pluginsMenu) {
                // Root tool actions control this editor pane.
            } else if (menu && !menu->isEmpty()) {
                addPaneAction(*pluginsMenu, *dock, menu->menuAction());
            } else {
                if (menu) {
                    delete menu;
                    menu = nullptr;
                }
                addPaneAction(*pluginsMenu, *dock);
            }
        }
        for (auto *extra : additionalDocks.value(plugin)) {
            if (menu == pluginsMenu) {
                continue;
            }
            addPaneAction(*pluginsMenu, *extra);
        }
        if (menu && menu != pluginsMenu && menu->isEmpty()) {
            delete menu;
        }
    }
    if (servers) {
        addProviderToolbar(*toolbar,
                           *servers,
                           *findChild<QAction *>(QStringLiteral("serversAction")),
                           QStringLiteral("serverProvider"),
                           tr("Servers"),
                           UiIcon::Server);
    }
    if (!dataSources->providers().isEmpty()) {
        addProviderToolbar(*toolbar,
                           *dataSources,
                           *manageData,
                           QStringLiteral("dataProvider"),
                           tr("Data providers"),
                           UiIcon::Data);
    }
    arrangeToolbarSections(*toolbar);
    mainMenu_ = new QMenu(tr("Menu"), this);
    mainMenu_->setObjectName(QStringLiteral("mainMenu"));
    // The menus keep the order of the menu bar.
    mainMenu_->setProperty(KeepOrderProperty, true);
    auto *mainMenuAction = mainMenu_->menuAction();
    mainMenuAction->setObjectName(QStringLiteral("mainMenuAction"));
    mainMenuAction->setIcon(uiIcon(UiIcon::Menu));
    mainMenuAction->setToolTip(tr("Menu (F10)"));
    mainMenuSeparator_ = new QAction(this);
    mainMenuSeparator_->setSeparator(true);
    mainMenuShortcut_ = new QAction(tr("Open menu"), this);
    mainMenuShortcut_->setObjectName(QStringLiteral("mainMenuShortcut"));
    mainMenuShortcut_->setShortcut(Qt::Key_F10);
    mainMenuShortcut_->setEnabled(false);
    addAction(mainMenuShortcut_);
    connect(mainMenuShortcut_, &QAction::triggered, this, [this] {
        const auto *button =
            toolbar_ ? toolbar_->widgetForAction(mainMenu_->menuAction()) : nullptr;
        // The menu opens above the command dock, or at the top left of the map without it.
        if (button && button->isVisible()) {
            const auto size = mainMenu_->sizeHint();
            mainMenu_->popup(button->mapToGlobal(QPoint(0, -size.height())));
        } else {
            mainMenu_->popup(panels_->mapToGlobal(QPoint(12, 12)));
        }
    });
    connect(toolbar, &QToolBar::visibilityChanged, this, &Window::updateMenuBar);
    addPaneAction(*pluginsMenu, *findChild<QDockWidget *>(QStringLiteral("pluginsDock")));
    pluginsMenu->setEnabled(!pluginsMenu->isEmpty());
    installMenuOrder();
    installUiBehavior();
    defaultLayout_ = panels_->saveState(LayoutVersion);
    defaultWindowLayout_ = QMainWindow::saveState(LayoutVersion);
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("helpMenu"));
    auto *credits = helpMenu->addAction(tr("Credits"));
    credits->setObjectName(QStringLiteral("creditsAction"));
    credits->setProperty("menuOrder", -1);
    connect(credits, &QAction::triggered, this, [this] {
        showCredits(this);
    });
    auto *about = helpMenu->addAction(tr("&About"));
    about->setObjectName(QStringLiteral("aboutAction"));
    connect(about, &QAction::triggered, this, [this] {
        if (auto *existing = findChild<QMessageBox *>(QStringLiteral("aboutDialog"))) {
            existing->raise();
            existing->activateWindow();
            return;
        }
        auto *dialog = new QMessageBox(
            QMessageBox::NoIcon,
            tr("About SWAK"),
            tr("SWAK\nVersion %1\n\nYet another TAK client.").arg(QStringLiteral(SWAK_VERSION)),
            QMessageBox::Close,
            this);
        dialog->setInformativeText(tr("Git SHA: %1").arg(QStringLiteral(SWAK_REVISION)) +
                                   (QStringLiteral(SWAK_GIT_TAG).isEmpty()
                                        ? QString{}
                                        : tr("\nGit tag: %1").arg(QStringLiteral(SWAK_GIT_TAG))));
        dialog->setObjectName(QStringLiteral("aboutDialog"));
        dialog->setTextFormat(Qt::PlainText);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
    auto *status = new QLabel(this);
    status->setObjectName(QStringLiteral("cotEventCount"));
    statusBar()->addWidget(status);
    const auto separator = [this] {
        auto *line = new QFrame(this);
        line->setObjectName(QStringLiteral("statusSeparator"));
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        statusBar()->addWidget(line);
    };
    separator();
    auto *internal = new QLabel(this);
    internal->setObjectName(QStringLiteral("internalEventCount"));
    statusBar()->addWidget(internal);
    const auto updateStatus = [this, status] {
        status->setText(
            tr("CoT Messages: %1 / %2")
                .arg(QLocale().toString(store_.rowCount()), QLocale().toString(MaxEvents)));
    };
    connect(&store_, &Store::changed, this, updateStatus);
    updateStatus();
    const auto updateInternal = [activity, internal] {
        internal->setText(tr("Events: %1").arg(QLocale().toString(activity->rowCount())));
    };
    connect(activity, &QAbstractItemModel::rowsInserted, internal, updateInternal);
    connect(activity, &QAbstractItemModel::rowsRemoved, internal, updateInternal);
    connect(activity, &QAbstractItemModel::modelReset, internal, updateInternal);
    updateInternal();
    separator();
    auto *toolStatus = new QLabel(statusBar());
    toolStatus->setObjectName(QStringLiteral("mapToolStatus"));
    toolStatus->setTextFormat(Qt::PlainText);
    toolStatus->setAccessibleName(tr("Active map tools"));
    toolStatus->setToolTip(tr("Active map tools"));
    toolStatus->setMinimumWidth(toolStatus->fontMetrics().horizontalAdvance(QStringLiteral("M")) +
                                8);
    statusBar()->addWidget(toolStatus);
    const auto updateToolStatus = [mapTools, toolStatus] {
        toolStatus->setText(mapTools->statusText());
    };
    connect(mapTools, &MapTools::changed, toolStatus, updateToolStatus);
    updateToolStatus();
    if (auto *provider = dynamic_cast<StatusProvider *>(mainView)) {
        if (auto *widget = provider->createStatusWidget(statusBar())) {
            separator();
            statusBar()->addWidget(widget, 1);
        }
    }
    auto *preferenceSeparator = new QFrame(this);
    preferenceSeparator->setObjectName(QStringLiteral("preferencesSeparator"));
    preferenceSeparator->setFrameShape(QFrame::VLine);
    preferenceSeparator->setFrameShadow(QFrame::Sunken);
    statusBar()->addPermanentWidget(preferenceSeparator);
    preferencesStatus_ = new QLabel(this);
    preferencesStatus_->setObjectName(QStringLiteral("preferencesStatus"));
    preferencesStatus_->setTextFormat(Qt::PlainText);
    preferencesStatus_->setWordWrap(true);
    preferencesStatus_->setMaximumWidth(420);
    statusBar()->addPermanentWidget(preferencesStatus_);
    saveTimer_.setInterval(750);
    for (const auto &page : settingsPages()) {
        settingsDefaults_.insert(page.id, page.values);
    }
    settingsDefaults_.insert(QStringLiteral("logging"),
                             {{QStringLiteral("enabled"), false},
                              {QStringLiteral("severity"), 2},
                              {QStringLiteral("urgency"), 0},
                              {QStringLiteral("rate"), 20},
                              {QStringLiteral("qt"), true},
                              {QStringLiteral("prefix"), QString{}}});
    for (auto *menu : findChildren<QMenu *>()) {
        sortMenuActions(*menu);
    }
    // The mode comes first, so that the saved layout of the mode meets the fonts of the mode.
    connect(displayStyle_, &DisplayStyle::changed, this, &Window::applyDisplayLayout);
    displayStyle_->setMode(displayModeFromId(preferences_.document()
                                                 .value(QStringLiteral("application"))
                                                 .toObject()
                                                 .value(QStringLiteral("displayMode"))
                                                 .toString())
                               .value_or(DisplayMode::Standard));
    restorePreferences();
    connect(mapSources, &MapSources::changed, this, [this, mapSources] {
        savePreferences();
        const auto source = mapSources->activeSource();
        logMessage(
            Severity::Info,
            Urgency::Routine,
            QStringLiteral("swak.maps"),
            source.isEmpty()
                ? tr("Map provider cleared.")
                : tr("Map provider: %1").arg(source.value(QStringLiteral("name")).toString()));
    });
    logMessage(
        Severity::Info, Urgency::Routine, QStringLiteral("swak.application"), tr("SWAK ready."));
    preferencesStatus_->setText(preferences_.error());
    findChild<QFrame *>(QStringLiteral("preferencesSeparator"))
        ->setVisible(!preferences_.error().isEmpty());
    if (!preferences_.error().isEmpty()) {
        lastSaveError_ = preferences_.error();
        logMessage(Severity::Error,
                   Urgency::Attention,
                   QStringLiteral("swak.preferences"),
                   lastSaveError_);
    }
    preferencesStatus_->setToolTip(preferences_.path());
    const auto application =
        preferences_.document().value(QStringLiteral("application")).toObject();
    saveTimer_.setInterval(
        preferenceInteger(application, QStringLiteral("saveInterval"), 750, 250, 60000));
    settingsGeometry_ = geometryBytes(application, QStringLiteral("settingsGeometry"));
    mapManagerGeometry_ = geometryBytes(application, QStringLiteral("mapManagerGeometry"));
    identityGeometry_ = geometryBytes(application, QStringLiteral("identityGeometry"));
    serversGeometry_ = geometryBytes(application, QStringLiteral("serversGeometry"));
    dataManagerGeometry_ = geometryBytes(application, QStringLiteral("dataManagerGeometry"));
    markerManagerGeometry_ = geometryBytes(application, QStringLiteral("markerManagerGeometry"));
    settingsNavigation_ = application.value(QStringLiteral("settingsNavigation")).toObject();
    connect(&saveTimer_, &QTimer::timeout, this, [this] {
        if (isVisible() && !closing_ && preferences_.writable() &&
            (!failedSave_.isValid() || failedSave_.elapsed() >= 30000)) {
            savePreferences();
        }
    });
}
void Window::addDockWidget(Qt::DockWidgetArea area, QDockWidget *dock)
{
    panels_->addDockWidget(area, dock);
}
void Window::tabifyDockWidget(QDockWidget *first, QDockWidget *second)
{
    panels_->tabifyDockWidget(first, second);
}
void Window::resizeDocks(const QList<QDockWidget *> &docks,
                         const QList<int> &sizes,
                         Qt::Orientation orientation)
{
    panels_->resizeDocks(docks, sizes, orientation);
}
Qt::DockWidgetArea Window::dockWidgetArea(QDockWidget *dock) const
{
    return panels_->dockWidgetArea(dock);
}
QList<QDockWidget *> Window::tabifiedDockWidgets(QDockWidget *dock) const
{
    return panels_->tabifiedDockWidgets(dock);
}
Window::~Window()
{
    saveTimer_.stop();
    // The toolbar hides while its parent deletes it. This window must not react then.
    if (toolbar_) {
        toolbar_->disconnect(this);
    }
    if (shown_ && !closing_) {
        savePreferences();
    }
    const auto docks = findChildren<QDockWidget *>();
    for (auto *dock : docks) {
        delete dock;
    }
    delete takeCentralWidget();
    delete findChild<Servers *>();
}
void Window::setDisplayMode(DisplayMode mode)
{
    const auto current = displayStyle_->mode();
    if (mode == current) {
        return;
    }
    // Each mode keeps its own layout, because the fonts of a mode change the pane sizes.
    modeLayouts_.insert(displayModeId(current),
                        {QMainWindow::saveState(LayoutVersion), panels_->saveState(LayoutVersion)});
    displayStyle_->setMode(mode);
    if (const auto saved = modeLayouts_.find(displayModeId(mode)); saved != modeLayouts_.end()) {
        const auto layout = *saved;
        modeLayouts_.erase(saved);
        // The toolbar choice is shared by both modes. A Modern layout has no toolbar.
        const bool toolbarVisible = toolbar_ && !toolbar_->isHidden();
        const auto restore = [this, layout, toolbarVisible] {
            QMainWindow::restoreState(layout.first, LayoutVersion);
            panels_->restoreState(layout.second, LayoutVersion);
            if (toolbar_) {
                toolbar_->setVisible(toolbarVisible);
            }
        };
        restore();
        // The new fonts change minimum pane sizes through posted layout requests, which can
        // limit the restored sizes. A second restore after these requests gives the exact
        // sizes.
        QTimer::singleShot(0, this, restore);
    }
    savePreferences();
}
void Window::applyDisplayLayout(DisplayMode mode)
{
    const bool modern = mode == DisplayMode::Modern;
    panels_->setModern(modern);
    // Modern shows the menus in the command dock instead of the menu bar.
    mainMenu_->clear();
    if (modern) {
        mainMenu_->addActions(menuBar()->actions());
    }
    if (!toolbar_) {
        updateMenuBar();
        return;
    }
    // Modern moves the main toolbar into a command dock at the bottom of the map.
    const bool visible = !toolbar_->isHidden();
    const bool docked = panels_->commandDock() == toolbar_;
    if (modern && !docked) {
        removeToolBar(toolbar_);
        toolbar_->setParent(panels_);
        toolbar_->setProperty("commandDock", true);
        panels_->setCommandDock(toolbar_);
    } else if (!modern && docked) {
        panels_->setCommandDock(nullptr);
        toolbar_->setProperty("commandDock", false);
        addToolBar(Qt::TopToolBarArea, toolbar_);
    }
    // The dynamic property changes the style sheet rules.
    toolbar_->style()->unpolish(toolbar_);
    toolbar_->style()->polish(toolbar_);
    // Only the command dock has the menu button, so the Standard toolbar does not change.
    const bool hasMenu = toolbar_->actions().contains(mainMenu_->menuAction());
    if (modern && !hasMenu) {
        const auto first = toolbar_->actions().value(0);
        toolbar_->insertAction(first, mainMenu_->menuAction());
        toolbar_->insertAction(first, mainMenuSeparator_);
        if (auto *button =
                qobject_cast<QToolButton *>(toolbar_->widgetForAction(mainMenu_->menuAction()))) {
            button->setObjectName(QStringLiteral("mainMenuButton"));
            button->setPopupMode(QToolButton::InstantPopup);
        }
    } else if (!modern && hasMenu) {
        toolbar_->removeAction(mainMenu_->menuAction());
        toolbar_->removeAction(mainMenuSeparator_);
    }
    toolbar_->setVisible(visible);
    updateMenuBar();
}
void Window::updateMenuBar()
{
    if (!mainMenu_) {
        return;
    }
    const bool modern = displayStyle_->mode() == DisplayMode::Modern;
    // Without the command dock, the menu bar stays the way to the menus.
    const bool dock = modern && toolbar_ && !toolbar_->isHidden();
    menuBar()->setVisible(!dock);
    mainMenuShortcut_->setEnabled(modern);
}
void Window::restorePreferences()
{
    const auto &document = preferences_.document();
    const auto window = document.value(QStringLiteral("window")).toObject();
    const auto geometry = geometryBytes(window, QStringLiteral("geometry"));
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    const auto layout = geometryBytes(window, QStringLiteral("layout"));
    if (!layout.isEmpty()) {
        QMainWindow::restoreState(layout, LayoutVersion);
    }
    // The layouts of the other display modes wait until the user selects that mode.
    modeLayouts_.clear();
    const auto others = window.value(QStringLiteral("modeLayouts")).toObject();
    for (auto it = others.begin(); it != others.end(); ++it) {
        if (displayModeFromId(it.key()) && it.key() != displayModeId(displayStyle_->mode())) {
            const auto saved = it.value().toObject();
            modeLayouts_.insert(it.key(),
                                {geometryBytes(saved, QStringLiteral("layout")),
                                 geometryBytes(saved, QStringLiteral("overlayLayout"))});
        }
    }
    const auto overlayLayout = geometryBytes(window, QStringLiteral("overlayLayout"));
    const auto paneLayout = overlayLayout.isEmpty() ? layout : overlayLayout;
    const bool restored = !paneLayout.isEmpty() && panels_->restoreState(paneLayout, LayoutVersion);
    const auto panes = document.value(QStringLiteral("panes")).toObject();
    for (auto *dock : findChildren<QDockWidget *>()) {
        const auto pane = panes.value(dock->objectName()).toObject();
        if (!restored && !pane.isEmpty()) {
            const int area =
                preferenceInteger(pane, QStringLiteral("area"), Qt::RightDockWidgetArea, 1, 8);
            if (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea ||
                area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea) {
                addDockWidget(static_cast<Qt::DockWidgetArea>(area), dock);
            }
            dock->setFloating(pane.value(QStringLiteral("floating")).toBool(false));
            dock->setVisible(pane.value(QStringLiteral("visible")).toBool(true));
        }
        if (dock->isFloating()) {
            const auto floatingGeometry = geometryBytes(pane, QStringLiteral("geometry"));
            if (!floatingGeometry.isEmpty()) {
                dock->restoreGeometry(floatingGeometry);
            }
        }
    }
    const auto components = document.value(QStringLiteral("components")).toObject();
    for (auto *participant : participants()) {
        auto values = components.value(participant->preferenceId());
        if (!values.isObject() && !participant->preferenceFallbackId().isEmpty()) {
            values = components.value(participant->preferenceFallbackId());
        }
        if (values.isObject()) {
            participant->restorePreferences(values.toObject());
        }
    }
    fitWindowsToScreens();
}
bool Window::savePreferences()
{
    if (!preferences_.writable()) {
        return false;
    }
    auto document = preferences_.document();
    auto window = document.value(QStringLiteral("window")).toObject();
    window.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    window.insert(QStringLiteral("layout"),
                  QString::fromLatin1(QMainWindow::saveState(LayoutVersion).toBase64()));
    window.insert(QStringLiteral("overlayLayout"),
                  QString::fromLatin1(panels_->saveState(LayoutVersion).toBase64()));
    QJsonObject others;
    for (auto it = modeLayouts_.cbegin(); it != modeLayouts_.cend(); ++it) {
        others.insert(it.key(),
                      QJsonObject{{QStringLiteral("layout"),
                                   QString::fromLatin1(it.value().first.toBase64())},
                                  {QStringLiteral("overlayLayout"),
                                   QString::fromLatin1(it.value().second.toBase64())}});
    }
    window.insert(QStringLiteral("modeLayouts"), others);
    document.insert(QStringLiteral("window"), window);
    auto panes = document.value(QStringLiteral("panes")).toObject();
    for (auto *dock : findChildren<QDockWidget *>()) {
        auto pane = panes.value(dock->objectName()).toObject();
        pane.insert(QStringLiteral("area"), static_cast<int>(dockWidgetArea(dock)));
        pane.insert(QStringLiteral("floating"), dock->isFloating());
        pane.insert(QStringLiteral("visible"), !dock->isHidden());
        if (dock->isFloating()) {
            pane.insert(QStringLiteral("geometry"),
                        QString::fromLatin1(dock->saveGeometry().toBase64()));
        }
        panes.insert(dock->objectName(), pane);
    }
    document.insert(QStringLiteral("panes"), panes);
    auto components = document.value(QStringLiteral("components")).toObject();
    for (const auto *participant : participants()) {
        auto values = components.value(participant->preferenceId()).toObject();
        mergeValues(values, participant->savePreferences());
        components.insert(participant->preferenceId(), values);
    }
    document.insert(QStringLiteral("components"), components);
    auto logging = document.value(QStringLiteral("logging")).toObject();
    mergeValues(logging, loggingConfiguration());
    document.insert(QStringLiteral("logging"), logging);
    auto application = document.value(QStringLiteral("application")).toObject();
    application.insert(QStringLiteral("saveInterval"), saveTimer_.interval());
    application.insert(QStringLiteral("displayMode"), displayModeId(displayStyle_->mode()));
    application.insert(QStringLiteral("settingsGeometry"),
                       QString::fromLatin1(settingsGeometry_.toBase64()));
    application.insert(QStringLiteral("settingsNavigation"), settingsNavigation_);
    application.insert(QStringLiteral("identityGeometry"),
                       QString::fromLatin1(identityGeometry_.toBase64()));
    application.insert(QStringLiteral("serversGeometry"),
                       QString::fromLatin1(serversGeometry_.toBase64()));
    application.insert(QStringLiteral("mapManagerGeometry"),
                       QString::fromLatin1(mapManagerGeometry_.toBase64()));
    if (auto *dialog = findChild<DataManager *>(); dialog && dialog->isWindow()) {
        dataManagerGeometry_ = dialog->saveGeometry();
    }
    application.insert(QStringLiteral("dataManagerGeometry"),
                       QString::fromLatin1(dataManagerGeometry_.toBase64()));
    if (auto *dialog = findChild<MarkerManager *>(); dialog && dialog->isWindow()) {
        markerManagerGeometry_ = dialog->saveGeometry();
    }
    application.insert(QStringLiteral("markerManagerGeometry"),
                       QString::fromLatin1(markerManagerGeometry_.toBase64()));
    document.insert(QStringLiteral("application"), application);
    document.insert(QStringLiteral("version"), 1);
    const bool saved = preferences_.save(document);
    preferencesStatus_->setText(preferences_.error());
    findChild<QFrame *>(QStringLiteral("preferencesSeparator"))
        ->setVisible(!preferences_.error().isEmpty());
    if (!saved) {
        failedSave_.start();
        if (lastSaveError_ != preferences_.error()) {
            logMessage(Severity::Error,
                       Urgency::Attention,
                       QStringLiteral("swak.preferences"),
                       preferences_.error());
        }
        lastSaveError_ = preferences_.error();
    } else {
        failedSave_.invalidate();
        lastSaveError_.clear();
    }
    return saved;
}
QList<PreferenceParticipant *> Window::participants() const
{
    QList<PreferenceParticipant *> result;
    const auto append = [&result](QObject *object) {
        if (auto *participant = dynamic_cast<PreferenceParticipant *>(object);
            participant && !result.contains(participant)) {
            result.append(participant);
        }
    };
    for (auto *plugin : plugins_) {
        append(plugin);
    }
    for (auto *object : findChildren<QObject *>()) {
        append(object);
    }
    return result;
}
QList<SettingsPage> Window::settingsPages()
{
    QList<SettingsPage> pages;
    pages.append(
        {QStringLiteral("logging"),
         tr("Logging"),
         {booleanSetting(QStringLiteral("enabled"), tr("Enable text logging to standard error")),
          choiceSetting(QStringLiteral("severity"),
                        tr("Minimum severity"),
                        {tr("Debug"), tr("Info"), tr("Warning"), tr("Error"), tr("Fatal")}),
          choiceSetting(QStringLiteral("urgency"),
                        tr("Minimum urgency"),
                        {tr("Routine"), tr("Attention"), tr("Immediate")}),
          booleanSetting(QStringLiteral("qt"), tr("Include Qt diagnostics")),
          textSetting(QStringLiteral("prefix"), tr("Category prefix (empty means all)"), 128),
          numberSetting(QStringLiteral("rate"), tr("Maximum messages per second"), 1, 1000)},
         loggingConfiguration(),
         {},
         [](const QJsonObject &values) {
             configureLogging(values);
         }});
    pages.append(
        {QStringLiteral("application"),
         tr("Workspace"),
         {numberSetting(QStringLiteral("width"), tr("Window width"), minimumWidth(), 16384),
          numberSetting(QStringLiteral("height"), tr("Window height"), minimumHeight(), 16384),
          numberSetting(QStringLiteral("x"), tr("Window X position"), -65536, 65536),
          numberSetting(QStringLiteral("y"), tr("Window Y position"), -65536, 65536),
          booleanSetting(QStringLiteral("maximized"), tr("Maximized")),
          numberSetting(QStringLiteral("saveInterval"),
                        tr("Automatic save interval (milliseconds)"),
                        250,
                        60000)},
         {{QStringLiteral("width"), normalGeometry().width()},
          {QStringLiteral("height"), normalGeometry().height()},
          {QStringLiteral("x"), normalGeometry().x()},
          {QStringLiteral("y"), normalGeometry().y()},
          {QStringLiteral("maximized"), isMaximized()},
          {QStringLiteral("saveInterval"), saveTimer_.interval()}},
         {},
         [this](const QJsonObject &values) {
             showNormal();
             setGeometry(values.value(QStringLiteral("x")).toInt(),
                         values.value(QStringLiteral("y")).toInt(),
                         values.value(QStringLiteral("width")).toInt(),
                         values.value(QStringLiteral("height")).toInt());
             fitWindowsToScreens();
             if (values.value(QStringLiteral("maximized")).toBool()) {
                 showMaximized();
             }
             saveTimer_.setInterval(values.value(QStringLiteral("saveInterval")).toInt());
         }});
    for (auto *participant : participants()) {
        if (dynamic_cast<Identity *>(participant)) {
            continue;
        }
        if (auto *maps = dynamic_cast<MapSources *>(participant)) {
            pages.append({maps->preferenceId(),
                          maps->settingsTitle(),
                          {},
                          {},
                          {},
                          {},
                          false,
                          [this, maps](QWidget *parent) {
                              return new MapManager(
                                  *maps,
                                  parent,
                                  [this] {
                                      return savePreferences() ? QString{} : preferences_.error();
                                  },
                                  true);
                          }});
        } else if (auto *feeds = dynamic_cast<DataSources *>(participant)) {
            pages.append({feeds->preferenceId(),
                          feeds->settingsTitle(),
                          {},
                          {},
                          {},
                          {},
                          false,
                          [this, feeds](QWidget *parent) {
                              return new DataManager(
                                  *feeds,
                                  parent,
                                  [this] {
                                      return savePreferences() ? QString{} : preferences_.error();
                                  },
                                  true);
                          }});
        } else if (auto *collections = dynamic_cast<MarkerCollections *>(participant)) {
            pages.append({collections->preferenceId(),
                          collections->settingsTitle(),
                          {},
                          {},
                          {},
                          {},
                          false,
                          [this, collections](QWidget *parent) {
                              return new MarkerManager(
                                  *collections,
                                  parent,
                                  [this] {
                                      return savePreferences() ? QString{} : preferences_.error();
                                  },
                                  true);
                          }});
        } else if (auto *custom = dynamic_cast<SettingsWidgetProvider *>(participant)) {
            auto *provider = dynamic_cast<SettingsParticipant *>(participant);
            pages.append({participant->preferenceId(),
                          provider ? provider->settingsTitle() : participant->preferenceId(),
                          {},
                          {},
                          {},
                          {},
                          false,
                          [custom](QWidget *parent) {
                              return custom->createSettingsWidget(parent);
                          }});
        } else if (auto *provider = dynamic_cast<SettingsParticipant *>(participant)) {
            pages.append({provider->preferenceId(),
                          provider->settingsTitle(),
                          provider->settingsFields(),
                          provider->savePreferences(),
                          [provider](const QJsonObject &values) {
                              return provider->validateSettings(values);
                          },
                          [provider](const QJsonObject &values) {
                              provider->restorePreferences(values);
                          }});
        }
    }
    for (auto *plugin : plugins_) {
        if (dynamic_cast<SettingsParticipant *>(plugin) || qobject_cast<ToolPlugin *>(plugin) ||
            qobject_cast<SourcePlugin *>(plugin) || qobject_cast<PanePlugin *>(plugin) ||
            qobject_cast<ViewPlugin *>(plugin)) {
            continue;
        }
        QString title;
        if (auto *codec = qobject_cast<Codec *>(plugin)) {
            title = codec->name();
        } else if (auto *layer = qobject_cast<MapLayer *>(plugin)) {
            title = layer->name();
        }
        if (!title.isEmpty()) {
            pages.append(
                {QString::fromLatin1(plugin->metaObject()->className()), title, {}, {}, {}, {}});
        }
    }
    for (auto *dock : findChildren<QDockWidget *>()) {
        pages.append(
            {dock->objectName(),
             dock->windowTitle(),
             {booleanSetting(QStringLiteral("visible"), tr("Visible")),
              booleanSetting(QStringLiteral("floating"), tr("Detached")),
              choiceSetting(QStringLiteral("area"),
                            tr("Overlay edge"),
                            {tr("Left"), tr("Right"), tr("Top"), tr("Bottom")},
                            {1, 2, 4, 8}),
              numberSetting(QStringLiteral("width"), tr("Width"), 100, 16384),
              numberSetting(QStringLiteral("height"), tr("Height"), 100, 16384),
              numberSetting(QStringLiteral("x"), tr("Detached X position"), -65536, 65536),
              numberSetting(QStringLiteral("y"), tr("Detached Y position"), -65536, 65536)},
             {{QStringLiteral("visible"), !dock->isHidden()},
              {QStringLiteral("floating"), dock->isFloating()},
              {QStringLiteral("area"), static_cast<int>(dockWidgetArea(dock))},
              {QStringLiteral("width"), dock->width()},
              {QStringLiteral("height"), dock->height()},
              {QStringLiteral("x"),
               dock->isFloating() ? dock->x() : dock->mapToGlobal(QPoint{}).x()},
              {QStringLiteral("y"),
               dock->isFloating() ? dock->y() : dock->mapToGlobal(QPoint{}).y()}},
             {},
             [this, dock](const QJsonObject &values) {
                 const auto area =
                     static_cast<Qt::DockWidgetArea>(values.value(QStringLiteral("area")).toInt());
                 if (dockWidgetArea(dock) != area) {
                     addDockWidget(area, dock);
                 }
                 dock->setFloating(values.value(QStringLiteral("floating")).toBool());
                 if (dock->isFloating()) {
                     dock->setGeometry(values.value(QStringLiteral("x")).toInt(),
                                       values.value(QStringLiteral("y")).toInt(),
                                       values.value(QStringLiteral("width")).toInt(),
                                       values.value(QStringLiteral("height")).toInt());
                 } else {
                     resizeDocks(
                         {dock}, {values.value(QStringLiteral("width")).toInt()}, Qt::Horizontal);
                     resizeDocks(
                         {dock}, {values.value(QStringLiteral("height")).toInt()}, Qt::Vertical);
                 }
                 dock->setVisible(values.value(QStringLiteral("visible")).toBool());
                 fitWindowsToScreens();
             },
             true});
    }
    for (auto &page : pages) {
        page.defaults = settingsDefaults_.value(page.id);
    }
    return pages;
}
void Window::openSettings()
{
    Modal<SettingsDialog> dialog(
        settingsPages(),
        [this] {
            return savePreferences() ? QString{} : preferences_.error();
        },
        this);
    dialog->restoreNavigation(settingsNavigation_);
    if (!settingsGeometry_.isEmpty()) {
        dialog->restoreGeometry(settingsGeometry_);
    }
    if (screen()) {
        dialog->setGeometry(
            fitWindowRectangle(dialog->geometry(), {screen()->availableGeometry()}));
    }
    dialog->exec();
    if (!dialog) {
        return;
    }
    settingsGeometry_ = dialog->saveGeometry();
    settingsNavigation_ = dialog->navigation();
    savePreferences();
}
void Window::fitWindowsToScreens()
{
    QList<QRect> screens;
    for (const auto *screen : QGuiApplication::screens()) {
        const QRect area = screen->availableGeometry();
        screens.append(area.adjusted(8, 32, -8, -8));
    }
    const auto fit = [&screens](QWidget *widget) {
        if (!widget->isMaximized() && !widget->isFullScreen()) {
            widget->setGeometry(fitWindowRectangle(widget->geometry(), screens));
        }
    };
    fit(this);
    for (auto *dock : findChildren<QDockWidget *>()) {
        if (dock->isFloating()) {
            fit(dock);
        }
    }
}
void Window::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (!shown_) {
        shown_ = true;
        QTimer::singleShot(0, this, [this] {
            fitWindowsToScreens();
        });
    }
    closing_ = false;
    saveTimer_.start();
}
void Window::closeEvent(QCloseEvent *event)
{
    savePreferences();
    closing_ = true;
    saveTimer_.stop();
    QMainWindow::closeEvent(event);
}

} // namespace swak
