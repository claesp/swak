#include "tak_load_meter.h"

#include <QTest>

class TakLoadTest final : public QObject
{
    Q_OBJECT
private slots:
    void counts()
    {
        LoadMeter meter(5);
        QCOMPARE(meter.positions(), 5);
        meter.sent(0, 100);
        meter.sent(1, 110);
        meter.sent(2, 120);
        meter.connection(true);
        meter.received(0, 130);
        meter.received(1, 150);
        auto result = meter.result(0.5);
        QCOMPARE(result.value(QStringLiteral("sent")).toInt(), 3);
        QCOMPARE(result.value(QStringLiteral("delivered")).toInt(), 2);
        QCOMPARE(result.value(QStringLiteral("latencyMsP50")).toInt(), 30);
        QCOMPARE(result.value(QStringLiteral("latencyMsMax")).toInt(), 40);
        QVERIFY(result.value(QStringLiteral("passed")).toBool());
        // A lost position fails a run that needs every position.
        QVERIFY(!meter.result(1.0).value(QStringLiteral("passed")).toBool());
    }
    void repeats()
    {
        LoadMeter meter(2);
        meter.sent(0, 0);
        meter.received(0, 5);
        meter.received(0, 6);
        const auto result = meter.result(0.0);
        QCOMPARE(result.value(QStringLiteral("duplicates")).toInt(), 1);
        QCOMPARE(result.value(QStringLiteral("delivered")).toInt(), 1);
        QVERIFY(!result.value(QStringLiteral("passed")).toBool());
    }
    void disconnects()
    {
        LoadMeter meter(4);
        meter.sent(0, 0);
        meter.received(0, 1);
        // Two moves to disconnected. Staying disconnected does not count again.
        meter.connection(false);
        meter.connection(false);
        meter.connection(true);
        meter.connection(false);
        const auto result = meter.result(0.0);
        QCOMPARE(result.value(QStringLiteral("disconnects")).toInt(), 2);
        QVERIFY(!result.value(QStringLiteral("passed")).toBool());
    }
    void ignoresInvalidInput()
    {
        LoadMeter meter(2);
        // Unsent, negative, and out-of-range numbers do not count.
        meter.received(0, 1);
        meter.received(-1, 1);
        meter.received(2, 1);
        meter.sent(-1, 0);
        meter.sent(2, 0);
        meter.sent(1, -5);
        // A second send of the same number keeps the first time.
        meter.sent(1, 10);
        meter.sent(1, 50);
        // A clock step back gives zero latency, not a negative one.
        meter.received(1, 5);
        const auto result = meter.result(1.0);
        QCOMPARE(result.value(QStringLiteral("sent")).toInt(), 1);
        QCOMPARE(result.value(QStringLiteral("delivered")).toInt(), 1);
        QCOMPARE(result.value(QStringLiteral("duplicates")).toInt(), 0);
        QCOMPARE(result.value(QStringLiteral("latencyMsMax")).toInt(), 0);
        QVERIFY(result.value(QStringLiteral("passed")).toBool());
    }
    void emptyRun()
    {
        // Nothing sent never passes, and empty latencies report -1.
        const auto result = LoadMeter(0).result(0.0);
        QVERIFY(!result.value(QStringLiteral("passed")).toBool());
        QCOMPARE(result.value(QStringLiteral("ratio")).toDouble(), 0.0);
        QCOMPARE(result.value(QStringLiteral("latencyMsP95")).toInt(), -1);
        // The meter limits its memory.
        QCOMPARE(LoadMeter(LoadMeter::MaxPositions + 1).positions(), LoadMeter::MaxPositions);
        QCOMPARE(LoadMeter(-3).positions(), 0);
    }
};

QTEST_GUILESS_MAIN(TakLoadTest)
#include "test_tak_load.moc"
