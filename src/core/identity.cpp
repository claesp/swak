#include "core/identity.h"
#include <QUuid>
namespace swak {
Identity::Identity(QObject *parent) : QObject(parent), values_{{QStringLiteral("uid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
    {QStringLiteral("callsign"), QStringLiteral("SWAK")}, {QStringLiteral("type"), QStringLiteral("a-f-G-U")},
    {QStringLiteral("how"), QStringLiteral("h-e")}, {QStringLiteral("team"), QStringLiteral("Cyan")}, {QStringLiteral("role"), QStringLiteral("Team Member")}} {}
QList<SettingField> Identity::settingsFields() const
{
    return {textSetting(QStringLiteral("uid"), tr("UID"), 128), textSetting(QStringLiteral("callsign"), tr("Callsign"), 128),
        textSetting(QStringLiteral("type"), tr("Type"), 128), textSetting(QStringLiteral("how"), tr("Location method"), 3),
        textSetting(QStringLiteral("team"), tr("Team"), 64), textSetting(QStringLiteral("role"), tr("Role"), 64)};
}
QString Identity::validateSettings(const QJsonObject &values) const
{
    Event event; event.uid = values.value(QStringLiteral("uid")).toString(); event.callsign = values.value(QStringLiteral("callsign")).toString();
    event.type = values.value(QStringLiteral("type")).toString(); event.how = values.value(QStringLiteral("how")).toString();
    event.team = values.value(QStringLiteral("team")).toString(); event.role = values.value(QStringLiteral("role")).toString();
    event.time = event.start = QDateTime::currentDateTimeUtc(); event.stale = event.time.addSecs(300);
    return eventError(event);
}
void Identity::restorePreferences(const QJsonObject &values)
{
    if (!validateSettings(values).isEmpty()) return;
    values_ = values; emit changed();
}
void Identity::apply(Event &event) const
{
    event.uid = values_.value(QStringLiteral("uid")).toString(); event.callsign = values_.value(QStringLiteral("callsign")).toString();
    event.type = values_.value(QStringLiteral("type")).toString(); event.how = values_.value(QStringLiteral("how")).toString();
    event.team = values_.value(QStringLiteral("team")).toString(); event.role = values_.value(QStringLiteral("role")).toString();
}
}
