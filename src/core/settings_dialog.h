#pragma once
#include "core/settings.h"

#include <QDialog>

#include <functional>

class QLabel;
class QComboBox;
namespace swak {
struct SettingsPage
{
    QString id;
    QString title;
    QList<SettingField> fields;
    QJsonObject values;
    std::function<QString(const QJsonObject &)> validate;
    std::function<void(const QJsonObject &)> apply;
    bool paneCustomization = false;
    std::function<QWidget *(QWidget *)> createContent = {};
    QJsonObject defaults = {};
};
class SettingsDialog final : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(QList<SettingsPage> pages,
                   std::function<QString()> save,
                   QWidget *parent,
                   bool providerForm = false,
                   bool newProvider = false);
    QJsonObject navigation() const;
    void restoreNavigation(const QJsonObject &values);

private:
    bool applyChanges();
    QList<SettingsPage> pages_;
    QList<QList<QWidget *>> editors_;
    std::function<QString()> save_;
    QLabel *status_;
    bool newProvider_ = false;
    QComboBox *providerChoice_ = nullptr;
};
} // namespace swak
