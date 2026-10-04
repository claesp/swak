#include "core/settings.h"

#include <utility>
namespace swak {
SettingField booleanSetting(QString path, QString label)
{
    return {std::move(path), std::move(label), SettingField::Kind::Boolean, 0, 0, 0, {}, {}};
}
SettingField
numberSetting(QString path, QString label, double minimum, double maximum, int decimals)
{
    return {std::move(path),
            std::move(label),
            SettingField::Kind::Number,
            minimum,
            maximum,
            decimals,
            {},
            {}};
}
SettingField textSetting(QString path, QString label, int maxLength)
{
    return {std::move(path),
            std::move(label),
            SettingField::Kind::Text,
            0,
            static_cast<double>(maxLength),
            0,
            {},
            {}};
}
SettingField cotSetting(QString path, QString label, CotField picker)
{
    auto field = textSetting(std::move(path),
                             std::move(label),
                             picker == CotField::How    ? 3
                             : picker == CotField::Type ? 128
                                                        : 64);
    field.picker = picker;
    return field;
}
SettingField keySetting(QString path, QString label)
{
    return {std::move(path), std::move(label), SettingField::Kind::Key, 0, 32, 0, {}, {}};
}
SettingField choiceSetting(QString path, QString label, QStringList choices, QJsonArray values)
{
    if (values.isEmpty()) {
        for (int i = 0; i < choices.size(); ++i) {
            values.append(i);
        }
    }
    return {std::move(path),
            std::move(label),
            SettingField::Kind::Choice,
            0,
            0,
            0,
            std::move(choices),
            std::move(values)};
}
QJsonValue settingValue(const QJsonValue &root, const QString &path)
{
    QJsonValue value = root;
    for (const auto &key : path.split('/')) {
        value = value.isArray() ? value.toArray().at(key.toInt()) : value.toObject().value(key);
    }
    return value;
}
QJsonValue withSettingValue(QJsonValue root, const QString &path, const QJsonValue &value)
{
    const auto slash = path.indexOf('/');
    const auto key = slash < 0 ? path : path.first(slash);
    const auto updated =
        slash < 0 ? value : withSettingValue(settingValue(root, key), path.mid(slash + 1), value);
    if (root.isArray()) {
        auto array = root.toArray();
        const int index = key.toInt();
        if (index >= 0 && index < array.size()) {
            array.replace(index, updated);
        }
        return array;
    }
    auto object = root.toObject();
    object.insert(key, updated);
    return object;
}
} // namespace swak
