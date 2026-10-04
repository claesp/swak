#include <QUuid>
#include "core/map_label.h"
#include "core/measure.h"
#include <QProcess>
#include "core/projection.h"
#include <QVBoxLayout>
#include "core/cot_picker.h"
#include <QTreeWidget>
#include <QPointer>
#include <QCollator>
#include "core/servers.h"
#include "core/identity.h"
#include "core/cot.h"
#include <QTcpServer>
#include <QTcpSocket>
#include "core/ui_icons.h"
#include "core/activity.h"
#include "core/maps.h"
#include "core/tiles.h"
#include "app/window.h"
#include "app/map_manager.h"
#include "app/data_manager.h"
#include "core/maps.h"
#include "tile_server.h"
#include "vector_fixture.h"
#include <QContextMenuEvent>
#include <QPlainTextEdit>
#include <QClipboard>
#include "core/map_selection.h"
#include "core/map_tools.h"
#include <QPainter>
#include <QStandardPaths>
#include <QDesktopServices>
#include "swak/version.h"
#include "core/logging.h"
#include "core/settings_dialog.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QTableWidget>
#include <QRadioButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QStatusBar>
#include <QKeySequenceEdit>
#include <QCollator>
#include <QScopeGuard>
#include "core/interfaces.h"
#include "core/marker_collections.h"
#include "app/marker_manager.h"
#include "core/user_objects.h"
#include "core/object_editor.h"
#include "core/polygon.h"
#include <QApplication>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QAbstractItemModelTester>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLineEdit>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QStyle>
#include <QStyleFactory>
#include <QPlainTextEdit>
#include <QClipboard>
#include "core/map_selection.h"
#include "core/map_tools.h"
#include <QPainter>
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

static void chooseMapObject(QWidget *map, const QString &label)
{
    QTimer::singleShot(0, map, [label] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        const auto close = qScopeGuard([menu] { menu->close(); });
        QCOMPARE(menu->objectName(), QStringLiteral("mapObjectChooser"));
        for (auto *action : menu->actions()) if (action->text().contains(label)) { action->trigger(); return; }
        QFAIL("Object is absent from the chooser");
    });
}

static void withObjectContext(QWidget *map, const QString &label, std::function<void(QMenu *)> callback)
{
    QTimer::singleShot(0, map, [map, label, callback] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        const auto close = qScopeGuard([menu] { menu->close(); });
        if (menu->objectName() != QStringLiteral("mapObjectChooser")) { callback(menu); return; }
        for (auto *action : menu->actions()) if (!action->isSeparator() && (label.isEmpty() || action->text().contains(label))) {
            action->trigger(); withObjectContext(map, label, callback); return;
        }
        QFAIL("Object is absent from the chooser");
    });
}

