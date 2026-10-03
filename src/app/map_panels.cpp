#include "app/map_panels.h"
#include <QDockWidget>
#include <QEvent>
#include <QLayout>
#include <QPainter>
#include <QTabBar>
namespace swak {
MapPanels::MapPanels(QWidget *map) : QMainWindow(map, Qt::Widget)
{
    setWindowFlags(Qt::Widget);
    setObjectName(QStringLiteral("mapPanels"));
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    auto *space = new QWidget(this);
    space->setMinimumSize(240, 180);
    setCentralWidget(space);
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    maskTimer_.setSingleShot(true);
    connect(&maskTimer_, &QTimer::timeout, this, &MapPanels::updateMask);
    map->installEventFilter(this);
    setGeometry(map->rect());
    setMask(QRegion(-1, -1, 1, 1));
    show();
}
void MapPanels::paintEvent(QPaintEvent *)
{
    { QPainter painter(this); painter.setClipRegion(mask()); painter.fillRect(rect(), palette().window()); }
}
void MapPanels::track(QDockWidget *pane)
{
    pane->installEventFilter(this);
    connect(pane, &QDockWidget::visibilityChanged, this, [this] { maskTimer_.start(0); });
    connect(pane, &QDockWidget::topLevelChanged, this, [this] { maskTimer_.start(0); });
    maskTimer_.start(0);
}
bool MapPanels::event(QEvent *event)
{
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Show)
        maskTimer_.start(0);
    return QMainWindow::event(event);
}
bool MapPanels::eventFilter(QObject *object, QEvent *event)
{
    if (object == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Hide)
        maskTimer_.start(0);
    return QMainWindow::eventFilter(object, event);
}
void MapPanels::updateMask()
{
    QRegion region;
    for (auto *pane : findChildren<QDockWidget *>()) if (pane->isVisible() && !pane->isFloating())
        region += QRect(pane->mapTo(this, QPoint{}), pane->size()).adjusted(-4, -4, 4, 4);
    for (auto *tabs : findChildren<QTabBar *>()) if (tabs->isVisible())
        region += QRect(tabs->mapTo(this, QPoint{}), tabs->size());
    region &= rect();
    setMask(region.isEmpty() ? QRegion(-1, -1, 1, 1) : region);
    update();
    parentWidget()->update();
}
}
