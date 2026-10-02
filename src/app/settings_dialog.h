#pragma once
#include "core/settings.h"
#include <QDialog>
#include <functional>

class QLabel;
namespace swak {
struct SettingsPage {
    QString id;
    QString title;
    QList<SettingField> fields;
    QJsonObject values;
    std::function<QString(const QJsonObject &)> validate;
    std::function<void(const QJsonObject &)> apply;
    bool paneCustomization = false;
};
class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(QList<SettingsPage> pages, std::function<QString()> save, QWidget *parent);
    QJsonObject navigation() const;
    void restoreNavigation(const QJsonObject &values);
private:
    bool applyChanges();
    QList<SettingsPage> pages_;
    QList<QList<QWidget *>> editors_;
    std::function<QString()> save_;
    QLabel *status_;
};
}
