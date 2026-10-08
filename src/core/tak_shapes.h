#pragma once
#include "core/event.h"
#include "core/user_objects.h"

#include <QColor>
#include <QList>

#include <optional>

namespace swak {
// One user that receives a directed share. TAK Server reads the marti dest element.
struct ShareRecipient
{
    QString uid;
    QString callsign;
    bool operator==(const ShareRecipient &) const = default;
};
// Shared objects stay current on receivers for one day. SWAK sends them again before then.
inline constexpr int SharedObjectStaleSeconds = 24 * 60 * 60;

// Returns the signed 32-bit ARGB integer that TAK uses for colors, as text.
QString takArgb(const QColor &color);
// Returns the TAK CoT type for a local object, or an empty string when TAK has no equal type.
QString sharedCotType(const UserObject &item);
// Builds the TAK CoT event for a local object. The sender is the identity of this client.
// Without recipients, TAK Server sends the event to the groups of the sender.
// Returns no event when the object type or geometry cannot be sent.
std::optional<Event> sharedObjectEvent(const UserObject &item,
                                       const Event &sender,
                                       const QList<ShareRecipient> &recipients,
                                       const QDateTime &now);
// Builds the TAK delete event (t-x-d-d) that removes a shared object from receivers.
Event sharedDeleteEvent(const QString &uid,
                        const QString &type,
                        const QList<ShareRecipient> &recipients,
                        const QDateTime &now);
} // namespace swak
