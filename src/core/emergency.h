#pragma once
#include "core/event.h"

#include <QList>
#include <QString>

namespace swak {
// One ATAK emergency kind. The label is the emergency type text that TAK Server repeats.
struct EmergencyKind
{
    QString label;
    QString cotType;
};
// 911 Alert, Ring The Bell, Troops In Contact, and Geo-fence Breached.
const QList<EmergencyKind> &emergencyKinds();
inline constexpr char EmergencyCancelType[] = "b-a-o-can";
// Builds an emergency for the sender, as ATAK does: the UID is the sender UID with "-9-1-1",
// a link points to the sender, and the contact callsign ends with "-Alert". The position is the
// sender position. Returns an event that fails eventError() for an unknown label.
Event emergencyEvent(const Event &sender, const QString &label);
// Builds the cancel event for the sender's emergency. It uses the same UID.
Event emergencyCancelEvent(const Event &sender);
// True for an emergency event that is not cancelled.
bool activeEmergency(const Event &event);
// The emergency type text, such as "911 Alert", or the CoT type for an unknown kind.
QString emergencyLabel(const Event &event);
} // namespace swak
