#include "core/activity.h"
#include "core/maps.h"
#include "core/tiles.h"
#include "app/window.h"
#include "app/map_manager.h"
#include "core/maps.h"
#include "tile_server.h"
#include <QStandardPaths>
#include <QDesktopServices>
#include "swak/version.h"
#include "core/logging.h"
#include "app/settings_dialog.h"
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QCollator>
#include <QScopeGuard>
#include "core/interfaces.h"
#include <QApplication>
#include <QWheelEvent>
#include <QAbstractItemModelTester>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLineEdit>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QStyle>
#include <QStyleFactory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QScreen>
#include <QSignalSpy>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSizeGrip>
#include <QTableView>
#include <QTest>
#include <QTemporaryDir>
#include <QPluginLoader>
#include <QPointer>
#include <QToolBar>
#include <QToolButton>
#include <QMessageBox>
#include <QHeaderView>
#include <QSpinBox>
#include <QJsonDocument>
#include <QFile>
#include <QUdpSocket>
#include <QLabel>

class BrowserCapture final : public QObject {
    Q_OBJECT
public:
    QUrl opened;
public slots:
    void open(const QUrl &url) { opened = url; }
};

class ExampleSettings final : public QObject, public swak::SettingsParticipant {
public:
    bool enabled = false;
    QString preferenceId() const override { return QStringLiteral("org.swak.example"); }
    QString settingsTitle() const override { return QStringLiteral("Example extension"); }
    QList<swak::SettingField> settingsFields() const override
    { return {swak::booleanSetting(QStringLiteral("enabled"), QStringLiteral("Enabled"))}; }
    QJsonObject savePreferences() const override { return {{QStringLiteral("enabled"), enabled}}; }
    void restorePreferences(const QJsonObject &values) override { enabled = values.value(QStringLiteral("enabled")).toBool(); }
};

class ExampleMenu final : public QObject, public swak::MenuPlugin {
    Q_OBJECT
    Q_INTERFACES(swak::MenuPlugin)
public:
    bool empty = false;
    int calls = 0;
    QString name() const override { return QStringLiteral("Example menu"); }
    void populateMenu(swak::PluginServices &, QMenu &menu) override
    {
        if (empty) return;
        auto *action = menu.addAction(QStringLiteral("Example action"));
        connect(action, &QAction::triggered, this, [this] { ++calls; });
    }
};

class ExampleToolbar final : public QObject, public swak::ToolbarPlugin {
    Q_OBJECT
    Q_INTERFACES(swak::ToolbarPlugin)
public:
    int calls = 0;
    QString name() const override { return QStringLiteral("Example toolbar"); }
    void populateToolbar(swak::PluginServices &, QToolBar &toolbar, QMenu *menu) override
    {
        Q_ASSERT(!menu);
        auto *action = toolbar.addAction(toolbar.style()->standardIcon(QStyle::SP_DialogApplyButton), QStringLiteral("Example"));
        connect(action, &QAction::triggered, this, [this] { ++calls; });
    }
};

class ExampleMapTool final : public QObject, public swak::MapExtension {
    Q_OBJECT
    Q_INTERFACES(swak::MapExtension)
public:
    QString mapId() const override { return QStringLiteral("example.tool"); }
    QString mapName() const override { return QStringLiteral("Example map tool"); }
    Kind mapKind() const override { return Kind::Tool; }
};

