#pragma once
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QTabBar;
namespace swak {
// The overlay that holds the panes over the map. It moves docked panes itself: a drag on a
// title bar or a pane tab shows the drop place, and the release docks the pane there. This
// works also on Wayland, where Qt cannot dock a dragged pane again.
class MapPanels final : public QMainWindow
{
public:
    explicit MapPanels(QWidget *map);
    void track(QDockWidget *pane);
    // Modern panes float with a margin over the map and paint rounded glass behind them.
    void setModern(bool modern);
    // A widget that floats at the bottom center of the free map area, such as the main
    // toolbar in Modern. Null removes it. The panels do not own it.
    void setCommandDock(QWidget *dock);
    QWidget *commandDock() const
    {
        return commandDock_.data();
    }

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void updateMask();
    void placeCommandDock();
    bool filterPaneDrag(QObject *object, QEvent *event);
    QDockWidget *tabPane(const QTabBar &tabs, QPoint position) const;
    void updateDrop(QPoint position);
    void finishDrag(bool apply);
    // Ends a drag without a change and releases the keyboard.
    void cancelDrag();
    QTimer maskTimer_;
    // The pane under a drag, the widget that receives its mouse events, and the drop place.
    // A QWidget pointer: Qt sends events to a pane while its QWidget destructor runs. A
    // QDockWidget pointer would then downcast an object that is no longer a QDockWidget.
    QPointer<QWidget> dragPane_;
    QPointer<QWidget> dragSource_;
    QPoint dragStart_;
    bool dragging_ = false;
    Qt::DockWidgetArea dropArea_ = Qt::NoDockWidgetArea;
    QPointer<QDockWidget> dropPane_;
    QWidget *dropIndicator_ = nullptr;
    bool modern_ = false;
    QPointer<QWidget> commandDock_;
};
} // namespace swak
