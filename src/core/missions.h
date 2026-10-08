#pragma once
#include "core/event.h"
#include "core/preferences.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace swak {
class Identity;
class Servers;
class Store;
class TakServerApi;

// One mission (Data Sync feed) from the TAK Server mission list.
struct Mission
{
    QString name;
    QString description;
    QString creatorUid;
    QString tool;
    QStringList keywords;
    QDateTime created;
    bool passwordProtected = false;
    int objects = 0;
    int files = 0;
};
// One file in a mission.
struct MissionFile
{
    QString hash;
    QString name;
    QString submitter;
    qint64 size = -1;
};
// One mission log entry.
struct MissionLog
{
    QString id;
    QString creatorUid;
    QString content;
    QDateTime time;
};
// A subscription of this client to a mission on one server entry.
struct MissionSubscription
{
    enum class Role
    {
        ReadOnly,
        Subscriber,
        Owner
    };
    QString serverId;
    QString name;
    Role role = Role::Subscriber;
    bool visible = true;
    // The bearer token from the subscription. SWAK keeps it in memory only.
    QString token;
    QList<MissionFile> files;
    QList<MissionLog> logs;
    int objects = 0;
    QDateTime updated;
    QString status;
    bool editable() const
    {
        return role != Role::ReadOnly;
    }
};
inline constexpr int MaxMissions = 1000;
inline constexpr int MaxMissionObjects = 2048;
inline constexpr int MaxMissionFiles = 256;
inline constexpr int MaxMissionLogs = 500;
inline constexpr int MaxSubscriptions = 32;

// True for a mission name that SWAK can put in a URL path.
bool validMissionName(const QString &name);
// Reads a /Marti/api/missions reply. Returns false for other content.
bool readMissions(const QByteArray &json, QList<Mission> *missions);
// Reads the files of the first mission in a /Marti/api/missions/{name} reply.
QList<MissionFile> readMissionFiles(const QByteArray &json);
// Reads a mission log reply.
QList<MissionLog> readMissionLogs(const QByteArray &json);
// Reads the role and token of a subscription reply.
bool readSubscription(const QByteArray &json, MissionSubscription::Role *role, QString *token);
// Splits a /missions/{name}/cot reply into CoT events. Invalid events are skipped.
QList<Event> readMissionEvents(const QByteArray &xml);
// The Store origin of the events of a mission.
QString missionOrigin(const QString &serverId, const QString &name);

// The mission subscriptions of one window.
//
// The service lists missions, subscribes and unsubscribes, and loads the CoT, files, and log
// of each subscribed mission. A mission change event (t-x-m-c) from the same server starts a
// reload after a short delay. Mission CoT goes to the Store with a mission origin and a
// one-day lifetime. The service reloads each mission every hour, so its objects stay current.
// The preferences keep the subscribed names and visibility, never tokens or passwords.
class Missions final : public QObject, public PreferenceParticipant
{
    Q_OBJECT
public:
    static constexpr int ReloadDelayMilliseconds = 2000;
    static constexpr int RefreshMilliseconds = 60 * 60 * 1000;
    Missions(Store &store,
             Servers *servers,
             TakServerApi *api,
             Identity *identity,
             QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.missions");
    }
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;

    const QList<MissionSubscription> &subscriptions() const
    {
        return subscriptions_;
    }
    const MissionSubscription *subscription(const QString &serverId, const QString &name) const;
    // Reads the mission list of a server. listed reports the result.
    void list(const QString &serverId);
    // Subscribes and loads the mission. subscribed reports the result.
    QString subscribe(const QString &serverId, const QString &name, const QString &password = {});
    void unsubscribe(const QString &serverId, const QString &name);
    void setVisible(const QString &serverId, const QString &name, bool visible);
    // Reads the mission CoT, files, and log again.
    void reload(const QString &serverId, const QString &name);
    // Adds a CoT event to a mission through a marti mission destination.
    QString addEvent(const QString &serverId, const QString &name, const Event &event);
    // Removes an object from a mission.
    QString removeObject(const QString &serverId, const QString &name, const QString &uid);
    // Writes a log entry.
    QString addLog(const QString &serverId, const QString &name, const QString &text);
    // Downloads the mission archive. exported reports the bytes.
    QString exportArchive(const QString &serverId, const QString &name);
    // Invitations from t-x-m-i events, as server and mission name pairs. Tests and later
    // invitation lists read them.
    // cppcheck-suppress unusedFunction
    const QList<std::pair<QString, QString>> &invitations() const
    {
        return invitations_;
    }
    // Receives one mission control event. The Store calls it.
    void receive(const Event &event);
signals:
    void
    listed(const QString &serverId, const QList<swak::Mission> &missions, const QString &error);
    void subscribed(const QString &serverId, const QString &name, const QString &error);
    void changed();
    void exported(const QString &serverId,
                  const QString &name,
                  const QByteArray &archive,
                  const QString &error);
    void invited(const QString &serverId, const QString &name);

private:
    MissionSubscription *find(const QString &serverId, const QString &name);
    QList<std::pair<QByteArray, QByteArray>>
    authorization(const MissionSubscription &mission) const;
    void receiveChange(const QString &serverId, const QString &name);
    void loadEvents(const QString &serverId, const QString &name, const QByteArray &xml);
    void showEvents(const MissionSubscription &mission);
    void hideEvents(const MissionSubscription &mission);
    QString clientUid() const;
    Store &store_;
    QPointer<Servers> servers_;
    QPointer<TakServerApi> api_;
    QPointer<Identity> identity_;
    QList<MissionSubscription> subscriptions_;
    // The last loaded events of each mission, so a hidden mission can show again.
    QHash<QString, QList<Event>> events_;
    QList<std::pair<QString, QString>> invitations_;
    QHash<QString, QTimer *> pendingReloads_;
    QTimer refresh_;
};
} // namespace swak
Q_DECLARE_METATYPE(swak::Mission)
