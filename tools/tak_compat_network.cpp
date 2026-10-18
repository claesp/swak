#include "core/identity.h"
#include "core/missions.h"
#include "core/network_client.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_channels.h"
#include "core/tak_enrollment.h"
#include "core/tak_server_api.h"
#include "tak_compat_features.h"
#include "tak_load_meter.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <memory>

using namespace swak;
namespace {
constexpr int Wait = 20000;
// UDP and federation can lose the first events, so the checks send again at this interval.
constexpr int ResendMs = 2000;
// Without a cluster, TAK Server keeps missions in a cache of each process for up to 120
// seconds. The messaging process adds stream and federated content, so the API process can
// show an old mission until its cache entry ends. Mission checks wait longer than that.
constexpr int MissionCacheWait = 150000;
// Mission file sizes around the hub replay limit in tools/tak-server/hub.
constexpr qsizetype MissionSmallFile = 1024;
constexpr qsizetype MissionLargeFile = 16384;

bool connected(const Servers &servers, const QString &id)
{
    return servers.statistics().value(id).toObject().value(QStringLiteral("connected")).toBool();
}
bool has(const Store &store, const QString &uid)
{
    return std::any_of(store.events().cbegin(), store.events().cend(), [&uid](const Event &event) {
        return event.uid == uid;
    });
}
// The datagram counters of one entry, for the check details. The worker publishes them every
// 500 ms, so wait for the next publication first.
QString counters(const Servers &servers)
{
    QTest::qWait(1000);
    const auto stats = servers.statistics().value(QStringLiteral("test")).toObject();
    return QStringLiteral("sent %1, received %2")
        .arg(stats.value(QStringLiteral("sent")).toInt())
        .arg(stats.value(QStringLiteral("received")).toInt());
}
// Sends the event again until the condition is true or the time ends.
bool sendUntil(Servers &sender, Event event, const std::function<bool()> &arrived, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(180);
        static_cast<void>(sender.send(event));
        if (QTest::qWaitFor(arrived, ResendMs)) {
            return true;
        }
    }
    return false;
}
QJsonObject meshEntry(const QJsonObject &multicast, const QJsonObject &group, const QString &name)
{
    auto entry = serverDefaults();
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("protocol"), QStringLiteral("mcast"));
    entry.insert(QStringLiteral("host"), group.value(QStringLiteral("host")));
    entry.insert(QStringLiteral("port"), group.value(QStringLiteral("port")));
    entry.insert(QStringLiteral("localAddress"), multicast.value(QStringLiteral("localAddress")));
    entry.insert(QStringLiteral("enabled"), true);
    return entry;
}
// Resident memory of this process in KiB, or -1 when Linux does not report it.
qint64 residentKiB()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) {
        return -1;
    }
    // Proc files report size zero, so read a bounded block instead of testing for the end.
    const auto lines = status.read(16384).split('\n');
    const auto found = std::find_if(lines.cbegin(), lines.cend(), [](const QByteArray &line) {
        return line.startsWith("VmRSS:");
    });
    if (found == lines.cend()) {
        return -1;
    }
    bool ok = false;
    const auto value = found->mid(6).trimmed().split(' ').value(0).toLongLong(&ok);
    return ok ? value : -1;
}
using Record = std::function<void(const QString &, bool, const QString &)>;
const Event *find(const Store &store, const QString &uid)
{
    const auto found =
        std::find_if(store.events().cbegin(), store.events().cend(), [&uid](const Event &event) {
            return event.uid == uid;
        });
    return found == store.events().cend() ? nullptr : &*found;
}
// Server A sends the groups SwakTest and Ops to its federate. Through the hub, the policy lets
// only SwakTest through. In direct federation, server B also delivers Ops events, which shows
// that the hub, not the servers, drops them.
// Connects a client from a profile entry as server entry "test".
QString connectClient(Servers &servers, const QJsonObject &client, const QString &name)
{
    auto entry = serverDefaults();
    for (auto it = client.begin(); it != client.end(); ++it) {
        entry.insert(it.key(), it.value());
    }
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("enabled"), true);
    auto error = servers.put(QStringLiteral("test"), entry);
    if (error.isEmpty() && !QTest::qWaitFor(
                               [&] {
                                   return connected(servers, QStringLiteral("test"));
                               },
                               Wait)) {
        error = servers.status(QStringLiteral("test"));
    }
    return error;
}
void groupFilterCheck(Servers &right,
                      const Store &leftStore,
                      const Store &remote,
                      const Event &a,
                      const QJsonObject &profile,
                      const Record &record)
{
    const auto opsProfile = profile.value(QStringLiteral("ops")).toObject();
    if (opsProfile.isEmpty()) {
        return;
    }
    const bool hub = profile.value(QStringLiteral("federation"))
                         .toObject()
                         .value(QStringLiteral("hub"))
                         .toBool();
    Store opsStore;
    Servers ops(opsStore);
    const auto error = connectClient(ops, opsProfile, QStringLiteral("Ops"));
    const bool ready = error.isEmpty();
    auto allowed = a;
    allowed.uid += QStringLiteral("-ALLOWED");
    allowed.callsign = QStringLiteral("SWAK TEST ALLOWED");
    auto blocked = a;
    blocked.uid += QStringLiteral("-OPS");
    blocked.callsign = QStringLiteral("SWAK TEST OPS");
    // The allowed event shows that the path works. Then the Ops event gets enough time.
    const bool allowedArrived = ready && sendUntil(
                                             right,
                                             allowed,
                                             [&] {
                                                 return has(remote, allowed.uid);
                                             },
                                             3 * Wait);
    const bool blockedArrived = ready && sendUntil(
                                             ops,
                                             blocked,
                                             [&] {
                                                 return has(remote, blocked.uid);
                                             },
                                             hub ? 12000 : 3 * Wait);
    record(QStringLiteral("federationGroupFilter"),
           allowedArrived && blockedArrived != hub,
           QStringLiteral("%1; SwakTest event %2; Ops event %3")
                   .arg(hub ? QStringLiteral("hub policy") : QStringLiteral("direct, no filter"),
                        allowedArrived ? QStringLiteral("delivered") : QStringLiteral("missing"),
                        blockedArrived ? QStringLiteral("delivered") : QStringLiteral("dropped")) +
               (ready ? QString{} : QStringLiteral("; ") + error));
    // The hub policy has a separate edge from the CA group of server B to the CA group of
    // server A, which lets every group through. Ops events from server B arrive.
    const auto reverseProfile = profile.value(QStringLiteral("opsB")).toObject();
    if (reverseProfile.isEmpty()) {
        return;
    }
    Store reverseStore;
    Servers reverse(reverseStore);
    const auto reverseError = connectClient(reverse, reverseProfile, QStringLiteral("Ops B"));
    auto reverseEvent = blocked;
    reverseEvent.uid += QStringLiteral("-B");
    const bool reverseArrived =
        reverseError.isEmpty() && sendUntil(
                                      reverse,
                                      reverseEvent,
                                      [&] {
                                          return has(leftStore, reverseEvent.uid);
                                      },
                                      3 * Wait);
    record(QStringLiteral("federationGroupReverse"),
           reverseArrived,
           reverseError.isEmpty()
               ? (reverseArrived ? QStringLiteral("Ops event from server B delivered")
                                 : QStringLiteral("Ops event from server B missing"))
               : reverseError);
}
// The SWAK test plugins of the hub rename events with the suffix -PLUGIN and send their own
// event to every federate.
// A mission that server A creates for SwakTest appears on server B with its object. SWAK adds
// the object on server A and reads the mission on server B as the directory user.
// The optional hooks cut the federation of server B before server A creates the mission, and
// restore it after the object is added. Each hook returns an error text or nothing.
using Hook = std::function<QString()>;
void missionFederationCheck(Servers &left,
                            Servers &remote,
                            Store &leftStore,
                            Store &remoteStore,
                            TakServerApi &remoteApi,
                            NetworkClient &network,
                            const Event &a,
                            const Record &record,
                            const QString &check = QStringLiteral("missionFederation"),
                            const Hook &disconnect = {},
                            const Hook &reconnect = {},
                            bool expectMissing = false,
                            const QString &filesCheck = {},
                            bool expectLarge = true)
{
    TakServerApi leftApi(left, network);
    Identity leftIdentity, remoteIdentity;
    for (auto *identity : {&leftIdentity, &remoteIdentity}) {
        auto values = identity->savePreferences();
        values.insert(QStringLiteral("uid"),
                      a.uid + (identity == &leftIdentity ? QString{} : QStringLiteral("-LDAP")));
        values.insert(QStringLiteral("callsign"), a.callsign);
        identity->restorePreferences(values);
    }
    const auto name =
        QStringLiteral("swak-federated-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
    const auto cut = disconnect ? disconnect() : QString{};
    if (!cut.isEmpty()) {
        record(check, false, QStringLiteral("disconnect: ") + cut);
        return;
    }
    const auto created = call(leftApi,
                              "PUT",
                              QStringLiteral("/api/missions/%1?creatorUid=%2&group=%3")
                                  .arg(name, a.uid, QStringLiteral("SwakTest")));
    Missions leftMissions(leftStore, &left, &leftApi, &leftIdentity);
    QSignalSpy leftSubscribed(&leftMissions, &Missions::subscribed);
    leftMissions.subscribe(QStringLiteral("test"), name);
    const bool joined = created.ok() && leftSubscribed.wait(Wait) &&
                        leftSubscribed.last().at(2).toString().isEmpty();
    auto object = a;
    object.uid += disconnect ? QStringLiteral("-OFFLINE") : QStringLiteral("-MISSION");
    object.callsign = QStringLiteral("SWAK TEST MISSION");
    object.time = object.start = QDateTime::currentDateTimeUtc();
    object.stale = object.time.addSecs(600);
    const auto added =
        joined ? leftMissions.addEvent(QStringLiteral("test"), name, object)
               : QStringLiteral("server A did not create the mission: ") + replyText(created);
    // Server A also adds a small and a large file. The hub replays only files up to its size
    // limit of 4096 bytes. The live path and server A send both.
    QByteArray small(MissionSmallFile, Qt::Uninitialized),
        large(MissionLargeFile, Qt::Uninitialized);
    for (auto *file : {&small, &large}) {
        std::generate(file->begin(), file->end(), [] {
            return static_cast<char>(QRandomGenerator::global()->bounded(256));
        });
    }
    const auto hash = [](const QByteArray &bytes) {
        return QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    };
    QString filesError;
    if (!filesCheck.isEmpty() && added.isEmpty()) {
        for (const auto *file : {&small, &large}) {
            const auto uploaded =
                call(leftApi,
                     "POST",
                     QStringLiteral("/sync/upload?name=%1-%2.bin&creatorUid=%3")
                         .arg(name,
                              file == &small ? QStringLiteral("small") : QStringLiteral("large"),
                              a.uid),
                     *file,
                     "application/octet-stream");
            if (!uploaded.ok()) {
                filesError = QStringLiteral("upload: ") + replyText(uploaded);
            }
        }
        const auto body = QJsonDocument(QJsonObject{{QStringLiteral("hashes"),
                                                     QJsonArray{hash(small), hash(large)}}})
                              .toJson(QJsonDocument::Compact);
        const auto contents =
            call(leftApi,
                 "PUT",
                 QStringLiteral("/api/missions/%1/contents?creatorUid=%2").arg(name, a.uid),
                 body,
                 "application/json");
        if (filesError.isEmpty() && !contents.ok()) {
            filesError = QStringLiteral("add files: ") + replyText(contents);
        }
    }
    // Give server A or the hub time to keep the change, then restore the federation.
    QString restored;
    if (reconnect) {
        QTest::qWait(5000);
        restored = reconnect();
    }
    // Server B creates the mission from the federated changes. Subscribe when it exists.
    Missions remoteMissions(remoteStore, &remote, &remoteApi, &remoteIdentity);
    QSignalSpy remoteSubscribed(&remoteMissions, &Missions::subscribed);
    QString status = restored.isEmpty() ? added : QStringLiteral("reconnect: ") + restored;
    QElapsedTimer elapsed;
    elapsed.start();
    const bool federated = added.isEmpty() && restored.isEmpty() &&
                           QTest::qWaitFor(
                               [&] {
                                   const auto *mission =
                                       remoteMissions.subscription(QStringLiteral("enroll"), name);
                                   if (!mission) {
                                       remoteSubscribed.clear();
                                       remoteMissions.subscribe(QStringLiteral("enroll"), name);
                                       static_cast<void>(remoteSubscribed.wait(5000));
                                       if (!remoteSubscribed.isEmpty()) {
                                           status = remoteSubscribed.last().at(2).toString();
                                       }
                                       return false;
                                   }
                                   remoteMissions.reload(QStringLiteral("enroll"), name);
                                   QTest::qWait(1500);
                                   status = mission->status;
                                   return mission->objects > 0 && has(remoteStore, object.uid);
                               },
                               MissionCacheWait);
    // The files: wait for the small one, and for the large one when it must arrive. When it must
    // not arrive, give it the same time again. Then compare the bytes from server B.
    if (!filesCheck.isEmpty()) {
        const auto listed = [&](const QByteArray &bytes) {
            const auto *subscription = remoteMissions.subscription(QStringLiteral("enroll"), name);
            return subscription && std::any_of(subscription->files.cbegin(),
                                               subscription->files.cend(),
                                               [&](const MissionFile &file) {
                                                   return file.hash == hash(bytes);
                                               });
        };
        const auto refresh = [&] {
            remoteMissions.reload(QStringLiteral("enroll"), name);
            QTest::qWait(1500);
        };
        QElapsedTimer filesTime;
        filesTime.start();
        const bool smallListed = federated && filesError.isEmpty() &&
                                 QTest::qWaitFor(
                                     [&] {
                                         refresh();
                                         return listed(small) && (!expectLarge || listed(large));
                                     },
                                     MissionCacheWait);
        const auto waited = filesTime.elapsed();
        if (smallListed && !expectLarge) {
            static_cast<void>(QTest::qWaitFor(
                [&] {
                    refresh();
                    return listed(large);
                },
                static_cast<int>(std::max<qint64>(10000, waited))));
        }
        const auto content = [&](const QByteArray &bytes) {
            return call(remoteApi,
                        "GET",
                        QStringLiteral("/sync/content?hash=") + hash(bytes),
                        {},
                        {},
                        QStringLiteral("enroll"))
                .body;
        };
        const bool smallSame = smallListed && content(small) == small;
        const bool largeListed = listed(large);
        const bool largeSame = largeListed && content(large) == large;
        const bool passed = smallSame && (expectLarge ? largeSame : !largeListed);
        record(filesCheck,
               passed,
               filesError.isEmpty()
                   ? QStringLiteral("small file %1, large file %2, expected large file %3, %4 s")
                         .arg(smallSame ? QStringLiteral("same bytes")
                                        : (smallListed ? QStringLiteral("other bytes")
                                                       : QStringLiteral("missing")),
                              largeSame ? QStringLiteral("same bytes")
                                        : (largeListed ? QStringLiteral("other bytes")
                                                       : QStringLiteral("missing")),
                              expectLarge ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(waited / 1000)
                   : filesError);
    }
    const auto *mission = remoteMissions.subscription(QStringLiteral("enroll"), name);
    // On a failure, also show what server B answers now, without the SWAK mission code.
    QString direct;
    if (!federated) {
        const auto reply = call(remoteApi,
                                "GET",
                                QStringLiteral("/api/missions/%1/cot").arg(name),
                                {},
                                {},
                                QStringLiteral("enroll"));
        direct = QStringLiteral("; server B cot: %1, %2 events")
                     .arg(replyText(reply))
                     .arg(readMissionEvents(reply.body).size());
    }
    if (expectMissing) {
        // A known fault: after a replay, the hub closes the mission stream of server B, and
        // TAK Server does not open it again. This check fails when the fault is gone.
        record(check,
               !federated,
               federated ? QStringLiteral("server B received the live change, so the hub fault is "
                                          "gone; update the probe")
                         : QStringLiteral("server B did not receive the live change after the "
                                          "replay closed its mission stream"));
        leftMissions.unsubscribe(QStringLiteral("test"), name);
        remoteMissions.unsubscribe(QStringLiteral("enroll"), name);
        return;
    }
    record(check,
           federated,
           federated ? QStringLiteral("server B has %1 with the object after %2 s")
                           .arg(name)
                           .arg(elapsed.elapsed() / 1000)
                     : QStringLiteral("%1; subscribed %2, objects %3, in store %4")
                               .arg(status, mission ? QStringLiteral("yes") : QStringLiteral("no"))
                               .arg(mission ? mission->objects : 0)
                               .arg(has(remoteStore, object.uid) ? QStringLiteral("yes")
                                                                 : QStringLiteral("no")) +
                           direct);
    leftMissions.unsubscribe(QStringLiteral("test"), name);
    remoteMissions.unsubscribe(QStringLiteral("enroll"), name);
    call(leftApi, "DELETE", QStringLiteral("/api/missions/%1?creatorUid=%2").arg(name, a.uid));
}
// A disabled Servers object with the administrator certificate for the REST API of a server.
// The target gives host, port, and API port.
std::unique_ptr<Servers>
adminServers(Store &store, const QJsonObject &admin, const QJsonObject &target)
{
    auto servers = std::make_unique<Servers>(store);
    auto entry = serverDefaults();
    for (auto it = admin.begin(); it != admin.end(); ++it) {
        entry.insert(it.key(), it.value());
    }
    for (const auto *key : {"host", "port", "apiPort"}) {
        if (target.contains(QLatin1String(key))) {
            entry.insert(QLatin1String(key), target.value(QLatin1String(key)));
        }
    }
    entry.insert(QStringLiteral("name"), QStringLiteral("Admin"));
    entry.insert(QStringLiteral("enabled"), false);
    servers->put(QStringLiteral("test"), entry);
    return servers;
}
// Reads the outgoing federation connections of a server as the administrator.
QJsonArray outgoingConnections(Servers &servers, NetworkClient &network)
{
    TakServerApi api(servers, network);
    const auto reply = call(api, "GET", QStringLiteral("/api/outgoingconnections"));
    return reply.ok() ? QJsonDocument::fromJson(reply.body)
                            .object()
                            .value(QStringLiteral("data"))
                            .toArray()
                      : QJsonArray{};
}
// Through the hub, server B connects with an automatic token to the token port. The deployment
// script checks the refusals of the token port with its own client.
void hubTokenChecks(const QJsonObject &profile, NetworkClient &network, const Record &record)
{
    const auto federation = profile.value(QStringLiteral("federation")).toObject();
    const auto token = federation.value(QStringLiteral("token")).toObject();
    if (token.isEmpty()) {
        return;
    }
    const auto admin = profile.value(QStringLiteral("admin")).toObject();
    const auto connection = [](const QJsonArray &list, const QString &name) {
        const auto found = std::find_if(list.begin(), list.end(), [&name](const QJsonValue &value) {
            return value.toObject().value(QStringLiteral("displayName")).toString() == name;
        });
        return found == list.end() ? QJsonObject{} : (*found).toObject();
    };
    const auto state = [](const QJsonObject &item) {
        return item.value(QStringLiteral("connectionInfoSummary"))
            .toObject()
            .value(QStringLiteral("connectionStatusValue"))
            .toString();
    };
    Store storeB;
    const auto serversB = adminServers(storeB, admin, federation);
    const auto good = connection(outgoingConnections(*serversB, network),
                                 token.value(QStringLiteral("connection")).toString());
    const bool tokenUsed =
        good.value(QStringLiteral("useToken")).toBool() &&
        good.value(QStringLiteral("port")).toInt() == token.value(QStringLiteral("port")).toInt() &&
        state(good) == u"CONNECTED";
    record(QStringLiteral("hubTokenFederation"),
           tokenUsed,
           QStringLiteral("server B port %1, token %2, %3")
               .arg(good.value(QStringLiteral("port")).toInt())
               .arg(good.value(QStringLiteral("useToken")).toBool() ? QStringLiteral("yes")
                                                                    : QStringLiteral("no"),
                    state(good)));
}
// Disconnects the federation of server B, changes a mission on server A, and reconnects. In
// direct federation, server A sends the missed change when server B connects again. Through
// the hub, server A stays connected, so the hub must replay the change from MongoDB.
void missionDisruptionCheck(Servers &left,
                            Servers &remote,
                            Store &leftStore,
                            Store &remoteStore,
                            TakServerApi &remoteApi,
                            NetworkClient &network,
                            const Event &a,
                            const QJsonObject &profile,
                            const Record &record)
{
    const auto federation = profile.value(QStringLiteral("federation")).toObject();
    const auto outgoing = federation.value(QStringLiteral("outgoing")).toString();
    // Without replay, the hub does not keep the change, and server A stays connected to it.
    if (outgoing.isEmpty() || (federation.value(QStringLiteral("hub")).toBool() &&
                               !federation.value(QStringLiteral("hubDisruption")).toBool())) {
        return;
    }
    Store adminStore;
    const auto admin =
        adminServers(adminStore, profile.value(QStringLiteral("admin")).toObject(), federation);
    TakServerApi api(*admin, network);
    const auto path = QStringLiteral("/api/outgoingconnectionstatus/") +
                      QString::fromLatin1(QUrl::toPercentEncoding(outgoing));
    const auto state = [&] {
        for (const auto &value : outgoingConnections(*admin, network)) {
            const auto item = value.toObject();
            if (item.value(QStringLiteral("displayName")).toString() == outgoing) {
                return item.value(QStringLiteral("connectionInfoSummary"))
                    .toObject()
                    .value(QStringLiteral("connectionStatusValue"))
                    .toString();
            }
        }
        return QString{};
    };
    // Sets the connection and waits for the expected state.
    const auto set = [&](bool enabled, const QString &expected) {
        const auto reply = call(api,
                                "POST",
                                path + (enabled ? QStringLiteral("?newStatus=true")
                                                : QStringLiteral("?newStatus=false")));
        if (!reply.ok()) {
            return replyText(reply);
        }
        QString current;
        const bool reached = QTest::qWaitFor(
            [&] {
                current = state();
                return current == expected;
            },
            3 * Wait);
        return reached ? QString{} : QStringLiteral("state %1, not %2").arg(current, expected);
    };
    missionFederationCheck(
        left,
        remote,
        leftStore,
        remoteStore,
        remoteApi,
        network,
        a,
        record,
        QStringLiteral("missionDisruption"),
        [&] {
            return set(false, QStringLiteral("DISABLED"));
        },
        [&] {
            return set(true, QStringLiteral("CONNECTED"));
        },
        false,
        QStringLiteral("missionDisruptionFiles"),
        // The hub replays only files up to its size limit. Server A sends every file.
        !federation.value(QStringLiteral("hub")).toBool());
    // Leave the federation connected for later runs, also after a failure.
    if (state() != u"CONNECTED") {
        static_cast<void>(set(true, QStringLiteral("CONNECTED")));
    }
}
// The receiver plugin writes the UIDs that it receives to a file. Read a bounded part.
QStringList receivedUids(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.read(1024 * 1024)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}
void hubPluginChecks(Servers &left,
                     Servers &remoteServers,
                     const Store &leftStore,
                     const Store &remote,
                     const Event &a,
                     const QJsonObject &federation,
                     const Record &record)
{
    auto marked = a;
    marked.uid += QStringLiteral("-PLUGIN");
    marked.callsign = QStringLiteral("SWAK TEST PLUGIN");
    const auto expected = QStringLiteral("SWAK HUB PLUGIN");
    const bool renamed = sendUntil(
        left,
        marked,
        [&] {
            const auto *event = find(remote, marked.uid);
            return event && event->callsign == expected;
        },
        3 * Wait);
    const auto *received = find(remote, marked.uid);
    record(QStringLiteral("hubPluginIntercept"),
           renamed,
           received ? QStringLiteral("callsign: ") + received->callsign
                    : QStringLiteral("no event"));
    const auto uid = QStringLiteral("SWAK-HUB-PLUGIN-SENDER");
    const bool sent = QTest::qWaitFor(
        [&] {
            return has(remote, uid) && has(leftStore, uid);
        },
        3 * Wait);
    record(QStringLiteral("hubPluginSender"),
           sent,
           QStringLiteral("server A %1, server B %2")
               .arg(has(leftStore, uid) ? QStringLiteral("received") : QStringLiteral("missing"),
                    has(remote, uid) ? QStringLiteral("received") : QStringLiteral("missing")));
    // The receiver policy takes messages only from the CA group of server A. An event from
    // server B passes the hub but does not reach the receiver.
    const auto log = federation.value(QStringLiteral("receiverLog")).toString();
    if (log.isEmpty()) {
        return;
    }
    auto fromA = a;
    fromA.uid += QStringLiteral("-RECEIVER-A");
    auto fromB = a;
    fromB.uid += QStringLiteral("-RECEIVER-B");
    fromB.callsign = QStringLiteral("SWAK TEST RECEIVER B");
    const bool receivedA = sendUntil(
        left,
        fromA,
        [&] {
            return has(remote, fromA.uid) && receivedUids(log).contains(fromA.uid);
        },
        3 * Wait);
    const bool deliveredB = sendUntil(
        remoteServers,
        fromB,
        [&] {
            return has(leftStore, fromB.uid);
        },
        3 * Wait);
    // Give the receiver the same time that the event from server A needed.
    QTest::qWait(3000);
    const bool receivedB = receivedUids(log).contains(fromB.uid);
    record(QStringLiteral("hubPluginReceiver"),
           receivedA && deliveredB && !receivedB,
           QStringLiteral("server A event %1; server B event %2 and %3")
               .arg(receivedA ? QStringLiteral("received") : QStringLiteral("not received"),
                    deliveredB ? QStringLiteral("delivered") : QStringLiteral("not delivered"),
                    receivedB ? QStringLiteral("received") : QStringLiteral("not received")));
}
} // namespace

EnrolledClient enrollClient(Servers &servers,
                            NetworkClient &network,
                            const QJsonObject &server,
                            const QJsonObject &enrollment,
                            const QString &clientUid,
                            const QString &directory)
{
    EnrolledClient result;
    auto entry = serverDefaults();
    entry.insert(QStringLiteral("name"), QStringLiteral("Enrolled"));
    entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
    for (const auto *key : {"host", "port", "ca", "apiPort"}) {
        if (server.contains(QLatin1String(key))) {
            entry.insert(QLatin1String(key), server.value(QLatin1String(key)));
        }
    }
    entry.insert(QStringLiteral("enabled"), false);
    result.error = servers.put(QStringLiteral("enroll"), entry);
    if (!result.error.isEmpty()) {
        return result;
    }
    TakEnrollment enroll(servers, network);
    enroll.setDirectory(directory);
    TakEnrollment::Options options;
    options.serverId = QStringLiteral("enroll");
    options.username = enrollment.value(QStringLiteral("username")).toString();
    options.password =
        qgetenv(enrollment.value(QStringLiteral("passwordEnv")).toString().toLatin1().constData());
    options.port = enrollment.value(QStringLiteral("port")).toInt();
    options.clientUid = clientUid;
    QSignalSpy finished(&enroll, &TakEnrollment::finished);
    result.error = enroll.start(options);
    if (result.error.isEmpty()) {
        result.error = finished.wait(Wait) ? finished.last().at(0).toString()
                                           : QStringLiteral("no enrollment reply");
    }
    result.signedIn = result.error.isEmpty();
    if (!result.signedIn) {
        return result;
    }
    auto enrolled = servers.sources().value(QStringLiteral("enroll")).toObject();
    enrolled.insert(QStringLiteral("enabled"), true);
    servers.put(QStringLiteral("enroll"), enrolled);
    result.connected = QTest::qWaitFor(
        [&] {
            return connected(servers, QStringLiteral("enroll"));
        },
        Wait);
    result.status = servers.status(QStringLiteral("enroll"));
    return result;
}

void runNetworkChecks(Servers &left,
                      Servers &right,
                      Store &leftStore,
                      Store &rightStore,
                      const Event &a,
                      const QJsonObject &profile,
                      QJsonObject &checks)
{
    const auto record = [&checks](const QString &name, bool passed, const QString &detail) {
        checks.insert(
            name,
            QJsonObject{{QStringLiteral("passed"), passed}, {QStringLiteral("detail"), detail}});
    };
    // Multicast: SWAK sends to the TAK multicast input and receives from a static subscription.
    const auto multicast = profile.value(QStringLiteral("multicast")).toObject();
    if (!multicast.isEmpty()) {
        Store sendStore, receiveStore;
        Servers sender(sendStore), receiver(receiveStore);
        auto error = sender.put(QStringLiteral("test"),
                                meshEntry(multicast,
                                          multicast.value(QStringLiteral("send")).toObject(),
                                          QStringLiteral("Multicast send")));
        error += receiver.put(QStringLiteral("test"),
                              meshEntry(multicast,
                                        multicast.value(QStringLiteral("receive")).toObject(),
                                        QStringLiteral("Multicast receive")));
        const bool joined =
            error.isEmpty() && QTest::qWaitFor(
                                   [&] {
                                       return connected(sender, QStringLiteral("test")) &&
                                              connected(receiver, QStringLiteral("test"));
                                   },
                                   Wait);
        record(QStringLiteral("multicastJoin"),
               joined,
               error.isEmpty() ? sender.status(QStringLiteral("test")) : error);
        auto mesh = a;
        mesh.uid += QStringLiteral("-MCAST");
        mesh.callsign = QStringLiteral("SWAK TEST MCAST");
        const bool sent = joined && sendUntil(
                                        sender,
                                        mesh,
                                        [&] {
                                            return has(rightStore, mesh.uid);
                                        },
                                        Wait);
        record(QStringLiteral("multicastSend"), sent, counters(sender));
        const bool received = joined && sendUntil(
                                            left,
                                            a,
                                            [&] {
                                                return has(receiveStore, a.uid);
                                            },
                                            Wait);
        record(QStringLiteral("multicastReceive"), received, counters(receiver));
    }
    // LDAP and federation: a directory user enrolls on the second server, which federates with
    // the first server.
    const auto federation = profile.value(QStringLiteral("federation")).toObject();
    if (!federation.isEmpty()) {
        NetworkClient network;
        QTemporaryDir folder;
        Store store;
        Servers remote(store);
        const auto enrolled =
            enrollClient(remote,
                         network,
                         federation,
                         federation.value(QStringLiteral("enrollment")).toObject(),
                         a.uid + QStringLiteral("-LDAP"),
                         folder.path());
        record(QStringLiteral("ldapEnrollment"), enrolled.signedIn, enrolled.error);
        record(QStringLiteral("ldapConnection"), enrolled.connected, enrolled.status);
        if (enrolled.connected) {
            TakServerApi api(remote, network);
            TakChannels channels(api);
            QSignalSpy loaded(&channels, &TakChannels::loaded);
            channels.load(QStringLiteral("enroll"));
            QStringList names;
            if (loaded.wait(Wait) && loaded.last().at(2).toString().isEmpty()) {
                names = channelNames(loaded.last().at(1).value<QList<TakGroup>>());
            }
            const auto group = federation.value(QStringLiteral("group")).toString();
            record(QStringLiteral("ldapGroups"),
                   names.contains(group),
                   names.isEmpty() && !loaded.isEmpty() ? loaded.last().at(2).toString()
                                                        : names.join(QStringLiteral(", ")));
            auto far = a;
            far.uid += QStringLiteral("-FED");
            far.callsign = QStringLiteral("SWAK TEST FED");
            const bool inbound = sendUntil(
                remote,
                far,
                [&] {
                    return has(leftStore, far.uid);
                },
                3 * Wait);
            record(QStringLiteral("federationInbound"),
                   inbound,
                   inbound ? QString{} : QStringLiteral("server A did not deliver the event"));
            const bool outbound = sendUntil(
                left,
                a,
                [&] {
                    return has(store, a.uid);
                },
                3 * Wait);
            record(QStringLiteral("federationOutbound"),
                   outbound,
                   outbound ? QString{} : QStringLiteral("server B did not deliver the event"));
            groupFilterCheck(right, leftStore, store, a, profile, record);
            // With hub replay, the live change is expected to be lost. See the check text.
            const bool replay = federation.value(QStringLiteral("hub")).toBool() &&
                                federation.value(QStringLiteral("hubDisruption")).toBool();
            missionFederationCheck(left,
                                   remote,
                                   leftStore,
                                   store,
                                   api,
                                   network,
                                   a,
                                   record,
                                   replay ? QStringLiteral("hubReplayStopsLiveMissions")
                                          : QStringLiteral("missionFederation"),
                                   {},
                                   {},
                                   replay,
                                   replay ? QString{} : QStringLiteral("missionFiles"));
            hubTokenChecks(profile, network, record);
            if (federation.value(QStringLiteral("hub")).toBool()) {
                hubPluginChecks(left, remote, leftStore, store, a, federation, record);
            }
            // This check cuts the federation for a time, so it runs last.
            missionDisruptionCheck(
                left, remote, leftStore, store, api, network, a, profile, record);
        }
    }
}

QJsonObject runLoad(Servers &left, const Servers &right, Store &rightStore, const QJsonObject &load)
{
    const int seconds = std::clamp(load.value(QStringLiteral("seconds")).toInt(60), 5, 86400);
    const int rate = std::clamp(load.value(QStringLiteral("rate")).toInt(5), 1, 200);
    const int objects = std::clamp(load.value(QStringLiteral("objects")).toInt(10), 1, 1000);
    // Each position carries its sequence number in the altitude, so the receiver can count
    // lost and repeated positions.
    const auto prefix = QStringLiteral("SWAK-LOAD-") +
                        QUuid::createUuid().toString(QUuid::Id128).left(12) + QLatin1Char('-');
    LoadMeter meter(static_cast<qint64>(seconds) * rate);
    const auto total = meter.positions();
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&rightStore, &Store::eventAccepted, &rightStore, [&](const Event &event) {
        if (event.uid.startsWith(prefix)) {
            meter.received(static_cast<qint64>(event.altitude), clock.elapsed());
        }
    });
    const auto startMemory = residentKiB();
    qint64 peakMemory = startMemory;
    const qint64 interval = 1000 / rate;
    for (qint64 sequence = 0; sequence < total; ++sequence) {
        Event event;
        event.uid = prefix + QString::number(sequence % objects);
        event.callsign = QStringLiteral("SWAK LOAD %1").arg(sequence % objects);
        event.type = QStringLiteral("a-f-G-U-C");
        event.how = QStringLiteral("m-g");
        event.latitude = 0.5 + static_cast<double>(sequence % 1000) * 1e-5;
        event.longitude = 0.5 + static_cast<double>(sequence % objects) * 1e-3;
        event.altitude = static_cast<double>(sequence);
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(120);
        const auto sentTime = clock.elapsed();
        if (left.send(event).isEmpty()) {
            meter.sent(sequence, sentTime);
        }
        const auto due = (sequence + 1) * interval;
        QTest::qWait(static_cast<int>(std::max<qint64>(1, due - clock.elapsed())));
        meter.connection(connected(left, QStringLiteral("test")) &&
                         connected(right, QStringLiteral("test")));
        if (sequence % rate == 0) {
            peakMemory = std::max(peakMemory, residentKiB());
            // Old positions expire, so the store does not grow during long runs.
            rightStore.expire();
        }
    }
    // Wait for the last positions.
    static_cast<void>(QTest::qWaitFor(
        [&] {
            return meter.deliveredCount() >= meter.sentCount();
        },
        10000));
    const auto endMemory = residentKiB();
    const auto dropped = left.statistics().value(QStringLiteral("dropped")).toDouble() +
                         right.statistics().value(QStringLiteral("dropped")).toDouble();
    auto result = meter.result(load.value(QStringLiteral("minimumRatio")).toDouble(0.99));
    for (const auto &[key, value] :
         {std::pair{QStringLiteral("seconds"), QJsonValue(seconds)},
          std::pair{QStringLiteral("rate"), QJsonValue(rate)},
          std::pair{QStringLiteral("objects"), QJsonValue(objects)},
          std::pair{QStringLiteral("queueDrops"), QJsonValue(dropped)},
          std::pair{QStringLiteral("residentKiBStart"),
                    QJsonValue(static_cast<double>(startMemory))},
          std::pair{QStringLiteral("residentKiBPeak"), QJsonValue(static_cast<double>(peakMemory))},
          std::pair{QStringLiteral("residentKiBEnd"),
                    QJsonValue(static_cast<double>(endMemory))}}) {
        result.insert(key, value);
    }
    return result;
}
