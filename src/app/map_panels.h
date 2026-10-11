#pragma once
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
namespace swak {
class MapPanels final : public QMainWindow
{
public:
    explicit MapPanels(QWidget *map);
    void track(QDockWidget *pane);
    // Modern panes float with a margin over the map and paint rounded glass behind them.
    void setModern(bool modern);
    bool modern() const
    {
        return modern_;
    }
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
    QTimer maskTimer_;
    bool modern_ = false;
    QPointer<QWidget> commandDock_;
};
} // namespace swak
