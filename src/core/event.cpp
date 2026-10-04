#include "core/event.h"

#include <QCoreApplication>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace swak {
QString eventError(const Event &event)
{
    const auto tr = [](const char *text) {
        return QCoreApplication::translate("Event", text);
    };
    const auto safeText = [](const QString &value, int max) {
        if (value.isEmpty() || value.size() > max) {
            return false;
        }
        return std::none_of(value.begin(), value.end(), [](QChar ch) {
            return ch.isNull() || ch.category() == QChar::Other_Control ||
                   ch.category() == QChar::Other_Format || ch.isSurrogate();
        });
    };
    static const QRegularExpression typePattern(
        QStringLiteral("\\A[A-Za-z0-9_]+(?:-[A-Za-z0-9_]+)*(?:;[^;]*)?\\z"));
    static const QRegularExpression howPattern(QStringLiteral("\\A[A-Za-z0-9_]-[A-Za-z0-9_]\\z"));
    if (!safeText(event.uid, 128) || !safeText(event.type, 128) || !safeText(event.how, 3) ||
        (!event.callsign.isEmpty() && !safeText(event.callsign, 128)) ||
        (!event.team.isEmpty() && !safeText(event.team, 64)) ||
        (!event.role.isEmpty() && !safeText(event.role, 64))) {
        return tr("An event text field is empty, too long, or contains unsupported characters.");
    }
    if (!typePattern.match(event.type).hasMatch() || !howPattern.match(event.how).hasMatch()) {
        return tr("The event type or location method is invalid.");
    }
    if (!event.time.isValid() || !event.start.isValid() || !event.stale.isValid() ||
        event.stale <= event.start || event.time > event.stale) {
        return tr("The event times are invalid or out of order.");
    }
    if (!std::isfinite(event.latitude) || event.latitude < -90 || event.latitude > 90 ||
        !std::isfinite(event.longitude) || event.longitude < -180 || event.longitude > 180 ||
        !std::isfinite(event.altitude) || std::abs(event.altitude) > 100000000 ||
        !std::isfinite(event.circularError) || event.circularError < 0 ||
        event.circularError > 100000000 || !std::isfinite(event.linearError) ||
        event.linearError < 0 || event.linearError > 100000000) {
        return tr("The event position or uncertainty is outside the supported range.");
    }
    if ((event.course &&
         (!std::isfinite(*event.course) || *event.course < 0 || *event.course > 360)) ||
        (event.speed &&
         (!std::isfinite(*event.speed) || *event.speed < 0 || *event.speed > 1000))) {
        return tr("The reported heading or speed is outside the supported range.");
    }
    return {};
}
} // namespace swak