class SelectionCanvas final : public QOpenGLWidget {
public:
    swak::Viewport view;
    swak::MapSelection selection;
    swak::DataSources *data = nullptr;
protected:
    void paintGL() override {
        QPainter painter(this); painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::Antialiasing);
        if (data) data->paint(painter, view); else selection.paint(painter, view);
    }
};

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
        auto gridValues = settings->savePreferences(); QCOMPARE(gridValues.value(QStringLiteral("gridOpacity")).toDouble(), 0.5);
        gridValues.remove(QStringLiteral("gridStyleVersion")); gridValues.insert(QStringLiteral("gridOpacity"), 0.2);
        settings->restorePreferences(gridValues); QCOMPARE(settings->savePreferences().value(QStringLiteral("gridOpacity")).toDouble(), 0.5);
        gridValues.insert(QStringLiteral("gridStyleVersion"), 2); settings->restorePreferences(gridValues);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("gridOpacity")).toDouble(), 0.2);
        gridValues.insert(QStringLiteral("gridOpacity"), 0.5); settings->restorePreferences(gridValues);
        const auto initial = map->grabFramebuffer();
        QCOMPARE(initial.pixelColor(20, 21), QColor(swak::MapBackgroundRgb));
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
        QCOMPARE(zoom(), std::max(2.0, std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0))));
        for (int i = 0; i < 5; ++i) QTest::keyClick(map, Qt::Key_Minus);
        QCOMPARE(zoom(), std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0)));
        QTest::keyClick(map, Qt::Key_Plus);
        QCOMPARE(zoom(), std::max(2.0, std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0))));
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
        for (auto *action : root->actions()) if (action->menu() && action->text() == QStringLiteral("CoT Messages")) events = action->menu();
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
            auto *status = window.findChild<QLabel *>(QStringLiteral("internalEventCount"));
            auto *table = window.findChild<QTableView *>(QStringLiteral("activityTable"));
            QVERIFY(history && status && table);
            QVERIFY(window.findChild<QLabel *>(QStringLiteral("cotEventCount"))->text().startsWith(QStringLiteral("CoT Messages:")));
            QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("statusSeparator")).size(), 3);
            QVERIFY(!window.findChild<QLabel *>(QStringLiteral("latestActivity")));
            history->clear();
            history->restorePreferences({{QStringLiteral("capacity"), 2}});
            swak::logMessage(swak::Severity::Error, swak::Urgency::Immediate, QStringLiteral("swak.test"), QStringLiteral("Example failure"));
            QVERIFY(QTest::qWaitFor([&] { return history->rowCount() > 0; }));
            QCOMPARE(status->text(), QStringLiteral("Events: %1").arg(history->rowCount()));
            QVERIFY(!table->isVisible());
            swak::logMessage(swak::Severity::Info, swak::Urgency::Routine, QStringLiteral("swak.test"), QStringLiteral("Recovered"));
            QVERIFY(QTest::qWaitFor([&] { return history->rowCount() == 2; }));
            QCOMPARE(status->text(), QStringLiteral("Events: 2"));
            QVERIFY(!table->isVisible());
            auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
            QMenu *historyMenu = nullptr;
            for (auto *action : plugins->actions()) if (action->text() == QStringLiteral("Events")) historyMenu = action->menu();
            QVERIFY(historyMenu);
            historyMenu->menuAction()->trigger();
            QVERIFY(table->isVisible());
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTest::qWait(50);
            QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("internal-events-smoke.png")));
            historyMenu->findChild<QAction *>(QStringLiteral("clearActivityAction"))->trigger();
            QCOMPARE(history->rowCount(), 0);
            QCOMPARE(status->text(), QStringLiteral("Events: 0"));
            table->horizontalHeader()->resizeSection(0, 250);
            auto *button = window.findChild<QToolButton *>(QStringLiteral("mapProviderButton"));
            QVERIFY(button);
            auto *maps = window.findChild<swak::MapSources *>();
            auto *menu = button->menu();
            QMetaObject::invokeMethod(menu, "aboutToShow");
            QCOMPARE(menu->actions().size(), maps->sources().size() + 3);
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

    void vectorMapAndInformation()
    {
        QTemporaryDir directory;
        TileServer server; server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const auto path = directory.filePath(QStringLiteral("vector.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *sources = window.findChild<swak::MapSources *>();
            const auto id = QStringLiteral("org.swak.mvt");
            auto source = sources->sources().value(id).toObject();
            source.insert(QStringLiteral("endpoint"), QStringLiteral("https://localhost:%1/{z}/{x}/{y}.pbf").arg(server.serverPort()));
            source.insert(QStringLiteral("ignoreTls"), true);
            QVERIFY(sources->put(id, source).isEmpty()); sources->select(id);
            auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
            auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map); QVERIFY(preferences);
            QCOMPARE(preferences->savePreferences().value(QStringLiteral("tileCacheMiB")).toInt(), 256);
            preferences->restorePreferences(QJsonObject{{QStringLiteral("zoom"), 0}, {QStringLiteral("tileCacheMiB"), 128}});
            window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(QTest::qWaitFor([&] {
                const auto frame = map->grabFramebuffer();
                return !frame.isNull() && frame.pixelColor(frame.width()/2 - 70, frame.height()/2 - 70) == QColor("#92bfd0");
            }, 5000));
            QVERIFY(map->grabFramebuffer().save(QStringLiteral("vector-map-smoke.png")));
            withObjectContext(map, QStringLiteral("Test place"), [](QMenu *menu) {
                auto *action = menu->findChild<QAction *>(QStringLiteral("mapInformation")); QVERIFY(action);
                const bool enabled = action->isEnabled(); action->trigger(); menu->close(); QVERIFY(enabled);
            });
            const QPoint fixturePoint = map->rect().center() + QPoint(-128, -128);
            QContextMenuEvent information(QContextMenuEvent::Mouse, fixturePoint, map->mapToGlobal(fixturePoint));
            QApplication::sendEvent(map, &information);
            auto *dialog = map->findChild<QDialog *>(QStringLiteral("mapInformationDialog")); QVERIFY(dialog);
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            const auto details = dialog->findChild<QPlainTextEdit *>(QStringLiteral("mapFeatureProperties"))->toPlainText();
            QVERIFY(details.contains(QStringLiteral("Test place"))); QVERIFY(details.contains(QStringLiteral("cafe")));
            QVERIFY(dialog->grab().save(QStringLiteral("vector-information-smoke.png")));
            dialog->close();
            withObjectContext(map, {}, [](QMenu *menu) {
                bool changed = false;
                for (auto *action : menu->actions()) if (auto *layers = action->menu())
                    for (auto *layer : layers->actions()) if (layer->text() == QStringLiteral("poi")) {
                        QVERIFY(layer->isChecked()); layer->trigger(); changed = true;
                    }
                QVERIFY(changed); menu->close();
            });
            QContextMenuEvent layers(QContextMenuEvent::Keyboard, map->rect().center(), map->mapToGlobal(map->rect().center()));
            QApplication::sendEvent(map, &layers);
            QVERIFY(!sources->activeSource().value(QStringLiteral("visibleLayers")).toObject().value(QStringLiteral("poi")).toBool(true));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *restoredMap = dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>());
        QVERIFY(restoredMap);
        QCOMPARE(restoredMap->savePreferences().value(QStringLiteral("tileCacheMiB")).toInt(), 128);
        const auto source = restored.findChild<swak::MapSources *>()->activeSource();
        QVERIFY(!source.value(QStringLiteral("visibleLayers")).toObject().value(QStringLiteral("poi")).toBool(true));
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
        QCOMPARE(sources->providers().size(), 4);
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
        }, 15000));
        auto *credit = map->findChild<QLabel *>(QStringLiteral("mapAttribution"));
        QVERIFY(credit);
        auto *cache = window.statusBar()->findChild<QLabel *>(QStringLiteral("mapStatus"));
        QVERIFY(cache);
        QVERIFY(QTest::qWaitFor([&] { return !cache->text().contains(QStringLiteral("inactive")) && !cache->text().contains(QStringLiteral("0 bytes")); }));
        QVERIFY(cache->text().startsWith(QStringLiteral("Map cache:")));
        QVERIFY(!map->findChild<QLabel *>(QStringLiteral("mapCacheSize")));
        QCOMPARE(credit->geometry().bottom(), map->height() - 9);
        auto *zoom = window.statusBar()->findChild<QLabel *>(QStringLiteral("mapZoomStatus"));
        auto *pointer = window.statusBar()->findChild<QLabel *>(QStringLiteral("mapPointerStatus"));
        auto *tls = window.statusBar()->findChild<QLabel *>(QStringLiteral("mapTlsStatus"));
        auto *tileStatus = window.statusBar()->findChild<QLabel *>(QStringLiteral("mapTileStatus"));
        QVERIFY(zoom && pointer && tls && tileStatus);
        QVERIFY(zoom->text().startsWith(QStringLiteral("Zoom ")));
        QVERIFY(!pointer->text().isEmpty());
        QVERIFY(tls->isVisible());
        QVERIFY(tls->text().contains(QStringLiteral("HTTPS")));
        for (auto *label : {cache, zoom, pointer, tls, tileStatus}) QVERIFY(!label->text().contains('|'));
        QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("mapStatusSeparator")).size(), 5);
        source.insert(QStringLiteral("ignoreTls"), false);
        QVERIFY(sources->put(id, source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return tls->parentWidget()->isHidden(); }));
        source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(sources->put(id, source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return tls->isVisible(); }));
        QVERIFY(QTest::qWaitFor([&] { return cache->toolTip() == cache->text(); }));
        QVERIFY(cache->width() > 0);
        QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("map-status-bar.png")));
        QVERIFY(!credit->text().contains(QStringLiteral("HTTPS certificate")));
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
        auto *list = dialog->findChild<QTableWidget *>(QStringLiteral("mapProviderList"));
        const auto originalCount = sources->sources().size();
        QCOMPARE(list->rowCount(), originalCount);
        QVERIFY(dialog->grab().save(QStringLiteral("map-providers-smoke.png")));
        dialog->findChild<QRadioButton *>(QStringLiteral("activeProvider/") + id)->click();
        QVERIFY(QTest::qWaitFor([&] { return sources->activeId() == id; }));
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
        QCOMPARE(list->rowCount(), originalCount);
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
        QCOMPARE(list->rowCount(), originalCount + 1);
        dialog->findChild<QPushButton *>(QStringLiteral("removeMapProvider"))->click();
        QCOMPARE(list->rowCount(), originalCount);
        for (int i = 0; i < list->rowCount(); ++i) if (list->item(i, 1)->data(Qt::UserRole).toString() == id) list->setCurrentCell(i, 1);
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
        auto *missingList = missing.findChild<QTableWidget *>(QStringLiteral("mapProviderList"));
        QVERIFY(missingList->item(0, 2)->text().contains(QStringLiteral("unavailable")));
        QVERIFY(!missing.findChild<QRadioButton *>()->isEnabled());
        QVERIFY(absent.activeSource().isEmpty());
        missing.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
    }
    void mapInformationPane()
    {
        QTemporaryDir directory;
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark><name>Station</name><description>Read only</description><ExtendedData><Data name=\"status\"><value>Ready</value></Data></ExtendedData><Point><coordinates>0,0</coordinates></Point></Placemark></kml>";
        swak::Window window(directory.filePath(QStringLiteral("info.json")), QPluginLoader::staticInstances()); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); auto *feeds = window.findChild<swak::DataSources *>();
        auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(map && feeds && selection);
        auto *pane = window.findChild<QWidget *>(QStringLiteral("informationPane")); QVERIFY(pane);
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane")); QVERIFY(dock); QVERIFY(!dock->isVisible());
        auto source = swak::dataDefaults(); source.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
        source.insert(QStringLiteral("name"), QStringLiteral("Fixture")); source.insert(QStringLiteral("attribution"), QStringLiteral("Fixture"));
        source.insert(QStringLiteral("endpoint"), QStringLiteral("https://localhost:%1/feed.kml").arg(server.serverPort()));
        source.insert(QStringLiteral("enabled"), true); source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(feeds->put(QStringLiteral("test"), source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return feeds->shapeCount(QStringLiteral("test")) == 1; }, 5000));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(selection->item()); QVERIFY(dock->isVisible());
        auto *title = pane->findChild<QLabel *>(QStringLiteral("informationTitle")); QCOMPARE(title->text(), QStringLiteral("Station"));
        auto *description = pane->findChild<QPlainTextEdit *>(); QVERIFY(description->isReadOnly()); QCOMPARE(description->toPlainText(), QStringLiteral("Read only"));
        auto *table = pane->findChild<QTableWidget *>(); QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
        bool copied = false;
        for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 0)->text() == QStringLiteral("status")) {
            table->scrollToItem(table->item(row, 1));
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->item(row, 1)).center());
            QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->item(row, 1)).center());
            QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Ready")); copied = true;
        }
        QVERIFY(copied);
        QTest::mouseDClick(title, Qt::LeftButton); QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Station"));
        QTest::mouseDClick(description->viewport(), Qt::LeftButton); QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Read only"));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped); map->update(); QVERIFY(frames.wait(2000));
        const auto selectedImage = map->grabFramebuffer();
        const QPoint imageCenter(selectedImage.width() / 2, selectedImage.height() / 2);
        const int outline = qRound(7 * map->devicePixelRatioF());
        QVERIFY(selectedImage.pixelColor(imageCenter + QPoint(outline, 0)).lightness() > 220);
        QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("information-pane.png")));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(40, 40));
        QVERIFY(!selection->item()); QVERIFY(!dock->isVisible());
        window.findChild<QPushButton *>(QStringLiteral("generateEvent"))->click();
        chooseMapObject(map, QStringLiteral("SWAK"));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(selection->item()); QCOMPARE(selection->item()->label, QStringLiteral("SWAK"));
        QVERIFY(selection->item()->properties.contains(QStringLiteral("UID")));
        QTest::keyClick(map, Qt::Key_Escape);
        QVERIFY(!selection->item()); QVERIFY(window.store().selected().isEmpty()); QVERIFY(!dock->isVisible());
        chooseMapObject(map, QStringLiteral("SWAK"));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center()); QVERIFY(selection->item());
        window.store().clear(); QVERIFY(!selection->item());
        withObjectContext(map, QStringLiteral("Station"), [](QMenu *menu) {
            auto *action = menu->findChild<QAction *>(QStringLiteral("mapItemInformation")); QVERIFY(action); action->trigger();
        });
        QContextMenuEvent context(QContextMenuEvent::Mouse, map->rect().center(), map->mapToGlobal(map->rect().center()));
        QApplication::sendEvent(map, &context); QVERIFY(selection->item()); QCOMPARE(selection->item()->label, QStringLiteral("Station"));
        auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map); QVERIFY(preferences);
        preferences->restorePreferences({{QStringLiteral("zoom"), 0}, {QStringLiteral("centerX"), 0}, {QStringLiteral("centerY"), 1}});
        const auto values = preferences->savePreferences(); const double scale = 256 * std::exp2(values.value(QStringLiteral("zoom")).toDouble());
        QVERIFY(scale >= map->width()); QVERIFY(scale >= map->height());
        QVERIFY(values.value(QStringLiteral("centerX")).toDouble() * scale >= map->width() / 2.0 - 0.01);
        QVERIFY((1 - values.value(QStringLiteral("centerY")).toDouble()) * scale >= map->height() / 2.0 - 0.01);
        window.close();
    }
    void dataProviders()
    {
        QTemporaryDir directory;
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark><name>Feed geometry</name><MultiGeometry>"
            "<Point><coordinates>0,0</coordinates></Point><LineString><coordinates>-25,15 25,15</coordinates></LineString>"
            "<Polygon><outerBoundaryIs><LinearRing><coordinates>-30,-25 30,-25 30,-5 -30,-5 -30,-25</coordinates></LinearRing></outerBoundaryIs></Polygon>"
            "</MultiGeometry></Placemark></kml>";
        const auto profile = directory.filePath(QStringLiteral("data.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *feeds = window.findChild<swak::DataSources *>(); QVERIFY(feeds); QCOMPARE(feeds->sources().size(), 2);
            window.findChild<QAction *>(QStringLiteral("dataProvidersAction"))->trigger();
            auto *manager = window.findChild<swak::DataManager *>(); QVERIFY(manager);
            auto *list = manager->findChild<QTableWidget *>(QStringLiteral("dataProviderList")); QVERIFY(list);
            QCOMPARE(list->rowCount(), 2);
            QTimer::singleShot(0, manager, [manager, &server] {
                auto *dialog = manager->findChild<swak::SettingsDialog *>(); QVERIFY(dialog);
                const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                QVERIFY(!dialog->findChild<QTabWidget *>());
                for (auto *field : dialog->findChildren<QLineEdit *>()) {
                    if (field->objectName().endsWith(QStringLiteral("/name"))) { QVERIFY(field->text().isEmpty()); field->setText(QStringLiteral("A test feed")); }
                    if (field->objectName().endsWith(QStringLiteral("/endpoint"))) { QVERIFY(field->text().isEmpty()); field->setText(QStringLiteral("https://127.0.0.1:%1/feed.kml").arg(server.serverPort())); }
                    if (field->objectName().endsWith(QStringLiteral("/attribution"))) field->setText(QStringLiteral("Test data"));
                }
                for (auto *field : dialog->findChildren<QCheckBox *>()) if (field->objectName().endsWith(QStringLiteral("/ignoreTls"))) field->setChecked(true);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            manager->findChild<QPushButton *>(QStringLiteral("addDataProvider"))->click();
            QCOMPARE(list->rowCount(), 3); QCOMPARE(list->item(0, 1)->text(), QStringLiteral("A test feed"));
            const auto id = list->item(0, 1)->data(Qt::UserRole).toString(); manager->findChild<QCheckBox *>(QStringLiteral("activeProvider/") + id)->click();
            QVERIFY(QTest::qWaitFor([&] { return feeds->shapeCount(id) == 3; }, 5000));
            QVERIFY(QTest::qWaitForWindowExposed(manager)); QVERIFY(manager->grab().save(QStringLiteral("data-providers-smoke.png")));
            manager->hide(); auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map); QVERIFY(map->isValid());
            const auto image = map->grabFramebuffer(); const auto center = image.pixelColor(image.width() / 2, image.height() / 2);
            QVERIFY(center.red() > 120); QVERIFY(image.save(QStringLiteral("data-layer-smoke.png")));
            QVERIFY(map->grab().save(QStringLiteral("data-attribution-smoke.png")));
            manager->show();
            QTimer::singleShot(0, manager, [manager] {
                auto *dialog = manager->findChild<swak::SettingsDialog *>(); QVERIFY(dialog);
                for (auto *field : dialog->findChildren<QDoubleSpinBox *>()) if (field->objectName().endsWith(QStringLiteral("/interval"))) field->setValue(600);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            manager->findChild<QPushButton *>(QStringLiteral("editDataProvider"))->click();
            QCOMPARE(feeds->sources().value(id).toObject().value(QStringLiteral("interval")).toInt(), 600);
            auto values = feeds->sources().value(id).toObject(); values.insert(QStringLiteral("enabled"), false); QVERIFY(feeds->put(id, values).isEmpty());
            QVERIFY(window.savePreferences()); manager->close();
        }
        swak::Window restored(profile, QPluginLoader::staticInstances()); auto *feeds = restored.findChild<swak::DataSources *>();
        QCOMPARE(feeds->sources().size(), 3);
        restored.findChild<QAction *>(QStringLiteral("dataProvidersAction"))->trigger();
        auto *manager = restored.findChild<swak::DataManager *>();
        manager->findChild<QTableWidget *>(QStringLiteral("dataProviderList"))->setCurrentCell(0, 1);
        manager->findChild<QPushButton *>(QStringLiteral("removeDataProvider"))->click(); QCOMPARE(feeds->sources().size(), 2);
        manager->close();
    }
    void legacyOverlayLayout()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("legacy.json"));
        {
            QMainWindow legacy;
            legacy.resize(1200, 800);
            auto *editor = new QDockWidget(&legacy);
            editor->setObjectName(QStringLiteral("org.swak.editor.pane"));
            auto *udp = new QDockWidget(&legacy);
            udp->setObjectName(QStringLiteral("org.swak.udp.pane"));
            legacy.addDockWidget(Qt::LeftDockWidgetArea, editor);
            legacy.addDockWidget(Qt::LeftDockWidgetArea, udp);
            legacy.tabifyDockWidget(editor, udp);
            auto *toolbar = legacy.addToolBar(QStringLiteral("Main"));
            toolbar->setObjectName(QStringLiteral("mainToolbar")); toolbar->hide();
            swak::Preferences preferences(path);
            QVERIFY(preferences.save({{QStringLiteral("window"), QJsonObject{
                {QStringLiteral("layout"), QString::fromLatin1(legacy.saveState(1).toBase64())}}}}));
        }
        swak::Window window(path, QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *editor = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
        auto *udp = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
        QCOMPARE(window.dockWidgetArea(editor), Qt::LeftDockWidgetArea);
        QVERIFY(window.tabifiedDockWidgets(editor).contains(udp));
        QVERIFY(window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))->isHidden());
        auto *map = window.findChild<QOpenGLWidget *>();
        const auto bounds = map->geometry();
        editor->hide(); udp->hide();
        QCoreApplication::processEvents();
        QCOMPARE(map->geometry(), bounds);
        QCOMPARE(map->width(), window.contentsRect().width());
        QVERIFY(window.savePreferences());
    }
    void mapOverlayPanels()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("overlay.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *panels = window.findChild<QMainWindow *>(QStringLiteral("mapPanels")); QVERIFY(map && panels); QVERIFY(!panels->isWindow());
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map); QVERIFY(settings);
        const QRect bounds = map->geometry(); const auto view = settings->savePreferences();
        for (auto *pane : window.findChildren<QDockWidget *>()) {
            pane->show(); pane->raise();
            QVERIFY(QTest::qWaitFor([&] { return panels->mask().contains(pane->mapTo(panels, pane->rect().center())); }));
            QCOMPARE(map->geometry(), bounds);
            QCOMPARE(settings->savePreferences().value(QStringLiteral("zoom")), view.value(QStringLiteral("zoom")));
            QCOMPARE(settings->savePreferences().value(QStringLiteral("centerX")), view.value(QStringLiteral("centerX")));
            QCOMPARE(settings->savePreferences().value(QStringLiteral("centerY")), view.value(QStringLiteral("centerY")));
            pane->hide();
        }
        QVERIFY(QTest::qWaitFor([&] { return !panels->mask().contains(panels->rect().center()); }));
        QCOMPARE(QApplication::widgetAt(map->mapToGlobal(map->rect().center())), map);
        auto *info = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        info->show(); info->raise();
        QVERIFY(QTest::qWaitFor([&] { return panels->mask().contains(info->mapTo(panels, info->rect().center())); }));
        QCOMPARE(map->geometry(), bounds);
        QVERIFY(QTest::qWaitFor([&] { return !panels->mask().contains(QPoint(100, panels->height() - 20)); }));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped); map->update(); QVERIFY(frames.wait(2000));
        QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("map-overlays.png")));
    }
    void markerCollectionArchiveDialogs()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        struct RestoreDialogs { bool value; ~RestoreDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, value); } } restore{native};
        swak::MarkerCollections collections(QPluginLoader::staticInstances()); collections.setDirectory(directory.path());
        auto value = swak::coreMarkerCollection(); const auto id = QStringLiteral("archive.ui"); value.insert(QStringLiteral("id"), id);
        value.insert(QStringLiteral("name"), QStringLiteral("Custom")); value.insert(QStringLiteral("description"), QStringLiteral("<img src='file:///private' />"));
        QVERIFY(collections.put(value).isEmpty());
        int saves = 0;
        swak::MarkerManager manager(collections, nullptr, [&] { ++saves; return QString{}; }); manager.show(); QVERIFY(QTest::qWaitForWindowExposed(&manager));
        auto *table = manager.findChild<QTableWidget *>(QStringLiteral("markerCollectionList"));
        for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 1)->data(Qt::UserRole).toString() == id) {
            table->setCurrentCell(row, 1); QVERIFY(table->item(row, 1)->toolTip().contains(QStringLiteral("&lt;img")));
        }
        const auto chooseFile = [&](const QString &button, const QString &path) {
            bool found = false;
            QTimer::singleShot(0, &manager, [&] {
                auto *dialog = manager.findChild<QFileDialog *>(); QVERIFY(dialog); found = true;
                if (path.isEmpty()) dialog->reject();
                else { dialog->selectFile(path); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection); }
            });
            manager.findChild<QPushButton *>(button)->click(); QVERIFY(found);
        };
        const auto file = directory.filePath(QStringLiteral("symbols.zip"));
        chooseFile(QStringLiteral("exportMarkerCollection"), file.chopped(4)); QVERIFY(QFileInfo::exists(file)); QCOMPARE(saves, 1);
        chooseFile(QStringLiteral("exportMarkerCollection"), {}); QCOMPARE(saves, 1);
        manager.findChild<QPushButton *>(QStringLiteral("deleteMarkerCollection"))->click(); QVERIFY(!collections.collections().contains(id));
        chooseFile(QStringLiteral("importMarkerCollection"), file); QVERIFY(collections.collections().contains(id));
        const auto before = collections.collections();
        chooseFile(QStringLiteral("importMarkerCollection"), file); QCOMPARE(collections.collections(), before);
        QVERIFY(!manager.findChild<QLabel *>(QStringLiteral("markerCollectionStatus"))->text().isEmpty());
        chooseFile(QStringLiteral("importMarkerCollection"), {}); QCOMPARE(collections.collections(), before);
        QCOMPARE(collections.directory(), directory.path());
        for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 1)->data(Qt::UserRole).toString() == swak::CoreMarkerCollection) table->setCurrentCell(row, 1);
        manager.findChild<QPushButton *>(QStringLiteral("editMarkerCollection"))->click();
        QVERIFY(manager.findChild<QLabel *>(QStringLiteral("markerCollectionStatus"))->text().contains(QStringLiteral("read-only")));
        manager.findChild<QPushButton *>(QStringLiteral("deleteMarkerCollection"))->click(); QVERIFY(collections.collections().contains(swak::CoreMarkerCollection));
    }
    void markerCollectionWorkflow()
    {
        QTemporaryDir directory; const auto profile = directory.filePath(QStringLiteral("collections.json"));
        QString collectionId, markerId;
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *collections = window.findChild<swak::MarkerCollections *>(); QVERIFY(collections);
            window.findChild<QAction *>(QStringLiteral("markerCollectionsAction"))->trigger();
            auto *manager = window.findChild<swak::MarkerManager *>(); QVERIFY(manager);
            auto *table = manager->findChild<QTableWidget *>(QStringLiteral("markerCollectionList")); QVERIFY(table); QCOMPARE(table->rowCount(), 2);
            bool edited = false;
            QTimer::singleShot(0, manager, [&] {
                auto *dialog = manager->findChild<QDialog *>(QStringLiteral("editMarkerCollectionDialog")); QVERIFY(dialog);
                dialog->findChild<QLineEdit *>(QStringLiteral("collectionName"))->setText(QStringLiteral("Custom units"));
                auto *source = dialog->findChild<QComboBox *>(QStringLiteral("collectionTemplate")); source->setCurrentIndex(source->findData(swak::MilitaryMarkerCollection));
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click(); edited = true;
            });
            manager->findChild<QPushButton *>(QStringLiteral("addMarkerCollection"))->click(); QVERIFY(edited); QCOMPARE(table->rowCount(), 3);
            const auto entries = collections->collections();
            for (auto it = entries.begin(); it != entries.end(); ++it) if (!collections->builtIn(it.key())) collectionId = it.key();
            QVERIFY(!collectionId.isEmpty());
            QTimer::singleShot(0, manager, [&] {
                auto *dialog = manager->findChild<QDialog *>(QStringLiteral("editMarkerCollectionDialog")); QVERIFY(dialog);
                dialog->findChild<QLineEdit *>(QStringLiteral("collectionDescription"))->setText(QStringLiteral("Portable symbols"));
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            manager->findChild<QPushButton *>(QStringLiteral("editMarkerCollection"))->click();
            QCOMPARE(collections->collections().value(collectionId).toObject().value(QStringLiteral("description")).toString(), QStringLiteral("Portable symbols"));
            auto *toggle = manager->findChild<QAbstractButton *>(QStringLiteral("activeProvider/") + collectionId); QVERIFY(toggle); toggle->click();
            QVERIFY(QTest::qWaitFor([&] { return !collections->collections().value(collectionId).toObject().value(QStringLiteral("enabled")).toBool(); }, 5000));
            toggle = manager->findChild<QAbstractButton *>(QStringLiteral("activeProvider/") + collectionId); toggle->click();
            QVERIFY(QTest::qWaitFor([&] { return collections->collections().value(collectionId).toObject().value(QStringLiteral("enabled")).toBool(); }, 5000));
            QTimer::singleShot(0, manager, [&] {
                auto *dialog = manager->findChild<QDialog *>(QStringLiteral("editMarkerCollectionDialog")); QVERIFY(dialog); dialog->reject();
            });
            manager->findChild<QPushButton *>(QStringLiteral("addMarkerCollection"))->click(); QCOMPARE(table->rowCount(), 3);
            QVERIFY(collections->exportArchive(collectionId, directory.filePath(QStringLiteral("custom.zip"))).isEmpty());
            QVERIFY(manager->grab().save(QStringLiteral("marker-collections.png")));
            manager->close();
            auto *map = window.findChild<QOpenGLWidget *>(); auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            auto *objects = window.findChild<swak::UserObjects *>(); auto *selection = window.findChild<swak::MapSelection *>();
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            auto *collection = editor->findChild<QComboBox *>(QStringLiteral("objectCollection")); auto *type = editor->findChild<QComboBox *>(QStringLiteral("objectType"));
            QCOMPARE(collection->currentData().toString(), swak::CoreMarkerCollection); QCOMPARE(type->count(), 1);
            collection->setCurrentIndex(collection->findData(collectionId)); QCOMPARE(type->count(), 16);
            type->setCurrentIndex(type->findData(QStringLiteral("a-h-G-U-C-I")));
            QVERIFY(editor->grab().save(QStringLiteral("marker-collection-editor.png")));
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
            markerId = objects->items().firstKey(); QCOMPARE(objects->find(markerId)->collection, collectionId);
            QSignalSpy frames(map, &QOpenGLWidget::frameSwapped); map->update(); QVERIFY(frames.wait(2000));
            QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("marker-collection-map.png")));
            const auto outline = selection->item()->markerOutline; QVERIFY(!outline.isEmpty());
            QVERIFY(collections->setEnabled(collectionId, false).isEmpty()); QCOMPARE(selection->item()->markerOutline, outline);
            editor->edit(*objects->find(markerId)); QCOMPARE(collection->currentData().toString(), collectionId); editor->cancel();
            QVERIFY(collections->remove(collectionId).isEmpty()); QCOMPARE(selection->item()->type, QStringLiteral("Unavailable marker"));
            editor->edit(*objects->find(markerId)); QCOMPARE(collection->currentData().toString(), collectionId); QCOMPARE(type->currentData().toString(), QStringLiteral("a-h-G-U-C-I"));
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click(); QCOMPARE(objects->find(markerId)->collection, collectionId);
            QVERIFY(collections->importArchive(directory.filePath(QStringLiteral("custom.zip"))).isEmpty()); QCOMPARE(selection->item()->markerOutline, outline);
            objects->remove(markerId); objects->undoStack().undo(); QCOMPARE(objects->find(markerId)->collection, collectionId);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances()); restored.show(); QVERIFY(QTest::qWaitForWindowExposed(&restored));
        auto *collections = restored.findChild<swak::MarkerCollections *>(); QVERIFY(collections->collections().contains(collectionId));
        auto *objects = restored.findChild<swak::UserObjects *>(); QCOMPARE(objects->find(markerId)->collection, collectionId);
        auto *editor = restored.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")); QCOMPARE(editor->newCollection(), collectionId); QCOMPARE(editor->newSymbol(), QStringLiteral("a-h-G-U-C-I"));
        for (const auto &id : collections->collections().keys()) QVERIFY(collections->setEnabled(id, false).isEmpty());
        QVERIFY(editor->newCollection().isEmpty());
        auto *map = restored.findChild<QOpenGLWidget *>(); map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(!editor->draft()); QCOMPARE(objects->items().size(), 1);
        QVERIFY(restored.findChild<QLabel *>(QStringLiteral("mapToolStatus"))->text().isEmpty()); QTest::keyClick(map, Qt::Key_Escape);
    }
    void militaryMarkerWorkflow()
    {
        QTemporaryDir directory; const auto profile = directory.filePath(QStringLiteral("symbols.json")); QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>(); auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            auto *objects = window.findChild<swak::UserObjects *>(); auto *selection = window.findChild<swak::MapSelection *>();
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            auto *type = editor->findChild<QComboBox *>(QStringLiteral("objectType"));
            auto *collection = editor->findChild<QComboBox *>(QStringLiteral("objectCollection")); QVERIFY(collection);
            collection->setCurrentIndex(collection->findData(swak::MilitaryMarkerCollection));
            for (const auto &identity : {QStringLiteral("f"), QStringLiteral("h"), QStringLiteral("n"), QStringLiteral("u")}) {
                const auto symbol = QStringLiteral("a-%1-G-U").arg(identity);
                type->setCurrentIndex(type->findData(symbol));
                auto *group = editor->findChild<QButtonGroup *>(); QVERIFY(group);
                auto *color = group->checkedButton(); QVERIFY(color);
                auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject")); apply->click();
                const auto record = objects->items().first();
                QCOMPARE(record.color, color->property("markerColor").toString());
                editor->edit(record); QVERIFY(color->isChecked());
            }
            const auto code = QStringLiteral("a-h-G-U-C-I"); type->setCurrentIndex(type->findData(code));
            QVERIFY(editor->grab().save(QStringLiteral("military-marker-editor.png")));
            auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject")); apply->click();
            id = objects->items().firstKey(); QCOMPARE(objects->find(id)->symbol, code);
            QCOMPARE(objects->find(id)->color, QStringLiteral("#ff8080")); QCOMPARE(selection->item()->symbol, code);
            QVERIFY(selection->item()->type.contains(QStringLiteral("infantry")));
            objects->remove(id); objects->undoStack().undo(); QCOMPARE(objects->find(id)->symbol, code);
            editor->edit(*objects->find(id)); QCOMPARE(type->currentData().toString(), code);
            type->setCurrentIndex(0); editor->cancel(); QCOMPARE(objects->find(id)->symbol, code);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<swak::UserObjects *>()->find(id)->symbol, QStringLiteral("a-h-G-U-C-I"));
        restored.show(); QVERIFY(QTest::qWaitForWindowExposed(&restored));
        auto *map = restored.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map); QVERIFY(settings);
        const auto values = settings->savePreferences();
        const auto position = swak::unproject(QPointF(values.value(QStringLiteral("centerX")).toDouble(), values.value(QStringLiteral("centerY")).toDouble()));
        swak::Event event; event.uid = QStringLiteral("received-symbol"); event.type = QStringLiteral("a-n-G-U-C-A"); event.how = QStringLiteral("h-e");
        event.time = QDateTime::currentDateTimeUtc(); event.start = event.time; event.stale = event.time.addSecs(60);
        event.latitude = position.y(); event.longitude = position.x();
        QVERIFY(restored.store().accept(event).isEmpty());
        restored.findChild<swak::UserObjects *>()->remove(id);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        auto *selection = restored.findChild<swak::MapSelection *>(); QVERIFY(selection->item());
        QCOMPARE(selection->item()->symbol, event.type);
    }
    void markerAppearance()
    {
        QTemporaryDir directory; const auto profile = directory.filePath(QStringLiteral("colors.json")); QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>(); auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            auto *objects = window.findChild<swak::UserObjects *>(); auto *selection = window.findChild<swak::MapSelection *>();
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            auto *type = editor->findChild<QComboBox *>(QStringLiteral("objectType")); QVERIFY(type);
            QCOMPARE(type->count(), 1); QCOMPARE(type->currentText(), QStringLiteral("Generic marker"));
            QVERIFY(!type->itemIcon(0).isNull());
            auto *orange = editor->findChild<QToolButton *>(QStringLiteral("objectColor1")); QVERIFY(orange && orange->isChecked());
            for (int i = 0; i < 12; ++i) QVERIFY(editor->findChild<QToolButton *>(QStringLiteral("objectColor%1").arg(i)));
            auto *blue = editor->findChild<QToolButton *>(QStringLiteral("objectColor5")); blue->click(); QVERIFY(blue->isChecked()); QVERIFY(!orange->isChecked());
            QVERIFY(editor->grab().save(QStringLiteral("marker-colors.png")));
            auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject")); apply->click();
            id = objects->items().firstKey(); QCOMPARE(objects->find(id)->color, QStringLiteral("#1e88e5"));
            QCOMPARE(selection->item()->color, QColor("#1e88e5"));
            objects->remove(id); objects->undoStack().undo(); QCOMPARE(objects->find(id)->color, QStringLiteral("#1e88e5"));
            editor->edit(*objects->find(id)); QVERIFY(blue->isChecked());
            editor->findChild<QToolButton *>(QStringLiteral("objectColor0"))->click(); editor->cancel();
            QCOMPARE(objects->find(id)->color, QStringLiteral("#1e88e5"));
            editor->edit(*objects->find(id)); editor->findChild<QToolButton *>(QStringLiteral("objectColor7"))->click(); apply->click();
            QCOMPARE(selection->item()->color, QColor("#d81b60")); QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<swak::UserObjects *>()->find(id)->color, QStringLiteral("#d81b60"));
    }
    void undoMenuHistory()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("undo.json")), QPluginLoader::staticInstances());
        window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *objects = window.findChild<swak::UserObjects *>(); QVERIFY(objects);
        auto *undo = window.findChild<QAction *>(QStringLiteral("undoAction")); QVERIFY(undo);
        auto *menu = window.findChild<QMenu *>(QStringLiteral("editMenu")); QVERIFY(menu);
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        QVERIFY(menu->actions().contains(undo));
        QVERIFY(!undo->isEnabled());
        for (int i = 0; i < 3; ++i) {
            swak::UserObject item;
            item.id = QString::number(i); item.type = QStringLiteral("circle"); item.title = item.id;
            item.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}};
            QVERIFY(objects->put(item).isEmpty());
            objects->remove(item.id);
        }
        for (int remaining = 3; remaining > 0; --remaining) {
            QVERIFY(undo->isEnabled());
            if (remaining == 2) {
                map->setFocus(); QTest::keyClick(map, Qt::Key_Z, Qt::ControlModifier);
            } else {
                menu->popup(window.mapToGlobal(QPoint(30, 30)));
                QVERIFY(QTest::qWaitForWindowExposed(menu));
                menu->setActiveAction(undo); QTest::keyClick(menu, Qt::Key_Return);
            }
            QCOMPARE(objects->undoStack().index(), remaining - 1);
            QCOMPARE(undo->isEnabled(), remaining > 1);
        }
        QCOMPARE(objects->items().size(), 3);
        menu->popup(window.mapToGlobal(QPoint(30, 30)));
        QVERIFY(QTest::qWaitForWindowExposed(menu));
        QVERIFY(!undo->isEnabled());
        QVERIFY(menu->grab().save(QStringLiteral("undo-empty-menu.png")));
        menu->setActiveAction(undo); QTest::keyClick(menu, Qt::Key_Return);
        menu->close();
        map->setFocus(); QTest::keyClick(map, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(objects->items().size(), 3); QCOMPARE(objects->undoStack().index(), 0);
        objects->remove(QStringLiteral("0")); QVERIFY(undo->isEnabled());
        objects->undoStack().clear(); QVERIFY(!undo->isEnabled());
    }
    void markerFocusAfterApply_data()
    {
        QTest::addColumn<bool>("floatingEditor");
        QTest::addColumn<bool>("floatingInformation");
        for (bool editor : {false, true}) for (bool information : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("editor-%1-information-%2").arg(editor).arg(information))) << editor << information;
    }
    void markerFocusAfterApply()
    {
        QFETCH(bool, floatingEditor); QFETCH(bool, floatingInformation);
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("focus.json")), QPluginLoader::staticInstances());
        window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *editorDock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.markers.pane"));
        auto *infoDock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        QVERIFY(map && editor && objects && editorDock && infoDock);
        auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"));
        auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("mapToolStatus"));
        QVERIFY(title && apply && status);
        editorDock->setFloating(floatingEditor); infoDock->setFloating(floatingInformation);
        editorDock->hide(); infoDock->hide(); window.activateWindow(); map->setFocus();
        QVERIFY(QTest::qWaitFor([&] { return map->hasFocus(); }));
        for (int cycle = 0; cycle < 6; ++cycle) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_M);
            QCOMPARE(status->text(), QStringLiteral("M"));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            QVERIFY(editor->draft());
            editor->window()->activateWindow(); QTest::mouseClick(title, Qt::LeftButton);
            QVERIFY(QTest::qWaitFor([&] { return title->hasFocus(); }));
            QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Marker M"));
            QVERIFY(status->text().isEmpty());
            if (cycle % 2 == 0) QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
            else QTest::mouseClick(apply, Qt::LeftButton);
            QCOMPARE(objects->items().size(), cycle + 1); QVERIFY(!editor->draft());
            QVERIFY(infoDock->isVisible());
            QVERIFY2(QTest::qWaitFor([&] { return map->hasFocus(); }), "Saving must return keyboard focus to the map without another user action");
        }
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_M); QCOMPARE(status->text(), QStringLiteral("M"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape); QVERIFY(status->text().isEmpty());
    }
    void markerPointerLatency()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("pointer.json")), QPluginLoader::staticInstances());
        window.show(); window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (auto *pane : window.findChildren<QDockWidget *>()) pane->hide();
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *overlay = map->findChild<QWidget *>(QStringLiteral("mapToolOverlay")); QVERIFY(overlay);
        map->setFocus(); QTest::keyClick(map, Qt::Key_M);
        const QPoint current(210, 180);
        QCursor::setPos(map->mapToGlobal(current));
        QVERIFY(QTest::qWaitFor([&] { return map->mapFromGlobal(QCursor::pos()) == current; }));
        QTest::qWait(80);
        struct PaintCounter final : QObject {
            int count = 0;
            bool eventFilter(QObject *, QEvent *event) override { if (event->type() == QEvent::Paint) ++count; return false; }
        } overlayPaints, mapPaints;
        overlay->installEventFilter(&overlayPaints); map->installEventFilter(&mapPaints);
        const QPoint stale(150, 120);
        QMouseEvent motion(QEvent::MouseMove, stale, map->mapToGlobal(stale), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(map, &motion);
        QVERIFY2(QTest::qWaitFor([&] { return overlayPaints.count > 0; }, 200), "Guide painting did not complete promptly");
        QCOMPARE(mapPaints.count, 0);
        const auto frame = overlay->grab().toImage();
        const qreal scale = overlay->devicePixelRatioF();
        const auto at = [&](int x, int y) { return frame.pixelColor(qRound(x * scale), qRound(y * scale)); };
        QVERIFY(at(current.x(), 40) != at(current.x() + 10, 40));
        QCOMPARE(at(stale.x(), 40), at(stale.x() + 10, 40));
        QTest::keyClick(map, Qt::Key_Escape);
        QTest::qWait(80); overlayPaints.count = 0;
        QTest::qWait(80); QCOMPARE(overlayPaints.count, 0);
    }
    void editableMarkers()
    {
        QTemporaryDir directory; const auto profile = directory.filePath(QStringLiteral("markers.json")); QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>(); auto *objects = window.findChild<swak::UserObjects *>(); QVERIFY(map && objects);
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")); QVERIFY(editor);
            auto *selection = window.findChild<swak::MapSelection *>();
            auto *pane = window.findChild<QDockWidget *>(QStringLiteral("org.swak.markers.pane")); QVERIFY(pane);
            auto *regular = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane")); QVERIFY(regular);
            auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"));
            auto *description = editor->findChild<QPlainTextEdit *>(QStringLiteral("objectDescription"));
            auto *table = editor->findChild<QTableWidget *>(QStringLiteral("objectProperties"));
            auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject")); QVERIFY(title && description && table && apply);
            map->setFocus(); QVERIFY(QTest::qWaitFor([&] { return map->hasFocus(); }));
            auto *modeTools = window.findChild<swak::MapTools *>(); QVERIFY(modeTools); QVERIFY(!modeTools->coordinateGridActive());
            auto *modeStatus = window.findChild<QLabel *>(QStringLiteral("mapToolStatus")); QVERIFY(modeStatus); QVERIFY(modeStatus->text().isEmpty());
            QTest::keyClick(map, Qt::Key_M); QCOMPARE(map->cursor().shape(), Qt::CrossCursor); QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QKeyEvent repeat(QEvent::KeyPress, Qt::Key_M, Qt::NoModifier, QStringLiteral("m"), true);
            QApplication::sendEvent(map, &repeat); QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QTest::keyClick(map, Qt::Key_M); QVERIFY(modeStatus->text().isEmpty()); QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            QVERIFY(!modeTools->coordinateGridActive());
            QTest::keyClick(map, Qt::Key_M); QCOMPARE(modeStatus->text(), QStringLiteral("M")); QVERIFY(modeTools->coordinateGridActive());
            auto *tools = window.findChild<swak::MapTools *>(); QVERIFY(tools);
            QSignalSpy sceneChanges(tools, &swak::MapTools::changed);
            QSignalSpy overlayChanges(tools, &swak::MapTools::overlayChanged);
            const QPoint point = map->rect().center(); QTest::mouseMove(map, point);
            QCOMPARE(sceneChanges.count(), 0); QVERIFY(overlayChanges.count() > 0);
            QImage guides(map->size(), QImage::Format_ARGB32_Premultiplied); guides.fill(Qt::transparent);
            { QPainter painter(&guides); swak::Viewport view; view.size = map->size(); tools->paintOverlay(painter, view, point); }
            const auto guide = guides.pixelColor(point.x(), 30); QCOMPARE(guide.red(), 0); QCOMPARE(guide.alpha(), 128);
            QVERIFY(map->findChild<QWidget *>(QStringLiteral("mapToolOverlay")));
            struct PaintCounter final : QObject {
                int count = 0;
                bool eventFilter(QObject *, QEvent *event) override { if (event->type() == QEvent::Paint) ++count; return false; }
            } paints;
            map->installEventFilter(&paints);
            QTest::qWait(80); paints.count = 0;
            for (int i = 1; i <= 20; ++i) { QTest::mouseMove(map, point + QPoint(i, i)); QTest::qWait(2); }
            QVERIFY2(paints.count <= 2, qPrintable(QStringLiteral("Pointer motion repainted the map %1 times").arg(paints.count)));
            QSignalSpy guideFrames(map, &QOpenGLWidget::frameSwapped);
            QTest::mouseMove(map, point);
            QVERIFY(guideFrames.count() > 0 || guideFrames.wait(2000));
            map->removeEventFilter(&paints);


            QVERIFY(QTest::qWaitFor([&] { return map->isValid(); }));
            QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("marker-crosshair.png")));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
            QVERIFY(modeStatus->text().isEmpty()); QVERIFY(!modeTools->coordinateGridActive());
            QVERIFY(pane->isVisible()); QVERIFY(editor->draft()); QVERIFY(objects->items().isEmpty());
            QCOMPARE(title->placeholderText(), QStringLiteral("New Marker"));
            QVERIFY(editor->grab().save(QStringLiteral("marker-editor.png")));
            title->setText(QStringLiteral("Observation post")); description->setPlainText(QStringLiteral("Local marker description"));
            table->setRowCount(2);
            for (int row = 0; row < 2; ++row) { table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("Unit"))); table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("Alpha"))); }
            QTest::keyClick(title, Qt::Key_Return); QVERIFY(objects->items().isEmpty()); QVERIFY(editor->draft());
            table->removeRow(1); QTest::keyClick(title, Qt::Key_Return); QCOMPARE(objects->items().size(), 1); QVERIFY(!editor->draft());
            id = objects->items().firstKey(); QCOMPARE(objects->find(id)->title, QStringLiteral("Observation post"));
            QVERIFY(selection->item()); QCOMPARE(selection->item()->id, id); QVERIFY(regular->isVisible()); QVERIFY(!pane->isVisible());
            QCOMPARE(selection->item()->properties.value(QStringLiteral("Unit")).toString(), QStringLiteral("Alpha"));
            const auto position = [&] {
                const auto value = dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
                const double scale = 256 * std::exp2(value.value(QStringLiteral("zoom")).toDouble());
                const auto geometry = objects->find(id)->geometry;
                return QPointF((geometry.value(QStringLiteral("x")).toDouble() - value.value(QStringLiteral("centerX")).toDouble()) * scale + map->width()/2.0,
                    (geometry.value(QStringLiteral("y")).toDouble() - value.value(QStringLiteral("centerY")).toDouble()) * scale + map->height()/2.0).toPoint();
            };
            const auto context = [&](const QString &actionName) {
                QTimer::singleShot(0, map, [actionName] {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
                    auto *action = menu->findChild<QAction *>(actionName); QVERIFY(action); action->trigger(); menu->close();
                });
                const auto hitPoint = position(); QContextMenuEvent event(QContextMenuEvent::Mouse, hitPoint, map->mapToGlobal(hitPoint)); QApplication::sendEvent(map, &event);
            };
            context(QStringLiteral("editMarker")); QVERIFY(editor->draft()); QCOMPARE(title->text(), QStringLiteral("Observation post"));
            title->setText(QStringLiteral("Discarded")); QTest::keyClick(title, Qt::Key_Escape); QVERIFY(!editor->draft()); QCOMPARE(objects->find(id)->title, QStringLiteral("Observation post"));
            context(QStringLiteral("editMarker")); title->setText(QStringLiteral("Updated marker")); apply->click(); QCOMPARE(objects->find(id)->title, QStringLiteral("Updated marker"));
            const auto original = objects->find(id)->geometry;
            context(QStringLiteral("moveMarker")); QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            QTest::mouseMove(map, position() + QPoint(70, 40)); QCOMPARE(objects->find(id)->geometry, original);
            QTest::keyClick(map, Qt::Key_Escape); QCOMPARE(objects->find(id)->geometry, original); QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            QVERIFY(modeStatus->text().isEmpty());
            context(QStringLiteral("moveMarker")); QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QTest::mouseMove(map, position() + QPoint(40, 30)); QTest::keyClick(map, Qt::Key_M);
            QCOMPARE(objects->find(id)->geometry, original); QVERIFY(modeStatus->text().isEmpty());
            context(QStringLiteral("moveMarker")); const auto destination = position() + QPoint(70, 40);
            QTest::mouseMove(map, destination); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, destination); QVERIFY(objects->find(id)->geometry != original);
            auto *undo = window.findChild<QAction *>(QStringLiteral("undoAction")); QVERIFY(undo); QVERIFY(!undo->isEnabled());
            context(QStringLiteral("deleteMarker")); QVERIFY(objects->items().isEmpty()); QVERIFY(!selection->item()); QVERIFY(undo->isEnabled());
            map->setFocus(); QTest::keyClick(map, Qt::Key_Z, Qt::ControlModifier); QCOMPARE(objects->items().size(), 1); QCOMPARE(objects->find(id)->title, QStringLiteral("Updated marker"));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, position()); QVERIFY(selection->item());
            QTest::keyClick(map, Qt::Key_Backspace); QVERIFY(objects->items().isEmpty()); undo->trigger(); QCOMPARE(objects->items().size(), 1);
            auto *readOnly = regular->findChild<QPlainTextEdit *>(); QVERIFY(readOnly);
            QTest::keyClick(readOnly, Qt::Key_Delete); QVERIFY(objects->items().isEmpty()); undo->trigger(); QCOMPARE(objects->items().size(), 1);
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::keyClick(map, Qt::Key_Escape); QVERIFY(!editor->draft()); QCOMPARE(objects->items().size(), 1);
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            QVERIFY(editor->draft()); QTest::keyClick(title, Qt::Key_Escape); QVERIFY(!editor->draft()); QCOMPARE(objects->items().size(), 1);
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            QTest::keyClick(title, Qt::Key_Return); QCOMPARE(objects->items().size(), 2); QCOMPARE(selection->item()->label, QStringLiteral("New Marker"));
            const auto temporaryId = selection->item()->id; objects->remove(temporaryId);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, position()); QVERIFY(selection->item()); QCOMPARE(selection->item()->id, id);
            regular->setFloating(true); regular->show(); regular->activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(regular));
            readOnly->setFocus(); QTest::keyClick(readOnly, Qt::Key_Delete); QVERIFY(!objects->find(id));
            QTest::keyClick(readOnly, Qt::Key_Z, Qt::ControlModifier); QVERIFY(objects->find(id));
            regular->setFloating(false); regular->show(); window.activateWindow(); map->setFocus();
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, position());
            QVERIFY(QTest::qWaitFor([&] {
                const auto frame = map->grabFramebuffer(); const auto pixel = position() * map->devicePixelRatioF();
                return !frame.isNull() && frame.rect().contains(pixel) && frame.pixelColor(pixel) == QColor("#ffc857");
            }));
            QVERIFY(QTest::qWaitFor([&] { return regular->isVisible(); }));
            QVERIFY(window.screen()->grabWindow(window.winId()).save(QStringLiteral("marker-information.png")));
            auto *configuration = dynamic_cast<swak::SettingsParticipant *>(editor); QVERIFY(configuration);
            auto options = configuration->savePreferences(); options.insert(QStringLiteral("createKey"), QStringLiteral("N")); options.insert(QStringLiteral("radius"), 9);
            QVERIFY(configuration->validateSettings(options).isEmpty()); configuration->restorePreferences(options);
            map->setFocus(); QTest::keyClick(map, Qt::Key_M); QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            QTest::keyClick(map, Qt::Key_N); QCOMPARE(map->cursor().shape(), Qt::CrossCursor); QTest::keyClick(map, Qt::Key_Escape);
            options.insert(QStringLiteral("color"), QStringLiteral("invalid")); QVERIFY(!configuration->validateSettings(options).isEmpty());
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances()); auto *objects = restored.findChild<swak::UserObjects *>();
        QCOMPARE(objects->items().size(), 1); QCOMPARE(objects->find(id)->title, QStringLiteral("Updated marker"));
        QCOMPARE(objects->find(id)->description, QStringLiteral("Local marker description")); QVERIFY(!objects->undoStack().canUndo());
        const auto configuration = dynamic_cast<swak::SettingsParticipant *>(restored.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")))->savePreferences();
        QCOMPARE(configuration.value(QStringLiteral("createKey")).toString(), QStringLiteral("N")); QCOMPARE(configuration.value(QStringLiteral("radius")).toInt(), 9);
    }
    void keybindingsAndDismissal()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("keys.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map); QVERIFY(settings);
            settings->restorePreferences({{QStringLiteral("zoom"), 4}, {QStringLiteral("panNorthKey"), QStringLiteral("Q")}});
            QCOMPARE(settings->savePreferences().value(QStringLiteral("panNorthKey")).toString(), QStringLiteral("Q"));
            QVERIFY(settings->savePreferences().value(QStringLiteral("zoomInKey")).toString().isEmpty());
            settings->restorePreferences({{QStringLiteral("zoom"), 4}});
            const auto zoom = [&] { return settings->savePreferences().value(QStringLiteral("zoom")).toInt(); };
            map->setFocus(); QVERIFY(QTest::qWaitFor([&] { return map->hasFocus(); }));
            QTest::keyClick(map, Qt::Key_Q); QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_E); QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_Plus, Qt::ShiftModifier); QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_Minus); QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_Equal); QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_Minus); QCOMPARE(zoom(), 4);
            const auto center = [&] { const auto v = settings->savePreferences(); return QPointF(v.value(QStringLiteral("centerX")).toDouble(), v.value(QStringLiteral("centerY")).toDouble()); };
            for (const auto key : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down}) {
                const auto origin = center(); QTest::keyPress(map, key);
                QVERIFY(QTest::qWaitFor([&] { return center() != origin; }, 1000)); QTest::keyRelease(map, key);
                if (key == Qt::Key_Left) QVERIFY(center().x() < origin.x());
                if (key == Qt::Key_Right) QVERIFY(center().x() > origin.x());
                if (key == Qt::Key_Up) QVERIFY(center().y() < origin.y());
                if (key == Qt::Key_Down) QVERIFY(center().y() > origin.y());
            }
            QTest::keyClick(map, Qt::Key_E, Qt::ControlModifier); QCOMPARE(zoom(), 4);
            auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(selection);
            auto *pane = window.findChild<QWidget *>(QStringLiteral("informationPane"));
            auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane")); QVERIFY(pane && dock);
            swak::MapItem item; item.label = QStringLiteral("Selected point"); selection->select(item);
            QVERIFY(dock->isVisible());
            QTest::keyClick(map, Qt::Key_Escape); QVERIFY(!selection->item()); QVERIFY(!dock->isVisible());
            selection->select(item); dock->setFloating(true); dock->show(); dock->activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(dock));
            auto *description = pane->findChild<QPlainTextEdit *>(); description->setFocus();
            QTest::keyClick(description, Qt::Key_Q); QCOMPARE(zoom(), 4);
            QTest::keyClick(description, Qt::Key_Escape); QVERIFY(!selection->item()); QVERIFY(!dock->isVisible());
            dock->show(); QTest::keyClick(description, Qt::Key_Escape); QVERIFY(!dock->isVisible());
            selection->select(item);
            {
                QDialog modal(&window); modal.setModal(true); modal.show();
                QVERIFY(QTest::qWaitForWindowExposed(&modal));
                QTest::keyClick(&modal, Qt::Key_Escape); QVERIFY(selection->item()); QVERIFY(dock->isVisible());
            }
            auto *info = dynamic_cast<swak::SettingsParticipant *>(pane); QVERIFY(info);
            swak::SettingsDialog dialog({{settings->preferenceId(), settings->settingsTitle(), settings->settingsFields(), settings->savePreferences(),
                [settings](const QJsonObject &v) { return settings->validateSettings(v); },
                [settings](const QJsonObject &v) { settings->restorePreferences(v); }},
                {info->preferenceId(), info->settingsTitle(), info->settingsFields(), info->savePreferences(),
                [info](const QJsonObject &v) { return info->validateSettings(v); },
                [info](const QJsonObject &v) { info->restorePreferences(v); }}}, [] { return QString{}; }, &window);
            dialog.show(); dialog.findChild<QTabWidget *>()->setCurrentIndex(2);
            auto *keys = dialog.findChild<QListWidget *>(QStringLiteral("keybindingSettingsSections")); QVERIFY(keys); QCOMPARE(keys->count(), 2);
            keys->setCurrentRow(1);
            auto *in = dialog.findChild<QKeySequenceEdit *>(QStringLiteral("org.swak.map/zoomInKey")); QVERIFY(in);
            auto *out = dialog.findChild<QKeySequenceEdit *>(QStringLiteral("org.swak.map/zoomOutKey")); QVERIFY(out);
            auto *close = dialog.findChild<QKeySequenceEdit *>(QStringLiteral("org.swak.information/dismissKey")); QVERIFY(close);
            in->setKeySequence(QKeySequence(Qt::Key_R)); out->setKeySequence(QKeySequence(Qt::Key_R));
            auto *apply = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply); apply->click();
            QCOMPARE(settings->savePreferences().value(QStringLiteral("zoomInKey")).toString(), QStringLiteral("Q"));
            out->setKeySequence(QKeySequence(Qt::Key_T)); close->setKeySequence(QKeySequence(Qt::Key_F12)); apply->click();
            QCOMPARE(info->savePreferences().value(QStringLiteral("dismissKey")).toString(), QStringLiteral("F12"));
            QVERIFY(QTest::qWaitForWindowExposed(&dialog));
            QVERIFY(dialog.screen()->grabWindow(dialog.winId()).save(QStringLiteral("keybindings-settings.png")));
            const auto navigation = dialog.navigation(); QCOMPARE(navigation.value(QStringLiteral("tab")).toString(), QStringLiteral("keybindings"));
            dialog.close(); window.activateWindow(); map->setFocus();
            QTest::keyClick(map, Qt::Key_Q); QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_R); QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_T); QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_Escape); QVERIFY(selection->item());
            QTest::keyClick(map, Qt::Key_F12); QVERIFY(!selection->item()); QVERIFY(!dock->isVisible());
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *map = dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QOpenGLWidget *>());
        QCOMPARE(map->savePreferences().value(QStringLiteral("zoomInKey")).toString(), QStringLiteral("R"));
        auto *info = dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QWidget *>(QStringLiteral("informationPane")));
        QCOMPARE(info->savePreferences().value(QStringLiteral("dismissKey")).toString(), QStringLiteral("F12"));
    }
    void keyboardPanAndWheelSettings()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("navigation.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map); QVERIFY(settings);
            map->setFocus(); QVERIFY(QTest::qWaitFor([&] { return map->hasFocus(); }));
            QJsonObject values{{QStringLiteral("zoom"), 4}, {QStringLiteral("panSpeed"), 320}};
            settings->restorePreferences(values);
            const auto center = [&] { const auto state = settings->savePreferences(); return QPointF(state.value(QStringLiteral("centerX")).toDouble(), state.value(QStringLiteral("centerY")).toDouble()); };
            const auto zoom = [&] { return settings->savePreferences().value(QStringLiteral("zoom")).toDouble(); };
            const auto origin = center();
            QElapsedTimer clock; clock.start();
            QTest::keyPress(map, Qt::Key_Up);
            QTest::qWait(240);
            QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier, QStringLiteral("w"), true);
            QApplication::sendEvent(map, &repeat);
            QKeyEvent repeatRelease(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier, QStringLiteral("w"), true);
            QApplication::sendEvent(map, &repeatRelease);
            QTest::qWait(240);
            QTest::keyRelease(map, Qt::Key_Up);
            const double pixels = (origin.y() - center().y()) * 4096;
            const double expected = static_cast<double>(clock.elapsed()) * 0.32;
            QVERIFY(pixels > expected * 0.65 && pixels < expected * 1.2);
            QCOMPARE(center().x(), origin.x());
            auto stopped = center(); QTest::qWait(80); QCOMPARE(center(), stopped);
            for (const auto key : {Qt::Key_Left, Qt::Key_Down, Qt::Key_Right}) {
                const auto before = center(); QTest::keyPress(map, key);
                QVERIFY(QTest::qWaitFor([&] { return center() != before; }, 1000));
                QTest::keyRelease(map, key);
                if (key == Qt::Key_Left) QVERIFY(center().x() < before.x());
                if (key == Qt::Key_Down) QVERIFY(center().y() > before.y());
                if (key == Qt::Key_Right) QVERIFY(center().x() > before.x());
            }
            QTest::keyPress(map, Qt::Key_Up); QTest::keyPress(map, Qt::Key_Down);
            stopped = center(); QTest::qWait(80); QCOMPARE(center(), stopped);
            QTest::keyRelease(map, Qt::Key_Up); QTest::keyRelease(map, Qt::Key_Down);
            QTest::keyPress(map, Qt::Key_Right);
            auto *editorPane = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane")); QVERIFY(editorPane);
            editorPane->show(); editorPane->raise();
            auto *editor = editorPane->findChild<QLineEdit *>(); QVERIFY(editor); editor->setFocus();
            QVERIFY(QTest::qWaitFor([&] { return !map->hasFocus(); }));
            stopped = center(); QTest::qWait(80); QCOMPARE(center(), stopped);
            QTest::keyRelease(map, Qt::Key_Right);
            values.insert(QStringLiteral("panNorthKey"), QStringLiteral("I"));
            values.insert(QStringLiteral("panWestKey"), QStringLiteral("J"));
            values.insert(QStringLiteral("panSouthKey"), QStringLiteral("K"));
            values.insert(QStringLiteral("panEastKey"), QStringLiteral("L"));
            QVERIFY(settings->validateSettings(values).isEmpty());
            auto invalid = values; invalid.insert(QStringLiteral("panEastKey"), QStringLiteral("I")); QVERIFY(!settings->validateSettings(invalid).isEmpty());
            invalid = values; invalid.insert(QStringLiteral("panEastKey"), QStringLiteral("Ctrl+L")); QVERIFY(!settings->validateSettings(invalid).isEmpty());
            values.insert(QStringLiteral("wheelZoom"), false); settings->restorePreferences(values);
            const auto wheel = [&](QPointF position) {
                QWheelEvent event(position, map->mapToGlobal(position.toPoint()), QPoint{}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(map, &event);
            };
            wheel(map->rect().center()); QCOMPARE(zoom(), 4.0);
            values.insert(QStringLiteral("wheelZoom"), true); values.insert(QStringLiteral("wheelReverse"), true); values.insert(QStringLiteral("wheelSteps"), 2);
            settings->restorePreferences(values);
            wheel(QPointF(-10, -10)); QCOMPARE(zoom(), 4.0);
            wheel(map->rect().center()); QCOMPARE(zoom(), std::max(2.0, std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0))));
            map->setFocus(); QVERIFY(QTest::qWaitFor([&] { return map->hasFocus(); }));
            QTest::keyPress(map, Qt::Key_I); QVERIFY(QTest::qWaitFor([&] { return center().y() < 0.5; })); QTest::keyRelease(map, Qt::Key_I);
            QTest::keyPress(map, Qt::Key_L); map->hide(); stopped = center(); QTest::qWait(80); QCOMPARE(center(), stopped);
            map->show(); QTest::keyRelease(map, Qt::Key_L);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        const auto state = dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>())->savePreferences();
        QCOMPARE(state.value(QStringLiteral("panNorthKey")).toString(), QStringLiteral("I"));
        QCOMPARE(state.value(QStringLiteral("panSpeed")).toDouble(), 320.0);
        QCOMPARE(state.value(QStringLiteral("wheelSteps")).toInt(), 2);
        QVERIFY(state.value(QStringLiteral("wheelReverse")).toBool());
    }
    void selectionStrokeAtZoom_data()
    {
        QTest::addColumn<int>("zoom"); QTest::addColumn<int>("kind"); QTest::addColumn<bool>("feed");
        for (int zoom : {2, 12, 19}) for (int kind : {0, 1, 2}) for (bool feed : {false, true}) {
            if (feed && kind == 2) continue;
            QTest::newRow(qPrintable(QStringLiteral("zoom-%1-kind-%2-feed-%3").arg(zoom).arg(kind).arg(feed))) << zoom << kind << feed;
        }
    }
    void selectionStrokeAtZoom()
    {
        QFETCH(int, zoom); QFETCH(int, kind); QFETCH(bool, feed);
        TileServer server; swak::DataSources sources(QPluginLoader::staticInstances());
        SelectionCanvas canvas; canvas.resize(320, 240);
        canvas.view = {QSizeF(320, 240), QPointF(0.63, 0.37), static_cast<double>(zoom)};
        QPainterPath path;
        if (kind == 0) { path.moveTo(50, 70); path.lineTo(270, 170); }
        else path.addRect(QRectF(50, 50, 220, 140));
        QTransform world; world.translate(canvas.view.center.x(), canvas.view.center.y());
        world.scale(1 / canvas.view.scale(), 1 / canvas.view.scale()); world.translate(-160, -120);
        swak::MapItem item; item.kind = kind == 0 ? swak::MapItem::Kind::Line : swak::MapItem::Kind::Polygon;
        item.color = Qt::black; item.lineWidth = 2; item.path = world.map(path); item.outlineStyle = kind == 2 ? Qt::DashLine : Qt::SolidLine;
        if (feed) {
            QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost));
            QStringList coordinates;
            for (int i = 0; i < item.path.elementCount(); ++i) {
                const auto point = item.path.elementAt(i); const auto coordinate = swak::unproject({point.x, point.y});
                coordinates.append(QStringLiteral("%1,%2").arg(QString::number(coordinate.x(), 'g', 17), QString::number(coordinate.y(), 'g', 17)));
            }
            const auto geometry = kind == 0 ? QStringLiteral("<LineString><coordinates>%1</coordinates></LineString>")
                : QStringLiteral("<Polygon><outerBoundaryIs><LinearRing><coordinates>%1</coordinates></LinearRing></outerBoundaryIs></Polygon>");
            server.body = (QStringLiteral("<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark>") + geometry.arg(coordinates.join(' ')) + QStringLiteral("</Placemark></kml>")).toUtf8();
            auto source = swak::dataDefaults(); source.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
            source.insert(QStringLiteral("name"), QStringLiteral("Stroke test")); source.insert(QStringLiteral("attribution"), QStringLiteral("Test fixture"));
            source.insert(QStringLiteral("endpoint"), QStringLiteral("https://127.0.0.1:%1/stroke.kml").arg(server.serverPort()));
            source.insert(QStringLiteral("enabled"), true); source.insert(QStringLiteral("ignoreTls"), true);
            source.insert(QStringLiteral("color"), QStringLiteral("#ffffff")); source.insert(QStringLiteral("opacity"), 1);
            source.insert(QStringLiteral("fillOpacity"), 35.0 / 255); source.insert(QStringLiteral("lineWidth"), 2); source.insert(QStringLiteral("labels"), false);
            QVERIFY(sources.put(QStringLiteral("stroke"), source).isEmpty());
            QVERIFY2(QTest::qWaitFor([&] { return sources.shapeCount(QStringLiteral("stroke")) == 1; }, 5000), qPrintable(sources.status(QStringLiteral("stroke")))); canvas.data = &sources;
        }
        canvas.selection.select(item); canvas.show(); QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(QTest::qWaitFor([&] { return canvas.isValid(); }));
        const auto actual = canvas.grabFramebuffer();
        QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied); expected.setDevicePixelRatio(canvas.devicePixelRatioF()); expected.fill(Qt::black);
        {
            QPainter painter(&expected); painter.setRenderHint(QPainter::Antialiasing);
            QPen pen(Qt::white, kind == 0 && !feed ? 4 : 2); pen.setStyle(item.outlineStyle);
            painter.setPen(pen); painter.setBrush(kind == 0 ? Qt::NoBrush : QBrush(QColor(255, 255, 255, 35))); painter.drawPath(path);
        }
        // Sum coverage so different edge antialiasing does not change the width measurement.
        const auto inkCoverage = [](const QImage &image) {
            qint64 sum = 0;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
                sum += image.pixelColor(x, y).red();
            return sum;
        };
        const auto actualCount = inkCoverage(actual), expectedCount = inkCoverage(expected);
        QVERIFY2(std::abs(actualCount - expectedCount) < expectedCount / 8,
            qPrintable(QStringLiteral("Rendered intensity is %1; expected about %2.").arg(actualCount).arg(expectedCount)));
        QVERIFY(actual.save(QStringLiteral("selection-%1-%2-%3.png").arg(zoom).arg(kind).arg(feed)));
    }

    void polygonInputLimits()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("polygon-limits.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("polygonEditor")); QVERIFY(editor);
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(editor); QVERIFY(settings);
        auto configuration = settings->savePreferences(); configuration.insert(QStringLiteral("createKey"), QStringLiteral("Ctrl+P"));
        QVERIFY(!settings->validateSettings(configuration).isEmpty());
        configuration.insert(QStringLiteral("createKey"), QStringLiteral("O")); QVERIFY(settings->validateSettings(configuration).isEmpty()); settings->restorePreferences(configuration);
        auto *tools = window.findChild<swak::MapTools *>(); QVERIFY(tools);
        auto *tool = editor->findChild<swak::MapTool *>(); QVERIFY(tool);
        const swak::Viewport view{QSizeF(800, 600), {0.5, 0.5}, 2};
        const auto click = [&](QPointF point) {
            QMouseEvent event(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier); tool->handle(event, view, *map);
        };
        QTest::keyClick(map, Qt::Key_O); QCOMPARE(tools->statusText(), QStringLiteral("A"));
        click({400, 200}); click({400, 200}); QVERIFY(!editor->draft());
        tools->cancel();
        for (int repeat = 0; repeat < 4; ++repeat) {
            tool->activate(*map);
            for (int i = 0; i < swak::MaxPolygonVertices; ++i) {
                const double angle = i * 6.283185307179586 / swak::MaxPolygonVertices;
                click({400 + 200 * std::cos(angle), 300 + 200 * std::sin(angle)});
            }
            click({400, 300});
            QKeyEvent complete(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QVERIFY(tool->handle(complete, view, *map));
            QVERIFY(editor->draft()); QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), swak::MaxPolygonVertices);
            QImage overlay(800, 600, QImage::Format_ARGB32_Premultiplied); overlay.fill(Qt::transparent);
            { QPainter painter(&overlay); tool->paintOverlay(painter, view, {500, 500}); }
            QCOMPARE(overlay.pixelColor(600, 300).alpha(), 0);
            editor->cancel();
        }
        tool->activate(*map); click({100, 100}); click({300, 300}); click({100, 300}); click({300, 100});
        QKeyEvent complete(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QVERIFY(tool->handle(complete, view, *map));
        QVERIFY(!editor->draft()); QCOMPARE(tools->statusText(), QStringLiteral("A"));
        tools->cancel(); QVERIFY(!tools->coordinateGridActive());
        QVERIFY(window.findChild<swak::UserObjects *>()->items().isEmpty());
    }

    void cachedLabelInversion()
    {
        SelectionCanvas canvas; canvas.resize(320, 240); canvas.view = {QSizeF(320, 240), QPointF(0.5, 0.5), 4};
        swak::MapItem item; item.kind = swak::MapItem::Kind::Line;
        const auto scale = canvas.view.scale();
        item.cachedLabelBounds = QRectF(canvas.view.center + QPointF(-100, -70) / scale, QSizeF(90 / scale, 30 / scale));
        canvas.selection.select(item); canvas.show(); QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const auto frame = canvas.grabFramebuffer(); const auto ratio = canvas.devicePixelRatioF();
        QVERIFY(frame.pixelColor(QPoint(qRound(70 * ratio), qRound(60 * ratio))).lightness() > 240);
        QVERIFY(frame.pixelColor(QPoint(qRound(40 * ratio), qRound(40 * ratio))).lightness() < 10);
    }
    void generatedIdentityPersists()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("identity.json")); QString uid;
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            uid = window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(); QVERIFY(!QUuid(uid).isNull()); QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances()); QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), uid);
    }
    void selectableLabels()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("labels.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *objects = window.findChild<swak::UserObjects *>(); auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(objects && selection);
        for (auto *dock : window.findChildren<QDockWidget *>()) dock->hide();
        auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map);
        preferences->restorePreferences({{QStringLiteral("centerX"), 0.5}, {QStringLiteral("centerY"), 0.5}, {QStringLiteral("zoom"), 8}});
        const swak::Viewport view{map->size(), {0.5, 0.5}, 8};
        const auto world = [&](QPointF point) { return view.center + (point - QPointF(map->width() / 2.0, map->height() / 2.0)) / view.scale(); };
        const QPoint point(180, 180); const auto coordinate = world(point);
        swak::UserObject marker; marker.id = QStringLiteral("label-marker"); marker.type = QStringLiteral("circle"); marker.title = QStringLiteral("Select this marker label");
        marker.geometry = {{QStringLiteral("x"), coordinate.x()}, {QStringLiteral("y"), coordinate.y()}}; QVERIFY(objects->put(marker).isEmpty());
        const auto actions = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))->actions();
        const auto index = actions.indexOf(window.findChild<QAction *>(QStringLiteral("markerModeAction"))); QVERIFY(index >= 0);
        QCOMPARE(actions.at(index + 1)->objectName(), QStringLiteral("polygonModeAction"));
        QCOMPARE(actions.at(index + 2)->objectName(), QStringLiteral("shapeMode0"));
        QCOMPARE(actions.at(index + 3)->objectName(), QStringLiteral("shapeMode2"));
        QVERIFY(actions.at(index + 4)->isSeparator()); QCOMPARE(actions.at(index + 5)->objectName(), QStringLiteral("shapeMode1"));
        const auto bounds = swak::mapLabelBounds(map->fontMetrics(), point + QPointF(16, -8), marker.title);
        const auto before = map->grabFramebuffer();
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, bounds.center().toPoint());
        QVERIFY(selection->item()); QCOMPARE(selection->item()->id, marker.id);
        const auto after = map->grabFramebuffer();
        const auto sample = ((bounds.topLeft() + QPointF(1, 1)) * map->devicePixelRatioF()).toPoint();
        QVERIFY(after.pixelColor(sample).lightness() > before.pixelColor(sample).lightness() + 80);
        QVERIFY(after.save(QStringLiteral("selected-label.png")));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, bounds.topRight().toPoint() + QPoint(5, -5)); QVERIFY(!selection->item());
        objects->remove(marker.id);
        swak::Event event; event.uid = QStringLiteral("label-cot"); event.callsign = QStringLiteral("Selectable CoT label"); event.type = QStringLiteral("a-f-G-U"); event.how = QStringLiteral("m-g");
        const auto geo = swak::unproject(coordinate); event.latitude = geo.y(); event.longitude = geo.x();
        event.time = QDateTime::currentDateTimeUtc(); event.start = event.time; event.stale = event.time.addSecs(300); QVERIFY(window.store().accept(event).isEmpty());
        const auto cotBounds = swak::mapLabelBounds(map->fontMetrics(), point + QPointF(16, -8), event.callsign);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, cotBounds.center().toPoint());
        QCOMPARE(window.store().selected(), event.uid); QVERIFY(selection->item()); QCOMPARE(selection->item()->id, event.uid);
        QImage normal(320, 80, QImage::Format_ARGB32_Premultiplied), inverted = normal; normal.fill(Qt::transparent); inverted.fill(Qt::transparent);
        { QPainter painter(&normal); swak::paintMapLabel(painter, {10, 40}, QStringLiteral("Label")); }
        { QPainter painter(&inverted); swak::paintMapLabel(painter, {10, 40}, QStringLiteral("Label"), true); }
        for (int y = 0; y < normal.height(); ++y) for (int x = 0; x < normal.width(); ++x) {
            const auto a = normal.pixelColor(x, y), b = inverted.pixelColor(x, y); QCOMPARE(a.alpha(), b.alpha());
            if (a.alpha() > 0) QVERIFY(std::abs(a.red() + b.red() - 255) <= 2);
        }
    }
    void overlappingObjects()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("overlap.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *objects = window.findChild<swak::UserObjects *>(); QVERIFY(objects);
        auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(selection);
        const auto state = dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
        const swak::Viewport view{map->size(), {state.value(QStringLiteral("centerX")).toDouble(), state.value(QStringLiteral("centerY")).toDouble()}, state.value(QStringLiteral("zoom")).toDouble()};
        const QPoint point(200, 200);
        const auto world = [&](QPointF screen) { return view.center + (screen - QPointF(map->width() / 2.0, map->height() / 2.0)) / view.scale(); };
        const auto coordinate = world(point);
        swak::UserObject marker; marker.id = QStringLiteral("marker-a"); marker.title = QStringLiteral("First marker"); marker.type = QStringLiteral("circle");
        marker.color = QStringLiteral("#fb8c00"); marker.geometry = {{QStringLiteral("x"), coordinate.x()}, {QStringLiteral("y"), coordinate.y()}};
        QVERIFY(objects->put(marker).isEmpty()); marker.id = QStringLiteral("marker-b"); marker.title = QStringLiteral("Second marker"); QVERIFY(objects->put(marker).isEmpty());
        swak::UserObject polygon; polygon.id = QStringLiteral("area-a"); polygon.title = QStringLiteral("First area"); polygon.type = QStringLiteral("polygon"); polygon.color = QStringLiteral("#1e88e5");
        polygon.geometry = swak::polygonGeometry({world({100, 100}), world({300, 100}), world({300, 300}), world({100, 300})});
        QVERIFY(objects->put(polygon).isEmpty()); polygon.id = QStringLiteral("area-b"); polygon.title = QStringLiteral("Second area"); QVERIFY(objects->put(polygon).isEmpty());
        for (const auto &id : {QStringLiteral("marker-a"), QStringLiteral("marker-b"), QStringLiteral("area-a"), QStringLiteral("area-b")}) {
            chooseMapObject(map, objects->find(id)->title);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
            QVERIFY(selection->item()); QCOMPARE(selection->item()->id, id);
        }
        QTimer::singleShot(0, map, [map] {
            auto *menu = map->findChild<QMenu *>(QStringLiteral("mapObjectChooser")); QVERIFY(menu);
            QCOMPARE(menu->actions().size(), 4); QVERIFY(menu->toolTipsVisible());
            for (auto *action : menu->actions()) { QVERIFY(!action->isSeparator()); QVERIFY(!action->icon().isNull()); QVERIFY(action->text().startsWith(QStringLiteral("First ")) || action->text().startsWith(QStringLiteral("Second "))); QVERIFY(!action->text().contains('(')); }
            QVERIFY(menu->grab().save(QStringLiteral("overlap-chooser.png"))); menu->close();
        });
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(selection->item()->id, QStringLiteral("area-b"));
        // A model update cancels the menu before its snapshot becomes stale.
        QTimer::singleShot(0, map, [objects] { objects->remove(QStringLiteral("area-b")); });
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point); QVERIFY(!selection->item());
        for (const auto &pair : {qMakePair(QStringLiteral("First marker"), QStringLiteral("deleteMarker")), qMakePair(QStringLiteral("First area"), QStringLiteral("deletePolygon"))}) {
            QTimer timer; timer.setInterval(0); int step = 0;
            connect(&timer, &QTimer::timeout, map, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!menu) return;
                const auto close = qScopeGuard([menu] { menu->close(); });
                if (step++ == 0) {
                    QCOMPARE(menu->objectName(), QStringLiteral("mapObjectChooser"));
                    for (auto *action : menu->actions()) if (action->text().contains(pair.first)) { action->trigger(); return; }
                    QFAIL("Object is absent from the chooser");
                }
                timer.stop(); auto *action = menu->findChild<QAction *>(pair.second); QVERIFY(action); action->trigger();
            });
            timer.start();
            QContextMenuEvent context(QContextMenuEvent::Mouse, point, map->mapToGlobal(point)); QApplication::sendEvent(map, &context);
            QCOMPARE(step, 2);
        }
        QCOMPARE(objects->items().size(), 1);
        // A single hit selects directly, and empty space clears the selection.
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point); QCOMPARE(selection->item()->id, QStringLiteral("marker-b"));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(400, 400)); QVERIFY(!selection->item());
        for (int i = 0; i < 31; ++i) { marker.id = QStringLiteral("extra-%1").arg(i, 2, 10, QLatin1Char('0')); QVERIFY(objects->put(marker).isEmpty()); }
        QTimer::singleShot(0, map, [&] {
            auto *menu = map->findChild<QMenu *>(QStringLiteral("mapObjectChooser")); QVERIFY(menu);
            const auto close = qScopeGuard([menu] { menu->close(); });
            auto *more = menu->actions().last()->menu(); QVERIFY(more);
            QVERIFY(QMetaObject::invokeMethod(more, "aboutToShow", Qt::DirectConnection));
            for (auto *action : more->actions()) if (action->data().toString() == QStringLiteral("marker-b")) { action->trigger(); return; }
            QFAIL("Last overlapping marker is absent from the next page");
        });
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(selection->item()); QCOMPARE(selection->item()->id, QStringLiteral("marker-b"));
        QTest::mousePress(map, Qt::LeftButton, Qt::NoModifier, point);
        const auto destination = point + QPoint(40, 0);
        QMouseEvent movement(QEvent::MouseMove, destination, map->mapToGlobal(destination), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(map, &movement);
        QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, destination);
        QVERIFY(selection->item()); QCOMPARE(selection->item()->id, QStringLiteral("marker-b"));
        QVERIFY(dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences().value(QStringLiteral("centerX")) != state.value(QStringLiteral("centerX")));
    }

    void shapeWorkflow()
    {
        QTemporaryDir directory; const auto profile = directory.filePath(QStringLiteral("shapes.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances()); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("shapesEditor")); QVERIFY(editor);
            auto *objects = window.findChild<swak::UserObjects *>(); auto *selection = window.findChild<swak::MapSelection *>(); auto *tools = window.findChild<swak::MapTools *>();
            QVERIFY(objects && selection && tools);
            for (auto *dock : window.findChildren<QDockWidget *>()) dock->hide();
            auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map); auto values = mapSettings->savePreferences(); values.insert(QStringLiteral("centerX"), 0.5); values.insert(QStringLiteral("centerY"), 0.5); values.insert(QStringLiteral("zoom"), 8); mapSettings->restorePreferences(values);
            auto *areaSettings = dynamic_cast<swak::SettingsParticipant *>(window.findChild<QWidget *>(QStringLiteral("polygonEditor"))); QVERIFY(areaSettings);
            auto areaValues = areaSettings->savePreferences(); areaValues.insert(QStringLiteral("fillOpacity"), 0.65); areaSettings->restorePreferences(areaValues); QCOMPARE(tools->areaFillOpacity(), 0.65);
            const auto click = [&](QPoint p) { QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, p); };
            const auto motion = [&](QPoint p) { QMouseEvent event(QEvent::MouseMove, QPointF(p), QPointF(map->mapToGlobal(p)), Qt::NoButton, Qt::NoButton, Qt::NoModifier); QApplication::sendEvent(map, &event); };
            const auto contextAction = [&](const QString &name, QPoint p) {
                bool invoked = false;
                QTimer::singleShot(0, &window, [&] { auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (!menu) return; if (auto *action = menu->findChild<QAction *>(name)) { action->trigger(); invoked = true; } menu->close(); });
                QContextMenuEvent event(QContextMenuEvent::Mouse, p, map->mapToGlobal(p)); QApplication::sendEvent(map, &event); return invoked;
            };
            for (const auto key : {Qt::Key_C, Qt::Key_L}) {
                QTest::keyClick(map, key); QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
                click({180, 180}); motion({260, 180}); click({260, 180}); QVERIFY(editor->draft());
                const bool circle = key == Qt::Key_C;
                QCOMPARE(editor->draft()->type, circle ? QStringLiteral("range-circle") : QStringLiteral("line"));
                editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"))->setText(circle ? QStringLiteral("Circle test") : QStringLiteral("Line test"));
                editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click(); QVERIFY(selection->item());
                const auto id = selection->item()->id; const auto original = objects->find(id)->geometry;
                QVERIFY(selection->item()->properties.contains(circle ? QStringLiteral("Area") : QStringLiteral("Distance")));
                QVERIFY(contextAction(QStringLiteral("moveShape"), {220, 180})); motion({240, 210}); QTest::keyClick(map, Qt::Key_Escape); QCOMPARE(objects->find(id)->geometry, original);
                QVERIFY(contextAction(QStringLiteral("moveShape"), {220, 180})); motion({240, 210}); click({240, 210}); QVERIFY(objects->find(id)->geometry != original);
                QVERIFY(contextAction(QStringLiteral("editShape"), {240, 210})); QVERIFY(editor->draft());
                const auto editing = editor->draft()->geometry;
                QVERIFY(contextAction(QStringLiteral("moveShapePoint"), {200, 210})); motion({210, 220}); QTest::keyClick(map, Qt::Key_Escape); QCOMPARE(editor->draft()->geometry, editing);
                QVERIFY(contextAction(QStringLiteral("moveShapePoint"), {200, 210})); motion({210, 220}); click({210, 220}); QVERIFY(editor->draft()->geometry != editing);
                auto points = swak::polygonVertices(editor->draft()->geometry); points[1].setY(points[1].y() + 0.0001); QVERIFY(editor->setGeometry(swak::polygonGeometry(points)));
                editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
                QVERIFY(QTest::qWaitFor([&] { return map->isValid(); }));
                const auto frame = map->grabFramebuffer(); QVERIFY(!frame.isNull());
                QVERIFY(frame.save(circle ? QStringLiteral("circle-tool.png") : QStringLiteral("line-tool.png")));
                if (circle) { const auto color = frame.pixelColor(QPoint(200, 250) * map->devicePixelRatioF()); QVERIFY(color.blue() > color.red()); }
                QTest::keyClick(map, Qt::Key_Delete); QVERIFY(!objects->find(id)); objects->undoStack().undo(); QVERIFY(objects->find(id));
                objects->remove(id);
            }
            objects->undoStack().undo(); QCOMPARE(objects->items().size(), 1);
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(editor); auto temporary = settings->savePreferences(); temporary.insert(QStringLiteral("holdSeconds"), 1); temporary.insert(QStringLiteral("fadeMs"), 100); settings->restorePreferences(temporary);
            auto *tool = dynamic_cast<swak::MapTool *>(window.findChild<QObject *>(QStringLiteral("shapesTool"))); QVERIFY(tool);
            const swak::Viewport view{QSizeF(map->size()), QPointF(0.5, 0.5), 8};
            const auto frame = [&] { QImage image(map->size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); QPainter painter(&image); tool->paint(painter, view); return image; };
            selection->clear();
            const auto before = frame(); const auto count = objects->items().size();
            QTest::keyClick(map, Qt::Key_D); click({400, 200}); motion({500, 250}); click({500, 250}); QCOMPARE(objects->items().size(), count); QVERIFY(frame() != before);
            QVERIFY(QTest::qWaitFor([&] { return frame() == before; }, 2500));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances()); auto *objects = restored.findChild<swak::UserObjects *>(); QVERIFY(objects); QCOMPARE(objects->items().size(), 1); QCOMPARE(objects->items().first().type, QStringLiteral("line"));
    }
    void polygonWorkflow()
    {
        QTemporaryDir directory; const auto profile = directory.filePath(QStringLiteral("polygons.json"));
        QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("polygonEditor")); QVERIFY(editor);
            auto *tools = window.findChild<swak::MapTools *>(); QVERIFY(tools);
            auto *objects = window.findChild<swak::UserObjects *>(); QVERIFY(objects);
            auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(selection);
            auto *action = window.findChild<QAction *>(QStringLiteral("polygonModeAction")); QVERIFY(action);
            const QPoint a(100, 140), b(300, 140), c(300, 300), d(100, 300), middle(200, 220);
            const auto click = [&](QPoint point) { QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point); };
            const auto motion = [&](QPoint point) {
                QMouseEvent event(QEvent::MouseMove, QPointF(point), QPointF(map->mapToGlobal(point)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(map, &event);
            };
            QTest::keyClick(map, Qt::Key_A); QCOMPARE(tools->statusText(), QStringLiteral("A")); QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            QTest::keyClick(map, Qt::Key_M); QCOMPARE(tools->statusText(), QStringLiteral("M"));
            QTest::keyClick(map, Qt::Key_A); QCOMPARE(tools->statusText(), QStringLiteral("A"));
            click(a); QCOMPARE(map->cursor().shape(), Qt::CrossCursor); click(b); motion(c);
            QImage overlay(map->size(), QImage::Format_ARGB32_Premultiplied); overlay.fill(Qt::transparent);
            const auto savedView = dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
            swak::Viewport view{map->size(), {savedView.value(QStringLiteral("centerX")).toDouble(), savedView.value(QStringLiteral("centerY")).toDouble()}, savedView.value(QStringLiteral("zoom")).toDouble()};
            { QPainter painter(&overlay); tools->paintOverlay(painter, view, c); }
            const auto shade = overlay.pixelColor(250, 180); QCOMPARE(shade.alpha(), 77); QVERIFY(shade.blue() > shade.red());
            QCOMPARE(overlay.pixelColor(20, c.y()).alpha(), 128);
            QVERIFY(overlay.save(QStringLiteral("polygon-preview.png")));
            click(c); click(d); click(a); QVERIFY(editor->draft()); QVERIFY(editor->isVisible()); QVERIFY(tools->statusText().isEmpty());
            QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), 4);
            QVERIFY(!editor->findChild<QComboBox *>(QStringLiteral("objectCollection"))->isVisible());
            QCOMPARE(editor->findChild<QComboBox *>(QStringLiteral("objectType"))->currentText(), QStringLiteral("Area"));
            auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle")); title->setText(QStringLiteral("Test area"));
            editor->findChild<QPlainTextEdit *>(QStringLiteral("objectDescription"))->setPlainText(QStringLiteral("Area description"));
            editor->findChild<QToolButton *>(QStringLiteral("objectColor3"))->click();
            editor->findChild<QPushButton *>(QStringLiteral("addObjectProperty"))->click();
            auto *properties = editor->findChild<QTableWidget *>(QStringLiteral("objectProperties"));
            auto *propertyName = properties->findChild<QLineEdit *>(); QVERIFY(propertyName);
            propertyName->setText(QStringLiteral("Owner")); QTest::keyClick(propertyName, Qt::Key_Return);
            QVERIFY(QTest::qWaitFor([&] { return properties->item(0, 0)->text() == QStringLiteral("Owner"); }, 1000)); properties->item(0, 1)->setText(QStringLiteral("Local"));
            QVERIFY(editor->grab().save(QStringLiteral("polygon-editor.png")));
            QTest::keyClick(title, Qt::Key_Return); QCOMPARE(objects->items().size(), 1); QVERIFY(!editor->draft());
            id = objects->items().firstKey(); QCOMPARE(objects->find(id)->color, QStringLiteral("#43a047"));
            QCOMPARE(objects->find(id)->properties.value(QStringLiteral("Owner")).toString(), QStringLiteral("Local"));
            click(middle); QVERIFY(selection->item()); QCOMPARE(selection->item()->id, id); QCOMPARE(selection->item()->outlineStyle, Qt::SolidLine);
            QVERIFY(!selection->item()->properties.value(QStringLiteral("Area")).toString().isEmpty());
            QVERIFY(QTest::qWaitFor([&] { return map->isValid(); }));
            const auto frame = map->grabFramebuffer();
            const double ratio = map->devicePixelRatioF();
            QVERIFY(frame.pixelColor(qRound(150 * ratio), qRound(180 * ratio)) != frame.pixelColor(qRound(450 * ratio), qRound(180 * ratio)));
            QVERIFY(frame.save(QStringLiteral("polygon-frame.png")));
            QVERIFY(map->grab().save(QStringLiteral("polygon-selected.png")));
            const auto context = [&](QPoint point, const QString &name, bool enabled = true) {
                bool seen = false;
                QTimer::singleShot(0, map, [&] {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
                    auto *entry = menu->findChild<QAction *>(name); QVERIFY(entry); seen = true; QCOMPARE(entry->isEnabled(), enabled);
                    if (enabled) entry->trigger();
                    menu->close();
                });
                QContextMenuEvent event(QContextMenuEvent::Mouse, point, map->mapToGlobal(point)); QApplication::sendEvent(map, &event); QVERIFY(seen);
            };
            context(middle, QStringLiteral("editPolygon")); QVERIFY(editor->draft());
            const auto original = editor->draft()->geometry;
            overlay.fill(Qt::transparent);
            { QPainter painter(&overlay); tools->paintOverlay(painter, view, middle); }
            QCOMPARE(overlay.pixelColor(a), QColor("#43a047"));
            editor->findChild<QToolButton *>(QStringLiteral("objectColor0"))->click();
            overlay.fill(Qt::transparent);
            { QPainter painter(&overlay); tools->paintOverlay(painter, view, middle); }
            QCOMPARE(overlay.pixelColor(a), QColor(editor->draft()->color));
            QVERIFY(overlay.save(QStringLiteral("polygon-corner-color.png")));
            context(a, QStringLiteral("movePolygonVertex")); QCOMPARE(tools->statusText(), QStringLiteral("A")); motion(b); click(b);
            QCOMPARE(editor->draft()->geometry, original); QCOMPARE(tools->statusText(), QStringLiteral("A"));
            QTest::keyClick(map, Qt::Key_Escape); QVERIFY(editor->draft()); QVERIFY(tools->statusText().isEmpty());
            context(a, QStringLiteral("movePolygonVertex")); click(QPoint(80, 120)); QVERIFY(editor->draft()->geometry != original);
            QCOMPARE(objects->find(id)->geometry, original);
            QTest::keyClick(editor, Qt::Key_Escape); QVERIFY(!editor->draft()); QCOMPARE(objects->find(id)->geometry, original);
            context(middle, QStringLiteral("editPolygon")); context(a, QStringLiteral("deletePolygonVertex"));
            QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), 3);
            context(b, QStringLiteral("deletePolygonVertex"), false);
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click(); QCOMPARE(swak::polygonVertices(objects->find(id)->geometry).size(), 3);
            QTest::keyClick(map, Qt::Key_Delete); QVERIFY(objects->items().isEmpty());
            objects->undoStack().undo(); QCOMPARE(objects->items().size(), 1); QVERIFY(selection->item());
            context(QPoint(250, 250), QStringLiteral("deletePolygon")); QVERIFY(objects->items().isEmpty());
            objects->undoStack().undo(); QCOMPARE(objects->items().size(), 1);
            action->trigger(); click(a); click(b); motion(c); QTest::keyClick(map, Qt::Key_Return);
            QVERIFY(editor->draft()); QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), 3);
            QTest::keyClick(editor, Qt::Key_Escape); QCOMPARE(objects->items().size(), 1);
            action->trigger(); click(a); click(b); motion(QPoint(200, 140)); QTest::keyClick(map, Qt::Key_Return);
            QVERIFY(!editor->draft()); QCOMPARE(tools->statusText(), QStringLiteral("A"));
            QTest::keyClick(map, Qt::Key_A); QVERIFY(tools->statusText().isEmpty());
            action->trigger(); click(a); QTest::keyClick(map, Qt::Key_Return); QVERIFY(!editor->draft());
            QTest::keyClick(map, Qt::Key_Escape); QVERIFY(tools->statusText().isEmpty());
            click(QPoint(500, 100)); QVERIFY(!selection->item());
            window.close();
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        const auto *objects = restored.findChild<swak::UserObjects *>(); QVERIFY(objects->find(id));
        QCOMPARE(objects->find(id)->title, QStringLiteral("Test area")); QCOMPARE(swak::polygonVertices(objects->find(id)->geometry).size(), 3);
    }

    void markerMenuActivation()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("marker-menu.json")), QPluginLoader::staticInstances());
        window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *menu = window.findChild<QMenu *>(QStringLiteral("pluginsMenu")); QVERIFY(menu);
        auto *action = window.findChild<QAction *>(QStringLiteral("markerModeAction")); QVERIFY(action);
        QVERIFY(menu->actions().contains(action)); QCOMPARE(action->text(), QStringLiteral("Marker"));
        for (auto *entry : menu->actions()) QVERIFY(entry->text() != QStringLiteral("Edit marker"));
        auto *tools = window.findChild<swak::MapTools *>(); QVERIFY(tools);
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")); QVERIFY(editor);
        auto *objects = window.findChild<swak::UserObjects *>(); QVERIFY(objects);
        QVERIFY(!editor->isVisible());
        menu->popup(window.mapToGlobal(QPoint(400, 300))); QVERIFY(QTest::qWaitForWindowExposed(menu));
        QSignalSpy triggered(action, &QAction::triggered);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
        QCOMPARE(triggered.count(), 1);
        QVERIFY(!menu->isVisible());
        QCOMPARE(tools->statusText(), QStringLiteral("M")); QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
        QVERIFY(!editor->isVisible());
        QTest::keyClick(map, Qt::Key_Escape); QVERIFY(tools->statusText().isEmpty());
        action->trigger(); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(editor->isVisible()); QVERIFY(tools->statusText().isEmpty());
        editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"))->setText(QStringLiteral("Menu marker"));
        editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click(); QCOMPARE(objects->items().size(), 1);
        action->trigger(); QCOMPARE(tools->statusText(), QStringLiteral("M"));
        QTest::keyClick(map, Qt::Key_M); QVERIFY(tools->statusText().isEmpty());
    }
    void mapCredentialDialog()
    {
        swak::MapSources maps(QPluginLoader::staticInstances());
        const auto id = QStringLiteral("org.swak.xyz");
        auto source = maps.sources().value(id).toObject(); source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        QVERIFY(maps.put(id, source).isEmpty());
        swak::MapManager manager(maps, nullptr, [] { return QString{}; }); manager.show(); QVERIFY(QTest::qWaitForWindowExposed(&manager));
        auto *table = manager.findChild<QTableWidget *>(QStringLiteral("mapProviderList"));
        for (int row = 0; row < table->rowCount(); ++row) if (table->item(row, 1)->data(Qt::UserRole).toString() == id) table->setCurrentCell(row, 1);
        const auto enter = [&](const QString &text, bool accept) {
            bool seen = false;
            QTimer::singleShot(0, &manager, [&] {
                auto *dialog = manager.findChild<QDialog *>(QStringLiteral("mapCredentialsDialog")); QVERIFY(dialog); seen = true;
                const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                auto *secret = dialog->findChild<QLineEdit *>(QStringLiteral("mapSecret")); QVERIFY(secret); QVERIFY(secret->text().isEmpty());
                QCOMPARE(secret->echoMode(), QLineEdit::Password); secret->setText(text);
                QVERIFY(dialog->grab().save(QStringLiteral("map-credentials.png")));
                if (accept) dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click(); else dialog->reject();
            });
            manager.findChild<QPushButton *>(QStringLiteral("mapCredentials"))->click(); QVERIFY(seen);
        };
        enter(QStringLiteral("test-token"), true); QCOMPARE(maps.credential(id), QByteArray("test-token"));
        QVERIFY(!QJsonDocument(maps.savePreferences()).toJson().contains("test-token"));
        enter(QStringLiteral("cancelled-token"), false); QCOMPARE(maps.credential(id), QByteArray("test-token"));
        enter({}, true); QVERIFY(maps.credential(id).isEmpty());
        QTimer::singleShot(0, &manager, [&] {
            auto *dialog = manager.findChild<swak::SettingsDialog *>(); QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *mode = dialog->findChild<QComboBox *>(id + QStringLiteral("/authMode")); QVERIFY(mode);
            QCOMPARE(mode->currentData().toString(), QStringLiteral("bearer"));
            QVERIFY(dialog->grab().save(QStringLiteral("map-auth-settings.png")));
        });
        manager.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
        source.remove(QStringLiteral("authMode")); QVERIFY(maps.put(id, source).isEmpty());
        QTimer::singleShot(0, &manager, [&] {
            auto *dialog = manager.findChild<swak::SettingsDialog *>(); QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            QCOMPARE(dialog->findChild<QComboBox *>(id + QStringLiteral("/authMode"))->currentData().toString(), QStringLiteral("none"));
        });
        manager.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
        manager.findChild<QPushButton *>(QStringLiteral("mapCredentials"))->click(); QVERIFY(maps.credential(id).isEmpty());
    }
    void providerActivationTables()
    {
        swak::MapSources maps(QPluginLoader::staticInstances());
        swak::MapManager manager(maps);
        auto *table = manager.findChild<QTableWidget *>(QStringLiteral("mapProviderList")); QVERIFY(table);
        const auto first = table->item(0, 1)->data(Qt::UserRole).toString();
        const auto second = table->item(1, 1)->data(Qt::UserRole).toString();
        manager.findChild<QRadioButton *>(QStringLiteral("activeProvider/") + first)->click();
        QVERIFY(QTest::qWaitFor([&] { return maps.activeId() == first; }));
        table->setCurrentCell(1, 2);
        QCOMPARE(maps.activeId(), first);
        manager.findChild<QRadioButton *>(QStringLiteral("activeProvider/") + second)->click();
        QVERIFY(QTest::qWaitFor([&] { return maps.activeId() == second; }));
        QVERIFY(!manager.findChild<QRadioButton *>(QStringLiteral("activeProvider/") + first)->isChecked());
        manager.findChild<QPushButton *>(QStringLiteral("clearMapProvider"))->click();
        QVERIFY(maps.activeSource().isEmpty());
        for (auto *radio : manager.findChildren<QRadioButton *>()) QVERIFY(!radio->isChecked());
        TileServer server; QVERIFY(server.load(QFINDTESTDATA("fixtures"))); QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = "<kml><Placemark><Point><coordinates>0,0</coordinates></Point></Placemark></kml>";
        swak::DataSources feeds(QPluginLoader::staticInstances());
        feeds.restorePreferences({{QStringLiteral("sources"), QJsonObject{}}});
        for (const auto &id : {QStringLiteral("a"), QStringLiteral("b")}) {
            auto source = swak::dataDefaults();
            source.insert(QStringLiteral("format"), feeds.providers().first()->dataId());
            source.insert(QStringLiteral("name"), id);
            source.insert(QStringLiteral("attribution"), QStringLiteral("Test data"));
            source.insert(QStringLiteral("endpoint"), QStringLiteral("https://localhost:%1/%2.kml").arg(server.serverPort()).arg(id));
            source.insert(QStringLiteral("ignoreTls"), true);
            QVERIFY(feeds.put(id, source).isEmpty());
        }
        swak::DataManager dataManager(feeds, nullptr, [] { return QString{}; });
        auto *dataTable = dataManager.findChild<QTableWidget *>(QStringLiteral("dataProviderList")); QVERIFY(dataTable);
        for (const auto &id : {QStringLiteral("a"), QStringLiteral("b")}) {
            dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/") + id)->click();
            QVERIFY(QTest::qWaitFor([&] { return feeds.sources().value(id).toObject().value(QStringLiteral("enabled")).toBool(); }));
        }
        QVERIFY(dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/a"))->isChecked());
        QVERIFY(dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/b"))->isChecked());
        dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/a"))->click();
        QVERIFY(QTest::qWaitFor([&] { return !feeds.sources().value(QStringLiteral("a")).toObject().value(QStringLiteral("enabled")).toBool(); }));
        QVERIFY(feeds.sources().value(QStringLiteral("b")).toObject().value(QStringLiteral("enabled")).toBool());
        dataTable->setCurrentCell(0, 2);
        dataManager.findChild<QPushButton *>(QStringLiteral("removeDataProvider"))->click();
        QVERIFY(!feeds.sources().contains(QStringLiteral("a")));
        QVERIFY(feeds.sources().contains(QStringLiteral("b")));
    }
    void mapDoubleClickAndProviderSettings()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("map-center.json"));
        double expectedX = 0;
        double expectedY = 0;
        {
            swak::Window window(path, QPluginLoader::staticInstances()); window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
            auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map); QVERIFY(preferences);
            preferences->restorePreferences({{QStringLiteral("zoom"), 4}, {QStringLiteral("diskCacheMiB"), 256}});
            const QPoint target(map->width() / 2 + 64, map->height() / 2 - 32);
            expectedX = 0.5 + (target.x() - map->width() / 2.0) / 4096;
            expectedY = 0.5 + (target.y() - map->height() / 2.0) / 4096;
            QTest::mouseDClick(map, Qt::LeftButton, Qt::NoModifier, target);
            QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, target);
            const auto centered = preferences->savePreferences();
            QCOMPARE(centered.value(QStringLiteral("centerX")).toDouble(), expectedX);
            QCOMPARE(centered.value(QStringLiteral("centerY")).toDouble(), expectedY);
            QCOMPARE(centered.value(QStringLiteral("zoom")).toDouble(), 4.0);
            QCOMPARE(centered.value(QStringLiteral("diskCacheMiB")).toInt(), 256);
            bool tested = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>(); QVERIFY(dialog);
                const auto close = qScopeGuard([dialog] { dialog->reject(); });
                QVERIFY(dialog->findChild<QTableWidget *>(QStringLiteral("mapProviderList")));
                QVERIFY(dialog->findChild<QTableWidget *>(QStringLiteral("dataProviderList")));
                QVERIFY(!dialog->findChild<QComboBox *>(QStringLiteral("org.swak.maps/active")));
                auto *sections = dialog->findChild<QListWidget *>(QStringLiteral("settingsSections"));
                for (int i = 0; i < sections->count(); ++i)
                    if (sections->item(i)->data(Qt::UserRole).toString() == QStringLiteral("org.swak.maps")) sections->setCurrentRow(i);
                QVERIFY(QTest::qWaitForWindowExposed(dialog));
                QVERIFY(dialog->grab().save(QStringLiteral("provider-settings-smoke.png")));
                tested = true;
            });
            window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
            QVERIFY(tested);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        const auto saved = dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>())->savePreferences();
        QCOMPARE(saved.value(QStringLiteral("centerX")).toDouble(), expectedX);
        QCOMPARE(saved.value(QStringLiteral("diskCacheMiB")).toInt(), 256);
    }
    void compactToolbarAndKeys()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("compact.json")), QPluginLoader::staticInstances());
        window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar")); QVERIFY(toolbar);
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *tools = window.findChild<swak::MapTools *>(); QVERIFY(tools);
        auto *marker = window.findChild<QAction *>(QStringLiteral("markerModeAction"));
        auto *area = window.findChild<QAction *>(QStringLiteral("polygonModeAction")); QVERIFY(marker && area);
        QVERIFY(toolbar->actions().contains(marker)); QVERIFY(toolbar->actions().contains(area));
        QVERIFY(!marker->icon().isNull()); QVERIFY(!area->icon().isNull()); QCOMPARE(area->text(), QStringLiteral("Area"));
        QVERIFY(!toolbar->findChild<QLabel *>(QStringLiteral("toolbarPluginLabel")));
        QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        auto *markerButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(marker)); QVERIFY(markerButton);
        QTest::mouseClick(markerButton, Qt::LeftButton); QCOMPARE(tools->statusText(), QStringLiteral("M")); QVERIFY(marker->isChecked());
        auto *areaButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(area)); QVERIFY(areaButton);
        QTest::mouseClick(areaButton, Qt::LeftButton); QCOMPARE(tools->statusText(), QStringLiteral("A")); QVERIFY(area->isChecked()); QVERIFY(!marker->isChecked());
        QTest::mouseClick(areaButton, Qt::LeftButton); QVERIFY(tools->statusText().isEmpty()); QVERIFY(!area->isChecked());
        QTest::keyClick(map, Qt::Key_A); QCOMPARE(tools->statusText(), QStringLiteral("A"));
        QTest::keyClick(map, Qt::Key_A); QVERIFY(tools->statusText().isEmpty());
        auto *navigation = dynamic_cast<swak::SettingsParticipant *>(map); QVERIFY(navigation);
        navigation->restorePreferences({{QStringLiteral("panNorthKey"), QStringLiteral("W")}, {QStringLiteral("panWestKey"), QStringLiteral("A")}, {QStringLiteral("panSouthKey"), QStringLiteral("S")}, {QStringLiteral("panEastKey"), QStringLiteral("D")}, {QStringLiteral("stepWestKey"), QStringLiteral("Left")}});
        const auto keys = navigation->savePreferences();
        QCOMPARE(keys.value(QStringLiteral("panNorthKey")).toString(), QStringLiteral("Up")); QCOMPARE(keys.value(QStringLiteral("panWestKey")).toString(), QStringLiteral("Left"));
        QCOMPARE(keys.value(QStringLiteral("panSouthKey")).toString(), QStringLiteral("Down")); QCOMPARE(keys.value(QStringLiteral("panEastKey")).toString(), QStringLiteral("Right"));
        QVERIFY(keys.value(QStringLiteral("stepWestKey")).toString().isEmpty());
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("polygonEditor"));
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(editor); QVERIFY(settings);
        settings->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("P")}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("createKey")).toString(), QStringLiteral("A"));
        settings->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("B")}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("createKey")).toString(), QStringLiteral("B")); QVERIFY(area->toolTip().contains('B'));
        settings->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("P")}, {QStringLiteral("areaShortcutVersion"), 2}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("createKey")).toString(), QStringLiteral("P")); settings->restorePreferences({});
        QVERIFY(toolbar->grab().save(QStringLiteral("compact-toolbar.png")));
        const auto previousPalette = QApplication::palette();
        const auto previousStyle = QApplication::style()->objectName();
        const auto restorePalette = qScopeGuard([previousPalette, previousStyle] {
            if (auto *style = QStyleFactory::create(previousStyle)) QApplication::setStyle(style);
            QApplication::setPalette(previousPalette);
        });
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QPalette dark(QColor("#202020")); dark.setColor(QPalette::WindowText, Qt::white); dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::Window, QColor("#202020")); dark.setColor(QPalette::Button, QColor("#202020"));
        QApplication::setPalette(dark); window.setPalette(dark); toolbar->setPalette(dark);
        QVERIFY(QTest::qWaitFor([&] { const auto frame = toolbar->grab().toImage(); return frame.pixelColor(frame.width() - 4, frame.height() / 2).lightness() < 100; }, 1000));
        QVERIFY(toolbar->grab().save(QStringLiteral("compact-toolbar-dark.png")));
        for (const auto icon : {swak::UiIcon::Clear, swak::UiIcon::Marker, swak::UiIcon::Area, swak::UiIcon::Map, swak::UiIcon::Edit, swak::UiIcon::Move}) {
            const auto image = swak::uiIcon(icon); QVERIFY(!image.pixmap(QSize(24, 24), 2).isNull());
            QVERIFY(!image.pixmap(QSize(24, 24), QIcon::Disabled).isNull()); QVERIFY(!image.pixmap(QSize(24, 24), QIcon::Selected).isNull());
        }
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
            QCOMPARE(toolbar->actions().size(), 1);
            QVERIFY(!toolbar->findChild<QLabel *>(QStringLiteral("toolbarPluginLabel")));
            QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
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
            QVERIFY(dialog->informativeText().contains(QStringLiteral(SWAK_REVISION)));
            QVERIFY(QStringLiteral(SWAK_VERSION).contains(QStringLiteral("+g")) || QStringLiteral(SWAK_VERSION).endsWith(QStringLiteral("+unknown")));
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
        QCOMPARE(activated, 7);
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
            if (action->menu() && action->menu()->title() == QStringLiteral("CoT Messages")) events = action->menu();
        }
        QVERIFY(events);
        auto *clear = window.findChild<QAction *>(QStringLiteral("clearEventsAction"));
        QVERIFY(clear && events->actions().contains(clear));
        QCOMPARE(clear->text(), QStringLiteral("&Clear"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(toolbar && toolbar->actions().contains(clear));
        QVERIFY(!clear->icon().isNull());
        QCOMPARE(clear->toolTip(), QStringLiteral("Clear CoT Messages"));
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
    void pauseAndInformationTrace()
    {
        QTemporaryDir directory; swak::Window window(directory.filePath(QStringLiteral("pause.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        swak::Event event; event.uid = QStringLiteral("paused"); event.callsign = QStringLiteral("Paused unit"); event.type = QStringLiteral("a-f-G-U"); event.how = QStringLiteral("m-s");
        event.time = event.start = QDateTime::currentDateTimeUtc(); event.stale = event.time.addSecs(60); QVERIFY(window.store().accept(event).isEmpty());
        const auto before = window.store().motion().snapshot().first().world;
        map->setFocus(); QTest::keyClick(map, Qt::Key_Space); QVERIFY(window.store().motion().paused());
        event.longitude = 1; event.time = event.time.addMSecs(100); QVERIFY(window.store().accept(event).isEmpty());
        QCOMPARE(window.store().events().first().longitude, 1.0); QCOMPARE(window.store().motion().snapshot().first().world, before);
        QTest::keyClick(map, Qt::Key_Space); QVERIFY(!window.store().motion().paused());
        QCOMPARE(window.store().motion().snapshot().first().world, swak::project(0, 1));
        auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(selection);
        swak::MapItem item; item.owner = QStringLiteral("org.swak.events"); item.id = event.uid; item.label = event.callsign; item.type = event.type; item.properties.insert(QStringLiteral("How"), event.how);
        selection->select(item);
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("informationType"))->text(), QStringLiteral("Object / Friend / Ground / Unit (a-f-G-U)"));
        QCOMPARE(window.findChild<QTableWidget *>(QStringLiteral("informationProperties"))->item(0, 1)->text(), QStringLiteral("Machine generated / Simulation (m-s)"));
        QCOMPARE(swak::cotFieldDescription(swak::CotField::How, QStringLiteral("x-z")), QStringLiteral("Unknown category (x-z)"));
        QCOMPARE(swak::cotFieldDescription(swak::CotField::Type, QStringLiteral("a-f-G-Z")), QStringLiteral("Object / Friend / Ground / Unknown category (a-f-G-Z)"));
        auto *box = window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker")); QVERIFY(box); QVERIFY(!box->isChecked()); box->click();
        auto *trace = dynamic_cast<swak::SettingsParticipant *>(window.findChild<QObject *>(QStringLiteral("traceTool"))); QVERIFY(trace);
        QCOMPARE(trace->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
        QVERIFY(QTest::qWaitFor([&] { return window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker"))->isChecked(); }));
        auto *traceTool = dynamic_cast<swak::MapTool *>(window.findChild<QObject *>(QStringLiteral("traceTool"))); QVERIFY(traceTool);
        const auto traceImage = [&] { QImage image(800, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); QPainter painter(&image); traceTool->paint(painter, swak::Viewport{QSizeF(800, 200), swak::project(0, 1), 8}); return image; };
        event.longitude = 1.2; event.time = event.time.addMSecs(100); QVERIFY(window.store().accept(event).isEmpty());
        const auto trail = traceImage();
        map->setFocus(); QTest::keyClick(map, Qt::Key_Space); QVERIFY(window.store().motion().paused());
        event.longitude = 1.5; event.time = event.time.addMSecs(100); QVERIFY(window.store().accept(event).isEmpty());
        QCOMPARE(traceImage(), trail); QCOMPARE(window.store().events().first().longitude, 1.5);
        QTest::keyClick(map, Qt::Key_Space); QVERIFY(!window.store().motion().paused()); QVERIFY(traceImage() != trail);
        QVERIFY(window.grab().save(QStringLiteral("information-trace.png")));
        window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker"))->click();
        QCOMPARE(trace->savePreferences().value(QStringLiteral("targets")).toArray().size(), 0);
        item.kind = swak::MapItem::Kind::Polygon; item.owner = QStringLiteral("org.swak.polygons"); selection->select(item);
        QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker")));
    }
    void resetTraceSettings()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("trace-reset.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(window.findChild<QObject *>(QStringLiteral("traceTool"))); QVERIFY(settings);
            auto values = settings->savePreferences(); values.insert(QStringLiteral("positions"), 10); values.insert(QStringLiteral("opacity"), 0.35);
            values.insert(QStringLiteral("targets"), QJsonArray{QJsonObject{{QStringLiteral("owner"), QStringLiteral("org.swak.events")}, {QStringLiteral("id"), QStringLiteral("retained")}}});
            settings->restorePreferences(values);
            for (const bool apply : {false, true}) {
                QTimer::singleShot(0, &window, [&] {
                    auto *dialog = window.findChild<swak::SettingsDialog *>(); QVERIFY(dialog);
                    dialog->restoreNavigation({{QStringLiteral("settingsSections"), QStringLiteral("org.swak.trace")}});
                    auto *count = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.trace/positions")); QVERIFY(count); QCOMPARE(count->value(), 10.0);
                    auto *reset = dialog->findChild<QPushButton *>(QStringLiteral("org.swak.trace/restoreDefaults")); QVERIFY(reset); reset->click();
                    QCOMPARE(count->value(), 50.0);
                    QCOMPARE(dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.trace/opacity"))->value(), 0.7);
                    QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), 10);
                    QVERIFY(dialog->grab().save(QStringLiteral("trace-reset.png")));
                    dialog->findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"))->button(apply ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
                });
                window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
                QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), apply ? 50 : 10);
                QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
            }
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QObject *>(QStringLiteral("traceTool"))); QVERIFY(settings);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(), 50);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("opacity")).toDouble(), 0.7);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
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
            QCOMPARE(window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"))->windowTitle(), QStringLiteral("CoT Messages"));
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
                if (!tabs || tabs->count() != 3 || !panes || panes->count() != 9) {
                    dialog->reject(); QFAIL("The pane tab is incomplete."); return;
                }
                for (auto *label : dialog->findChildren<QLabel *>()) {
                    if (label->wordWrap()) { dialog->reject(); QFAIL("A Settings label wraps across lines."); return; }
                    if (label->buddy() && label->minimumWidth() < label->sizeHint().width()) {
                        dialog->reject(); QFAIL("A Settings field label can be clipped."); return;
                    }
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
                auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/how"));
                auto *enabled = dialog->findChild<QCheckBox *>(QStringLiteral("logging/enabled"));
                auto *severity = dialog->findChild<QComboBox *>(QStringLiteral("logging/severity"));
                auto *interval = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("application/saveInterval"));
                auto *zoom = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.map/zoom"));
                auto *width = dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.events/columns/0/width"));
                if (!buttons || !identity || !address || !enabled || !severity || !interval || !zoom || !width) {
                    dialog->reject(); QFAIL("A settings field is missing."); return;
                }
                identity->setText(QStringLiteral("configured"));
                address->setText(QStringLiteral("!"));
                buttons->button(QDialogButtonBox::Apply)->click();
                if (window.findChild<QLineEdit *>(QStringLiteral("identity"))->text() != initial) {
                    dialog->reject(); QFAIL("An invalid page allowed partial application."); return;
                }
                address->setText(QStringLiteral("h-e"));
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
            window.findChild<swak::Servers *>()->restorePreferences({{QStringLiteral("address"), QStringLiteral("::1")}, {QStringLiteral("port"), 17001}});
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
            QCOMPARE(window.findChild<swak::Servers *>()->sources().value(QStringLiteral("legacy-udp")).toObject().value(QStringLiteral("host")).toString(), QStringLiteral("::1"));
            QCOMPARE(window.findChild<swak::Servers *>()->sources().value(QStringLiteral("legacy-udp")).toObject().value(QStringLiteral("port")).toInt(), 17001);
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
            window.findChild<swak::Servers *>()->restorePreferences({{QStringLiteral("port"), 17654}});
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
            QVERIFY(!window.findChild<swak::Servers *>());
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
            QCOMPARE(window.findChild<swak::Servers *>()->sources().value(QStringLiteral("legacy-udp")).toObject().value(QStringLiteral("port")).toInt(), 17654);
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
    void cotFieldPickers()
    {
        QWidget host; QVBoxLayout layout(&host);
        auto *field = new QLineEdit(&host); field->setObjectName(QStringLiteral("value")); field->setAccessibleName(QStringLiteral("Type")); field->setMaxLength(128);
        layout.addWidget(swak::cotFieldEditor(field, swak::CotField::Type)); host.show(); QVERIFY(QTest::qWaitForWindowExposed(&host));
        auto *button = host.findChild<QPushButton *>(QStringLiteral("value/select")); QVERIFY(button);
        field->setText(QStringLiteral("a-f-G-U-C-I-custom")); button->click();
        auto *dialog = field->findChild<QDialog *>(); QVERIFY(dialog);
        auto *value = dialog->findChild<QLineEdit *>(QStringLiteral("cotPickerValue")); QCOMPARE(value->text(), field->text());
        value->setText(QStringLiteral("invalid code")); QVERIFY(!dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
        QTest::keyClick(dialog, Qt::Key_Escape); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(field->text(), QStringLiteral("a-f-G-U-C-I-custom"));
        field->setText(QStringLiteral("a-f-G-U")); button->click(); dialog = field->findChild<QDialog *>(); QVERIFY(dialog);
        auto *tree = dialog->findChild<QTreeWidget *>(); QVERIFY(tree->currentItem()); QCOMPARE(tree->currentItem()->text(1), field->text());
        auto matches = tree->findItems(QStringLiteral("a-h-G-E-V-C"), Qt::MatchExactly | Qt::MatchRecursive, 1); QCOMPARE(matches.size(), 1);
        tree->setCurrentItem(matches.first());
        QVERIFY(dialog->grab().save(QStringLiteral("cot-type-picker.png")));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(field->text(), QStringLiteral("a-h-G-E-V-C")); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (const auto kind : {swak::CotField::How, swak::CotField::Team, swak::CotField::Role}) {
            QWidget owner; auto *edit = new QLineEdit(&owner); edit->setMaxLength(kind == swak::CotField::How ? 3 : 64);
            auto *container = swak::cotFieldEditor(edit, kind); container->findChild<QPushButton *>()->click();
            auto *picker = edit->findChild<QDialog *>(); QVERIFY(picker); auto *list = picker->findChild<QTreeWidget *>();
            if (kind == swak::CotField::How) {
                const auto human = list->findItems(QStringLiteral("h"), Qt::MatchExactly, 1); QCOMPARE(human.size(), 1); list->setCurrentItem(human.first());
                QVERIFY(!picker->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
                const auto gps = list->findItems(QStringLiteral("m-g"), Qt::MatchExactly | Qt::MatchRecursive, 1); QCOMPARE(gps.size(), 1); list->setCurrentItem(gps.first());
            } else {
                QCOMPARE(list->topLevelItemCount(), kind == swak::CotField::Team ? 14 : 8);
                const QCollator collator;
                for (int i = 1; i < list->topLevelItemCount(); ++i) QVERIFY(collator.compare(list->topLevelItem(i - 1)->text(0), list->topLevelItem(i)->text(0)) <= 0);
                list->setCurrentItem(list->topLevelItem(0));
            }
            const auto selected = list->currentItem()->text(1);
            picker->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click(); QCOMPARE(edit->text(), selected);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            container->findChild<QPushButton *>()->click(); QPointer<QDialog> pending = edit->findChild<QDialog *>(); QVERIFY(pending);
            delete container; QVERIFY(pending.isNull());
        }
    }
    void cotPickerIntegration()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("pickers.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
            for (const auto *name : {"eventType", "locationMethod", "team", "role"}) QVERIFY(window.findChild<QPushButton *>(QString::fromLatin1(name) + QStringLiteral("/select")));
            window.findChild<QLineEdit *>(QStringLiteral("locationMethod"))->setText(QStringLiteral("m-g"));
            window.findChild<QLineEdit *>(QStringLiteral("team"))->setText(QStringLiteral("Dark Blue"));
            window.findChild<QLineEdit *>(QStringLiteral("role"))->setText(QStringLiteral("Medic"));
            window.findChild<QPushButton *>(QStringLiteral("generateEvent"))->click();
            const auto decoded = swak::decodeCot(window.findChild<QPlainTextEdit *>(QStringLiteral("xmlPreview"))->toPlainText().toUtf8());
            QVERIFY(decoded.event); QCOMPARE(decoded.event->how, QStringLiteral("m-g")); QCOMPARE(decoded.event->team, QStringLiteral("Dark Blue")); QCOMPARE(decoded.event->role, QStringLiteral("Medic"));
            QTimer::singleShot(0, &window, [&] {
                auto *settings = window.findChild<swak::SettingsDialog *>(); QVERIFY(settings);
                for (const auto *page : {"org.swak.identity", "org.swak.editor"}) for (const auto *name : {"type", "how", "team", "role"})
                    QVERIFY(settings->findChild<QPushButton *>(QString::fromLatin1(page) + '/' + QString::fromLatin1(name) + QStringLiteral("/select")));
                settings->reject();
            });
            window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("locationMethod"))->text(), QStringLiteral("m-g"));
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("team"))->text(), QStringLiteral("Dark Blue"));
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("role"))->text(), QStringLiteral("Medic"));
    }
    void cotAnimationRendering()
    {
        QTemporaryDir directory; swak::Window window(directory.filePath(QStringLiteral("motion.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (auto *dock : window.findChildren<QDockWidget *>()) dock->hide();
        auto *map = window.findChild<QOpenGLWidget *>(); auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto values = settings->savePreferences(); values.insert(QStringLiteral("zoom"), 8); values.insert(QStringLiteral("centerX"), 0.5); values.insert(QStringLiteral("centerY"), 0.5); settings->restorePreferences(values);
        auto *motion = window.findChild<swak::TrackMotion *>(); QVERIFY(motion);
        motion->restorePreferences({{QStringLiteral("duration"), 1000}, {QStringLiteral("fade"), 100}, {QStringLiteral("teleportKm"), 100}, {QStringLiteral("staleSeconds"), 1}});
        swak::Event event; event.uid = QStringLiteral("moving"); event.type = QStringLiteral("a-f-G-U"); event.how = QStringLiteral("m-g");
        event.time = event.start = QDateTime::currentDateTimeUtc(); event.stale = event.time.addSecs(120);
        QVERIFY(window.store().accept(event).isEmpty()); event.longitude = 0.5; QVERIFY(window.store().accept(event).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { const auto x = motion->snapshot().first().world.x(); return x > 0.5003 && x < 0.501; }, 800));
        const auto visual = motion->snapshot().first(); const auto logical = (visual.world - QPointF(0.5, 0.5)) * 65536 + QPointF(map->width() / 2.0, map->height() / 2.0);
        const auto image = map->grabFramebuffer(); const auto pixel = (logical * map->devicePixelRatioF()).toPoint();
        int blue = 0;
        for (int y = pixel.y() - 12; y <= pixel.y() + 12; ++y) for (int x = pixel.x() - 12; x <= pixel.x() + 12; ++x) { const auto color = image.pixelColor(x, y); if (color.blue() > 120 && color.blue() > color.red()) ++blue; }
        QVERIFY(blue >= 5);
        QVERIFY(QTest::qWaitFor([&] { return motion->snapshot().first().world == swak::project(event.latitude, event.longitude); }, 1500));
        window.store().expire(event.stale.addSecs(1)); QCOMPARE(window.store().rowCount(), 0);
        QVERIFY(QTest::qWaitFor([&] { return motion->snapshot().first().gray == 1; }, 500));
        const auto gray = map->grabFramebuffer(); QVERIFY(gray.save(QStringLiteral("cot-stale-gray.png")));
        const auto target = ((swak::project(event.latitude, event.longitude) - QPointF(0.5, 0.5)) * 65536 + QPointF(map->width() / 2.0, map->height() / 2.0)) * map->devicePixelRatioF();
        const auto color = gray.pixelColor(target.toPoint()); QVERIFY(qAbs(color.red() - color.blue()) < 20); QVERIFY(color.lightness() > 65);
        QVERIFY(QTest::qWaitFor([&] { return motion->snapshot().isEmpty(); }, 2000));
        QVERIFY(window.savePreferences());
    }
    void serverFormAndTrace()
    {
        auto values = swak::serverDefaults(); values.insert(QStringLiteral("enabled"), true);
        values.insert(QStringLiteral("protocol"), QStringLiteral("stcp")); values.insert(QStringLiteral("ca"), QStringLiteral("retained.pem"));
        swak::SettingsDialog dialog({{QStringLiteral("server"), QStringLiteral("Server"), swak::serverFields(), values, {}, {}}}, [] { return QString{}; }, nullptr, true);
        QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("server/enabled")));
        auto *protocol = dialog.findChild<QComboBox *>(QStringLiteral("server/protocol")); QVERIFY(protocol);
        auto *ca = dialog.findChild<QLineEdit *>(QStringLiteral("server/ca")); QVERIFY(ca); QVERIFY(!ca->isEnabled());
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("tls"))); QVERIFY(ca->isEnabled());
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("udp"))); QVERIFY(!ca->isEnabled()); QCOMPARE(ca->text(), QStringLiteral("retained.pem"));
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("stcp"))); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(dialog.grab().save(QStringLiteral("server-tcp-fields.png"))); dialog.hide();
        QTemporaryDir directory; swak::Window window(directory.filePath(QStringLiteral("trace.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *trace = dynamic_cast<swak::MapTool *>(window.findChild<QObject *>(QStringLiteral("traceTool"))); QVERIFY(trace);
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(trace); QVERIFY(settings);
        auto *selection = window.findChild<swak::MapSelection *>(); auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        swak::Event event; event.uid = QStringLiteral("traced"); event.type = QStringLiteral("a-f-G-U"); event.how = QStringLiteral("m-s");
        event.time = event.start = QDateTime::currentDateTimeUtc(); event.stale = event.time.addSecs(120);
        QVERIFY(window.store().accept(event).isEmpty());
        swak::MapItem item; item.owner = QStringLiteral("org.swak.events"); item.id = event.uid; item.label = QStringLiteral("Trace test"); item.point = swak::project(0, 0);
        selection->select(item); window.store().select(event.uid); QVERIFY(dock->isVisible());
        QDialog unrelated(&window); unrelated.show(); unrelated.close(); QVERIFY(selection->item());
        dock->close();
        QVERIFY(!selection->item()); QVERIFY(window.store().selected().isEmpty());
        event.longitude = 0.001; event.time = event.time.addMSecs(100); QVERIFY(window.store().accept(event).isEmpty()); QVERIFY(!dock->isVisible());
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map); auto mapValues = mapSettings->savePreferences();
        mapValues.insert(QStringLiteral("centerX"), 0.5); mapValues.insert(QStringLiteral("centerY"), 0.5); mapValues.insert(QStringLiteral("zoom"), 12); mapSettings->restorePreferences(mapValues);
        bool offered = false;
        QTimer::singleShot(0, &window, [&] {
            auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(popup);
            auto *entry = popup->findChild<QAction *>(QStringLiteral("traceMarker")); QVERIFY(entry); offered = true; entry->trigger(); popup->close();
        });
        const auto center = map->rect().center(); QContextMenuEvent context(QContextMenuEvent::Mouse, center, map->mapToGlobal(center)); QApplication::sendEvent(map, &context); QVERIFY(offered);
        QMenu menu; trace->populateContextMenu(item, menu); auto *action = menu.findChild<QAction *>(QStringLiteral("traceMarker")); QVERIFY(action); QVERIFY(action->isChecked());
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
        QList<swak::Event> batch;
        for (int i = 1; i <= 52; ++i) { event.longitude = i * 0.0025; event.time = event.time.addMSecs(100); batch.append(event); }
        window.store().acceptBatch(batch);
        const swak::Viewport view{QSizeF(600, 200), swak::project(0, 0.06), 12};
        QImage image(600, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        { QPainter painter(&image); trace->paint(painter, view); }
        const auto older = view.screen(0, 0.00875).toPoint(); const auto newer = view.screen(0, 0.12875).toPoint();
        QVERIFY(image.pixelColor(newer).alpha() > image.pixelColor(older).alpha());
        QVERIFY(image.pixelColor(newer).alpha() > 0); QCOMPARE(image.pixelColor(view.screen(0, 0.00375).toPoint()).alpha(), 0);
        QVERIFY(image.save(QStringLiteral("marker-trace.png")));
        action->trigger(); QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 0);
        image.fill(Qt::transparent); { QPainter painter(&image); trace->paint(painter, view); } QCOMPARE(image.pixelColor(newer).alpha(), 0);
    }
    void emulatorMap()
    {
        QTemporaryDir directory; swak::Window window(directory.filePath(QStringLiteral("emulator-ui.json")), QPluginLoader::staticInstances());
        window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        for (auto *dock : window.findChildren<QDockWidget *>()) dock->hide();
        auto *participant = dynamic_cast<swak::SettingsParticipant *>(map); QVERIFY(participant);
        auto settings = participant->savePreferences(); const auto center = swak::project(59.33, 18.0);
        settings.insert(QStringLiteral("zoom"), 11); settings.insert(QStringLiteral("centerX"), center.x()); settings.insert(QStringLiteral("centerY"), center.y());
        participant->restorePreferences(settings); const auto before = map->grabFramebuffer();
        QTcpServer reserve; QVERIFY(reserve.listen(QHostAddress::LocalHost, 0)); const auto port = reserve.serverPort(); reserve.close();
        struct Child final : QProcess {
            ~Child() override { if (state() != NotRunning) { terminate(); if (!waitForFinished(2000)) { kill(); waitForFinished(2000); } } }
        } process;
        process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/swak_cot_emulator"),
                      {QStringLiteral("--port"), QString::number(port), QStringLiteral("--frequency"), QStringLiteral("5"), QStringLiteral("--duration"), QStringLiteral("10")});
        QVERIFY(process.waitForStarted());
        auto source = swak::serverDefaults(); source.insert(QStringLiteral("name"), QStringLiteral("Emulator")); source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        source.insert(QStringLiteral("protocol"), QStringLiteral("stcp")); source.insert(QStringLiteral("port"), port); source.insert(QStringLiteral("enabled"), true); source.insert(QStringLiteral("retrySeconds"), 1);
        auto *servers = window.findChild<swak::Servers *>(); QVERIFY(servers->put(QStringLiteral("emulator"), source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return window.store().rowCount() == 22; }, 5000));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped); map->update(); QVERIFY(frames.wait(2000));
        const auto image = map->grabFramebuffer(); QVERIFY(image != before); QVERIFY(image.save(QStringLiteral("emulator-map.png")));
        int red = 0, blue = 0;
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (color.red() > 180 && color.red() > color.blue() + 80) ++red;
            if (color.blue() > 180 && color.blue() > color.red() + 80) ++blue;
        }
        QVERIFY(red > 20); QVERIFY(blue > 20);
        process.terminate(); QVERIFY(process.waitForFinished(3000)); QCOMPARE(process.exitCode(), 0);
    }
    void serverBrowserAndIdentity()
    {
        QTemporaryDir directory; swak::Window window(directory.filePath(QStringLiteral("servers-ui.json")), QPluginLoader::staticInstances());
        window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *servers = window.findChild<swak::Servers *>(); QVERIFY(servers);
        auto *list = window.findChild<QTableWidget *>(QStringLiteral("serverList")); QVERIFY(list); QCOMPARE(list->rowCount(), 0);
        auto *details = window.findChild<QPlainTextEdit *>(QStringLiteral("serverStatus")); QVERIFY(details); QVERIFY(details->isReadOnly());
        bool emptyAdd = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("serverEditor")); QVERIFY(dialog);
            for (auto *field : dialog->findChildren<QLineEdit *>()) if (field->objectName().endsWith(QStringLiteral("/name"))) { emptyAdd = field->text().isEmpty(); }
            dialog->reject();
        });
        window.findChild<QPushButton *>(QStringLiteral("addServer"))->click(); QVERIFY(emptyAdd); QCOMPARE(list->rowCount(), 0);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("identityDialog")); QVERIFY(dialog);
            auto *uid = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/uid")); QVERIFY(uid); QVERIFY(uid->isReadOnly()); QVERIFY(!uid->text().isEmpty());
            const auto generated = uid->text(); QTest::keyClicks(uid, "blocked"); QCOMPARE(uid->text(), generated);
            auto *lock = dialog->findChild<QToolButton *>(QStringLiteral("org.swak.identity/uid/lock")); QVERIFY(lock); QVERIFY(!lock->icon().isNull());
            lock->click(); QVERIFY(!uid->isReadOnly()); uid->setText(QStringLiteral("sender-test"));
            lock->click(); QVERIFY(uid->isReadOnly());
            QVERIFY(dialog->grab().save(QStringLiteral("identity-locked.png")));
            dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/callsign"))->setText(QStringLiteral("Test Sender"));
            dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/team"))->setText(QStringLiteral("Blue"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        window.findChild<QAction *>(QStringLiteral("identityAction"))->trigger();
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("identityDialog")); QVERIFY(dialog);
            auto *uid = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/uid")); QVERIFY(uid->isReadOnly()); QCOMPARE(uid->text(), QStringLiteral("sender-test")); dialog->reject();
        });
        window.findChild<QAction *>(QStringLiteral("identityAction"))->trigger();
        QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), QStringLiteral("sender-test"));
        QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(), QStringLiteral("Test Sender"));
        QTcpServer harness; QVERIFY(harness.listen(QHostAddress::LocalHost, 0));
        auto source = swak::serverDefaults(); source.insert(QStringLiteral("name"), QStringLiteral("Harness")); source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        source.insert(QStringLiteral("protocol"), QStringLiteral("stcp")); source.insert(QStringLiteral("port"), harness.serverPort()); source.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers->put(QStringLiteral("harness"), source).isEmpty()); QCOMPARE(list->rowCount(), 1);
        QVERIFY(QTest::qWaitFor([&] { return harness.hasPendingConnections(); }, 2000)); auto *socket = harness.nextPendingConnection();
        auto *map = window.findChild<QOpenGLWidget *>(); QVERIFY(map);
        for (auto *dock : window.findChildren<QDockWidget *>()) dock->hide();
        auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map); auto settings = mapSettings->savePreferences();
        settings.insert(QStringLiteral("zoom"), 8); settings.insert(QStringLiteral("centerX"), 0.5); settings.insert(QStringLiteral("centerY"), 0.5); mapSettings->restorePreferences(settings);
        swak::Event event; event.uid = QStringLiteral("remote"); event.type = QStringLiteral("a-f-G-U"); event.how = QStringLiteral("m-g"); event.callsign = QStringLiteral("Received contact");
        event.time = event.start = QDateTime::currentDateTimeUtc(); event.stale = event.time.addSecs(120);
        const auto before = map->grabFramebuffer(); socket->write(swak::encodeCot(event));
        QVERIFY(QTest::qWaitFor([&] { return window.store().rowCount() == 1; }, 2000));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped); map->update(); QVERIFY(frames.wait(2000));
        const auto after = map->grabFramebuffer(); QVERIFY(after != before); QVERIFY(after.save(QStringLiteral("server-cot-map.png")));
        int markerPixels = 0; const auto imageCenter = after.rect().center();
        for (int y = imageCenter.y() - 12; y <= imageCenter.y() + 12; ++y) for (int x = imageCenter.x() - 12; x <= imageCenter.x() + 12; ++x) {
            const auto color = after.pixelColor(x, y); if (color.blue() > 120 && color.blue() > color.red()) ++markerPixels;
        }
        QVERIFY2(markerPixels >= 5, "The received CoT symbol is absent from the OpenGL frame.");
        const auto center = map->rect().center(); QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, center);
        auto *selection = window.findChild<swak::MapSelection *>(); QVERIFY(selection->item()); QCOMPARE(selection->item()->id, event.uid);
        QVERIFY(QTest::qWaitFor([&] { return servers->status(QStringLiteral("harness")).contains(QStringLiteral("Connected")); }, 2000));
        window.findChild<QCheckBox *>(QStringLiteral("sendGenerated"))->setChecked(true); window.findChild<QPushButton *>(QStringLiteral("generateEvent"))->click();
        QVERIFY(QTest::qWaitFor([&] { return socket->bytesAvailable() > 0; }, 2000)); const auto sent = swak::decodeCot(socket->readAll());
        QVERIFY(sent.event); QCOMPARE(sent.event->uid, QStringLiteral("sender-test")); QCOMPARE(sent.event->team, QStringLiteral("Blue"));
        auto *browser = window.findChild<QWidget *>(QStringLiteral("serverBrowser")); QVERIFY(browser->grab().save(QStringLiteral("server-browser.png")));
        servers->remove(QStringLiteral("harness")); QCOMPARE(list->rowCount(), 0);
        QTcpServer unused; QVERIFY(unused.listen(QHostAddress::LocalHost, 0)); const auto unusedPort = unused.serverPort(); unused.close();
        source.insert(QStringLiteral("port"), unusedPort); QVERIFY(servers->put(QStringLiteral("missing"), source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return details->toPlainText().contains(QStringLiteral("Connection refused")); }, 3000));
        QVERIFY(details->toPlainText().contains(QString::number(unusedPort))); details->selectAll(); details->copy();
        QCOMPARE(QApplication::clipboard()->text(), details->toPlainText());
        QVERIFY(browser->grab().save(QStringLiteral("server-connection-error.png")));
        QVERIFY(window.savePreferences());
    }
    void receiverRestarts()
    {
        QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("servers.json"));
        QUdpSocket reservation; QVERIFY(reservation.bind(QHostAddress::LocalHost, 0)); const auto port = reservation.localPort(); reservation.close();
        for (int cycle = 0; cycle < 2; ++cycle) {
            swak::Window window(path, QPluginLoader::staticInstances()); auto *servers = window.findChild<swak::Servers *>(); QVERIFY(servers);
            if (!cycle) servers->restorePreferences({{QStringLiteral("address"), QStringLiteral("127.0.0.1")}, {QStringLiteral("port"), port}, {QStringLiteral("running"), true}});
            QVERIFY(QTest::qWaitFor([&] { return servers->status(QStringLiteral("legacy-udp")).contains(QStringLiteral("Listening")); }, 5000)); QVERIFY(!reservation.bind(QHostAddress::LocalHost, port));
            QVERIFY(window.savePreferences());
        }
        QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
        { swak::Window window(path, QPluginLoader::staticInstances()); auto *servers = window.findChild<swak::Servers *>(); QVERIFY(QTest::qWaitFor([&] { return servers->status(QStringLiteral("legacy-udp")).contains(QStringLiteral("Cannot bind")); }, 5000)); QVERIFY(window.savePreferences()); }
        reservation.close();
        { swak::Window window(path, QPluginLoader::staticInstances()); auto *servers = window.findChild<swak::Servers *>(); QVERIFY(QTest::qWaitFor([&] { return servers->status(QStringLiteral("legacy-udp")).contains(QStringLiteral("Listening")); }, 5000)); servers->remove(QStringLiteral("legacy-udp")); QVERIFY(window.savePreferences()); }
        { swak::Window window(path, QPluginLoader::staticInstances()); QVERIFY(window.findChild<swak::Servers *>()->sources().isEmpty()); }
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
        QCOMPARE(window.findChild<swak::Servers *>()->sources().value(QStringLiteral("legacy-udp")).toObject().value(QStringLiteral("port")).toInt(), 6969);
        const auto values = dynamic_cast<swak::PreferenceParticipant *>(window.findChild<QOpenGLWidget *>(QStringLiteral("map")))->savePreferences();
        QCOMPARE(values.value(QStringLiteral("zoom")).toDouble(), std::ceil(std::log2(std::max(window.findChild<QOpenGLWidget *>()->width(), window.findChild<QOpenGLWidget *>()->height()) / 256.0)));
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
        const auto docks = window.findChildren<QDockWidget *>();
        QCOMPARE(docks.size(), 9);
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
        auto *mapConfiguration = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(mapConfiguration);
        const auto gridValues = mapConfiguration->savePreferences();
        auto withoutGrid = gridValues; withoutGrid.insert(QStringLiteral("gridVisible"), false);
        mapConfiguration->restorePreferences(withoutGrid);
        const QImage frame = map->grabFramebuffer();
        QVERIFY(!frame.isNull());
        QCOMPARE(frame.pixelColor(20, 21), QColor(swak::MapBackgroundRgb));
        mapConfiguration->restorePreferences(gridValues);
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
