#pragma once
#include "core/cot_picker.h"
#include "core/preferences.h"

#include <QJsonArray>
#include <QKeySequence>
#include <QStringList>
#include <QtPlugin>

class QWidget;
namespace swak {
// The combined key of a one-letter tool shortcut: A to Z, with or without Shift. Other text,
// such as Ctrl+M or two keys, gives 0.
inline int letterShortcut(const QString &text)
{
    if (text.size() > 32) {
        return 0;
    }
    const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
    if (key.count() != 1 || (key[0].keyboardModifiers() != Qt::NoModifier &&
                             key[0].keyboardModifiers() != Qt::ShiftModifier)) {
        return 0;
    }
    return key[0].key() >= Qt::Key_A && key[0].key() <= Qt::Key_Z ? key[0].toCombined() : 0;
}
struct SettingField
{
    enum class Kind
    {
        Boolean,
        Number,
        Text,
        Choice,
        Key
    };
    QString path;
    QString label;
    Kind kind = Kind::Text;
    double minimum = 0;
    double maximum = 0;
    int decimals = 0;
    QStringList choices;
    QJsonArray choiceValues;
    CotField picker = CotField::None;
    bool unlockToEdit = false;
    bool describeCode = false;
    QString enabledBy = {};
    QJsonValue enabledValue = {};
};
SettingField booleanSetting(QString path, QString label);
SettingField
numberSetting(QString path, QString label, double minimum, double maximum, int decimals = 0);
SettingField textSetting(QString path, QString label, int maxLength);
SettingField cotSetting(QString path, QString label, CotField picker);
SettingField keySetting(QString path, QString label);
SettingField
choiceSetting(QString path, QString label, QStringList choices, QJsonArray values = {});

class SettingsParticipant : public PreferenceParticipant
{
public:
    virtual QString settingsTitle() const = 0;
    virtual QList<SettingField> settingsFields() const = 0;
    virtual QString validateSettings(const QJsonObject &) const
    {
        return {};
    }
};
class SettingsWidgetProvider
{
public:
    virtual ~SettingsWidgetProvider() = default;
    virtual QWidget *createSettingsWidget(QWidget *parent) = 0;
};
QJsonValue settingValue(const QJsonValue &root, const QString &path);
QJsonValue withSettingValue(QJsonValue root, const QString &path, const QJsonValue &value);
} // namespace swak
Q_DECLARE_INTERFACE(swak::SettingsParticipant, "org.swak.Settings/1.0")
