#pragma once
#include "core/preferences.h"
#include "core/settings_dialog.h"
#include "core/store.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QAction;
class QLabel;
class QMenu;
class QToolBar;
namespace swak {
class DisplayStyle;
enum class DisplayMode;
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
    void setDisplayMode(DisplayMode mode);
    void applyDisplayLayout(DisplayMode mode);
    void updateMenuBar();
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
    DisplayStyle *displayStyle_ = nullptr;
    QPointer<QToolBar> toolbar_;
    // Modern hides the menu bar. This menu holds the menus of the menu bar, and the command dock
    // and F10 open it.
    QMenu *mainMenu_ = nullptr;
    QAction *mainMenuSeparator_ = nullptr;
    QAction *mainMenuShortcut_ = nullptr;
    // Window and pane layouts of the modes that are not active, by mode identifier.
    QHash<QString, std::pair<QByteArray, QByteArray>> modeLayouts_;
};
} // namespace swak
