#pragma once
#include "core/map_selection.h"
#include "core/user_objects.h"
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QLabel;
namespace swak {
class ObjectEditor : public PaneContent {
    Q_OBJECT
public:
    ObjectEditor(UserObjects &objects, QWidget *parent);
    void edit(const UserObject &item);
    QJsonObject editorState() const;
    void restoreEditorState(const QJsonObject &values);
    bool cancel();
    const std::optional<UserObject> &draft() const { return draft_; }
    Qt::DockWidgetArea preferredDockArea() const override { return Qt::RightDockWidgetArea; }
signals:
    void applied(const QString &id);
    void cancelled();
    void configurationChanged();
protected:
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
private:
    void apply();
    UserObjects &objects_;
    std::optional<UserObject> draft_;
    QLineEdit *title_;
    QPlainTextEdit *description_;
    QTableWidget *properties_;
    QLabel *status_;
};
}
