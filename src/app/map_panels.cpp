#include "app/map_panels.h"

#include "core/map_decorations.h"

#include <QApplication>
#include <QDockWidget>
#include <QEvent>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QWheelEvent>

#include <algorithm>
namespace swak {
namespace {
// Shows where a dragged pane docks.
class DropIndicator final : public QWidget
{
public:
    explicit DropIndicator(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("paneDropIndicator"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        auto color = palette().color(QPalette::Highlight);
        painter.setPen(QPen(color, 2));
        color.setAlpha(60);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
    }
};
} // namespace
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
    dropIndicator_ = new DropIndicator(this);
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
    for (auto *pane : findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
        if (pane->isVisible() && !pane->isFloating()) {
            painter.drawRoundedRect(QRectF(QRect(pane->mapTo(this, QPoint{}), pane->size()))
                                        .adjusted(0.5, 0.5, -0.5, -0.5),
                                    12,
                                    12);
        }
    }
    // Only the tab bars of pane groups belong to the overlay. Tab bars inside pane content, also
    // in floating panes, would add rectangles over the map.
    for (auto *tabs : findChildren<QTabBar *>(Qt::FindDirectChildrenOnly)) {
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
    if (dragging_ && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        finishDrag(false);
        event->accept();
        return true;
    }
    // Another window, a dialog, or a hidden map ends the drag. The keyboard grab must not stay.
    if (dragPane_ && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)) {
        finishDrag(false);
    }
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
    if (filterPaneDrag(object, event)) {
        return true;
    }
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
    for (auto *pane : findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
        if (pane->isVisible() && !pane->isFloating()) {
            region += QRect(pane->mapTo(this, QPoint{}), pane->size()).adjusted(-4, -4, 4, 4);
        }
    }
    // Only the tab bars of pane groups belong to the overlay. Tab bars inside pane content, also
    // in floating panes, would add rectangles over the map.
    for (auto *tabs : findChildren<QTabBar *>(Qt::FindDirectChildrenOnly)) {
        if (tabs->isVisible()) {
            region += QRect(tabs->mapTo(this, QPoint{}), tabs->size());
        }
        // Qt creates the tab bars of pane groups as needed. Their tabs also start a drag.
        if (tabs->parentWidget() == this && !tabs->property("swakPaneDrag").toBool()) {
            tabs->setProperty("swakPaneDrag", true);
            tabs->installEventFilter(this);
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
    // The drop place shows during a drag only. It does not cover the map for the HUD.
    if (dropIndicator_->isVisible()) {
        region += dropIndicator_->geometry();
    }
    setMask(region.isEmpty() ? QRegion(-1, -1, 1, 1) : region);
    update();
    parentWidget()->update();
}
bool MapPanels::filterPaneDrag(QObject *object, QEvent *event)
{
    const auto type = event->type();
    auto *source = qobject_cast<QWidget *>(object);
    // A pane that hides during a drag, for example Information when its selection clears,
    // stays hidden. A drop must not show it again.
    if (dragPane_ && object == dragPane_ && type == QEvent::Hide) {
        finishDrag(false);
        return false;
    }
    if (!source || (type != QEvent::MouseButtonPress && type != QEvent::MouseMove &&
                    type != QEvent::MouseButtonRelease)) {
        return false;
    }
    const auto *mouse = static_cast<QMouseEvent *>(event);
    const auto position = mapFromGlobal(mouse->globalPosition().toPoint());
    if (type == QEvent::MouseButtonPress) {
        // A second press, such as a right click, ends a drag without a change.
        if (dragging_) {
            finishDrag(false);
            return true;
        }
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        if (auto *pane = qobject_cast<QDockWidget *>(source)) {
            const int titleBottom = pane->widget() ? pane->widget()->y() : pane->height();
            if (pane->isFloating() || pane->parentWidget() != this ||
                mouse->position().y() >= titleBottom) {
                return false;
            }
            dragPane_ = pane;
            dragSource_ = pane;
            dragStart_ = position;
            // Qt does not start its own drag. On Wayland, that drag cannot dock the pane again.
            return true;
        }
        if (auto *tabs = qobject_cast<QTabBar *>(source); tabs && tabs->parentWidget() == this) {
            dragPane_ = tabPane(*tabs, mouse->position().toPoint());
            dragSource_ = dragPane_ ? tabs : nullptr;
            dragStart_ = position;
        }
        return false;
    }
    if (source != dragSource_ || !dragPane_) {
        return false;
    }
    if (type == QEvent::MouseMove) {
        if (!(mouse->buttons() & Qt::LeftButton)) {
            finishDrag(false);
            return false;
        }
        if (!dragging_) {
            if ((position - dragStart_).manhattanLength() < QApplication::startDragDistance()) {
                return true;
            }
            dragging_ = true;
            grabKeyboard();
            // A deleted pane or tab bar ends the drag at once.
            connect(
                dragPane_, &QObject::destroyed, this, &MapPanels::cancelDrag, Qt::UniqueConnection);
            connect(dragSource_,
                    &QObject::destroyed,
                    this,
                    &MapPanels::cancelDrag,
                    Qt::UniqueConnection);
        }
        updateDrop(position);
        return true;
    }
    const bool dragged = dragging_;
    finishDrag(dragged);
    return dragged;
}
QDockWidget *MapPanels::tabPane(const QTabBar &tabs, QPoint position) const
{
    const int index = tabs.tabAt(position);
    if (index < 0) {
        return nullptr;
    }
    // A tab of a pane group shows the title of its pane.
    const auto title = tabs.tabText(index);
    for (auto *pane : findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
        if (!pane->isFloating() && pane->windowTitle() == title &&
            !tabifiedDockWidgets(pane).isEmpty()) {
            return pane;
        }
    }
    return nullptr;
}
void MapPanels::updateDrop(QPoint position)
{
    dropPane_ = nullptr;
    dropArea_ = Qt::NoDockWidgetArea;
    const auto bounds = rect().marginsRemoved(contentsMargins());
    const int left = position.x() - bounds.left(), right = bounds.right() - position.x(),
              top = position.y() - bounds.top(), bottom = bounds.bottom() - position.y();
    const int nearest = std::min({left, right, top, bottom});
    QRect place;
    // Inside the map, a drop on another pane adds the pane to its tab group.
    if (nearest > DropEdge) {
        for (auto *pane : findChildren<QDockWidget *>(Qt::FindDirectChildrenOnly)) {
            if (pane != dragPane_ && pane->isVisible() && !pane->isFloating() &&
                pane->geometry().contains(position)) {
                dropPane_ = pane;
                place = pane->geometry();
                break;
            }
        }
    }
    // Elsewhere, the nearest edge takes the pane.
    if (!dropPane_) {
        const int width = std::clamp(bounds.width() / 4, 160, 360);
        const int height = std::clamp(bounds.height() / 4, 120, 260);
        if (nearest == left) {
            dropArea_ = Qt::LeftDockWidgetArea;
            place = QRect(bounds.left(), bounds.top(), width, bounds.height());
        } else if (nearest == right) {
            dropArea_ = Qt::RightDockWidgetArea;
            place = QRect(bounds.right() - width + 1, bounds.top(), width, bounds.height());
        } else if (nearest == top) {
            dropArea_ = Qt::TopDockWidgetArea;
            place = QRect(bounds.left(), bounds.top(), bounds.width(), height);
        } else {
            dropArea_ = Qt::BottomDockWidgetArea;
            place = QRect(bounds.left(), bounds.bottom() - height + 1, bounds.width(), height);
        }
    }
    dropIndicator_->setGeometry(place & bounds);
    dropIndicator_->show();
    dropIndicator_->raise();
    maskTimer_.start(0);
}
void MapPanels::finishDrag(bool apply)
{
    const QPointer<QDockWidget> pane = dragPane_;
    const QPointer<QDockWidget> target = dropPane_;
    const auto area = dropArea_;
    if (dragging_) {
        releaseKeyboard();
    }
    for (QObject *watched :
         {static_cast<QObject *>(dragPane_), static_cast<QObject *>(dragSource_)}) {
        if (watched) {
            disconnect(watched, &QObject::destroyed, this, &MapPanels::cancelDrag);
        }
    }
    dragging_ = false;
    dragPane_ = nullptr;
    dragSource_ = nullptr;
    dropPane_ = nullptr;
    dropArea_ = Qt::NoDockWidgetArea;
    dropIndicator_->hide();
    maskTimer_.start(0);
    // Only a visible pane moves. The drop never changes whether a pane shows.
    if (!apply || !pane || pane->isHidden()) {
        return;
    }
    if (target) {
        tabifyDockWidget(target, pane);
    } else if (area != Qt::NoDockWidgetArea &&
               (dockWidgetArea(pane) != area || !tabifiedDockWidgets(pane).isEmpty())) {
        addDockWidget(area, pane);
    }
    // In a tab group, raise selects the tab of the moved pane.
    pane->raise();
}
void MapPanels::cancelDrag()
{
    finishDrag(false);
}
} // namespace swak
