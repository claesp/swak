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
    Window(const QString &preferencesPath, const QList<QObject *> &plugins, QWidget *parent = nullptr);
    ~Window() override;
    // Native GUI tests use this access to inspect event updates.
    // cppcheck-suppress unusedFunction
    Store &store() { return store_; }
    bool savePreferences();
protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    void restorePreferences();
    void fitWindowsToScreens();
    void openSettings();
    QList<PreferenceParticipant *> participants() const;
    QList<QObject *> plugins_;
    Store store_;
    Preferences preferences_;
    QTimer saveTimer_;
    QByteArray defaultLayout_;
    QLabel *preferencesStatus_ = nullptr;
    bool closing_ = false;
    bool shown_ = false;
    QByteArray settingsGeometry_;
    QByteArray mapManagerGeometry_;
    QJsonObject settingsNavigation_;
};
}
