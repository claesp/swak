#pragma once
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QPointer>

namespace swak {
class Servers;
class TakServerApi;

// One past position of a track, in normalized Mercator coordinates.
struct HistoryPoint
{
    QDateTime time;
    QPointF world;
};
// The past positions of one object from TAK Server.
struct HistoryTrack
{
    QString uid;
    QString label;
    QString serverId;
    QDateTime start;
    QDateTime end;
    QList<HistoryPoint> points;
};

// Reads past positions of objects from the TAK Server CoT history
// (/Marti/api/cot/xml/{uid}/all) for a time window.
//
// It keeps at most MaxTracks tracks with at most MaxPoints positions each, in time order.
// The Trace plugin continues a loaded track, and the history plugin draws it.
class TrackHistory final : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxTracks = 16;
    static constexpr int MaxPoints = 5000;
    static constexpr qsizetype MaxReplyBytes = 32 * 1024 * 1024;
    static constexpr qint64 MaxWindowSeconds = 7 * 24 * 60 * 60;
    TrackHistory(Servers *servers, TakServerApi *api, QObject *parent = nullptr);
    const QList<HistoryTrack> &tracks() const
    {
        return tracks_;
    }
    const HistoryTrack *track(const QString &uid) const;
    // True when the server entry has a TAK REST API for history queries.
    bool available(const QString &serverId) const;
    // Loads the positions of one object between start and end. loaded reports the result.
    QString load(const QString &serverId,
                 const QString &uid,
                 const QString &label,
                 const QDateTime &start,
                 const QDateTime &end);
    // Adds a track, for example from a load or an imported file. It replaces a track with
    // the same UID and emits loaded.
    void add(HistoryTrack track);
    void remove(const QString &uid);
    void clear();
signals:
    void changed();
    void loaded(const QString &uid, const QString &error);

private:
    QPointer<Servers> servers_;
    QPointer<TakServerApi> api_;
    QList<HistoryTrack> tracks_;
};
// Reads the positions of one UID from a CoT history reply, in time order. It skips other
// UIDs and invalid events and keeps the newest MaxPoints positions.
QList<HistoryPoint> readHistoryPoints(const QByteArray &xml, const QString &uid);
} // namespace swak
