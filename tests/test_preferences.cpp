#include "core/preferences.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <unistd.h>

using namespace swak;
class PreferencesTest final : public QObject {
    Q_OBJECT
private slots:
    void roundTrip()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("nested/preferences.json"));
        const QJsonObject document{{QStringLiteral("version"), 1},
            {QStringLiteral("futurePlugin"), QJsonObject{{QStringLiteral("custom"), QStringLiteral("Ångström")}}}};
        {
            Preferences preferences(path);
            QVERIFY(preferences.error().isEmpty());
            QVERIFY(preferences.save(document));
            QFile file(path);
            QVERIFY(file.permissions().testFlag(QFileDevice::ReadOwner));
            QVERIFY(!file.permissions().testFlag(QFileDevice::ReadOther));
        }
        Preferences restored(path);
        QCOMPARE(restored.document(), document);
        QVERIFY(restored.save(document));
    }
    void malformed_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("broken") << QByteArray("{not JSON");
        QTest::newRow("future-version") << QByteArray("{\"version\":2}");
        QTest::newRow("fractional-version") << QByteArray("{\"version\":1.5}");
        QTest::newRow("missing-version") << QByteArray("{}");
        QTest::newRow("large") << QByteArray(1024 * 1024 + 1, 'x');
    }
    void malformed()
    {
        QFETCH(QByteArray, bytes);
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("preferences.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        Preferences preferences(path);
        QVERIFY(!preferences.error().isEmpty());
        QVERIFY(!preferences.save({{QStringLiteral("version"), 1}}));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes);
    }
    void lockedFile()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("preferences.json"));
        Preferences first(path);
        const QJsonObject document{{QStringLiteral("version"), 1}, {QStringLiteral("value"), 42}};
        QVERIFY(first.save(document));
        Preferences second(path);
        QVERIFY(!second.error().isEmpty());
        QVERIFY(!second.save({{QStringLiteral("value"), 999}}));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(), document);
    }
    void readOnlyProfileAndCopiedLock()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("preferences.json"));
        const QJsonObject document{{QStringLiteral("version"), 1}, {QStringLiteral("value"), 42}};
        { Preferences profile(path); QVERIFY(profile.save(document)); }
        if (::geteuid() != 0) {
            const auto permissions = QFile::permissions(directory.path());
            const auto restore = qScopeGuard([&] { QFile::setPermissions(directory.path(), permissions); });
            QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
            Preferences profile(path);
            QVERIFY(!profile.writable());
            QCOMPARE(profile.document(), document);
            QVERIFY(profile.error().contains(QStringLiteral("lock cannot be created")));
        }
        QFile lock(path + QStringLiteral(".lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.write("999999\nswak\nother-host\n");
        QVERIFY(lock.flush());
        QVERIFY(lock.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-60), QFileDevice::FileModificationTime));
        lock.close();
        Preferences recovered(path);
        QVERIFY2(recovered.writable(), qPrintable(recovered.error()));
        QCOMPARE(recovered.document(), document);
    }
    void writeFailure()
    {
        QTemporaryDir directory;
        Preferences preferences(directory.filePath(QStringLiteral("preferences.json")));
        QVERIFY(preferences.error().isEmpty());
        QVERIFY(QDir().mkdir(preferences.path()));
        QVERIFY(!preferences.save({{QStringLiteral("version"), 1}}));
        QVERIFY(!preferences.error().isEmpty());
    }
    void invalidValues()
    {
        const QJsonObject values{{QStringLiteral("number"), 1000000}, {QStringLiteral("fraction"), 1.25},
            {QStringLiteral("text"), QString(500, 'x')}, {QStringLiteral("wrongType"), QStringLiteral("42")}};
        QCOMPARE(preferenceNumber(values, QStringLiteral("number"), 1, 0, 16), 1.0);
        QCOMPARE(preferenceInteger(values, QStringLiteral("fraction"), 300, 1, 86400), 300);
        QCOMPARE(preferenceInteger(values, QStringLiteral("wrongType"), 6969, 1, 65535), 6969);
        QCOMPARE(preferenceText(values, QStringLiteral("text"), QStringLiteral("default"), 128), QStringLiteral("default"));
    }
    void screenRecovery()
    {
        const QList<QRect> screens{QRect(0, 0, 1920, 1080), QRect(-1280, 0, 1280, 1024)};
        QCOMPARE(fitWindowRectangle(QRect(-1000, 50, 700, 600), screens), QRect(-1000, 50, 700, 600));
        const auto recovered = fitWindowRectangle(QRect(6000, 3000, 4000, 2000), screens);
        QVERIFY(screens.first().contains(recovered));
        QCOMPARE(recovered.size(), screens.first().size());
        const auto moved = fitWindowRectangle(QRect(-1000, 50, 700, 600), {screens.first()});
        QVERIFY(screens.first().contains(moved));
        QCOMPARE(moved.size(), QSize(700, 600));
    }
};
QTEST_GUILESS_MAIN(PreferencesTest)
#include "test_preferences.moc"
