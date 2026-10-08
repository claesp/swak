#include "core/track_history.h"

#include "core/cot.h"
#include "core/projection.h"
#include "core/servers.h"
#include "core/tak_server_api.h"

#include <QUrl>

#include <algorithm>

namespace swak {
QList<HistoryPoint> readHistoryPoints(const QByteArray &xml, const QString &uid)
{
    QList<HistoryPoint> result;
    // Read at most four times the point limit, so a large reply stays bounded.
    qsizetype position = 0;
    for (int count = 0; count < 4 * TrackHistory::MaxPoints; ++count) {
        const auto start = xml.indexOf("<event ", position);
        const auto end = start < 0 ? -1 : xml.indexOf("</event>", start);
        if (end < 0) {
            break;
        }
        position = end + 8;
        const auto decoded = decodeCot(xml.mid(start, position - start));
        if (decoded.event && decoded.event->uid == uid &&
            std::abs(decoded.event->latitude) <= 85.05) {
            result.append(
                {decoded.event->time, project(decoded.event->latitude, decoded.event->longitude)});
        }
    }
    std::stable_sort(
        result.begin(), result.end(), [](const HistoryPoint &a, const HistoryPoint &b) {
            return a.time < b.time;
        });
    if (result.size() > TrackHistory::MaxPoints) {
        result.remove(0, result.size() - TrackHistory::MaxPoints);
    }
    return result;
}
TrackHistory::TrackHistory(Servers *servers, TakServerApi *api, QObject *parent)
    : QObject(parent), servers_(servers), api_(api)
{
    setObjectName(QStringLiteral("trackHistory"));
}
const HistoryTrack *TrackHistory::track(const QString &uid) const
{
    const auto found = std::find_if(tracks_.cbegin(), tracks_.cend(), [&uid](const auto &item) {
        return item.uid == uid;
    });
    return found == tracks_.cend() ? nullptr : &*found;
}
bool TrackHistory::available(const QString &serverId) const
{
    return api_ && servers_ &&
           !TakServerApi::apiUrl(servers_->sources().value(serverId).toObject(),
                                 QStringLiteral("/cot"))
                .isEmpty();
}
QString TrackHistory::load(const QString &serverId,
                           const QString &uid,
                           const QString &label,
                           const QDateTime &start,
                           const QDateTime &end)
{
    if (!available(serverId)) {
        return tr("History needs a TAK Server entry with a REST API.");
    }
    if (uid.isEmpty() || uid.size() > 128 || !start.isValid() || !end.isValid() || start >= end ||
        start.secsTo(end) > MaxWindowSeconds) {
        return tr("Choose a time window of at most seven days.");
    }
    const auto path =
        QStringLiteral("/api/cot/xml/%1/all?start=%2&end=%3")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(uid)),
                 QString::fromLatin1(
                     QUrl::toPercentEncoding(start.toUTC().toString(Qt::ISODateWithMs))),
                 QString::fromLatin1(
                     QUrl::toPercentEncoding(end.toUTC().toString(Qt::ISODateWithMs))));
    api_->martiRequest(
        serverId,
        "GET",
        path,
        {},
        {},
        this,
        [this, serverId, uid, label, start, end](const NetworkReply &reply) {
            if (!reply.ok()) {
                emit loaded(uid,
                            reply.error.isEmpty()
                                ? tr("The server answered with HTTP %1.").arg(reply.status)
                                : reply.error);
                return;
            }
            HistoryTrack track{
                uid, label.left(64), serverId, start, end, readHistoryPoints(reply.body, uid)};
            if (track.points.isEmpty()) {
                emit loaded(uid, tr("The server has no positions in this time window."));
                return;
            }
            add(std::move(track));
        },
        MaxReplyBytes,
        120000);
    return {};
}
void TrackHistory::add(HistoryTrack track)
{
    if (track.uid.isEmpty() || track.points.isEmpty()) {
        return;
    }
    if (track.points.size() > MaxPoints) {
        track.points.remove(0, track.points.size() - MaxPoints);
    }
    const auto uid = track.uid;
    tracks_.removeIf([&uid](const auto &item) {
        return item.uid == uid;
    });
    if (tracks_.size() >= MaxTracks) {
        tracks_.removeFirst();
    }
    tracks_.append(std::move(track));
    emit changed();
    emit loaded(uid, {});
}
void TrackHistory::remove(const QString &uid)
{
    const auto before = tracks_.size();
    tracks_.removeIf([&uid](const auto &item) {
        return item.uid == uid;
    });
    if (tracks_.size() != before) {
        emit changed();
    }
}
void TrackHistory::clear()
{
    if (!tracks_.isEmpty()) {
        tracks_.clear();
        emit changed();
    }
}
} // namespace swak
