#include "core/activity.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTest>

#include <thread>
#include <vector>
using namespace swak;
class ActivityTest final : public QObject
{
    Q_OBJECT
private slots:
    void boundedHistory()
    {
        ActivityModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.clear();
        model.restorePreferences({{QStringLiteral("capacity"), 3}});
        QSignalSpy latest(&model, &ActivityModel::latest);
        configureLogging({});
        for (int i = 0; i < 10; ++i) {
            logMessage(
                Severity::Info, Urgency::Routine, QStringLiteral("swak.test"), QString::number(i));
        }
        QVERIFY(QTest::qWaitFor([&] {
            return model.rowCount() == 3;
        }));
        QCOMPARE(model.index(0, 4).data().toString(), QStringLiteral("7"));
        QCOMPARE(model.index(2, 4).data().toString(), QStringLiteral("9"));
        QCOMPARE(latest.last().first().toString(), QStringLiteral("9"));
        QVERIFY(!loggingConfiguration().value(QStringLiteral("enabled")).toBool());
        logMessage(
            Severity::Info, Urgency::Routine, QStringLiteral("swak.test"), QStringLiteral("10"));
        logMessage(
            Severity::Info, Urgency::Routine, QStringLiteral("swak.test"), QStringLiteral("11"));
        QVERIFY(QTest::qWaitFor([&] {
            return model.index(2, 4).data().toString() == QStringLiteral("11");
        }));
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.index(0, 4).data().toString(), QStringLiteral("9"));
        model.restorePreferences({{QStringLiteral("capacity"), 1}});
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.index(0, 4).data().toString(), QStringLiteral("11"));
        model.clear();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.data({}).isValid());
        QVERIFY(!model.headerData(99, Qt::Horizontal, Qt::DisplayRole).isValid());
    }
    void unicodeAndTypedSorting()
    {
        ActivityModel model;
        model.clear();
        const auto text = QString::fromUtf8("Map 🌍");
        publishActivity(Severity::Warning, Urgency::Immediate, {}, text);
        publishActivity(Severity::Error, Urgency::Routine, {}, QStringLiteral("error"));
        QVERIFY(QTest::qWaitFor([&] {
            return model.rowCount() == 2;
        }));
        QCOMPARE(model.index(0, 4).data().toString(), text);
        QCOMPARE(model.index(0, 0).data(ActivityModel::SortRole).metaType(),
                 QMetaType::fromType<QDateTime>());
        QSortFilterProxyModel proxy;
        proxy.setSourceModel(&model);
        proxy.setSortRole(ActivityModel::SortRole);
        proxy.sort(1, Qt::AscendingOrder);
        QCOMPARE(proxy.index(0, 4).data().toString(), text);
        proxy.sort(2, Qt::AscendingOrder);
        QCOMPARE(proxy.index(0, 4).data().toString(), QStringLiteral("error"));
    }
    void concurrentFlood()
    {
        ActivityModel model;
        model.clear();
        std::vector<std::thread> writers;
        for (int i = 0; i < 4; ++i) {
            writers.emplace_back([] {
                for (int j = 0; j < 1000; ++j) {
                    logMessage(Severity::Warning,
                               Urgency::Attention,
                               QStringLiteral("swak.test"),
                               QStringLiteral("event"));
                }
            });
        }
        for (auto &writer : writers) {
            writer.join();
        }
        logMessage(Severity::Error,
                   Urgency::Immediate,
                   QStringLiteral("swak.test"),
                   QString(10000, 'x') + '\n');
        QVERIFY(QTest::qWaitFor([&] {
            return model.rowCount() == 256;
        }));
        QCOMPARE(model.index(255, 4).data().toString().size(), 2048);
        QVERIFY(model.index(255, 4).data(Qt::ForegroundRole).isValid());
        QCOMPARE(model.index(255, 1).data().toString(), QStringLiteral("Error"));
        QCOMPARE(model.index(255, 2).data().toString(), QStringLiteral("Immediate"));
        model.clear();
        publishActivity(static_cast<Severity>(99), Urgency::Routine, {}, {});
        publishActivity(Severity::Info, static_cast<Urgency>(99), {}, {});
        publishActivity(
            Severity::Info, Urgency::Routine, QStringLiteral("test"), QStringLiteral("a\nb\033"));
        QVERIFY(QTest::qWaitFor([&] {
            return model.rowCount() == 1;
        }));
        QCOMPARE(model.index(0, 4).data().toString(), QStringLiteral("ab"));
    }
};
QTEST_GUILESS_MAIN(ActivityTest)
#include "test_activity.moc"