class GuiTest final : public QObject {
    Q_OBJECT
private slots:
    void integerMapZoomAndBackground()
    {
        QTemporaryDir directory;
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.hold = true;
        swak::Window window(directory.filePath(QStringLiteral("zoom.json")), QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto *sources = window.findChild<swak::MapSources *>();
        QVERIFY(map && settings && sources);
        const auto zoom = [&] { return settings->savePreferences().value(QStringLiteral("zoom")).toDouble(); };
        const auto initial = map->grabFramebuffer();
        QCOMPARE(initial.pixelColor(20, initial.height() / 2), QColor(swak::MapBackgroundRgb));
        const auto id = QStringLiteral("org.swak.xyz");
        auto source = sources->sources().value(id).toObject();
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        source.insert(QStringLiteral("minZoom"), 3);
        source.insert(QStringLiteral("maxZoom"), 5);
        QVERIFY(sources->put(id, source).isEmpty());
        sources->select(id);
        QCOMPARE(zoom(), 3.0);
        bool bounded = false;
        for (const auto &field : settings->settingsFields()) if (field.path == QStringLiteral("zoom")) {
            QCOMPARE(field.minimum, 3.0); QCOMPARE(field.maximum, 5.0); QCOMPARE(field.decimals, 0);
            bounded = true;
        }
        QVERIFY(bounded);
        for (int i = 0; i < 5; ++i) QTest::keyClick(map, Qt::Key_Plus);
        QCOMPARE(zoom(), 5.0);
        QTest::keyClick(map, Qt::Key_Minus);
        QCOMPARE(zoom(), 4.0);
        const auto wheel = [&](int delta) {
            QWheelEvent event(QPointF(map->rect().center()), QPointF(map->mapToGlobal(map->rect().center())), {}, QPoint(0, delta),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(map, &event);
        };
        wheel(60); QCOMPARE(zoom(), 4.0);
        wheel(60); QCOMPARE(zoom(), 5.0);
        wheel(120); QCOMPARE(zoom(), 5.0);
        wheel(-120); QCOMPARE(zoom(), 4.0);
        QTest::keyClick(map, Qt::Key_Home);
        QCOMPARE(zoom(), 3.0);
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("zoom"), 4.6);
        settings->restorePreferences(values);
        QCOMPARE(zoom(), 5.0);
        source.insert(QStringLiteral("minZoom"), 0);
        source.insert(QStringLiteral("maxZoom"), 2);
        QVERIFY(sources->put(id, source).isEmpty());
        QCOMPARE(zoom(), 2.0);
        for (int i = 0; i < 5; ++i) QTest::keyClick(map, Qt::Key_Minus);
        QCOMPARE(zoom(), 0.0);
        QTest::keyClick(map, Qt::Key_Plus);
        QCOMPARE(zoom(), 1.0);
        QVERIFY(map->grabFramebuffer().save(QStringLiteral("missing-provider-tiles.png")));
        sources->select({});
        QVERIFY(map->grabFramebuffer().save(QStringLiteral("no-provider-background.png")));
        QVERIFY(window.savePreferences());
        auto catalog = sources->savePreferences();
        source.insert(QStringLiteral("minZoom"), 3);
        source.insert(QStringLiteral("maxZoom"), 5);
        auto entries = sources->sources();
        entries.insert(id, source);
        catalog.insert(QStringLiteral("sources"), entries);
        catalog.insert(QStringLiteral("active"), id);
        catalog.insert(QStringLiteral("network"), true);
        const auto legacyPath = directory.filePath(QStringLiteral("legacy-zoom.json"));
        {
            swak::Preferences legacy(legacyPath);
            QVERIFY(legacy.save({{QStringLiteral("version"), 1}, {QStringLiteral("components"),
                QJsonObject{{QStringLiteral("org.swak.maps"), catalog},
                    {QStringLiteral("org.swak.map"), QJsonObject{{QStringLiteral("zoom"), 4.6}}}}}}));
        }
        swak::Window restored(legacyPath, QPluginLoader::staticInstances());
        auto *restoredMap = dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QOpenGLWidget *>());
        QVERIFY(restoredMap);
        QCOMPARE(restoredMap->savePreferences().value(QStringLiteral("zoom")).toDouble(), 5.0);
    }
    void selectionExpiryAndNumericSort()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("selection.json")), QPluginLoader::staticInstances());
        auto *table = window.findChild<QTableView *>(QStringLiteral("eventsTable"));
        auto &store = window.store();
        QAbstractItemModelTester tester(&store, QAbstractItemModelTester::FailureReportingMode::QtTest);
        swak::Event event;
        event.type = QStringLiteral("a-f-G"); event.how = QStringLiteral("h-e");
        event.time = QDateTime::currentDateTimeUtc(); event.start = event.time;
        const auto now = event.time;
        for (int n : {2, 10, -2, -10}) {
            event.uid = QString::number(n); event.latitude = n; event.longitude = n;
            event.stale = now.addSecs(n == 2 ? 1 : 60);
            QVERIFY(store.accept(event, now).isEmpty());
        }
        for (int column : {2, 3}) {
            table->sortByColumn(column, Qt::AscendingOrder);
            QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(), QStringLiteral("-10"));
            QCOMPARE(table->model()->index(3, 0).data(Qt::UserRole).toString(), QStringLiteral("10"));
        }
        store.select(QStringLiteral("2"));
        store.expire(now.addSecs(2));
        QVERIFY(store.selected().isEmpty());
        QVERIFY(!table->currentIndex().isValid());
        store.select(QStringLiteral("10"));
        const auto current = table->currentIndex();
        QVERIFY(current.isValid());
        store.select({});
        table->setCurrentIndex(current);
        QCOMPARE(store.selected(), QStringLiteral("10"));
    }
    void providerRecoveryAndSaveFailure()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("providers.json"));
        swak::MapSources seed(QPluginLoader::staticInstances());
        auto values = seed.savePreferences();
        auto sources = seed.sources();
        sources.insert(QStringLiteral("custom"), sources.value(QStringLiteral("org.swak.xyz")));
        auto invalid = sources.value(QStringLiteral("org.swak.wms")).toObject();
        invalid.insert(QStringLiteral("endpoint"), QStringLiteral("invalid"));
        sources.insert(QStringLiteral("org.swak.wms"), invalid);
        values.insert(QStringLiteral("sources"), sources);
        {
            swak::Preferences profile(path);
            QVERIFY(profile.save({{QStringLiteral("version"), 1}, {QStringLiteral("components"), QJsonObject{{seed.preferenceId(), values}}}}));
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QVERIFY(window.savePreferences());
        }
        swak::Preferences locked(path);
        QCOMPARE(locked.document().value(QStringLiteral("components")).toObject().value(seed.preferenceId()).toObject(), values);
        swak::Window second(path, QPluginLoader::staticInstances());
        second.show();
        auto *activity = second.findChild<swak::ActivityModel *>();
        activity->clear();
        QVERIFY(!QTest::qWaitFor([&] { return activity->rowCount() > 0; }, 1800));
        second.findChild<QAction *>(QStringLiteral("mapProvidersAction"))->trigger();
        auto *manager = second.findChild<swak::MapManager *>();
        bool tested = false;
        QTimer::singleShot(0, manager, [&] {
            auto *dialog = manager->findChild<swak::SettingsDialog *>();
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            for (auto *line : dialog->findChildren<QLineEdit *>())
                if (line->objectName().endsWith(QStringLiteral("/name"))) line->setText(QStringLiteral("Edited"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(dialog->result(), 0);
            QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("settingsStatus"))->text().contains(QStringLiteral("saving failed")));
            tested = true;
        });
        manager->findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
        QVERIFY(tested);
    }
    void repeatedSaveFailureRecovers()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("blocked.json"));
        swak::Window window(path, QPluginLoader::staticInstances());
        QVERIFY(QDir().mkdir(path));
        window.show();
        auto *activity = window.findChild<swak::ActivityModel *>();
        activity->clear();
        const auto failures = [&] {
            int count = 0;
            for (int i = 0; i < activity->rowCount(); ++i)
                if (activity->index(i, 3).data().toString() == QStringLiteral("swak.preferences")) ++count;
            return count;
        };
        QVERIFY(!window.savePreferences());
        QVERIFY(!window.savePreferences());
        QVERIFY(QTest::qWaitFor([&] { return failures() == 1; }, 2000));
        QVERIFY(!QTest::qWaitFor([&] { return failures() > 1; }, 1800));
        QVERIFY(QDir().rmdir(path));
        QVERIFY(window.savePreferences());
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("preferencesStatus"))->text().isEmpty());
    }
    void restoredMenuAndPaneTabs()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("panes.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"))->hide();
            QVERIFY(window.savePreferences());
        }
        swak::Window window(path, QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"));
        auto *root = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QMetaObject::invokeMethod(root, "aboutToShow");
        QMenu *events = nullptr;
        for (auto *action : root->actions()) if (action->menu() && action->text() == QStringLiteral("CoT Events")) events = action->menu();
        QVERIFY(events);
        QVERIFY(events->menuAction()->isCheckable());
        QVERIFY(!events->menuAction()->isChecked());
        events->menuAction()->trigger();
        QVERIFY(!dock->isHidden());
        auto *editor = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
        auto *udp = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
        window.tabifyDockWidget(editor, udp);
        bool tested = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<swak::SettingsDialog *>();
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *width = dialog->findChild<QDoubleSpinBox *>(editor->objectName() + QStringLiteral("/width"));
            QVERIFY(width);
            width->setValue(width->value() + 1);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
            QVERIFY(window.tabifiedDockWidgets(editor).contains(udp));
            tested = true;
        });
        window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
        QVERIFY(tested);
    }
    void internalEventsAndMapToolbar()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("activity.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *history = window.findChild<swak::ActivityModel *>();
            auto *status = window.findChild<QLabel *>(QStringLiteral("latestActivity"));
            auto *table = window.findChild<QTableView *>(QStringLiteral("activityTable"));
            QVERIFY(history && status && table);
            history->clear();
            history->restorePreferences({{QStringLiteral("capacity"), 2}});
            swak::logMessage(swak::Severity::Error, swak::Urgency::Immediate, QStringLiteral("swak.test"), QStringLiteral("Example failure"));
            QVERIFY(QTest::qWaitFor([&] { return status->text() == QStringLiteral("Example failure"); }));
            QVERIFY(status->styleSheet().contains(QStringLiteral("#e53935")));
            QTest::qWait(1100);
            swak::logMessage(swak::Severity::Info, swak::Urgency::Routine, QStringLiteral("swak.test"), QStringLiteral("Recovered"));
            QVERIFY(QTest::qWaitFor([&] { return status->text() == QStringLiteral("Recovered"); }));
            QVERIFY(status->styleSheet().isEmpty());
            QVERIFY(status->findChild<QTimer *>()->remainingTime() > 2800);
            QCOMPARE(history->rowCount(), 2);
            QVERIFY(QTest::qWaitFor([&] { return status->text().isEmpty(); }, 4000));
            QCOMPARE(history->rowCount(), 2);
            auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
            QMenu *historyMenu = nullptr;
            for (auto *action : plugins->actions()) if (action->text() == QStringLiteral("Internal events")) historyMenu = action->menu();
            QVERIFY(historyMenu);
            historyMenu->menuAction()->trigger();
            QVERIFY(table->isVisible());
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTest::qWait(50);
            QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("internal-events-smoke.png")));
            historyMenu->findChild<QAction *>(QStringLiteral("clearActivityAction"))->trigger();
            QCOMPARE(history->rowCount(), 0);
            table->horizontalHeader()->resizeSection(0, 250);
            auto *button = window.findChild<QToolButton *>(QStringLiteral("mapProviderButton"));
            QVERIFY(button);
            auto *maps = window.findChild<swak::MapSources *>();
            auto *menu = button->menu();
            QMetaObject::invokeMethod(menu, "aboutToShow");
            QCOMPARE(menu->actions().size(), 6);
            QCOMPARE(menu->actions().first()->text(), QStringLiteral("No map provider"));
            QVERIFY(menu->actions().first()->isChecked());
            menu->actions().at(1)->trigger();
            QVERIFY(!maps->activeSource().isEmpty());
            QCOMPARE(button->text(), maps->activeSource().value(QStringLiteral("name")).toString());
            menu->actions().first()->trigger();
            QVERIFY(maps->activeSource().isEmpty());
            menu->actions().last()->trigger();
            QVERIFY(window.findChild<swak::MapManager *>());
            window.findChild<swak::MapManager *>()->close();
            auto values = maps->savePreferences();
            auto sources = values.value(QStringLiteral("sources")).toObject();
            sources.insert(QStringLiteral("absent"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Absent provider")}, {QStringLiteral("format"), QStringLiteral("absent")}});
            values.insert(QStringLiteral("sources"), sources);
            maps->restorePreferences(values);
            QMetaObject::invokeMethod(menu, "aboutToShow");
            QVERIFY(!menu->actions().at(1)->isEnabled());
            QVERIFY(window.savePreferences());
            QVERIFY(window.grab().save(QStringLiteral("activity-toolbar-smoke.png")));
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<swak::ActivityModel *>()->savePreferences().value(QStringLiteral("capacity")).toInt(), 2);
        QCOMPARE(restored.findChild<QTableView *>(QStringLiteral("activityTable"))->horizontalHeader()->sectionSize(0), 250);
    }

    void mapProvidersAndGpu()
    {
        QTemporaryDir directory;
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        ExampleMapTool tool;
        auto plugins = QPluginLoader::staticInstances();
        plugins.append(&tool);
        auto owner = std::make_unique<swak::Window>(directory.filePath(QStringLiteral("maps.json")), plugins);
        auto &window = *owner;
        auto *sources = window.findChild<swak::MapSources *>();
        QVERIFY(sources);
        QCOMPARE(sources->providers().size(), 3);
        QVERIFY(!sources->networkEnabled());
        const auto id = QStringLiteral("org.swak.xyz");
        auto source = sources->sources().value(id).toObject();
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(sources->put(id, source).isEmpty());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        sources->select(id);
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        QVERIFY(QTest::qWaitFor([&] {
            const auto frame = map->grabFramebuffer();
            if (frame.isNull()) return false;
            for (int dx : {-100, 100}) for (int dy : {-100, 100})
                if (frame.pixelColor(frame.width() / 2 + dx, frame.height() / 2 + dy) != QColor("#eb426a")) return false;
            return true;
        }, 5000));
        auto *credit = map->findChild<QLabel *>(QStringLiteral("mapAttribution"));
        QVERIFY(credit);
        QCOMPARE(credit->alignment(), Qt::AlignRight);
        QVERIFY(credit->text().contains(QStringLiteral("href=")));
        QVERIFY(!credit->text().contains(QStringLiteral("| https")));
        QVERIFY(credit->styleSheet().contains(QStringLiteral("140")));
        BrowserCapture browser;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); });
        credit->setFocus();
        QTest::keyClick(credit, Qt::Key_Tab);
        QTest::keyClick(credit, Qt::Key_Return);
        QCOMPARE(browser.opened, QUrl(QStringLiteral("https://www.openstreetmap.org/copyright")));
        source.insert(QStringLiteral("attribution"), QStringLiteral("https://example.org/license"));
        QVERIFY(sources->put(id, source).isEmpty());
        map->grabFramebuffer();
        QVERIFY(credit->text().contains(QStringLiteral(">Map attribution</a>")));
        source.insert(QStringLiteral("attribution"), QStringLiteral("Example (https://example.org/license). | https://example.org/second"));
        QVERIFY(sources->put(id, source).isEmpty());
        map->grabFramebuffer();
        QCOMPARE(credit->text().count(QStringLiteral("href=")), 2);
        QVERIFY(credit->text().contains(QStringLiteral("href=\"https://example.org/license\"")));
        QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("map-attribution-smoke.png")));
        QVERIFY(map->grabFramebuffer().save(QStringLiteral("provider-map-smoke.png")));
        window.findChild<QAction *>(QStringLiteral("mapProvidersAction"))->trigger();
        auto *dialog = window.findChild<swak::MapManager *>();
        QVERIFY(dialog);
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("mapProviderList"));
        QCOMPARE(list->count(), 3);
        QVERIFY(dialog->grab().save(QStringLiteral("map-providers-smoke.png")));
        dialog->findChild<QPushButton *>(QStringLiteral("useMapProvider"))->click();
        QCOMPARE(sources->activeId(), id);
        QVERIFY(!dialog->findChild<QComboBox *>());
        QTimer::singleShot(0, dialog, [dialog] {
            auto *editor = dialog->findChild<swak::SettingsDialog *>();
            QVERIFY(editor);
            const auto close = qScopeGuard([editor] { editor->reject(); });
            QVERIFY(!editor->findChild<QTabWidget *>());
            for (auto *field : editor->findChildren<QLineEdit *>())
                if (field->objectName().contains(QLatin1Char('/'))) QVERIFY(field->text().isEmpty());
            editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(editor->isVisible());
            QVERIFY(!editor->findChild<QLabel *>(QStringLiteral("settingsStatus"))->text().isEmpty());
            QVERIFY(QTest::qWaitForWindowExposed(editor));
            QVERIFY(editor->grab().save(QStringLiteral("add-provider-smoke.png")));
        });
        dialog->findChild<QPushButton *>(QStringLiteral("addMapProvider"))->click();
        QCOMPARE(list->count(), 3);
        QTimer::singleShot(0, dialog, [dialog, id, source] {
            auto *editor = dialog->findChild<swak::SettingsDialog *>();
            QVERIFY(editor);
            const auto close = qScopeGuard([editor] { if (editor->isVisible()) editor->reject(); });
            auto *formats = editor->findChild<QComboBox *>(QStringLiteral("mapProviderFormats"));
            for (int i = 0; i < formats->count(); ++i) {
                formats->setCurrentIndex(i);
                if (editor->findChild<QLineEdit *>(id + QStringLiteral("/name"))->isVisible()) break;
            }
            for (const auto &key : {QStringLiteral("name"), QStringLiteral("endpoint"), QStringLiteral("attribution")})
                editor->findChild<QLineEdit *>(id + '/' + key)->setText(source.value(key).toString());
            editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        dialog->findChild<QPushButton *>(QStringLiteral("addMapProvider"))->click();
        QCOMPARE(list->count(), 4);
        dialog->findChild<QPushButton *>(QStringLiteral("removeMapProvider"))->click();
        QCOMPARE(list->count(), 3);
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(Qt::UserRole).toString() == id) list->setCurrentRow(i);
        QTimer::singleShot(0, dialog, [dialog, id] {
            auto *editor = dialog->findChild<swak::SettingsDialog *>();
            if (!editor) return;
            const auto close = qScopeGuard([editor] { if (editor->isVisible()) editor->reject(); });
            QVERIFY(!editor->findChild<QTabWidget *>());
            for (auto *label : editor->findChildren<QLabel *>())
                if (label->text() == QStringLiteral("Attribution and license URL")) QVERIFY(!label->wordWrap());
            QVERIFY(QTest::qWaitForWindowExposed(editor));
            QVERIFY(QTest::qWaitFor([editor] { return editor->findChild<QScrollArea *>()->horizontalScrollBar()->maximum() == 0; }));
            QVERIFY(editor->grab().save(QStringLiteral("edit-provider-smoke.png")));
            editor->findChild<QLineEdit *>(id + QStringLiteral("/name"))->setText(QStringLiteral("Local test map"));
            editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        dialog->findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
        source.insert(QStringLiteral("name"), QStringLiteral("Local test map"));
        QCOMPARE(sources->sources().value(id).toObject(), source);
        dialog->findChild<QPushButton *>(QStringLiteral("clearMapProvider"))->click();
        QVERIFY(!sources->networkEnabled());
        QVERIFY(window.savePreferences());
        dialog->close();
        window.close();
        owner.reset();
        swak::Window restored(directory.filePath(QStringLiteral("maps.json")), plugins);
        QCOMPARE(restored.findChild<swak::MapSources *>()->sources().value(id).toObject(), source);
        swak::MapSources absent({});
        absent.restorePreferences(restored.findChild<swak::MapSources *>()->savePreferences());
        swak::MapManager missing(absent);
        auto *missingList = missing.findChild<QListWidget *>(QStringLiteral("mapProviderList"));
        QVERIFY(missingList->item(0)->text().contains(QStringLiteral("unavailable")));
        missing.findChild<QPushButton *>(QStringLiteral("useMapProvider"))->click();
        QVERIFY(absent.activeSource().isEmpty());
        missing.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
    }
    void pluginToolbar()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("toolbar.json"));
        ExampleToolbar plugin;
        QPointer<QAction> action;
        {
            swak::Window window(path, {&plugin});
            auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
            QVERIFY(toolbar);
            QCOMPARE(window.toolBarArea(toolbar), Qt::TopToolBarArea);
            QVERIFY(!toolbar->isMovable());
            QVERIFY(!toolbar->isFloatable());
            QCOMPARE(toolbar->actions().size(), 2);
            QCOMPARE(toolbar->findChild<QLabel *>(QStringLiteral("toolbarPluginLabel"))->text(), QStringLiteral("Example toolbar"));
            action = toolbar->actions().last();
            QVERIFY(!action->icon().isNull());
            action->trigger();
            QCOMPARE(plugin.calls, 1);
            toolbar->hide();
            QVERIFY(window.savePreferences());
        }
        QVERIFY(action.isNull());
        {
            swak::Window absent(path, {});
            auto *toolbar = absent.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
            QVERIFY(toolbar->actions().isEmpty());
            QVERIFY(toolbar->isHidden());
            QVERIFY(absent.savePreferences());
        }
        swak::Window restored(path, {&plugin});
        auto *toolbar = restored.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(toolbar->isHidden());
        toolbar->actions().last()->trigger();
        QCOMPARE(plugin.calls, 2);
        restored.findChild<QAction *>(QStringLiteral("resetLayoutAction"))->trigger();
        QVERIFY(!toolbar->isHidden());
    }
    void aboutDialog()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("about.json")), {});
        auto *help = window.findChild<QMenu *>(QStringLiteral("helpMenu"));
        auto *about = window.findChild<QAction *>(QStringLiteral("aboutAction"));
        QVERIFY(help && about);
        QCOMPARE(window.menuBar()->actions().last(), help->menuAction());
        QVERIFY(help->actions().contains(about));
        for (int i = 0; i < 2; ++i) {
            about->trigger();
            QPointer<QMessageBox> dialog = window.findChild<QMessageBox *>(QStringLiteral("aboutDialog"));
            QVERIFY(dialog);
            QVERIFY(dialog->text().contains(QStringLiteral("Version ") + QStringLiteral(SWAK_VERSION)));
            QCOMPARE(dialog->textFormat(), Qt::PlainText);
            about->trigger();
            QCOMPARE(window.findChildren<QMessageBox *>().size(), 1);
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            QVERIFY(dialog->grab().save(QStringLiteral("about-smoke.png")));
            dialog->close();
            QVERIFY(QTest::qWaitFor([&dialog] { return dialog.isNull(); }));
        }
    }
    void pluginMenus()
    {
        QTemporaryDir directory;
        ExampleMenu plugin;
        QPointer<QAction> oldAction;
        for (int i = 0; i < 2; ++i) {
            swak::Window window(directory.filePath(QStringLiteral("menu.json")), {&plugin});
            auto *tools = window.findChild<QMenu *>(QStringLiteral("toolsMenu"));
            auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
            QVERIFY(tools && plugins);
            QVERIFY(window.menuBar()->actions().contains(tools->menuAction()));
            QVERIFY(tools->actions().contains(plugins->menuAction()));
            QCOMPARE(plugins->actions().size(), 2);
            auto *menu = plugins->actions().first()->menu();
            QVERIFY(menu);
            QCOMPARE(menu->title(), QStringLiteral("Example menu"));
            oldAction = menu->actions().first();
            oldAction->trigger();
            QCOMPARE(plugin.calls, i + 1);
        }
        QVERIFY(oldAction.isNull());
        plugin.empty = true;
        swak::Window empty(directory.filePath(QStringLiteral("empty.json")), {&plugin});
        auto *emptyMenu = empty.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QCOMPARE(emptyMenu->actions().size(), 1);
        QVERIFY(emptyMenu->isEnabled());
        swak::Window absent(directory.filePath(QStringLiteral("absent.json")), {});
        QCOMPARE(absent.findChild<QMenu *>(QStringLiteral("pluginsMenu"))->actions().size(), 1);
        QVERIFY(!absent.findChild<QAction *>(QStringLiteral("clearEventsAction")));
    }
    void pluginPaneActivation()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("activate.json")), QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *root = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QVERIFY(root);
        int activated = 0;
        for (auto *action : root->actions()) {
            auto docks = window.findChildren<QDockWidget *>();
            auto found = std::find_if(docks.begin(), docks.end(), [action](auto *dock) { return dock->windowTitle() == action->text(); });
            if (found == docks.end()) continue;
            ++activated;
            auto *dock = *found;
            QVERIFY(action->isCheckable());
            if (action->menu()) for (auto *child : action->menu()->actions()) QVERIFY(child->text() != QStringLiteral("Show pane"));
            dock->hide();
            root->popup(window.mapToGlobal(QPoint(40, 60)));
            QVERIFY(QTest::qWaitForWindowExposed(root));
            QTest::mouseClick(root, Qt::LeftButton, Qt::NoModifier, root->actionGeometry(action).center());
            QVERIFY2(!dock->isHidden(), qPrintable(action->text()));
            QVERIFY(!root->isVisible());
            QVERIFY(action->isChecked());
            action->trigger();
            QVERIFY(dock->isHidden());
            QVERIFY(!action->isChecked());
            if (action->menu()) {
                root->popup(window.mapToGlobal(QPoint(40, 60)));
                root->setActiveAction(action);
                QTest::keyClick(root, Qt::Key_Right);
                QVERIFY(QTest::qWaitForWindowExposed(action->menu()));
                QTest::keyClick(action->menu(), Qt::Key_Escape);
                root->close();
            }
            dock->setFloating(true);
            dock->hide();
            root->popup(window.mapToGlobal(QPoint(40, 60)));
            root->setActiveAction(action);
            QTest::keyClick(root, Qt::Key_Return);
            QVERIFY(!dock->isHidden());
            QVERIFY(dock->isFloating());
            QVERIFY(action->isChecked());
            root->popup(window.mapToGlobal(QPoint(40, 60)));
            root->setActiveAction(action);
            QTest::keyClick(root, Qt::Key_Space);
            QVERIFY(dock->isHidden());
            QVERIFY(!action->isChecked());
            action->trigger();
            QVERIFY(!dock->isHidden());
            dock->close();
            QVERIFY(!action->isChecked());
            dock->setFloating(false);
        }
        QCOMPARE(activated, 5);
        QVERIFY(window.grab().save(QStringLiteral("plugin-labels-smoke.png")));
    }
    void clearEventsMenu()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("clear.json")), QPluginLoader::staticInstances());
        auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QVERIFY(plugins);
        QMenu *events = nullptr;
        for (auto *action : plugins->actions()) {
            if (action->menu() && action->menu()->title() == QStringLiteral("CoT Events")) events = action->menu();
        }
        QVERIFY(events);
        auto *clear = window.findChild<QAction *>(QStringLiteral("clearEventsAction"));
        QVERIFY(clear && events->actions().contains(clear));
        QCOMPARE(clear->text(), QStringLiteral("&Clear"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(toolbar && toolbar->actions().contains(clear));
        QVERIFY(!clear->icon().isNull());
        QCOMPARE(clear->toolTip(), QStringLiteral("Clear CoT Events"));
        QVERIFY(!window.menuBar()->actions().contains(events->menuAction()));
        auto *generate = window.findChild<QPushButton *>(QStringLiteral("generateEvent"));
        QVERIFY(generate);
        generate->click();
        QCOMPARE(window.store().rowCount(), 1);
        QVERIFY(!window.store().selected().isEmpty());
        auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(clear));
        QVERIFY(button);
        button->click();
        QCOMPARE(window.store().rowCount(), 0);
        QVERIFY(window.store().selected().isEmpty());
        clear->trigger();
        QCOMPARE(window.store().rowCount(), 0);
        bool standalone = false;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (auto *provider = qobject_cast<swak::ToolbarPlugin *>(plugin); provider && qobject_cast<swak::ToolPlugin *>(plugin)) {
                QToolBar independent;
                swak::PluginServices services{window.store(), *window.findChild<swak::ActivityModel *>(), *window.findChild<swak::MapSources *>()};
                provider->populateToolbar(services, independent, nullptr);
                QCOMPARE(independent.actions().size(), 1);
                generate->click();
                QCOMPARE(window.store().rowCount(), 1);
                independent.actions().first()->trigger();
                QCOMPARE(window.store().rowCount(), 0);
                standalone = true;
            }
        }
        QVERIFY(standalone);
    }
    void localizedSettingsLists()
    {
        const QLocale previous;
        const auto restoreLocale = qScopeGuard([previous] { QLocale::setDefault(previous); });
        QLocale::setDefault(QLocale(QStringLiteral("sv_SE")));
        const QStringList labels{QStringLiteral("Örebro"), QStringLiteral("Ångström"), QStringLiteral("Zulu"),
                                 QStringLiteral("Älg"), QStringLiteral("Alpha")};
        QList<swak::SettingsPage> pages;
        int chosen = -1;
        for (const auto &label : labels) pages.append({label, label, {}, {}, {}, {}});
        pages.append({QStringLiteral("choice"), QStringLiteral("Choice"),
            {swak::choiceSetting(QStringLiteral("value"), QStringLiteral("Value"), labels, {10, 20, 30, 40, 50})},
            {{QStringLiteral("value"), 20}}, {}, [&](const QJsonObject &values) { chosen = values.value(QStringLiteral("value")).toInt(); }});
        pages.append({QStringLiteral("pane-z"), QStringLiteral("Zulu"), {}, {}, {}, {}, true});
        pages.append({QStringLiteral("pane-a"), QStringLiteral("Alpha"), {}, {}, {}, {}, true});
        swak::SettingsDialog dialog(pages, [] { return QString{}; }, nullptr);
        const QCollator collator;
        for (auto *list : dialog.findChildren<QListWidget *>()) {
            for (int i = 1; i < list->count(); ++i)
                QVERIFY(collator.compare(list->item(i - 1)->text(), list->item(i)->text()) <= 0);
        }
        auto *combo = dialog.findChild<QComboBox *>();
        QVERIFY(combo);
        QCOMPARE(combo->itemText(0), QStringLiteral("Alpha"));
        QCOMPARE(combo->itemText(1), QStringLiteral("Zulu"));
        QCOMPARE(combo->itemText(2), QStringLiteral("Ångström"));
        QCOMPARE(combo->itemText(3), QStringLiteral("Älg"));
        QCOMPARE(combo->itemText(4), QStringLiteral("Örebro"));
        QCOMPARE(combo->currentData().toInt(), 20);
        combo->setCurrentIndex(combo->findData(40));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(chosen, 40);
        dialog.restoreNavigation({{QStringLiteral("tab"), QStringLiteral("panes")},
                                  {QStringLiteral("paneSettingsSections"), QStringLiteral("pane-z")}});
        QCOMPARE(dialog.findChild<QTabWidget *>()->currentIndex(), 1);
        QCOMPARE(dialog.findChild<QListWidget *>(QStringLiteral("paneSettingsSections"))->currentItem()->text(), QStringLiteral("Zulu"));
        swak::SettingsDialog reopened(pages, [] { return QString{}; }, nullptr);
        reopened.restoreNavigation(dialog.navigation());
        QCOMPARE(reopened.navigation(), dialog.navigation());
    }
    void settingsPluginExtension()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("extension.json"));
        ExampleSettings example;
        auto plugins = QPluginLoader::staticInstances();
        plugins.append(&example);
        {
            swak::Window window(path, plugins);
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                if (!dialog) { QFAIL("Settings dialog did not open."); return; }
                auto *field = dialog->findChild<QCheckBox *>(QStringLiteral("org.swak.example/enabled"));
                if (!field) { dialog->reject(); QFAIL("The extension section is missing."); return; }
                field->setChecked(true);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
            QVERIFY(example.enabled);
        }
        {
            swak::Window absent(path, QPluginLoader::staticInstances());
            QVERIFY(absent.savePreferences());
        }
        example.enabled = false;
        swak::Window restored(path, plugins);
        QVERIFY(example.enabled);
    }
    void unchangedSettingsPreservePrecision()
    {
        int applications = 0;
        swak::SettingsDialog dialog({{QStringLiteral("test"), QStringLiteral("Precision"),
            {swak::numberSetting(QStringLiteral("number"), QStringLiteral("Number"), 0, 1, 2)},
            {{QStringLiteral("number"), 0.123456789}}, {}, [&](const QJsonObject &) { ++applications; }}}, [] { return QString{}; }, nullptr);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(applications, 0);
    }
    void tableColumnSettings()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("table.json")), QPluginLoader::staticInstances());
        auto *table = window.findChild<QTableView *>();
        QVERIFY(table);
        auto *provider = dynamic_cast<swak::SettingsParticipant *>(table);
        QVERIFY(provider);
        table->horizontalHeader()->resizeSection(0, 231);
        table->setColumnHidden(0, true);
        auto values = provider->savePreferences();
        QCOMPARE(swak::settingValue(values, QStringLiteral("columns/0/width")).toInt(), 231);
        values = swak::withSettingValue(values, QStringLiteral("columns/0/position"), 1).toObject();
        QVERIFY(!provider->validateSettings(values).isEmpty());
        values = swak::withSettingValue(values, QStringLiteral("columns/1/position"), 0).toObject();
        QVERIFY(provider->validateSettings(values).isEmpty());
        provider->restorePreferences(values);
        QCOMPARE(table->horizontalHeader()->visualIndex(0), 1);
        table->setColumnHidden(0, false);
        QCOMPARE(table->horizontalHeader()->sectionSize(0), 231);
    }
    void settingsDialog()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("settings.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *action = window.findChild<QAction *>(QStringLiteral("settingsAction"));
            QVERIFY(action);
            QVERIFY(window.findChild<QMenu *>(QStringLiteral("fileMenu"))->actions().contains(action));
            QCOMPARE(action->text(), QStringLiteral("&Settings"));
            QVERIFY(!window.findChild<QMenu *>(QStringLiteral("settingsMenu")));
            QCOMPARE(window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"))->windowTitle(), QStringLiteral("CoT Event Generator"));
            QCOMPARE(window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"))->windowTitle(), QStringLiteral("CoT Events"));
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                if (!dialog) { QFAIL("Settings dialog did not open."); return; }
                const auto cancel = [&] { dialog->reject(); };
                auto *identity = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.editor/identity"));
                if (!identity) { cancel(); QFAIL("The editor section is missing."); return; }
                identity->setText(QStringLiteral("cancelled"));
                cancel();
            });
            const auto initial = window.findChild<QLineEdit *>(QStringLiteral("identity"))->text();
            action->trigger();
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), initial);
            bool complete = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                if (!dialog) { QFAIL("Settings dialog did not open."); return; }
                auto *tabs = dialog->findChild<QTabWidget *>();
                auto *panes = dialog->findChild<QListWidget *>(QStringLiteral("paneSettingsSections"));
                if (!tabs || tabs->count() != 2 || !panes || panes->count() != 5) {
                    dialog->reject(); QFAIL("The pane tab is incomplete."); return;
                }
                const QCollator collator;
                for (auto *list : dialog->findChildren<QListWidget *>()) {
                    for (int i = 0; i < list->count(); ++i) {
                        if (list->item(i)->text().startsWith(QStringLiteral("Pane:")) ||
                            (i > 0 && collator.compare(list->item(i - 1)->text(), list->item(i)->text()) > 0)) {
                            dialog->reject(); QFAIL("Settings sections are not correctly named and sorted."); return;
                        }
                    }
                }
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                auto *identity = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.editor/identity"));
                auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.udp/address"));
                auto *enabled = dialog->findChild<QCheckBox *>(QStringLiteral("logging/enabled"));
                auto *severity = dialog->findChild<QComboBox *>(QStringLiteral("logging/severity"));
                auto *interval = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("application/saveInterval"));
                auto *zoom = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.map/zoom"));
                auto *width = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.events/columns/0/width"));
                if (!buttons || !identity || !address || !enabled || !severity || !interval || !zoom || !width) {
                    dialog->reject(); QFAIL("A settings field is missing."); return;
                }
                identity->setText(QStringLiteral("configured"));
                address->setText(QStringLiteral("invalid-address"));
                buttons->button(QDialogButtonBox::Apply)->click();
                if (window.findChild<QLineEdit *>(QStringLiteral("identity"))->text() != initial) {
                    dialog->reject(); QFAIL("An invalid page allowed partial application."); return;
                }
                address->setText(QStringLiteral("127.0.0.1"));
                enabled->setChecked(true);
                severity->setCurrentIndex(severity->findData(3));
                interval->setValue(2000);
                zoom->setValue(4);
                width->setValue(230);
                buttons->button(QDialogButtonBox::Apply)->click();
                if (!QTest::qWaitFor([dialog] {
                    for (auto *scroll : dialog->findChildren<QScrollArea *>())
                        if (scroll->isVisible() && scroll->widget()->width() > scroll->viewport()->width()) return false;
                    return true;
                }, 1000)) { dialog->reject(); QFAIL("The Settings form overflows horizontally."); return; }
                if (!dialog->grab().save(QStringLiteral("settings-smoke.png"))) {
                    dialog->reject(); QFAIL("Settings screenshot failed."); return;
                }
                tabs->setCurrentIndex(1);
                if (!QTest::qWaitFor([dialog] {
                    for (auto *scroll : dialog->findChildren<QScrollArea *>())
                        if (scroll->isVisible() && scroll->widget()->width() > scroll->viewport()->width()) return false;
                    return true;
                }, 1000)) { dialog->reject(); QFAIL("The pane form overflows horizontally."); return; }
                if (!dialog->grab().save(QStringLiteral("settings-panes.png"))) {
                    dialog->reject(); QFAIL("Pane screenshot failed."); return;
                }
                identity->setText(QStringLiteral("discard-after-apply"));
                dialog->reject();
                complete = true;
            });
            action->trigger();
            QVERIFY(complete);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), QStringLiteral("configured"));
            QVERIFY(swak::loggingConfiguration().value(QStringLiteral("enabled")).toBool());
            window.close();
        }
        {
            swak::Window restored(path, QPluginLoader::staticInstances());
            QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), QStringLiteral("configured"));
            QCOMPARE(swak::loggingConfiguration().value(QStringLiteral("severity")).toInt(), 3);
            auto *map = dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>());
            QVERIFY(map);
            QCOMPARE(map->savePreferences().value(QStringLiteral("zoom")).toDouble(), 4.0);
        }
        swak::configureLogging({});
    }
    void settingsSaveFailure()
    {
        bool applied = false;
        swak::SettingsDialog dialog({{QStringLiteral("test"), QStringLiteral("Test"),
            {swak::textSetting(QStringLiteral("value"), QStringLiteral("Value"), 20)},
            {{QStringLiteral("value"), QStringLiteral("before")}}, {},
            [&](const QJsonObject &) { applied = true; }}}, [] { return QStringLiteral("Read-only profile"); }, nullptr);
        dialog.findChild<QLineEdit *>()->setText(QStringLiteral("after"));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QVERIFY(applied);
        QCOMPARE(dialog.result(), 0);
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("settingsStatus"))->text().contains(QStringLiteral("Read-only")));
    }
    void preferencesRestart()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        QJsonObject mapValues;
        QSize mainSize;
        QSize floatingSize;
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            window.resize(1140, 780);
            window.move(80, 70);
            mainSize = window.size();
            auto *events = window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"));
            events->setFloating(true);
            events->resize(640, 300);
            events->move(160, 180);
            floatingSize = events->size();
            window.findChild<QDockWidget *>(QStringLiteral("pluginsDock"))->hide();
            auto *editor = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
            window.addDockWidget(Qt::LeftDockWidgetArea, editor);
            window.findChild<QLineEdit *>(QStringLiteral("identity"))->setText(QStringLiteral("saved-identity"));
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))->setText(QStringLiteral("Saved callsign"));
            window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"))->setValue(12.345);
            window.findChild<QSpinBox *>(QStringLiteral("lifetime"))->setValue(1200);
            window.findChild<QLineEdit *>(QStringLiteral("bindAddress"))->setText(QStringLiteral("::1"));
            window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->setValue(17001);
            auto *table = window.findChild<QTableView *>(QStringLiteral("eventsTable"));
            table->horizontalHeader()->resizeSection(0, 240);
            table->horizontalHeader()->moveSection(3, 0);
            table->sortByColumn(2, Qt::DescendingOrder);
            auto *map = window.findChild<QOpenGLWidget *>(QStringLiteral("map"));
            QTest::keyClick(map, Qt::Key_Plus);
            QTest::keyClick(map, Qt::Key_Right);
            mapValues = dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QCOMPARE(window.size(), mainSize);
            auto *events = window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"));
            QVERIFY(events->isFloating());
            QCOMPARE(events->size(), floatingSize);
            QVERIFY(window.findChild<QDockWidget *>(QStringLiteral("pluginsDock"))->isHidden());
            QCOMPARE(window.dockWidgetArea(window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"))), Qt::LeftDockWidgetArea);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), QStringLiteral("saved-identity"));
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(), QStringLiteral("Saved callsign"));
            QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"))->value(), 12.345);
            QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("lifetime"))->value(), 1200);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("bindAddress"))->text(), QStringLiteral("::1"));
            QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->value(), 17001);
            auto *table = window.findChild<QTableView *>(QStringLiteral("eventsTable"));
            QCOMPARE(table->horizontalHeader()->sectionSize(0), 240);
            QCOMPARE(table->horizontalHeader()->logicalIndex(0), 3);
            QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 2);
            QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
            auto *map = dynamic_cast<swak::PreferenceParticipant *>(window.findChild<QOpenGLWidget *>(QStringLiteral("map")));
            QCOMPARE(map->savePreferences(), mapValues);
            window.close();
        }
    }
    void missingPlugin()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        auto available = QPluginLoader::staticInstances();
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (qobject_cast<swak::SourcePlugin *>(plugin)) available.removeAll(plugin);
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *receiver = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
            receiver->setFloating(true);
            receiver->resize(480, 310);
            receiver->move(170, 140);
            window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->setValue(17654);
            window.close();
        }
        {
            swak::Preferences preferences(path);
            auto values = preferences.document();
            values.insert(QStringLiteral("unknownFutureKey"), QStringLiteral("preserve"));
            QVERIFY(preferences.save(values));
        }
        {
            swak::Window window(path, available);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(!window.findChild<QWidget *>(QStringLiteral("bindPort")));
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))->setText(QStringLiteral("Changed without receiver"));
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *receiver = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
            QVERIFY(receiver->isFloating());
            QCOMPARE(receiver->size(), QSize(480, 310));
            QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->value(), 17654);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(), QStringLiteral("Changed without receiver"));
            window.close();
        }
        swak::Preferences preferences(path);
        QCOMPARE(preferences.document().value(QStringLiteral("unknownFutureKey")).toString(), QStringLiteral("preserve"));
    }
    void missingViewOrCodec_data()
    {
        QTest::addColumn<bool>("withoutCodec");
        QTest::newRow("missing-view") << false;
        QTest::newRow("missing-codec") << true;
    }
    void missingViewOrCodec()
    {
        QFETCH(bool, withoutCodec);
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))->setText(QStringLiteral("Keep draft"));
            QVERIFY(window.savePreferences());
        }
        auto available = QPluginLoader::staticInstances();
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (withoutCodec ? qobject_cast<swak::Codec *>(plugin) != nullptr
                             : qobject_cast<swak::ViewPlugin *>(plugin) != nullptr) available.removeAll(plugin);
        }
        {
            swak::Window window(path, available);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            if (withoutCodec) QVERIFY(!window.findChild<QWidget *>(QStringLiteral("callsign")));
            else QVERIFY(window.findChild<QWidget *>(QStringLiteral("missingMap")));
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(), QStringLiteral("Keep draft"));
        }
    }
    void receiverRestarts()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        QUdpSocket reservation;
        QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        const quint16 port = reservation.localPort();
        reservation.close();
        for (int cycle = 0; cycle < 2; ++cycle) {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *portField = window.findChild<QSpinBox *>(QStringLiteral("bindPort"));
            if (cycle == 0) {
                portField->setValue(port);
                window.findChild<QPushButton *>(QStringLiteral("receiverToggle"))->click();
            }
            QCOMPARE(portField->value(), static_cast<int>(port));
            QVERIFY(!portField->isEnabled());
            QVERIFY(!reservation.bind(QHostAddress::LocalHost, port));
            window.close();
        }
        QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QVERIFY(window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->isEnabled());
            QVERIFY(window.savePreferences());
        }
        reservation.close();
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QVERIFY(!window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->isEnabled());
            window.findChild<QPushButton *>(QStringLiteral("receiverToggle"))->click();
            QVERIFY(window.savePreferences());
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QVERIFY(window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->isEnabled());
        }
    }
    void invalidPreferenceValues()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        {
            swak::Preferences preferences(path);
            QVERIFY(preferences.save({{QStringLiteral("version"), 1},
                {QStringLiteral("window"), QJsonObject{{QStringLiteral("geometry"), QStringLiteral("broken")},
                                                     {QStringLiteral("layout"), QStringLiteral("YnJva2Vu")}}},
                {QStringLiteral("components"), QJsonObject{
                    {QStringLiteral("org.swak.map"), QJsonObject{{QStringLiteral("zoom"), 10000}, {QStringLiteral("centerX"), -3}}},
                    {QStringLiteral("org.swak.udp"), QJsonObject{{QStringLiteral("port"), 99999}}},
                    {QStringLiteral("org.swak.editor"), QJsonObject{{QStringLiteral("latitude"), 999}, {QStringLiteral("lifetime"), -1}}}}}}));
        }
        swak::Window window(path, QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"))->value(), 0.0);
        QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("lifetime"))->value(), 300);
        QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("bindPort"))->value(), 6969);
        const auto values = dynamic_cast<swak::PreferenceParticipant *>(window.findChild<QOpenGLWidget *>(QStringLiteral("map")))->savePreferences();
        QCOMPARE(values.value(QStringLiteral("zoom")).toDouble(), 1.0);
        QCOMPARE(values.value(QStringLiteral("centerX")).toDouble(), 0.5);
        window.close();
    }
    void automaticSaveAndTabSelection()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *plugins = window.findChild<QDockWidget *>(QStringLiteral("pluginsDock"));
            plugins->raise();
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))->setText(QStringLiteral("Autosaved"));
            QVERIFY(QTest::qWaitFor([&path] {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) return false;
                const auto json = QJsonDocument::fromJson(file.readAll()).object();
                return json.value(QStringLiteral("components")).toObject().value(QStringLiteral("org.swak.editor"))
                    .toObject().value(QStringLiteral("callsign")).toString() == QStringLiteral("Autosaved");
            }, 2500));
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *plugins = window.findChild<QDockWidget *>(QStringLiteral("pluginsDock"));
            QVERIFY(!plugins->visibleRegion().isEmpty());
            auto *editor = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
            QVERIFY(editor->visibleRegion().isEmpty());
            plugins->setFloating(true);
            plugins->move(6000, 4000);
            window.showMaximized();
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(window.isMaximized());
            const auto *plugins = window.findChild<QDockWidget *>(QStringLiteral("pluginsDock"));
            QVERIFY(window.screen()->availableGeometry().contains(plugins->geometry().topLeft()));
            window.findChild<QAction *>(QStringLiteral("resetLayoutAction"))->trigger();
            QVERIFY(!plugins->isFloating());
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(), QStringLiteral("Autosaved"));
            window.close();
        }
    }
    void invalidFileIsPreserved()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray invalid("{broken");
        file.write(invalid);
        file.close();
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(!window.findChild<QLabel *>(QStringLiteral("preferencesStatus"))->text().isEmpty());
            window.close();
        }
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), invalid);
    }
    void independentPanes()
    {
        QTemporaryDir preferences;
        swak::Window window(preferences.filePath(QStringLiteral("preferences.json")), QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto docks = window.findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly);
        QCOMPARE(docks.size(), 5);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("toolsDock")));
        auto *viewMenu = window.findChild<QMenu *>(QStringLiteral("viewMenu"));
        auto *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
        auto *quit = window.findChild<QAction *>(QStringLiteral("quitAction"));
        QVERIFY(viewMenu && fileMenu && quit);
        QCOMPARE(viewMenu->actions().size(), 2);
        QVERIFY(viewMenu->actions().contains(window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))->toggleViewAction()));
        QVERIFY(fileMenu->actions().contains(quit));
        QCOMPARE(window.menuBar()->actions().first(), fileMenu->menuAction());
        QCOMPARE(quit->shortcut(), QKeySequence(QKeySequence::Quit));
        for (const auto *menu : window.findChildren<QMenu *>()) {
            if (menu != fileMenu) QVERIFY(!menu->actions().contains(quit));
        }
        for (auto *dock : docks) {
            QVERIFY(!viewMenu->actions().contains(dock->toggleViewAction()));
            dock->show();
            auto *grip = dock->findChild<QSizeGrip *>();
            QVERIFY(grip);
            QVERIFY(grip->isHidden());
            for (int cycle = 0; cycle < 2; ++cycle) {
                dock->setFloating(true);
                QVERIFY(QTest::qWaitForWindowExposed(dock));
                QVERIFY(dock->isFloating());
                QVERIFY(grip->isVisible());
                const QSize small = dock->minimumSizeHint().expandedTo(QSize(420, 240));
                dock->resize(small);
                const QSize original = dock->size();
                dock->resize(original + QSize(150, 100));
                QCOMPARE(dock->size(), original + QSize(150, 100));
                dock->resize(original);
                QCOMPARE(dock->size(), original);
                if (dock->objectName() == QStringLiteral("org.swak.events.pane")) {
                    const QPoint handle = grip->rect().center();
                    QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, handle);
                    QTest::mouseMove(grip, handle + QPoint(80, 60));
                    QTest::mouseRelease(grip, Qt::LeftButton);
                    QVERIFY(dock->width() > original.width());
                    QVERIFY(dock->height() > original.height());
                    QVERIFY(dock->grab().save(QStringLiteral("events-floating.png")));
                }
                dock->close();
                QVERIFY(dock->isHidden());
                dock->toggleViewAction()->trigger();
                QVERIFY(dock->isVisible());
                dock->setFloating(false);
                QVERIFY(!dock->isFloating());
                QVERIFY(grip->isHidden());
            }
        }
        quit->trigger();
        QVERIFY(!window.isVisible());
    }
    void workspace_data()
    {
        QTest::addColumn<bool>("light");
        QTest::newRow("desktop-theme") << false;
        QTest::newRow("light-theme") << true;
    }
    void workspace()
    {
        QFETCH(bool, light);
        if (light) {
            QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
            QPalette palette(QColor("#efefef"));
            palette.setColor(QPalette::WindowText, Qt::black);
            palette.setColor(QPalette::Text, Qt::black);
            palette.setColor(QPalette::Base, Qt::white);
            palette.setColor(QPalette::ButtonText, Qt::black);
            palette.setColor(QPalette::Highlight, QColor("#2266aa"));
            palette.setColor(QPalette::HighlightedText, Qt::white);
            QApplication::setPalette(palette);
        }
        QTemporaryDir preferences;
        swak::Window window(preferences.filePath(QStringLiteral("preferences.json")), QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(QStringLiteral("map"));
        QVERIFY(map);
        QVERIFY(map->isValid());
        map->makeCurrent();
        qInfo() << "OpenGL renderer:" << reinterpret_cast<const char *>(map->context()->functions()->glGetString(GL_RENDERER));
        map->doneCurrent();
        auto *generate = window.findChild<QPushButton *>(QStringLiteral("generateEvent"));
        auto *latitude = window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"));
        auto *longitude = window.findChild<QDoubleSpinBox *>(QStringLiteral("longitude"));
        auto *callsign = window.findChild<QLineEdit *>(QStringLiteral("callsign"));
        QVERIFY(generate && latitude && longitude && callsign);
        latitude->setValue(59.3293);
        longitude->setValue(18.0686);
        callsign->setText(QStringLiteral("Stockholm demo"));
        QTest::mouseClick(generate, Qt::LeftButton);
        QCOMPARE(window.store().rowCount(), 1);
        QVERIFY(!window.store().selected().isEmpty());
        auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("xmlPreview"));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("Stockholm demo")));
        QTest::mouseClick(generate, Qt::LeftButton);
        QCOMPARE(window.store().rowCount(), 1);
        auto updated = window.store().events().first();
        updated.callsign = QStringLiteral("Stockholm update");
        QVERIFY(window.store().accept(updated).isEmpty());
        QVERIFY(preview->toPlainText().contains(updated.callsign));
        map->setFocus();
        QTest::keyClick(map, Qt::Key_Plus);
        QTest::keyClick(map, Qt::Key_Right);
        QTest::keyClick(map, Qt::Key_Home);
        const QImage frame = map->grabFramebuffer();
        QVERIFY(!frame.isNull());
        QCOMPARE(frame.pixelColor(20, frame.height() / 2), QColor(swak::MapBackgroundRgb));
        QVERIFY(frame.save(QStringLiteral("map-smoke.png")));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(window.screen()->grabWindow(window.winId()).save(light ? QStringLiteral("gui-light.png") : QStringLiteral("gui-smoke.png")));
        window.resize(950, 700);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(map->width() >= 320);
        window.findChild<QAction *>(QStringLiteral("clearEventsAction"))->trigger();
        QCOMPARE(window.store().rowCount(), 0);
        window.close();
    }
};
int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QTemporaryDir mapCache;
    qputenv("XDG_CACHE_HOME", mapCache.path().toUtf8());
    GuiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_gui.moc"
