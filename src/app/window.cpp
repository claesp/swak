#include "app/window.h"
#include "core/interfaces.h"
#include <QAction>
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
    : QMainWindow(parent), preferences_(preferencesPath)
{
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
    }
    if (view) {
        setCentralWidget(view->create(store_, layers, this));
    } else {
        auto *empty = new QLabel(tr("No map view plugin is available."), this);
        empty->setObjectName(QStringLiteral("missingMap"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        setCentralWidget(empty);
    }
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    auto *quit = fileMenu->addAction(tr("&Quit"));
    quit->setObjectName(QStringLiteral("quitAction"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));
    const auto createDock = [this, viewMenu](const QString &title, const QString &id,
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
        viewMenu->addAction(dock->toggleViewAction());
        return dock;
    };
    QList<QDockWidget *> sideDocks;
    for (auto *plugin : plugins) {
        if (auto *tool = qobject_cast<ToolPlugin *>(plugin); tool && codec) {
            const bool isLayer = qobject_cast<MapLayer *>(plugin) != nullptr;
            const QString id = isLayer ? QStringLiteral("eventsDock")
                                       : QString::fromLatin1(plugin->metaObject()->className()) + QStringLiteral("Dock");
            auto *dock = createDock(tool->name(), id, tool->create(store_, *codec, this),
                                    isLayer ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea);
            if (isLayer) {
                resizeDocks({dock}, {170}, Qt::Vertical);
            } else {
                sideDocks.append(dock);
                names.append(tool->name());
            }
        }
    }
    for (auto *plugin : plugins) {
        if (auto *source = qobject_cast<SourcePlugin *>(plugin); source && codec) {
            sideDocks.append(createDock(source->name(),
                QString::fromLatin1(plugin->metaObject()->className()) + QStringLiteral("Dock"),
                source->create(store_, *codec, this), Qt::RightDockWidgetArea));
            names.append(source->name());
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
    defaultLayout_ = saveState(LayoutVersion);
    auto *resetLayout = viewMenu->addAction(tr("Reset layout"));
    resetLayout->setObjectName(QStringLiteral("resetLayoutAction"));
    connect(resetLayout, &QAction::triggered, this, [this] {
        restoreState(defaultLayout_, LayoutVersion);
        fitWindowsToScreens();
        savePreferences();
    });
    auto *eventsMenu = menuBar()->addMenu(tr("&Events"));
    auto *clear = eventsMenu->addAction(tr("&Clear events"));
    connect(clear, &QAction::triggered, &store_, &Store::clear);
    auto *status = new QLabel(this);
    statusBar()->addWidget(status);
    const auto updateStatus = [this, status] { status->setText(tr("%1 / %2 events · Offline map")
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
    preferencesStatus_->setText(preferences_.error());
    preferencesStatus_->setToolTip(preferences_.path());
    saveTimer_.setInterval(750);
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
    for (auto *widget : findChildren<QWidget *>()) {
        if (auto *participant = dynamic_cast<PreferenceParticipant *>(widget)) {
            const auto values = components.value(participant->preferenceId());
            if (values.isObject()) participant->restorePreferences(values.toObject());
        }
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
    for (const auto *widget : findChildren<QWidget *>()) {
        if (const auto *participant = dynamic_cast<const PreferenceParticipant *>(widget)) {
            auto values = components.value(participant->preferenceId()).toObject();
            mergeValues(values, participant->savePreferences());
            components.insert(participant->preferenceId(), values);
        }
    }
    document.insert(QStringLiteral("components"), components);
    document.insert(QStringLiteral("version"), 1);
    const bool saved = preferences_.save(document);
    preferencesStatus_->setText(preferences_.error());
    return saved;
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
