#include "app/map_panels.h"

#include "core/map_decorations.h"

#include <QDockWidget>
#include <QEvent>
#include <QLayout>
#include <QPainter>
#include <QTabBar>
#include <QWheelEvent>

#include <algorithm>
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
    // Side panes reach the bottom edge, and a bottom pane fills the space between them.
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
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
    QPainter painter(this);
    painter.setClipRegion(mask());
    if (!modern_) {
        painter.fillRect(rect(), palette().window());
        return;
    }
    // Rounded glass behind each docked pane. The map shows through the corners.
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 212, 255, 70), 1));
    painter.setBrush(QColor(12, 12, 20, 222));
    for (auto *pane : findChildren<QDockWidget *>()) {
        if (pane->isVisible() && !pane->isFloating()) {
            painter.drawRoundedRect(QRectF(QRect(pane->mapTo(this, QPoint{}), pane->size()))
                                        .adjusted(0.5, 0.5, -0.5, -0.5),
                                    12,
                                    12);
        }
    }
    for (auto *tabs : findChildren<QTabBar *>()) {
        if (tabs->isVisible()) {
            painter.drawRoundedRect(QRectF(QRect(tabs->mapTo(this, QPoint{}), tabs->size())), 6, 6);
        }
    }
}
void MapPanels::setModern(bool modern)
{
    modern_ = modern;
    // The margin lets panes float off the map edges.
    setContentsMargins(modern ? QMargins(12, 12, 12, 12) : QMargins());
    maskTimer_.start(0);
    update();
}
void MapPanels::setCommandDock(QWidget *dock)
{
    if (commandDock_ && commandDock_ != dock) {
        commandDock_->removeEventFilter(this);
    }
    commandDock_ = dock;
    if (dock) {
        dock->installEventFilter(this);
    }
    maskTimer_.start(0);
}
void MapPanels::placeCommandDock()
{
    if (!commandDock_) {
        return;
    }
    // The free area is the central widget: the map that the panes leave visible.
    const auto free = centralWidget() ? centralWidget()->geometry() : rect();
    const auto hint = commandDock_->sizeHint();
    const int width = std::max(std::min(hint.width(), free.width() - 24), 80);
    const QRect target(
        free.center().x() - width / 2, free.bottom() - 14 - hint.height(), width, hint.height());
    if (commandDock_->geometry() != target) {
        commandDock_->setGeometry(target);
    }
    commandDock_->raise();
}
void MapPanels::track(QDockWidget *pane)
{
    pane->installEventFilter(this);
    connect(pane, &QDockWidget::visibilityChanged, this, [this] {
        maskTimer_.start(0);
    });
    connect(pane, &QDockWidget::topLevelChanged, this, [this] {
        maskTimer_.start(0);
    });
    maskTimer_.start(0);
}
bool MapPanels::event(QEvent *event)
{
    if (event->type() == QEvent::Wheel || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::KeyRelease) {
        event->accept();
        return true;
    }
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize ||
        event->type() == QEvent::Show) {
        maskTimer_.start(0);
    }
    return QMainWindow::event(event);
}
bool MapPanels::eventFilter(QObject *object, QEvent *event)
{
    if (object == parentWidget() && event->type() == QEvent::Wheel && isVisible() &&
        mask().contains(static_cast<QWheelEvent *>(event)->position().toPoint())) {
        event->accept();
        return true;
    }
    if (object == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
        event->type() == QEvent::Show || event->type() == QEvent::Hide) {
        maskTimer_.start(0);
    }
    return QMainWindow::eventFilter(object, event);
}
void MapPanels::updateMask()
{
    QRegion region;
    for (auto *pane : findChildren<QDockWidget *>()) {
        if (pane->isVisible() && !pane->isFloating()) {
            region += QRect(pane->mapTo(this, QPoint{}), pane->size()).adjusted(-4, -4, 4, 4);
        }
    }
    for (auto *tabs : findChildren<QTabBar *>()) {
        if (tabs->isVisible()) {
            region += QRect(tabs->mapTo(this, QPoint{}), tabs->size());
        }
    }
    placeCommandDock();
    if (commandDock_ && commandDock_->isVisible()) {
        region += commandDock_->geometry();
    }
    region &= rect();
    // The central widget is the map area that the panes leave free. The command dock floats
    // at its bottom, so the free area ends above the dock.
    auto free = centralWidget() ? centralWidget()->geometry() : rect();
    if (commandDock_ && commandDock_->isVisible()) {
        free.setBottom(std::min(free.bottom(), commandDock_->geometry().top() - 6));
    }
    parentWidget()->setProperty(MapFreeAreaProperty, free);
    parentWidget()->setProperty(MapOcclusionProperty, QVariant::fromValue(region));
    setMask(region.isEmpty() ? QRegion(-1, -1, 1, 1) : region);
    update();
    parentWidget()->update();
}
} // namespace swak
