#include "app/window.h"
#include "core/interfaces.h"
#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QPluginLoader>
#include <QStatusBar>
#include <QSizeGrip>
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
        dock->setObjectName(id);
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
        if (auto *tool = qobject_cast<ToolPlugin *>(plugin)) {
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
        if (auto *source = qobject_cast<SourcePlugin *>(plugin)) {
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
    pluginLayout->addStretch();
    sideDocks.append(createDock(tr("Plugins"), QStringLiteral("pluginsDock"), pluginPanel, Qt::RightDockWidgetArea));
    for (qsizetype index = 1; index < sideDocks.size(); ++index) {
        tabifyDockWidget(sideDocks.first(), sideDocks.at(index));
    }
    resizeDocks(sideDocks, QList<int>(sideDocks.size(), 380), Qt::Horizontal);
    sideDocks.first()->raise();
    auto *eventsMenu = menuBar()->addMenu(tr("&Events"));
    auto *clear = eventsMenu->addAction(tr("&Clear events"));
    connect(clear, &QAction::triggered, &store_, &Store::clear);
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
