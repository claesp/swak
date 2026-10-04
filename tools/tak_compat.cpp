#include "core/files.h"
#include "core/servers.h"
#include "core/store.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <cstdio>
using namespace swak;
namespace {
QJsonObject report;
int finish(const QString &stage, bool passed)
{
    report.insert(QStringLiteral("passed"), passed);
    report.insert(QStringLiteral("stage"), stage);
    const auto bytes = QJsonDocument(report).toJson();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    return passed ? 0 : 1;
}
bool connected(const Servers &servers)
{
    return servers.statistics()
        .value(QStringLiteral("test"))
        .toObject()
        .value(QStringLiteral("connected"))
        .toBool();
}
bool contains(const Store &store, const Event &expected)
{
    return std::any_of(
        store.events().cbegin(), store.events().cend(), [&expected](const Event &event) {
            return event.uid == expected.uid && event.type == expected.type &&
                   event.how == expected.how && event.callsign == expected.callsign &&
                   event.team == expected.team && event.role == expected.role &&
                   event.latitude == expected.latitude && event.longitude == expected.longitude &&
                   event.altitude == expected.altitude;
        });
}
QJsonObject configuration(const QJsonObject &input)
{
    auto source = serverDefaults();
    for (auto it = input.begin(); it != input.end(); ++it) {
        source.insert(it.key(), it.value());
    }
    source.insert(QStringLiteral("enabled"), true);
    source.insert(QStringLiteral("name"), QStringLiteral("Compatibility test"));
    return source;
}
} // namespace
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::fputs("Usage: swak_tak_compat profile.json\nUse a disposable test server. This probe "
                   "sends synthetic CoT.\n",
                   stderr);
        return 2;
    }
    const auto file = readRegularFile(app.arguments().at(1), 16384);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.bytes, &error);
    if (!file.error.isEmpty() || error.error != QJsonParseError::NoError || !document.isObject()) {
        return finish(QStringLiteral("Read profile"), false);
    }
    const auto profile = document.object();
    if (!profile.value(QStringLiteral("left")).isObject() ||
        !profile.value(QStringLiteral("right")).isObject()) {
        return finish(QStringLiteral("Read two client configurations"), false);
    }
    auto leftConfig = configuration(profile.value(QStringLiteral("left")).toObject());
    const auto rightConfig = configuration(profile.value(QStringLiteral("right")).toObject());
    for (const auto &source : {leftConfig, rightConfig}) {
        const auto protocol = source.value(QStringLiteral("protocol")).toString();
        if ((protocol != QStringLiteral("tls") && protocol != QStringLiteral("stcp")) ||
            !validateServer(source).isEmpty()) {
            return finish(QStringLiteral("Validate stream configuration"), false);
        }
    }
    Store leftStore, rightStore;
    Servers left(leftStore), right(rightStore);
    if (!left.put(QStringLiteral("test"), leftConfig).isEmpty() ||
        !right.put(QStringLiteral("test"), rightConfig).isEmpty()) {
        return finish(QStringLiteral("Configure clients"), false);
    }
    if (!QTest::qWaitFor(
            [&] {
                return connected(left) && connected(right);
            },
            30000)) {
        report.insert(QStringLiteral("leftStatus"), left.status(QStringLiteral("test")));
        report.insert(QStringLiteral("rightStatus"), right.status(QStringLiteral("test")));
        return finish(QStringLiteral("Connect both clients"), false);
    }
    Event a;
    a.uid = QStringLiteral("SWAK-COMPAT-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    a.callsign = QStringLiteral("SWAK TEST A");
    a.type = QStringLiteral("a-f-G-U-C-I");
    a.how = QStringLiteral("m-g");
    a.team = QStringLiteral("Cyan");
    a.role = QStringLiteral("Team Member");
    a.latitude = 0.125;
    a.longitude = 0.25;
    a.altitude = 10;
    Event b = a;
    b.uid += QStringLiteral("-B");
    b.callsign = QStringLiteral("SWAK TEST B");
    b.team = QStringLiteral("Blue");
    b.role = QStringLiteral("Medic");
    const auto exchange = [&] {
        a.time = a.start = b.time = b.start = QDateTime::currentDateTimeUtc();
        a.stale = b.stale = a.time.addSecs(180);
        if (!left.send(a).isEmpty() || !right.send(b).isEmpty()) {
            return false;
        }
        return QTest::qWaitFor(
            [&] {
                return contains(leftStore, b) && contains(rightStore, a);
            },
            15000);
    };
    if (!exchange()) {
        return finish(QStringLiteral("Exchange identity fields in both directions"), false);
    }
    report.insert(QStringLiteral("bidirectional"), true);
    a.longitude += 0.125;
    b.longitude += 0.25;
    if (!exchange()) {
        return finish(QStringLiteral("Update positions with the same UIDs"), false);
    }
    report.insert(QStringLiteral("positionUpdates"), true);
    leftConfig.insert(QStringLiteral("enabled"), false);
    if (!left.put(QStringLiteral("test"), leftConfig).isEmpty() || !QTest::qWaitFor(
                                                                       [&] {
                                                                           return !connected(left);
                                                                       },
                                                                       5000)) {
        return finish(QStringLiteral("Disconnect"), false);
    }
    QTest::qWait(1000);
    leftConfig.insert(QStringLiteral("enabled"), true);
    if (!left.put(QStringLiteral("test"), leftConfig).isEmpty() || !QTest::qWaitFor(
                                                                       [&] {
                                                                           return connected(left);
                                                                       },
                                                                       30000)) {
        return finish(QStringLiteral("Reconnect"), false);
    }
    a.longitude += 0.125;
    b.longitude += 0.25;
    if (!exchange()) {
        return finish(QStringLiteral("Exchange after reconnect"), false);
    }
    report.insert(QStringLiteral("reconnect"), true);
    return finish(QStringLiteral("Complete"), true);
}
