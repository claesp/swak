#pragma once
#include <QMainWindow>
#include <QTimer>
namespace swak {
class MapPanels final : public QMainWindow
{
public:
    explicit MapPanels(QWidget *map);
    void track(QDockWidget *pane);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void updateMask();
    QTimer maskTimer_;
};
} // namespace swak
