#include "core/network_client.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_channels.h"
#include "core/tak_enrollment.h"
#include "core/tak_server_api.h"
#include "tak_compat_features.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

using namespace swak;
namespace {
constexpr int Wait = 20000;
// UDP and federation can lose the first events, so the checks send again at this interval.
constexpr int ResendMs = 2000;
// The load test keeps one send time for each position, so it limits their number.
constexpr qint64 MaxLoadPositions = 2'000'000;

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
double percentile(std::vector<qint64> values, double fraction)
{
    if (values.empty()) {
        return -1;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(
        std::clamp(std::ceil(fraction * static_cast<double>(values.size())) - 1.0,
                   0.0,
                   static_cast<double>(values.size() - 1)));
    return static_cast<double>(values[index]);
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
        }
    }
    static_cast<void>(right);
}

QJsonObject runLoad(Servers &left, Servers &right, Store &rightStore, const QJsonObject &load)
{
    const int seconds = std::clamp(load.value(QStringLiteral("seconds")).toInt(60), 5, 86400);
    const int rate = std::clamp(load.value(QStringLiteral("rate")).toInt(5), 1, 200);
    const int objects = std::clamp(load.value(QStringLiteral("objects")).toInt(10), 1, 1000);
    // Each position carries its sequence number in the altitude, so the receiver can count
    // lost and repeated positions.
    const auto prefix = QStringLiteral("SWAK-LOAD-") +
                        QUuid::createUuid().toString(QUuid::Id128).left(12) + QLatin1Char('-');
    std::vector<qint64> latencies;
    qint64 duplicates = 0;
    // The send time of each position in milliseconds. NotSent and Received mark the other states.
    constexpr qint64 NotSent = -2;
    constexpr qint64 Received = -1;
    const qint64 total = std::min<qint64>(static_cast<qint64>(seconds) * rate, MaxLoadPositions);
    std::vector<qint64> sentAt(static_cast<size_t>(total), NotSent);
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&rightStore, &Store::eventAccepted, &rightStore, [&](const Event &event) {
        if (!event.uid.startsWith(prefix)) {
            return;
        }
        const auto sequence = static_cast<qint64>(event.altitude);
        if (sequence < 0 || sequence >= total || sentAt[static_cast<size_t>(sequence)] == NotSent) {
            return;
        }
        auto &sentTime = sentAt[static_cast<size_t>(sequence)];
        if (sentTime == Received) {
            ++duplicates;
            return;
        }
        latencies.push_back(clock.elapsed() - sentTime);
        sentTime = Received;
    });
    int disconnects = 0;
    bool wasConnected = true;
    const auto startMemory = residentKiB();
    qint64 peakMemory = startMemory;
    qint64 sent = 0;
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
        sentAt[static_cast<size_t>(sequence)] = clock.elapsed();
        if (left.send(event).isEmpty()) {
            ++sent;
        }
        const auto due = (sequence + 1) * interval;
        QTest::qWait(static_cast<int>(std::max<qint64>(1, due - clock.elapsed())));
        const bool now =
            connected(left, QStringLiteral("test")) && connected(right, QStringLiteral("test"));
        if (wasConnected && !now) {
            ++disconnects;
        }
        wasConnected = now;
        if (sequence % rate == 0) {
            peakMemory = std::max(peakMemory, residentKiB());
            // Old positions expire, so the store does not grow during long runs.
            rightStore.expire();
        }
    }
    // Wait for the last positions.
    static_cast<void>(QTest::qWaitFor(
        [&] {
            return static_cast<qint64>(latencies.size()) >= sent;
        },
        10000));
    const auto delivered = static_cast<qint64>(latencies.size());
    const double ratio = sent > 0 ? static_cast<double>(delivered) / static_cast<double>(sent) : 0;
    const auto endMemory = residentKiB();
    const auto dropped = left.statistics().value(QStringLiteral("dropped")).toDouble() +
                         right.statistics().value(QStringLiteral("dropped")).toDouble();
    const double minimum =
        std::clamp(load.value(QStringLiteral("minimumRatio")).toDouble(0.99), 0.0, 1.0);
    const bool passed = ratio >= minimum && disconnects == 0 && duplicates == 0;
    return QJsonObject{{QStringLiteral("passed"), passed},
                       {QStringLiteral("seconds"), seconds},
                       {QStringLiteral("rate"), rate},
                       {QStringLiteral("objects"), objects},
                       {QStringLiteral("sent"), static_cast<double>(sent)},
                       {QStringLiteral("delivered"), static_cast<double>(delivered)},
                       {QStringLiteral("ratio"), ratio},
                       {QStringLiteral("duplicates"), static_cast<double>(duplicates)},
                       {QStringLiteral("disconnects"), disconnects},
                       {QStringLiteral("queueDrops"), dropped},
                       {QStringLiteral("latencyMsP50"), percentile(latencies, 0.50)},
                       {QStringLiteral("latencyMsP95"), percentile(latencies, 0.95)},
                       {QStringLiteral("latencyMsMax"), percentile(latencies, 1.0)},
                       {QStringLiteral("residentKiBStart"), static_cast<double>(startMemory)},
                       {QStringLiteral("residentKiBPeak"), static_cast<double>(peakMemory)},
                       {QStringLiteral("residentKiBEnd"), static_cast<double>(endMemory)}};
}
