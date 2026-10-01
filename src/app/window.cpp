#include "app/window.h"
#include "core/interfaces.h"
#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QPluginLoader>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <stdexcept>

namespace swak {
Window::Window(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(tr("SWAK | Cursor-on-Target workspace"));
    resize(1280, 800);
    setMinimumSize(850, 600);
    const auto plugins = QPluginLoader::staticInstances();
    Codec *codec = nullptr;
    ViewPlugin *view = nullptr;
    QList<MapLayer *> layers;
    QStringList names;
    for (auto *plugin : plugins) {
        if (auto *candidate = qobject_cast<Codec *>(plugin)) { codec = candidate; names.append(candidate->name()); }
        if (auto *candidate = qobject_cast<ViewPlugin *>(plugin)) { view = candidate; names.append(candidate->name()); }
        if (auto *layer = qobject_cast<MapLayer *>(plugin)) { layers.append(layer); names.append(layer->name()); }
    }
    if (!codec || !view) throw std::runtime_error("The required built-in plugins are missing.");
    setCentralWidget(view->create(store_, layers, this));
    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    auto *tools = new QDockWidget(tr("Workspace tools"), this);
    tools->setObjectName(QStringLiteral("toolsDock"));
    tools->setMinimumWidth(330);
    auto *tabs = new QTabWidget(tools);
    tabs->setObjectName(QStringLiteral("toolsTabs"));
    tools->setWidget(tabs);
    addDockWidget(Qt::RightDockWidgetArea, tools);
    viewMenu->addAction(tools->toggleViewAction());
    for (auto *plugin : plugins) {
        if (auto *tool = qobject_cast<ToolPlugin *>(plugin)) {
            if (qobject_cast<MapLayer *>(plugin)) {
                auto *dock = new QDockWidget(tool->name(), this);
                dock->setObjectName(QStringLiteral("eventsDock"));
                dock->setWidget(tool->create(store_, *codec, dock));
                addDockWidget(Qt::BottomDockWidgetArea, dock);
                resizeDocks({dock}, {170}, Qt::Vertical);
                viewMenu->addAction(dock->toggleViewAction());
            } else {
                tabs->addTab(tool->create(store_, *codec, tabs), tool->name());
                names.append(tool->name());
            }
        }
    }
    for (auto *plugin : plugins) {
        if (auto *source = qobject_cast<SourcePlugin *>(plugin)) {
            tabs->addTab(source->create(store_, *codec, tabs), source->name());
            names.append(source->name());
        }
    }
    auto *pluginPanel = new QWidget(tabs);
    auto *pluginLayout = new QVBoxLayout(pluginPanel);
    auto *pluginText = new QLabel(tr("Built-in plugins\n\n%1\n\nPlugins run with application permissions. This build does not load external plugin files.").arg(names.join('\n')), pluginPanel);
    pluginText->setTextFormat(Qt::PlainText);
    pluginText->setWordWrap(true);
    pluginLayout->addWidget(pluginText);
    pluginLayout->addStretch();
    tabs->addTab(pluginPanel, tr("Plugins"));
    resizeDocks({tools}, {380}, Qt::Horizontal);
    auto *eventsMenu = menuBar()->addMenu(tr("&Events"));
    auto *clear = eventsMenu->addAction(tr("&Clear events"));
    connect(clear, &QAction::triggered, &store_, &Store::clear);
    auto *quit = eventsMenu->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *status = new QLabel(this);
    statusBar()->addWidget(status);
    const auto updateStatus = [this, status] { status->setText(tr("%1 / %2 events · Offline map · Network reception requires Start")
                                                          .arg(store_.rowCount()).arg(MaxEvents)); };
    connect(&store_, &Store::changed, this, updateStatus);
    updateStatus();
}
Window::~Window()
{
    const auto docks = findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly);
    for (auto *dock : docks) delete dock;
    delete takeCentralWidget();
}
}
