#include "core/network_client.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_channels.h"
#include "core/tak_enrollment.h"
#include "core/tak_server_api.h"
#include "tak_compat_features.h"
#include "tak_load_meter.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <functional>

using namespace swak;
namespace {
constexpr int Wait = 20000;
// UDP and federation can lose the first events, so the checks send again at this interval.
constexpr int ResendMs = 2000;

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
void groupFilterCheck(Servers &right,
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
    auto entry = serverDefaults();
    for (auto it = opsProfile.begin(); it != opsProfile.end(); ++it) {
        entry.insert(it.key(), it.value());
    }
    entry.insert(QStringLiteral("name"), QStringLiteral("Ops"));
    entry.insert(QStringLiteral("enabled"), true);
    const auto error = ops.put(QStringLiteral("test"), entry);
    const bool ready = error.isEmpty() && QTest::qWaitFor(
                                              [&] {
                                                  return connected(ops, QStringLiteral("test"));
                                              },
                                              Wait);
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
               (ready ? QString{}
                      : QStringLiteral("; ") + error + ops.status(QStringLiteral("test"))));
}
// The SWAK test plugins of the hub rename events with the suffix -PLUGIN and send their own
// event to every federate.
void hubPluginChecks(Servers &left,
                     const Store &leftStore,
                     const Store &remote,
                     const Event &a,
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
            groupFilterCheck(right, store, a, profile, record);
            if (federation.value(QStringLiteral("hub")).toBool()) {
                hubPluginChecks(left, leftStore, store, a, record);
            }
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
