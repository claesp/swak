#pragma once
#include "core/store.h"
#include "core/preferences.h"
#include <QMainWindow>
#include <QTimer>

class QLabel;
namespace swak {
class Window final : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(QWidget *parent = nullptr);
    Window(const QString &preferencesPath, const QList<QObject *> &plugins, QWidget *parent = nullptr);
    ~Window() override;
    Store &store() { return store_; }
    bool savePreferences();
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    void restorePreferences();
    void fitWindowsToScreens();
    Store store_;
    Preferences preferences_;
    QTimer saveTimer_;
    QByteArray defaultLayout_;
    QLabel *preferencesStatus_ = nullptr;
    bool closing_ = false;
    bool shown_ = false;
};
}
