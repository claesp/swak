#pragma once
#include "core/preferences.h"
#include "core/settings_dialog.h"
#include "core/store.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

class QLabel;
namespace swak {
class MapPanels;
class Window final : public QMainWindow
{
    Q_OBJECT
public:
    Window(const QString &preferencesPath,
           const QList<QObject *> &plugins,
           QWidget *parent = nullptr);
    ~Window() override;
    // Native GUI tests use this access to inspect event updates.
    // cppcheck-suppress unusedFunction
    Store &store()
    {
        return store_;
    }
    bool savePreferences();
    void addDockWidget(Qt::DockWidgetArea area, QDockWidget *dock);
    void tabifyDockWidget(QDockWidget *first, QDockWidget *second);
    void resizeDocks(const QList<QDockWidget *> &docks,
                     const QList<int> &sizes,
                     Qt::Orientation orientation);
    Qt::DockWidgetArea dockWidgetArea(QDockWidget *dock) const;
    QList<QDockWidget *> tabifiedDockWidgets(QDockWidget *dock) const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void restorePreferences();
    void fitWindowsToScreens();
    void openSettings();
    QList<SettingsPage> settingsPages();
    QHash<QString, QJsonObject> settingsDefaults_;
    QList<PreferenceParticipant *> participants() const;
    QList<QObject *> plugins_;
    Store store_;
    Preferences preferences_;
    QTimer saveTimer_;
    MapPanels *panels_ = nullptr;
    QByteArray defaultLayout_;
    QByteArray defaultWindowLayout_;
    QLabel *preferencesStatus_ = nullptr;
    QString lastSaveError_;
    QElapsedTimer failedSave_;
    bool closing_ = false;
    bool shown_ = false;
    QByteArray settingsGeometry_;
    QByteArray mapManagerGeometry_;
    QByteArray identityGeometry_;
    QByteArray serversGeometry_;
    QByteArray dataManagerGeometry_;
    QByteArray markerManagerGeometry_;
    QJsonObject settingsNavigation_;
};
} // namespace swak
