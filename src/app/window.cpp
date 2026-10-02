#include "app/window.h"
#include "app/map_manager.h"
#include "swak/version.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/activity.h"
#include "app/settings_dialog.h"
#include <QAction>
#include <QToolBar>
#include <QMessageBox>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QSizeGrip>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QShowEvent>
#include <QGuiApplication>
#include <QScreen>

namespace swak {
namespace {
constexpr int LayoutVersion = 1;
QByteArray geometryBytes(const QJsonObject &values, const QString &key)
{
    const QString encoded = preferenceText(values, key, {}, 128 * 1024);
    return QByteArray::fromBase64(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
}
void mergeValues(QJsonObject &target, const QJsonObject &source)
{
    for (auto it = source.begin(); it != source.end(); ++it) target.insert(it.key(), it.value());
}
}
Window::Window(const QString &preferencesPath, const QList<QObject *> &plugins, QWidget *parent)
    : QMainWindow(parent), plugins_(plugins), preferences_(preferencesPath)
{
    auto *activity = new ActivityModel(this);
    configureLogging(preferences_.document().value(QStringLiteral("logging")).toObject());
    setWindowTitle(tr("SWAK | Cursor-on-Target workspace"));
    resize(1280, 800);
    setMinimumSize(850, 600);
    Codec *codec = nullptr;
    ViewPlugin *view = nullptr;
    QList<MapLayer *> layers;
    QStringList names;
    for (auto *plugin : plugins) {
        if (auto *candidate = qobject_cast<Codec *>(plugin)) { codec = candidate; names.append(candidate->name()); }
        if (auto *candidate = qobject_cast<ViewPlugin *>(plugin)) { view = candidate; names.append(candidate->name()); }
        if (auto *layer = qobject_cast<MapLayer *>(plugin)) { layers.append(layer); names.append(layer->name()); }
        if (auto *extension = qobject_cast<MapExtension *>(plugin)) names.append(extension->mapName());
    }
    auto *mapSources = new MapSources(plugins, this);
    PluginServices services{store_, *activity, *mapSources};
    if (view) {
        setCentralWidget(view->create(services, layers, this));
    } else {
        auto *empty = new QLabel(tr("No map view plugin is available."), this);
        empty->setObjectName(QStringLiteral("missingMap"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        setCentralWidget(empty);
    }
    mapSources->setObjectName(QStringLiteral("mapSources"));
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    auto *settings = fileMenu->addAction(tr("&Settings"));
    settings->setObjectName(QStringLiteral("settingsAction"));
    settings->setShortcut(QKeySequence::Preferences);
    connect(settings, &QAction::triggered, this, &Window::openSettings);
    fileMenu->addSeparator();
    auto *quit = fileMenu->addAction(tr("&Quit"));
    quit->setObjectName(QStringLiteral("quitAction"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));
    QHash<QObject *, QDockWidget *> pluginDocks;
    const auto createDock = [this](const QString &title, const QString &id,
                                           QWidget *content, Qt::DockWidgetArea area) {
        auto *dock = new QDockWidget(title, this);
        const auto *participant = dynamic_cast<const PreferenceParticipant *>(content);
        dock->setObjectName(participant ? participant->preferenceId() + QStringLiteral(".pane") : id);
        auto *container = new QWidget(dock);
        container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto *layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(content, 1);
        auto *grip = new QSizeGrip(container);
        grip->setAccessibleName(tr("Resize pane"));
        layout->addWidget(grip, 0, Qt::AlignRight);
        grip->hide();
        connect(dock, &QDockWidget::topLevelChanged, grip, &QWidget::setVisible);
        dock->setWidget(container);
        addDockWidget(area, dock);
        return dock;
    };
    QList<QDockWidget *> sideDocks;
    QDockWidget *bottomDock = nullptr;
    for (auto *plugin : plugins) {
        if (auto *tool = qobject_cast<ToolPlugin *>(plugin); tool && codec) {
            const bool isLayer = qobject_cast<MapLayer *>(plugin) != nullptr;
            const QString id = isLayer ? QStringLiteral("eventsDock")
                                       : QString::fromLatin1(plugin->metaObject()->className()) + QStringLiteral("Dock");
            auto *dock = createDock(tool->name(), id, tool->create(services, *codec, this),
                                    isLayer ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);
            pluginDocks.insert(plugin, dock);
            if (isLayer) {
                bottomDock = dock;
                resizeDocks({dock}, {170}, Qt::Vertical);
            } else {
                sideDocks.append(dock);
                names.append(tool->name());
            }
        }
    }
    for (auto *plugin : plugins) {
        if (auto *source = qobject_cast<SourcePlugin *>(plugin); source && codec) {
            auto *dock = createDock(source->name(),
                QString::fromLatin1(plugin->metaObject()->className()) + QStringLiteral("Dock"),
                source->create(services, *codec, this), Qt::RightDockWidgetArea);
            sideDocks.append(dock);
            pluginDocks.insert(plugin, dock);
            names.append(source->name());
        }
    }
    for (auto *plugin : plugins) {
        if (auto *pane = qobject_cast<PanePlugin *>(plugin)) {
            auto *dock = createDock(pane->name(), QString::fromLatin1(plugin->metaObject()->className()) + QStringLiteral("Dock"),
                pane->create(services, this), Qt::BottomDockWidgetArea);
            pluginDocks.insert(plugin, dock);
            if (bottomDock) tabifyDockWidget(bottomDock, dock);
            dock->hide();
            names.append(pane->name());
        }
    }
    auto *pluginPanel = new QWidget(this);
    auto *pluginLayout = new QVBoxLayout(pluginPanel);
    auto *pluginText = new QLabel(tr("Built-in plugins\n\n%1\n\nPlugins run with application permissions. This build does not load external plugin files.").arg(names.join('\n')), pluginPanel);
    pluginText->setTextFormat(Qt::PlainText);
    pluginText->setWordWrap(true);
    pluginLayout->addWidget(pluginText);
    if (!codec) {
        auto *missing = new QLabel(tr("No data format plugin is available. Event tools are unavailable."), pluginPanel);
        missing->setWordWrap(true);
        pluginLayout->addWidget(missing);
    }
    pluginLayout->addStretch();
    sideDocks.append(createDock(tr("Plugins"), QStringLiteral("pluginsDock"), pluginPanel, Qt::RightDockWidgetArea));
    for (qsizetype index = 1; index < sideDocks.size(); ++index) {
        tabifyDockWidget(sideDocks.first(), sideDocks.at(index));
    }
    resizeDocks(sideDocks, QList<int>(sideDocks.size(), 380), Qt::Horizontal);
    sideDocks.first()->raise();
    auto *resetLayout = viewMenu->addAction(tr("Reset layout"));
    resetLayout->setObjectName(QStringLiteral("resetLayoutAction"));
    connect(resetLayout, &QAction::triggered, this, [this] {
        restoreState(defaultLayout_, LayoutVersion);
        fitWindowsToScreens();
        savePreferences();
    });
    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->setObjectName(QStringLiteral("toolsMenu"));
    auto *manageMaps = toolsMenu->addAction(tr("Map Providers"));
    manageMaps->setObjectName(QStringLiteral("mapProvidersAction"));
    connect(manageMaps, &QAction::triggered, this, [this, mapSources] {
        if (auto *existing = findChild<MapManager *>()) { existing->show(); existing->raise(); return; }
        auto *dialog = new MapManager(*mapSources, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (!mapManagerGeometry_.isEmpty()) dialog->restoreGeometry(mapManagerGeometry_);
        connect(dialog, &QDialog::finished, this, [this, dialog] { mapManagerGeometry_ = dialog->saveGeometry(); savePreferences(); });
        dialog->show();
    });
    connect(mapSources, &MapSources::managementRequested, manageMaps, &QAction::trigger);
    auto *pluginsMenu = toolsMenu->addMenu(tr("&Plugins"));
    pluginsMenu->setObjectName(QStringLiteral("pluginsMenu"));
    auto *toolbar = new QToolBar(tr("Main toolbar"), this);
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setAllowedAreas(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, toolbar);
    toolbar->toggleViewAction()->setText(tr("Show main toolbar"));
    viewMenu->insertAction(resetLayout, toolbar->toggleViewAction());
    for (auto *plugin : plugins) {
        QMenu *menu = nullptr;
        if (auto *provider = qobject_cast<MenuPlugin *>(plugin)) {
            menu = pluginsMenu->addMenu(provider->name());
            provider->populateMenu(services, *menu);
        }
        if (auto *provider = qobject_cast<ToolbarPlugin *>(plugin)) provider->populateToolbar(services, *toolbar, menu);
        if (auto *dock = pluginDocks.value(plugin)) {
            if (menu && !menu->isEmpty()) {
                auto *show = new QAction(tr("Show pane"), menu);
                show->setCheckable(true);
                show->setChecked(!dock->isHidden());
                menu->insertAction(menu->actions().first(), show);
                connect(show, &QAction::triggered, dock, [dock](bool visible) {
                    dock->setVisible(visible);
                    if (visible) dock->raise();
                });
                connect(dock->toggleViewAction(), &QAction::toggled, show, &QAction::setChecked);
                menu->menuAction()->setCheckable(true);
                menu->menuAction()->setChecked(!dock->isHidden());
                connect(dock->toggleViewAction(), &QAction::toggled, menu->menuAction(), &QAction::setChecked);
            } else {
                if (menu) { delete menu; menu = nullptr; }
                pluginsMenu->addAction(dock->toggleViewAction());
            }
        }
        if (menu && menu->isEmpty()) delete menu;
    }
    pluginsMenu->addAction(findChild<QDockWidget *>(QStringLiteral("pluginsDock"))->toggleViewAction());
    pluginsMenu->setEnabled(!pluginsMenu->isEmpty());
    defaultLayout_ = saveState(LayoutVersion);
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("helpMenu"));
    auto *about = helpMenu->addAction(tr("&About"));
    about->setObjectName(QStringLiteral("aboutAction"));
    connect(about, &QAction::triggered, this, [this] {
        if (auto *existing = findChild<QMessageBox *>(QStringLiteral("aboutDialog"))) {
            existing->raise();
            existing->activateWindow();
            return;
        }
        auto *dialog = new QMessageBox(QMessageBox::Information, tr("About SWAK"),
            tr("SWAK\nVersion %1\n\nA native Cursor-on-Target workspace.\nReceive, generate, and present CoT events with built-in plugins.")
                .arg(QStringLiteral(SWAK_VERSION)), QMessageBox::Close, this);
        dialog->setObjectName(QStringLiteral("aboutDialog"));
        dialog->setTextFormat(Qt::PlainText);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
    auto *status = new QLabel(this);
    statusBar()->addPermanentWidget(status);
    auto *latest = new QLabel(this);
    latest->setObjectName(QStringLiteral("latestActivity"));
    latest->setTextFormat(Qt::PlainText);
    latest->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(latest, 1);
    auto *statusTimer = new QTimer(latest);
    statusTimer->setSingleShot(true);
    statusTimer->setInterval(3000);
    statusTimer->setTimerType(Qt::PreciseTimer);
    connect(statusTimer, &QTimer::timeout, latest, &QLabel::clear);
    connect(activity, &ActivityModel::latest, latest, [latest, statusTimer](const QString &message, bool error) {
        latest->setText(message);
        latest->setStyleSheet(error ? QStringLiteral("color: #e53935;") : QString{});
        statusTimer->start();
    });
    const auto updateStatus = [this, status] { status->setText(tr("%1 / %2 events")
                                                          .arg(store_.rowCount()).arg(MaxEvents)); };
    connect(&store_, &Store::changed, this, updateStatus);
    updateStatus();
    preferencesStatus_ = new QLabel(this);
    preferencesStatus_->setObjectName(QStringLiteral("preferencesStatus"));
    preferencesStatus_->setTextFormat(Qt::PlainText);
    preferencesStatus_->setWordWrap(true);
    preferencesStatus_->setMaximumWidth(420);
    statusBar()->addPermanentWidget(preferencesStatus_);
    restorePreferences();
    connect(mapSources, &MapSources::changed, this, [this, mapSources] {
        savePreferences();
        const auto source = mapSources->activeSource();
        logMessage(Severity::Info, Urgency::Routine, QStringLiteral("swak.maps"), source.isEmpty()
            ? tr("Offline map selected.") : tr("Map provider: %1").arg(source.value(QStringLiteral("name")).toString()));
    });
    logMessage(Severity::Info, Urgency::Routine, QStringLiteral("swak.application"), tr("Workspace ready."));
    preferencesStatus_->setText(preferences_.error());
    preferencesStatus_->setToolTip(preferences_.path());
    const auto application = preferences_.document().value(QStringLiteral("application")).toObject();
    saveTimer_.setInterval(preferenceInteger(application, QStringLiteral("saveInterval"), 750, 250, 60000));
    settingsGeometry_ = geometryBytes(application, QStringLiteral("settingsGeometry"));
    mapManagerGeometry_ = geometryBytes(application, QStringLiteral("mapManagerGeometry"));
    settingsNavigation_ = application.value(QStringLiteral("settingsNavigation")).toObject();
    connect(&saveTimer_, &QTimer::timeout, this, [this] {
        if (isVisible() && !closing_) savePreferences();
    });
}
Window::~Window()
{
    saveTimer_.stop();
    if (shown_ && !closing_) savePreferences();
    const auto docks = findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto *dock : docks) delete dock;
    delete takeCentralWidget();
}
void Window::restorePreferences()
{
    const auto &document = preferences_.document();
    const auto window = document.value(QStringLiteral("window")).toObject();
    const auto geometry = geometryBytes(window, QStringLiteral("geometry"));
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const auto layout = geometryBytes(window, QStringLiteral("layout"));
    const bool restored = !layout.isEmpty() && restoreState(layout, LayoutVersion);
    const auto panes = document.value(QStringLiteral("panes")).toObject();
    for (auto *dock : findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly)) {
        const auto pane = panes.value(dock->objectName()).toObject();
        if (!restored && !pane.isEmpty()) {
            const int area = preferenceInteger(pane, QStringLiteral("area"), Qt::RightDockWidgetArea, 1, 8);
            if (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea ||
                area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea)
                addDockWidget(static_cast<Qt::DockWidgetArea>(area), dock);
            dock->setFloating(pane.value(QStringLiteral("floating")).toBool(false));
            dock->setVisible(pane.value(QStringLiteral("visible")).toBool(true));
        }
        if (dock->isFloating()) {
            const auto floatingGeometry = geometryBytes(pane, QStringLiteral("geometry"));
            if (!floatingGeometry.isEmpty()) dock->restoreGeometry(floatingGeometry);
        }
    }
    const auto components = document.value(QStringLiteral("components")).toObject();
    for (auto *participant : participants()) {
        const auto values = components.value(participant->preferenceId());
        if (values.isObject()) participant->restorePreferences(values.toObject());
    }
    fitWindowsToScreens();
}
bool Window::savePreferences()
{
    auto document = preferences_.document();
    auto window = document.value(QStringLiteral("window")).toObject();
    window.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    window.insert(QStringLiteral("layout"), QString::fromLatin1(saveState(LayoutVersion).toBase64()));
    document.insert(QStringLiteral("window"), window);
    auto panes = document.value(QStringLiteral("panes")).toObject();
    for (auto *dock : findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly)) {
        auto pane = panes.value(dock->objectName()).toObject();
        pane.insert(QStringLiteral("area"), static_cast<int>(dockWidgetArea(dock)));
        pane.insert(QStringLiteral("floating"), dock->isFloating());
        pane.insert(QStringLiteral("visible"), !dock->isHidden());
        if (dock->isFloating()) pane.insert(QStringLiteral("geometry"), QString::fromLatin1(dock->saveGeometry().toBase64()));
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
    application.insert(QStringLiteral("settingsGeometry"), QString::fromLatin1(settingsGeometry_.toBase64()));
    application.insert(QStringLiteral("settingsNavigation"), settingsNavigation_);
    application.insert(QStringLiteral("mapManagerGeometry"), QString::fromLatin1(mapManagerGeometry_.toBase64()));
    document.insert(QStringLiteral("application"), application);
    document.insert(QStringLiteral("version"), 1);
    const bool saved = preferences_.save(document);
    preferencesStatus_->setText(preferences_.error());
    if (!saved) logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.preferences"), tr("The application cannot save preferences."));
    return saved;
}
QList<PreferenceParticipant *> Window::participants() const
{
    QList<PreferenceParticipant *> result;
    const auto append = [&result](QObject *object) {
        if (auto *participant = dynamic_cast<PreferenceParticipant *>(object); participant && !result.contains(participant))
            result.append(participant);
    };
    for (auto *plugin : plugins_) append(plugin);
    for (auto *object : findChildren<QObject *>()) append(object);
    return result;
}
void Window::openSettings()
{
    QList<SettingsPage> pages;
    pages.append({QStringLiteral("logging"), tr("Logging"), {
        booleanSetting(QStringLiteral("enabled"), tr("Enable text logging to standard error")),
        choiceSetting(QStringLiteral("severity"), tr("Minimum severity"), {tr("Debug"), tr("Info"), tr("Warning"), tr("Error"), tr("Fatal")}),
        choiceSetting(QStringLiteral("urgency"), tr("Minimum urgency"), {tr("Routine"), tr("Attention"), tr("Immediate")}),
        booleanSetting(QStringLiteral("qt"), tr("Include Qt diagnostics")),
        textSetting(QStringLiteral("prefix"), tr("Category prefix (empty means all)"), 128),
        numberSetting(QStringLiteral("rate"), tr("Maximum messages per second"), 1, 1000)},
        loggingConfiguration(), {}, [](const QJsonObject &values) { configureLogging(values); }});
    pages.append({QStringLiteral("application"), tr("Workspace"), {
        numberSetting(QStringLiteral("width"), tr("Window width"), minimumWidth(), 16384),
        numberSetting(QStringLiteral("height"), tr("Window height"), minimumHeight(), 16384),
        numberSetting(QStringLiteral("x"), tr("Window X position"), -65536, 65536),
        numberSetting(QStringLiteral("y"), tr("Window Y position"), -65536, 65536),
        booleanSetting(QStringLiteral("maximized"), tr("Maximized")),
        numberSetting(QStringLiteral("saveInterval"), tr("Automatic save interval (milliseconds)"), 250, 60000)},
        {{QStringLiteral("width"), normalGeometry().width()}, {QStringLiteral("height"), normalGeometry().height()},
         {QStringLiteral("x"), normalGeometry().x()}, {QStringLiteral("y"), normalGeometry().y()},
         {QStringLiteral("maximized"), isMaximized()}, {QStringLiteral("saveInterval"), saveTimer_.interval()}}, {},
        [this](const QJsonObject &values) {
            showNormal();
            setGeometry(values.value(QStringLiteral("x")).toInt(), values.value(QStringLiteral("y")).toInt(),
                        values.value(QStringLiteral("width")).toInt(), values.value(QStringLiteral("height")).toInt());
            fitWindowsToScreens();
            if (values.value(QStringLiteral("maximized")).toBool()) showMaximized();
            saveTimer_.setInterval(values.value(QStringLiteral("saveInterval")).toInt());
        }});
    for (auto *participant : participants()) {
        if (auto *provider = dynamic_cast<SettingsParticipant *>(participant)) {
            pages.append({provider->preferenceId(), provider->settingsTitle(), provider->settingsFields(), provider->savePreferences(),
                [provider](const QJsonObject &values) { return provider->validateSettings(values); },
                [provider](const QJsonObject &values) { provider->restorePreferences(values); }});
        }
    }
    for (auto *plugin : plugins_) {
        if (dynamic_cast<SettingsParticipant *>(plugin) || qobject_cast<ToolPlugin *>(plugin) ||
            qobject_cast<SourcePlugin *>(plugin) || qobject_cast<PanePlugin *>(plugin) || qobject_cast<ViewPlugin *>(plugin)) continue;
        QString title;
        if (auto *codec = qobject_cast<Codec *>(plugin)) title = codec->name();
        else if (auto *layer = qobject_cast<MapLayer *>(plugin)) title = layer->name();
        if (!title.isEmpty()) pages.append({QString::fromLatin1(plugin->metaObject()->className()), title, {}, {}, {}, {}});
    }
    for (auto *dock : findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly)) {
        pages.append({dock->objectName(), dock->windowTitle(), {
            booleanSetting(QStringLiteral("visible"), tr("Visible")), booleanSetting(QStringLiteral("floating"), tr("Detached")),
            choiceSetting(QStringLiteral("area"), tr("Dock area"), {tr("Left"), tr("Right"), tr("Top"), tr("Bottom")}, {1, 2, 4, 8}),
            numberSetting(QStringLiteral("width"), tr("Width"), 100, 16384), numberSetting(QStringLiteral("height"), tr("Height"), 100, 16384),
            numberSetting(QStringLiteral("x"), tr("Detached X position"), -65536, 65536),
            numberSetting(QStringLiteral("y"), tr("Detached Y position"), -65536, 65536)},
            {{QStringLiteral("visible"), !dock->isHidden()}, {QStringLiteral("floating"), dock->isFloating()},
             {QStringLiteral("area"), static_cast<int>(dockWidgetArea(dock))},
             {QStringLiteral("width"), dock->width()}, {QStringLiteral("height"), dock->height()},
             {QStringLiteral("x"), dock->x()}, {QStringLiteral("y"), dock->y()}}, {},
            [this, dock](const QJsonObject &values) {
                addDockWidget(static_cast<Qt::DockWidgetArea>(values.value(QStringLiteral("area")).toInt()), dock);
                dock->setFloating(values.value(QStringLiteral("floating")).toBool());
                if (dock->isFloating()) dock->setGeometry(values.value(QStringLiteral("x")).toInt(), values.value(QStringLiteral("y")).toInt(),
                    values.value(QStringLiteral("width")).toInt(), values.value(QStringLiteral("height")).toInt());
                else {
                    resizeDocks({dock}, {values.value(QStringLiteral("width")).toInt()}, Qt::Horizontal);
                    resizeDocks({dock}, {values.value(QStringLiteral("height")).toInt()}, Qt::Vertical);
                }
                dock->setVisible(values.value(QStringLiteral("visible")).toBool());
                fitWindowsToScreens();
            }, true});
    }
    SettingsDialog dialog(std::move(pages), [this] {
        return savePreferences() ? QString{} : preferences_.error();
    }, this);
    dialog.restoreNavigation(settingsNavigation_);
    if (!settingsGeometry_.isEmpty()) dialog.restoreGeometry(settingsGeometry_);
    if (screen()) dialog.setGeometry(fitWindowRectangle(dialog.geometry(), {screen()->availableGeometry()}));
    dialog.exec();
    settingsGeometry_ = dialog.saveGeometry();
    settingsNavigation_ = dialog.navigation();
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
        if (!widget->isMaximized() && !widget->isFullScreen())
            widget->setGeometry(fitWindowRectangle(widget->geometry(), screens));
    };
    fit(this);
    for (auto *dock : findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly)) {
        if (dock->isFloating()) fit(dock);
    }
}
void Window::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (!shown_) {
        shown_ = true;
        QTimer::singleShot(0, this, [this] { fitWindowsToScreens(); });
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

}
