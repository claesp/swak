#include "core/emergency.h"

#include "core/cot_detail.h"

#include <algorithm>

namespace swak {
namespace {
// ATAK keeps an emergency until it cancels it. TAK Server repeats it until the cancel.
constexpr int EmergencyLifetimeSeconds = 24 * 60 * 60;
QString emergencyUid(const Event &sender)
{
    return sender.uid.left(120) + QStringLiteral("-9-1-1");
}
std::vector<CotElement> senderDetail(const Event &sender, CotElement emergency)
{
    CotElement link{QStringLiteral("link"),
                    {{QStringLiteral("uid"), sender.uid},
                     {QStringLiteral("type"), sender.type},
                     {QStringLiteral("relation"), QStringLiteral("p-p")}},
                    {},
                    {}};
    CotElement contact{
        QStringLiteral("contact"),
        {{QStringLiteral("callsign"), sender.callsign.left(120) + QStringLiteral("-Alert")}},
        {},
        {}};
    return {link, contact, std::move(emergency)};
}
} // namespace

const QList<EmergencyKind> &emergencyKinds()
{
    static const QList<EmergencyKind> kinds{
        {QStringLiteral("911 Alert"), QStringLiteral("b-a-o-tbl")},
        {QStringLiteral("Ring The Bell"), QStringLiteral("b-a-o-pan")},
        {QStringLiteral("Troops In Contact"), QStringLiteral("b-a-o-opn")},
        {QStringLiteral("Geo-fence Breached"), QStringLiteral("b-a-g")}};
    return kinds;
}
Event emergencyEvent(const Event &sender, const QString &label)
{
    const auto &kinds = emergencyKinds();
    const auto kind = std::find_if(kinds.cbegin(), kinds.cend(), [&label](const auto &item) {
        return item.label == label;
    });
    Event event = sender;
    event.uid = emergencyUid(sender);
    event.type = kind == kinds.cend() ? QString{} : kind->cotType;
    event.how = QStringLiteral("h-e");
    event.course.reset();
    event.speed.reset();
    event.time = event.start = QDateTime::currentDateTimeUtc();
    event.stale = event.time.addSecs(EmergencyLifetimeSeconds);
    event.origin.clear();
    event.detailTruncated = false;
    event.detail = detailText(senderDetail(
        sender,
        {QStringLiteral("emergency"), {{QStringLiteral("type"), label}}, sender.callsign, {}}));
    event.callsign = sender.callsign.left(120) + QStringLiteral("-Alert");
    event.team.clear();
    event.role.clear();
    return event;
}
Event emergencyCancelEvent(const Event &sender)
{
    auto event = emergencyEvent(sender, emergencyKinds().first().label);
    event.type = QString::fromLatin1(EmergencyCancelType);
    event.stale = event.time.addSecs(60);
    event.detail = detailText(senderDetail(sender,
                                           {QStringLiteral("emergency"),
                                            {{QStringLiteral("cancel"), QStringLiteral("true")}},
                                            sender.callsign,
                                            {}}));
    return event;
}
bool activeEmergency(const Event &event)
{
    const auto &kinds = emergencyKinds();
    const bool known = std::any_of(kinds.cbegin(), kinds.cend(), [&event](const auto &kind) {
        return kind.cotType == event.type;
    });
    // b-a-o-c is the ATAK custom emergency.
    if (!known && event.type != QLatin1String("b-a-o-c")) {
        return false;
    }
    const auto details = emergency(event);
    return !details || !details->cancel;
}
QString emergencyLabel(const Event &event)
{
    if (const auto details = emergency(event); details && !details->type.isEmpty()) {
        return details->type;
    }
    const auto &kinds = emergencyKinds();
    const auto kind = std::find_if(kinds.cbegin(), kinds.cend(), [&event](const auto &item) {
        return item.cotType == event.type;
    });
    return kind == kinds.cend() ? event.type : kind->label;
}
} // namespace swak
