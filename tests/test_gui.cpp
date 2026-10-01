#include "app/window.h"
#include "core/interfaces.h"
#include <QApplication>
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
#include <QHeaderView>
#include <QSpinBox>
#include <QJsonDocument>
#include <QFile>
#include <QUdpSocket>
#include <QLabel>

class GuiTest final : public QObject {
    Q_OBJECT
private slots:
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
        QCOMPARE(docks.size(), 4);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("toolsDock")));
        auto *viewMenu = window.findChild<QMenu *>(QStringLiteral("viewMenu"));
        auto *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
        auto *quit = window.findChild<QAction *>(QStringLiteral("quitAction"));
        QVERIFY(viewMenu && fileMenu && quit);
        QCOMPARE(viewMenu->actions().size(), 5);
        QVERIFY(fileMenu->actions().contains(quit));
        QCOMPARE(window.menuBar()->actions().first(), fileMenu->menuAction());
        QCOMPARE(quit->shortcut(), QKeySequence(QKeySequence::Quit));
        for (const auto *menu : window.findChildren<QMenu *>()) {
            if (menu != fileMenu) QVERIFY(!menu->actions().contains(quit));
        }
        for (auto *dock : docks) {
            QVERIFY(viewMenu->actions().contains(dock->toggleViewAction()));
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
        QVERIFY(frame.pixelColor(frame.width() / 2, frame.height() / 2) != frame.pixelColor(0, 0));
        QVERIFY(frame.save(QStringLiteral("map-smoke.png")));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(window.screen()->grabWindow(window.winId()).save(light ? QStringLiteral("gui-light.png") : QStringLiteral("gui-smoke.png")));
        window.resize(950, 700);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(map->width() >= 320);
        window.store().clear();
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
    GuiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_gui.moc"
