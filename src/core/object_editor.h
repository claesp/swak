#pragma once
#include "core/map_selection.h"
#include "core/user_objects.h"

#include <optional>
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QLabel;
class QComboBox;
class QCheckBox;
class QButtonGroup;
class QGridLayout;
namespace swak {
class MarkerCollections;
class ObjectEditor : public PaneContent
{
    Q_OBJECT
public:
    ObjectEditor(UserObjects &objects, MarkerCollections &collections, QWidget *parent);
    QString newCollection() const;
    QString newSymbol() const;
    void edit(const UserObject &item);
    bool setGeometry(const QJsonObject &geometry);
    void reportError(const QString &message);
    QJsonObject editorState() const;
    void restoreEditorState(const QJsonObject &values);
    bool cancel();
    const std::optional<UserObject> &draft() const
    {
        return draft_;
    }
    Qt::DockWidgetArea preferredDockArea() const override
    {
        return Qt::RightDockWidgetArea;
    }
signals:
    void applied(const QString &id);
    void cancelled();
    void configurationChanged();

protected:
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void apply();
    void refreshCollections(const QString &selected);
    void refreshTypes(const QString &selected);
    void refreshColors();
    UserObjects &objects_;
    std::optional<UserObject> draft_;
    QLineEdit *title_;
    QComboBox *type_;
    QComboBox *collection_;
    QLabel *collectionLabel_;
    MarkerCollections &collections_;
    QString lastCollection_;
    QString lastSymbol_;
    QGridLayout *colorLayout_;
    QButtonGroup *colors_;
    QString color_;
    QCheckBox *restricted_;
    QPlainTextEdit *description_;
    QTableWidget *properties_;
    QLabel *status_;
};
} // namespace swak
