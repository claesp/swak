#include "app/data_manager.h"
#include "app/map_manager.h"
#include "app/marker_manager.h"
#include "app/plugin_panel.h"
#include "app/window.h"
#include "core/activity.h"
#include "core/chat.h"
#include "core/contacts.h"
#include "core/cot.h"
#include "core/cot_picker.h"
#include "core/data.h"
#include "core/delete_confirmation.h"
#include "core/emergency.h"
#include "core/file_shares.h"
#include "core/gdal_support.h"
#include "core/graphics.h"
#include "core/grid_types.h"
#include "core/icon_cache.h"
#include "core/identity.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_decorations.h"
#include "core/map_grid.h"
#include "core/map_label.h"
#include "core/map_selection.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/marker_collections.h"
#include "core/measure.h"
#include "core/menu_order.h"
#include "core/missions.h"
#include "core/modal.h"
#include "core/object_editor.h"
#include "core/object_sharing.h"
#include "core/package_import.h"
#include "core/polygon.h"
#include "core/projection.h"
#include "core/routing.h"
#include "core/servers.h"
#include "core/settings_dialog.h"
#include "core/tak_server_api.h"
#include "core/tiles.h"
#include "core/toolbar.h"
#include "core/track_history.h"
#include "core/ui_icons.h"
#include "core/user_objects.h"
#include "core/video_sources.h"
#include "https_harness.h"
#include "swak/version.h"
#include "tile_server.h"
#include "vector_fixture.h"

#include <QAbstractItemModelTester>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCollator>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPluginLoader>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSizeGrip>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QStyleFactory>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUdpSocket>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <memory>

static void answerAction(Qt::Key key = Qt::Key_Return)
{
    QTimer::singleShot(0, qApp, [key] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QCOMPARE(dialog->objectName(), QStringLiteral("actionConfirmation"));
        QCOMPARE(dialog->icon(), QMessageBox::NoIcon);
        QTest::keyClick(dialog, key);
    });
}
static void chooseMapObject(QWidget *map, const QString &label)
{
    QTimer::singleShot(0, map, [label] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        const auto close = qScopeGuard([menu] {
            menu->close();
        });
        QCOMPARE(menu->objectName(), QStringLiteral("mapObjectChooser"));
        for (auto *action : menu->actions()) {
            if (action->text().contains(label)) {
                action->trigger();
                return;
            }
        }
        QFAIL("Object is absent from the chooser");
    });
}

static void
withObjectContext(QWidget *map, const QString &label, std::function<void(QMenu *)> callback)
{
    QTimer::singleShot(0, map, [map, label, callback] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        const auto close = qScopeGuard([menu] {
            menu->close();
        });
        if (menu->objectName() != QStringLiteral("mapObjectChooser")) {
            callback(menu);
            return;
        }
        for (auto *action : menu->actions()) {
            if (!action->isSeparator() && (label.isEmpty() || action->text().contains(label))) {
                action->trigger();
                withObjectContext(map, label, callback);
                return;
            }
        }
        QFAIL("Object is absent from the chooser");
    });
}

class SelectionCanvas final : public QOpenGLWidget
{
public:
    swak::Viewport view;
    swak::MapSelection selection;
    swak::DataSources *data = nullptr;

protected:
    void paintGL() override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        painter.setRenderHint(QPainter::Antialiasing);
        if (data) {
            data->paint(painter, view);
        } else {
            selection.paint(painter, view);
        }
    }
};

class BrowserCapture final : public QObject
{
    Q_OBJECT
public:
    QUrl opened;
public slots:
    void open(const QUrl &url)
    {
        opened = url;
    }
};

class ExampleSettings final : public QObject, public swak::SettingsParticipant
{
public:
    bool enabled = false;
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.example");
    }
    QString settingsTitle() const override
    {
        return QStringLiteral("Example extension");
    }
    QList<swak::SettingField> settingsFields() const override
    {
        return {swak::booleanSetting(QStringLiteral("enabled"), QStringLiteral("Enabled"))};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("enabled"), enabled}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        enabled = values.value(QStringLiteral("enabled")).toBool();
    }
};

class ExampleMenu final : public QObject, public swak::MenuPlugin
{
    Q_OBJECT
    Q_INTERFACES(swak::MenuPlugin)
public:
    bool empty = false;
    int calls = 0;
    QString name() const override
    {
        return QStringLiteral("Example menu");
    }
    void populateMenu(swak::PluginServices &, QMenu &menu) override
    {
        if (empty) {
            return;
        }
        auto *action = menu.addAction(QStringLiteral("Example action"));
        connect(action, &QAction::triggered, this, [this] {
            ++calls;
        });
    }
};

class ExampleToolbar final : public QObject, public swak::ToolbarPlugin
{
    Q_OBJECT
    Q_INTERFACES(swak::ToolbarPlugin)
public:
    int calls = 0;
    QString name() const override
    {
        return QStringLiteral("Example toolbar");
    }
    void populateToolbar(swak::PluginServices &, QToolBar &toolbar, QMenu *menu) override
    {
        Q_ASSERT(!menu);
        auto *action = toolbar.addAction(
            toolbar.style()->standardIcon(QStyle::SP_DialogApplyButton), QStringLiteral("Example"));
        connect(action, &QAction::triggered, this, [this] {
            ++calls;
        });
    }
};

class ExampleMapTool final : public QObject, public swak::MapExtension
{
    Q_OBJECT
    Q_INTERFACES(swak::MapExtension)
public:
    QString mapId() const override
    {
        return QStringLiteral("example.tool");
    }
    QString mapName() const override
    {
        return QStringLiteral("Example map tool");
    }
    Kind mapKind() const override
    {
        return Kind::Tool;
    }
};

static void answerDeletion(Qt::Key key = Qt::Key_Return)
{
    QTimer::singleShot(0, qApp, [key] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QCOMPARE(dialog->objectName(), QStringLiteral("deleteConfirmation"));
        QTest::keyClick(dialog, key);
    });
}

// Uses Qt file dialogs while it exists, so a test can find them. A desktop theme can supply
// native dialogs that a test cannot control.
struct QtDialogs
{
    QtDialogs()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    }
    ~QtDialogs()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native);
    }
    QtDialogs(const QtDialogs &) = delete;
    QtDialogs &operator=(const QtDialogs &) = delete;
    bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
};
class GuiTest final : public QObject
{
    Q_OBJECT
private slots:
    void objectsPaneAndRoutePicking()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath("objects.json"), QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *table = window.findChild<QTableView *>(QStringLiteral("objectsTable"));
        QVERIFY(table);
        QAbstractItemModelTester tester(table->model(),
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(table->model()->columnCount(), 2);
        QCOMPARE(table->model()->headerData(0, Qt::Horizontal).toString(), QStringLiteral("Title"));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto *selection = window.findChild<swak::MapSelection *>();
        auto *tools = window.findChild<swak::MapTools *>();
        QVERIFY(settings && selection && tools);
        swak::Event event;
        event.uid = QStringLiteral("object-one");
        event.callsign = QStringLiteral("Unit One");
        event.type = QStringLiteral("a-f-G");
        event.how = QStringLiteral("h-e");
        event.latitude = 20;
        event.longitude = 20;
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addSecs(60);
        QVERIFY(window.store().accept(event).isEmpty());
        QCOMPARE(table->model()->rowCount(), 1);
        auto values = settings->savePreferences();
        values.insert("zoom", 6);
        settings->restorePreferences(values);
        const auto selected = window.store().selected();
        table->doubleClicked(table->model()->index(0, 0));
        QCOMPARE(window.store().selected(), selected);
        QVERIFY(!selection->item());
        const auto center = swak::project(20, 20);
        QCOMPARE(settings->savePreferences().value("centerX").toDouble(), center.x());
        QCOMPARE(settings->savePreferences().value("centerY").toDouble(), center.y());
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.objects.pane"));
        QVERIFY(dock);
        dock->show();
        dock->raise();
        QTimer::singleShot(0, &window, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QStringList names;
            for (auto *action : menu->actions()) {
                names.append(action->objectName());
            }
            QVERIFY(names.contains(QStringLiteral("mapItemInformation")));
            QVERIFY(names.contains(QStringLiteral("headingConeObject")));
            QVERIFY(names.contains(QStringLiteral("traceMarker")));
            QVERIFY(names.contains(QStringLiteral("predictMarker")));
            for (auto *action : menu->actions()) {
                if (action->objectName() == QStringLiteral("mapItemInformation")) {
                    action->trigger();
                }
            }
            menu->close();
        });
        const auto point = table->visualRect(table->model()->index(0, 0)).center();
        QContextMenuEvent context(
            QContextMenuEvent::Mouse, point, table->viewport()->mapToGlobal(point));
        QApplication::sendEvent(table->viewport(), &context);
        QCOMPARE(window.store().selected(), event.uid);
        QVERIFY(selection->item());
        event.callsign = QStringLiteral("Updated title");
        QVERIFY(window.store().accept(event).isEmpty());
        QCOMPARE(table->model()->rowCount(), 1);
        QCOMPARE(table->model()->index(0, 0).data().toString(), event.callsign);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->label, event.callsign);
        window.store().clear();
        QVERIFY(!selection->item());
        QCOMPARE(table->model()->rowCount(), 0);
        auto *routePane = window.findChild<QWidget *>(QStringLiteral("routePlanner"));
        auto *routeDock =
            window.findChild<QDockWidget *>(QStringLiteral("org.swak.route-planner.pane"));
        QVERIFY(routePane && routeDock);
        QCOMPARE(window.dockWidgetArea(routeDock), Qt::RightDockWidgetArea);
        auto *routeSettings = dynamic_cast<swak::SettingsParticipant *>(routePane);
        QVERIFY(routeSettings);
        // The pane has no provider choice. Settings selects the provider.
        QVERIFY(!routePane->findChild<QComboBox *>(QStringLiteral("routeProviderChoice")));
        QVERIFY(!routePane->findChild<QDoubleSpinBox *>());
        auto *startChoice = routePane->findChild<QComboBox *>(QStringLiteral("routeStart"));
        auto *destinationChoice =
            routePane->findChild<QComboBox *>(QStringLiteral("routeDestination"));
        auto *pick = routePane->findChild<QToolButton *>(QStringLiteral("routeStartPick"));
        QVERIFY(startChoice && destinationChoice && pick &&
                routePane->findChild<QToolButton *>(QStringLiteral("routeDestinationPick")));
        auto *modeChoice = routePane->findChild<QComboBox *>(QStringLiteral("routeMode"));
        QVERIFY(modeChoice);
        // The travel mode is the first control in the pane.
        QCOMPARE(qobject_cast<QVBoxLayout *>(routePane->layout())->itemAt(0)->widget(),
                 static_cast<QWidget *>(modeChoice));
        auto *objects = window.findChild<swak::UserObjects *>();
        QVERIFY(objects);
        swak::UserObject depot;
        depot.id = QStringLiteral("route-depot");
        depot.type = QStringLiteral("circle");
        depot.title = QStringLiteral("Depot");
        const auto depotPoint = swak::project(20.1, 20.5);
        depot.geometry = {{QStringLiteral("x"), depotPoint.x()},
                          {QStringLiteral("y"), depotPoint.y()}};
        QVERIFY(objects->put(depot).isEmpty());
        const int depotIndex = destinationChoice->findData(QStringLiteral("marker:route-depot"));
        QVERIFY(depotIndex > 0);
        QCOMPARE(destinationChoice->itemText(depotIndex), QStringLiteral("Depot"));
        for (auto *pane : window.findChildren<QDockWidget *>()) {
            pane->hide();
        }
        pick->click();
        QVERIFY(tools->guidesActive());
        QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(!tools->guidesActive());
        QVERIFY(map->cursor().shape() != Qt::CrossCursor);
        QCOMPARE(startChoice->currentData().toString(), QStringLiteral("map"));
        const auto latitude = routeSettings->savePreferences()
                                  .value(QStringLiteral("start"))
                                  .toObject()
                                  .value(QStringLiteral("latitude"))
                                  .toDouble();
        const swak::Viewport pickedView{map->size(), center, 6};
        QVERIFY(std::abs(latitude - pickedView.coordinate(map->rect().center()).y()) < 0.000002);
        pick->click();
        QTest::keyClick(map, Qt::Key_Escape);
        QVERIFY(!tools->guidesActive());
        QVERIFY(map->cursor().shape() != Qt::CrossCursor);
        auto *status = routePane->findChild<QLabel *>(QStringLiteral("routeStatus"));
        QVERIFY(status);
        destinationChoice->setCurrentIndex(0);
        routePane->findChild<QPushButton *>(QStringLiteral("calculateRoute"))->click();
        QVERIFY(status->text().contains(QStringLiteral("destination")));
        auto *routes = window.findChild<swak::RouteSources *>();
        QVERIFY(routes && !routes->busy());
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body =
                    R"({"code":"Ok","routes":[{"distance":4100,"duration":2800,"geometry":{"type":"LineString","coordinates":[[19.5,20],[20,20],[20.5,20.1]]}}]})";
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        QVERIFY(routes
                    ->put("local",
                          {{"name", "Local routing test"},
                           {"format", "org.swak.valhalla"},
                           {"endpoint",
                            QStringLiteral("http://127.0.0.1:%1/route").arg(server.serverPort())},
                           {"attribution", "Test route"}})
                    .isEmpty());
        routes->select("local");
        routeSettings->restorePreferences(
            {{QStringLiteral("start"),
              QJsonObject{{QStringLiteral("longitude"), 19.5}, {QStringLiteral("latitude"), 20}}},
             {QStringLiteral("destination"),
              QJsonObject{{QStringLiteral("marker"), QStringLiteral("route-depot")}}}});
        QCOMPARE(destinationChoice->currentText(), QStringLiteral("Depot"));
        QSignalSpy finished(routes, &swak::RouteSources::completed);
        window.findChild<QPushButton *>(QStringLiteral("calculateRoute"))->click();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return finished.count() == 1;
            },
            5000));
        QVERIFY(routes->result().error.isEmpty());
        QCOMPARE(routes->result().coordinates.size(), 3);
        // The planner stores the result as a Route marker.
        std::optional<swak::UserObject> planned;
        for (const auto &item : objects->items()) {
            if (item.type == QStringLiteral("route")) {
                planned = item;
            }
        }
        QVERIFY(planned);
        QCOMPARE(planned->title, QStringLiteral("Route from map point to Depot"));
        QCOMPARE(swak::routePoints(planned->geometry).size(), 3);
        QCOMPARE(planned->properties.value(QStringLiteral("Travel mode")).toString(),
                 QStringLiteral("Driving"));
        QCOMPARE(planned->properties.value(QStringLiteral("Route provider")).toString(),
                 QStringLiteral("Local routing test"));
        QVERIFY(
            !planned->properties.value(QStringLiteral("Provider distance")).toString().isEmpty());
        QVERIFY(status->text().contains(QStringLiteral("Estimated time")));
        QImage rendered;
        QVERIFY(QTest::qWaitFor(
            [&] {
                rendered = map->grabFramebuffer();
                int purple = 0;
                for (int y = 0; y < rendered.height(); ++y) {
                    for (int x = 0; x < rendered.width(); ++x) {
                        const auto color = rendered.pixelColor(x, y);
                        if (std::abs(color.red() - 139) < 10 && std::abs(color.green() - 92) < 10 &&
                            std::abs(color.blue() - 246) < 10) {
                            ++purple;
                        }
                    }
                }
                return purple > 50;
            },
            5000));
        QVERIFY(rendered.save(QStringLiteral("route-preview.png")));
        // The map attribution panel lists the route provider while the route exists.
        auto *credit = map->findChild<QLabel *>(QStringLiteral("mapAttribution"));
        QVERIFY(credit);
        QVERIFY(QTest::qWaitFor(
            [&] {
                map->update();
                return credit->isVisible() && credit->text().contains(QStringLiteral("Test route"));
            },
            5000));
        routeDock->show();
        routeDock->raise();
        QVERIFY(routePane->grab().save(QStringLiteral("route-pane.png")));
        auto *clearRoute = routePane->findChild<QPushButton *>(QStringLiteral("clearRoute"));
        QVERIFY(clearRoute);
        QCOMPARE(clearRoute->text(), QStringLiteral("Clear ..."));
        QVERIFY(!clearRoute->icon().isNull());
        answerAction(Qt::Key_Escape);
        clearRoute->click();
        QVERIFY(!status->text().isEmpty());
        QCOMPARE(destinationChoice->currentText(), QStringLiteral("Depot"));
        answerAction();
        clearRoute->click();
        QVERIFY(status->text().isEmpty());
        QCOMPARE(startChoice->currentIndex(), 0);
        QCOMPARE(destinationChoice->currentIndex(), 0);
        QCOMPARE(startChoice->findData(QStringLiteral("map")), -1);
        // Clear keeps the route, so its attribution stays until the route is deleted.
        map->update();
        QTest::qWait(50);
        QVERIFY(credit->text().contains(QStringLiteral("Test route")));
        objects->remove(planned->id);
        QVERIFY(QTest::qWaitFor(
            [&] {
                map->update();
                return !credit->isVisible() ||
                       !credit->text().contains(QStringLiteral("Test route"));
            },
            5000));
        auto *providerWidget =
            dynamic_cast<swak::SettingsWidgetProvider *>(routePane)->createSettingsWidget(&window);
        providerWidget->setWindowFlag(Qt::Window);
        providerWidget->resize(700, 400);
        providerWidget->show();
        QVERIFY(QTest::qWaitForWindowExposed(providerWidget));
        QVERIFY(providerWidget->grab().save(QStringLiteral("route-providers.png")));
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.valhalla/name"));
            auto *endpoint =
                dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.valhalla/endpoint"));
            QVERIFY(name && endpoint);
            QVERIFY(name->text().isEmpty() && endpoint->text().isEmpty());
            name->setText(QStringLiteral("Added route server"));
            endpoint->setText(QStringLiteral("http://127.0.0.1:%1/route").arg(server.serverPort()));
            auto *buttons =
                dialog->findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
            QVERIFY(buttons);
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        providerWidget->findChild<QPushButton *>(QStringLiteral("addRouteProvider"))->click();
        QCOMPARE(routes->sources().size(), 3);
        auto *routeTable =
            providerWidget->findChild<QTableWidget *>(QStringLiteral("routeProviderTable"));
        QVERIFY(routeTable);
        for (int row = 0; row < routeTable->rowCount(); ++row) {
            if (routeTable->item(row, 1)->text() == QStringLiteral("Added route server")) {
                routeTable->setCurrentCell(row, 1);
            }
        }
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.valhalla/name"));
            QVERIFY(name);
            QCOMPARE(name->text(), QStringLiteral("Added route server"));
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        providerWidget->findChild<QPushButton *>(QStringLiteral("editRouteProvider"))->click();
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QTest::keyClick(dialog, Qt::Key_Return);
        });
        providerWidget->findChild<QPushButton *>(QStringLiteral("removeRouteProvider"))->click();
        QCOMPARE(routes->sources().size(), 2);
        delete providerWidget;
        QVERIFY(window.savePreferences());
    }

    void revisedMenusAndSettings()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("revised.json"));
        swak::Window window(profile, QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *view = window.findChild<QMenu *>(QStringLiteral("viewMenu"));
        auto *reset = window.findChild<QAction *>(QStringLiteral("resetLayoutAction"));
        QVERIFY(view && reset);
        swak::sortMenuActions(*view);
        QCOMPARE(view->actions().last(), reset);
        QVERIFY(view->actions().at(view->actions().size() - 2)->isSeparator());
        QCOMPARE(reset->text(), QStringLiteral("Reset Layout ..."));
        auto *toolbar = window.findChild<QToolBar *>();
        toolbar->hide();
        answerAction(Qt::Key_Escape);
        reset->trigger();
        QVERIFY(toolbar->isHidden());
        answerAction();
        reset->trigger();
        QVERIFY(!toolbar->isHidden());
        auto *help = window.findChild<QMenu *>(QStringLiteral("helpMenu"));
        swak::sortMenuActions(*help);
        QCOMPARE(help->actions().first()->objectName(), QStringLiteral("creditsAction"));
        help->actions().first()->trigger();
        auto *credits = window.findChild<QDialog *>(QStringLiteral("creditsDialog"));
        QVERIFY(credits);
        auto *table = credits->findChild<QTableWidget *>(QStringLiteral("creditsTable"));
        QVERIFY(table && table->rowCount() >= 5);
        QCOMPARE(table->columnCount(), 5);
        QStringList libraries;
        for (int row = 0; row < table->rowCount(); ++row) {
            libraries.append(table->item(row, 0)->text());
        }
        for (const auto &name : {QStringLiteral("GDAL"),
                                 QStringLiteral("PROJ"),
                                 QStringLiteral("GEOS"),
                                 QStringLiteral("libcurl"),
                                 QStringLiteral("GDAL format libraries")}) {
            QVERIFY2(libraries.contains(name), qPrintable(name));
        }
        QVERIFY(!libraries.contains(QStringLiteral("protozero")));
        QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
        help->actions().first()->trigger();
        QCOMPARE(window.findChildren<QDialog *>(QStringLiteral("creditsDialog")).size(), 1);
        QVERIFY(credits->grab().save(QStringLiteral("credits.png")));
        credits->close();
        window.findChild<QAction *>(QStringLiteral("aboutAction"))->trigger();
        auto *about = window.findChild<QMessageBox *>(QStringLiteral("aboutDialog"));
        QVERIFY(about);
        QCOMPARE(about->icon(), QMessageBox::NoIcon);
        QVERIFY(about->text().contains(QStringLiteral("Yet another TAK client.")));
        about->close();
        const auto inspectProvider = [](QDialog &dialog, const QString &file) {
            dialog.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dialog));
            auto *notice = dialog.findChild<QLabel *>(QStringLiteral("providerNotice"));
            QVERIFY(notice && notice->wordWrap());
            QVERIFY(notice->height() >= notice->fontMetrics().lineSpacing() * 2);
            QVERIFY(notice->heightForWidth(notice->width()) <= notice->height());
            QVERIFY(dialog.grab().save(file));
            dialog.close();
        };
        swak::MapManager maps(*window.findChild<swak::MapSources *>(), &window);
        QCOMPARE(maps.findChild<QPushButton *>(QStringLiteral("clearMapProvider"))->text(),
                 QStringLiteral("No map"));
        inspectProvider(maps, QStringLiteral("map-providers.png"));
        swak::DataManager data(*window.findChild<swak::DataSources *>(), &window);
        inspectProvider(data, QStringLiteral("data-providers.png"));
        swak::MarkerManager markers(*window.findChild<swak::MarkerCollections *>(), &window, {});
        inspectProvider(markers, QStringLiteral("marker-collections.png"));
        bool inspected = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto labels = [](QListWidget *list) {
                QStringList result;
                for (int i = 0; i < list->count(); ++i) {
                    result.append(list->item(i)->text());
                }
                return result;
            };
            for (const auto &id : {QStringLiteral("settingsSections"),
                                   QStringLiteral("paneSettingsSections"),
                                   QStringLiteral("keybindingSettingsSections")}) {
                auto *list = dialog->findChild<QListWidget *>(id);
                QVERIFY(list);
                const auto names = labels(list);
                QVERIFY(names.contains(QStringLiteral("Circles")));
                QVERIFY(names.contains(QStringLiteral("Lines")));
                QVERIFY(!names.contains(QStringLiteral("Circle and line tools")));
                QVERIFY(!names.contains(QStringLiteral("Identity")));
            }
            const auto names =
                labels(dialog->findChild<QListWidget *>(QStringLiteral("settingsSections")));
            QVERIFY(names.contains(QStringLiteral("Grids")));
            inspected = true;
            dialog->reject();
        });
        window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
        QVERIFY(inspected);
    }
    void separateShapePreferences()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("migration.json"));
        {
            swak::Preferences old(profile);
            QVERIFY(old.save(
                {{QStringLiteral("version"), 1},
                 {QStringLiteral("components"),
                  QJsonObject{
                      {QStringLiteral("org.swak.shapes"),
                       QJsonObject{{QStringLiteral("lineWidth"), 4},
                                   {QStringLiteral("circleKey"), QStringLiteral("J")},
                                   {QStringLiteral("lineKey"), QStringLiteral("K")},
                                   {QStringLiteral("distanceKey"), QStringLiteral("D")}}}}}}));
        }
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            auto *circles = dynamic_cast<swak::SettingsParticipant *>(
                window.findChild<QObject *>(QStringLiteral("shapesEditor")));
            auto *lines = dynamic_cast<swak::SettingsParticipant *>(
                window.findChild<QObject *>(QStringLiteral("linesEditor")));
            QVERIFY(circles && lines);
            QCOMPARE(circles->savePreferences().value(QStringLiteral("lineWidth")).toDouble(), 4.0);
            QCOMPARE(lines->savePreferences().value(QStringLiteral("lineWidth")).toDouble(), 4.0);
            QCOMPARE(lines->savePreferences().value(QStringLiteral("lineKey")).toString(),
                     QStringLiteral("K"));
            auto values = lines->savePreferences();
            values.insert(QStringLiteral("lineWidth"), 2);
            lines->restorePreferences(values);
            QCOMPARE(circles->savePreferences().value(QStringLiteral("lineWidth")).toDouble(), 4.0);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *lines = dynamic_cast<swak::SettingsParticipant *>(
            restored.findChild<QObject *>(QStringLiteral("linesEditor")));
        QCOMPARE(lines->savePreferences().value(QStringLiteral("lineWidth")).toDouble(), 2.0);
    }
    void clearFlagsAndConeChoices()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("flags.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            auto &store = window.store();
            swak::Event event;
            event.uid = QStringLiteral("flags-target");
            event.type = QStringLiteral("a-f-G");
            event.how = QStringLiteral("m-g");
            event.time = event.start = QDateTime::currentDateTimeUtc();
            event.stale = event.time.addSecs(600);
            QVERIFY(store.accept(event).isEmpty());
            swak::MapItem item;
            item.owner = QStringLiteral("org.swak.events");
            item.id = event.uid;
            auto *tools = window.findChild<swak::MapTools *>();
            QVERIFY(tools);
            for (const auto &pair : {std::pair{"predictMarker", "clearPredictions"},
                                     std::pair{"traceMarker", "clearTraces"}}) {
                QMenu menu;
                tools->populateInformationMenu(item, menu);
                auto *flag = menu.findChild<QAction *>(QString::fromLatin1(pair.first));
                QVERIFY(flag && !flag->isChecked());
                flag->trigger();
                auto *clear = window.findChild<QAction *>(QString::fromLatin1(pair.second));
                QVERIFY(clear && !clear->icon().isNull());
                QCOMPARE(clear->text(), QStringLiteral("Clear ..."));
                answerAction(Qt::Key_Escape);
                clear->trigger();
                QMenu cancelled;
                tools->populateInformationMenu(item, cancelled);
                QVERIFY(
                    cancelled.findChild<QAction *>(QString::fromLatin1(pair.first))->isChecked());
                answerAction();
                clear->trigger();
                event.time = event.time.addSecs(1);
                event.longitude += .001;
                QVERIFY(store.accept(event).isEmpty());
                QMenu cleared;
                tools->populateInformationMenu(item, cleared);
                QVERIFY(
                    !cleared.findChild<QAction *>(QString::fromLatin1(pair.first))->isChecked());
            }
            QMenu menu;
            tools->populateInformationMenu(item, menu);
            auto *cone = menu.findChild<QAction *>(QStringLiteral("headingConeObject"));
            QVERIFY(cone && !cone->isChecked());
            cone->trigger();
            QVERIFY(store.motion().headingConeEnabled(event.uid));
            QVERIFY(!store.motion().headingConeEnabled(QStringLiteral("other")));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        QVERIFY(restored.store().motion().headingConeEnabled(QStringLiteral("flags-target")));
        for (const auto &id : {QStringLiteral("predictTool"), QStringLiteral("traceTool")}) {
            auto *settings =
                dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QObject *>(id));
            QVERIFY(settings);
            QVERIFY(
                settings->savePreferences().value(QStringLiteral("targets")).toArray().isEmpty());
        }
    }
    void graphicsLifecycle()
    {
        for (int i = 0; i < 20; ++i) {
            QOpenGLWidget widget;
            widget.resize(320, 240);
            widget.show();
            QVERIFY(QTest::qWaitForWindowExposed(&widget));
            QVERIFY(widget.isValid());
            QVERIFY(!widget.grabFramebuffer().isNull());
        }
    }
    void headingCone()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("heading.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        swak::MapLayer *layer = nullptr;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (QByteArray(plugin->metaObject()->className()).endsWith("TracksPlugin")) {
                layer = qobject_cast<swak::MapLayer *>(plugin);
            }
        }
        QVERIFY(layer);
        auto &store = window.store();
        auto values = store.motion().savePreferences();
        values.insert(QStringLiteral("duration"), 0);
        store.motion().restorePreferences(values);
        swak::Event event;
        event.uid = QStringLiteral("heading-test");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-g");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(300);
        const auto render = [&] {
            QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            layer->paint(painter,
                         {QSizeF(400, 400), swak::project(event.latitude, event.longitude), 8},
                         store);
            return image;
        };
        QVERIFY(store.accept(event).isEmpty());
        QCOMPARE(render().pixelColor(200, 160).alpha(), 0);
        event.course = 0;
        QVERIFY(store.accept(event).isEmpty());
        QCOMPARE(render().pixelColor(200, 160).alpha(), 0);
        store.motion().setHeadingConeEnabled(event.uid, true);
        QVERIFY(render().pixelColor(200, 160).alpha() > 20);
        QCOMPARE(render().pixelColor(200, 240).alpha(), 0);
        event.course = 90;
        QVERIFY(store.accept(event).isEmpty());
        QVERIFY(render().pixelColor(240, 200).alpha() > 20);
        QCOMPARE(render().pixelColor(200, 160).alpha(), 0);
        store.motion().setPaused(true);
        event.course = 180;
        QVERIFY(store.accept(event).isEmpty());
        QVERIFY(render().pixelColor(240, 200).alpha() > 20);
        store.motion().setPaused(false);
        QVERIFY(render().pixelColor(200, 240).alpha() > 20);
        event.course.reset();
        event.time = event.time.addSecs(10);
        event.longitude = .001;
        QVERIFY(store.accept(event).isEmpty());
        QVERIFY(render().pixelColor(240, 200).alpha() > 20);
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto mapValues = settings->savePreferences();
        const auto world = swak::project(event.latitude, event.longitude);
        mapValues.insert(QStringLiteral("centerX"), world.x());
        mapValues.insert(QStringLiteral("centerY"), world.y());
        settings->restorePreferences(mapValues);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(selection->item());
        QVERIFY(selection->item()->properties.value(QStringLiteral("Speed")).toString() !=
                QStringLiteral("Unknown"));
        QCOMPARE(selection->item()->properties.value(QStringLiteral("Heading source")).toString(),
                 QStringLiteral("Estimated"));
        QVERIFY(map->grabFramebuffer().save(QStringLiteral("heading-cone-map.png")));
        QVERIFY(window.grab().save(QStringLiteral("heading-information.png")));
        // A click inside the cone, away from the symbol and label, selects the object.
        const auto insideCone = map->rect().center() + QPoint(40, 14);
        selection->clear();
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, insideCone);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, event.uid);
        QCOMPARE(store.selected(), event.uid);
        // The FOV settings add an outline along the cone edges.
        auto *fov = dynamic_cast<swak::SettingsParticipant *>(
            window.findChild<QObject *>(QStringLiteral("fovSettings")));
        QVERIFY(fov);
        auto fovValues = fov->savePreferences();
        const int plainEdge = render().pixelColor(235, 180).alpha();
        fovValues.insert(QStringLiteral("headingConeOutline"), 3);
        fov->restorePreferences(fovValues);
        QVERIFY(render().pixelColor(235, 180).alpha() > plainEdge);
        fovValues.insert(QStringLiteral("headingConeEnabled"), false);
        fov->restorePreferences(fovValues);
        QCOMPARE(render().pixelColor(240, 200).alpha(), 0);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, insideCone);
        QVERIFY(!selection->item());
    }
    void gridTypesAndCoordinates()
    {
        swak::GridTypes grids;
        QCOMPARE(grids.entries().size(), 3);
        QCOMPARE(swak::GridTypes::defaults().value(QStringLiteral("opacity")).toDouble(), .3);
        QVERIFY(grids.settingsFields().isEmpty());
        QCOMPARE(grids.selected(), QStringLiteral("geographic"));
        QCOMPARE(swak::gridCoordinate(0, 3, "mgrs", 5), QStringLiteral("31NEA0000000000"));
        QVERIFY(swak::gridCoordinate(60, 4, "utm").startsWith(QStringLiteral("UTM 32N")));
        QVERIFY(swak::gridCoordinate(78, 20, "mgrs").startsWith(QStringLiteral("33X")));
        QVERIFY(swak::gridCoordinate(85, 20, "mgrs").startsWith('Z'));
        QVERIFY(swak::gridCoordinate(-85, 20, "mgrs").startsWith('B'));
        QVERIFY(swak::gridCoordinate(91, 0, "mgrs").isEmpty());
        QVERIFY(swak::gridCoordinate(0, 0, "unknown").isEmpty());
        for (const auto &field :
             {QStringLiteral("name"), QStringLiteral("system"), QStringLiteral("format")}) {
            auto bad = swak::GridTypes::defaults();
            bad.insert(field, "");
            QVERIFY(!swak::GridTypes::validate(bad).isEmpty());
        }
        auto hiddenName = swak::GridTypes::defaults();
        hiddenName.insert("name", QString(QChar(0x202e)));
        QVERIFY(!swak::GridTypes::validate(hiddenName).isEmpty());
        auto invalid = swak::GridTypes::defaults();
        invalid.insert("spacing", 0);
        QVERIFY(!grids.put("invalid", invalid).isEmpty());
        invalid = swak::GridTypes::defaults();
        invalid.insert("color", "red");
        QVERIFY(!grids.put("invalid", invalid).isEmpty());
        for (const auto *color : {"#12345g",
                                  "#1234567",
                                  "#12345",
                                  "123456#",
                                  "#\u0661\u0662\u0663\u0664\u0665\u0666"}) {
            invalid.insert("color", QString::fromUtf8(color));
            QVERIFY2(!swak::GridTypes::validate(invalid).isEmpty(), color);
        }
        for (const auto *color : {"#abcdef", "#ABCDEF", "#09aF3c"}) {
            invalid.insert("color", QString::fromUtf8(color));
            QVERIFY2(swak::GridTypes::validate(invalid).isEmpty(), color);
        }
        for (const auto &field : {QStringLiteral("opacity"), QStringLiteral("width")}) {
            invalid = swak::GridTypes::defaults();
            invalid.insert(field, 1000);
            QVERIFY(!swak::GridTypes::validate(invalid).isEmpty());
        }
        auto value = swak::GridTypes::defaults("mgrs");
        value.insert("name", "Custom grid");
        QVERIFY(grids.put("custom", value).isEmpty());
        grids.select("custom");
        const auto saved = grids.savePreferences();
        swak::GridTypes restored;
        restored.restorePreferences(saved);
        QCOMPARE(restored.savePreferences(), saved);
        restored.remove("custom");
        QVERIFY(restored.selected().isEmpty());
        restored.setVisible(true);
        QCOMPARE(restored.selected(), QStringLiteral("geographic"));
        restored.restorePreferences(
            {{"types", QJsonObject{{"bad", invalid}}}, {"selected", "bad"}});
        QVERIFY(restored.entries().isEmpty());
        QVERIFY(restored.selected().isEmpty());
        for (int i = 0; i < 32; ++i) {
            QVERIFY(restored.put(QString::number(i), value).isEmpty());
        }
        QVERIFY(!restored.put("overflow", value).isEmpty());
        restored.select("missing");
        QVERIFY(restored.selected().isEmpty());
        swak::GridTypes hiddenLegacy;
        hiddenLegacy.migrate({{"gridVisible", false}});
        QVERIFY(hiddenLegacy.selected().isEmpty());
        swak::GridTypes legacy;
        legacy.migrate({{"gridVisible", true}, {"gridOpacity", .2}});
        QCOMPARE(legacy.entries().value("geographic").toObject().value("opacity").toDouble(), .3);
        for (const auto &system :
             {QStringLiteral("geographic"), QStringLiteral("utm"), QStringLiteral("mgrs")}) {
            for (const auto position : {QPointF(4, 60),
                                        QPointF(20, 78),
                                        QPointF(20, 85),
                                        QPointF(20, -85),
                                        QPointF(179.9, 0)}) {
                QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::white);
                QPainter painter(&image);
                swak::paintGridType(
                    painter,
                    {QSizeF(800, 600), swak::project(position.y(), position.x()), 9},
                    swak::GridTypes::defaults(system));
                painter.end();
                QVERIFY(image.save(QStringLiteral("grid-%1-%2.png").arg(system).arg(position.y())));
            }
        }
        for (int format = 0; format < 3; ++format) {
            auto type = swak::GridTypes::defaults();
            type.insert("format", format);
            QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            const auto blank = image;
            QPainter painter(&image);
            swak::paintGridType(painter, {QSizeF(800, 600), swak::project(0, 0), 4}, type);
            painter.end();
            QVERIFY(image != blank);
        }
        for (const auto &system : {QStringLiteral("utm"), QStringLiteral("mgrs")}) {
            QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            const auto blank = image;
            QPainter painter(&image);
            swak::paintGridType(painter,
                                {QSizeF(800, 600), swak::project(0, 0), 2},
                                swak::GridTypes::defaults(system));
            painter.end();
            QVERIFY(image != blank);
        }
        std::unique_ptr<QWidget> widget(grids.createSettingsWidget(nullptr));
        widget->show();
        auto *table = widget->findChild<QTableWidget *>(QStringLiteral("gridTypesTable"));
        QVERIFY(table);
        auto *utm = widget->findChild<QRadioButton *>(QStringLiteral("activeProvider/utm"));
        QVERIFY(utm);
        utm->click();
        QCOMPARE(grids.selected(), QStringLiteral("utm"));
        QTimer::singleShot(0, widget.get(), [&] {
            auto *dialog =
                widget->findChild<swak::SettingsDialog *>(QStringLiteral("gridTypeEditor"));
            QVERIFY(dialog);
            auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("grid/name"));
            QVERIFY(name && name->text().isEmpty());
            name->setText(QStringLiteral("New test grid"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        widget->findChild<QPushButton *>(QStringLiteral("addGridType"))->click();
        QVERIFY(QTest::qWaitFor([&] {
            return table->rowCount() == 5;
        }));
        QVERIFY(widget->grab().save(QStringLiteral("grid-types-editor.png")));
        table->setCurrentCell(0, 1);
        const auto beforeEdit = grids.savePreferences();
        QTimer::singleShot(0, widget.get(), [&] {
            auto *dialog =
                widget->findChild<swak::SettingsDialog *>(QStringLiteral("gridTypeEditor"));
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>(QStringLiteral("grid/name"))->setText("Canceled name");
            dialog->reject();
        });
        widget->findChild<QPushButton *>(QStringLiteral("editGridType"))->click();
        QCOMPARE(grids.savePreferences(), beforeEdit);
        QTimer::singleShot(0, widget.get(), [&] {
            auto *dialog =
                widget->findChild<swak::SettingsDialog *>(QStringLiteral("gridTypeEditor"));
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>(QStringLiteral("grid/name"))->setText("Edited name");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        widget->findChild<QPushButton *>(QStringLiteral("editGridType"))->click();
        QVERIFY(grids.savePreferences() != beforeEdit);
        answerDeletion(Qt::Key_Escape);
        widget->findChild<QPushButton *>(QStringLiteral("deleteGridType"))->click();
        QCOMPARE(grids.entries().size(), 5);
        answerDeletion();
        widget->findChild<QPushButton *>(QStringLiteral("deleteGridType"))->click();
        QCOMPARE(grids.entries().size(), 4);
        widget->findChild<QPushButton *>(QStringLiteral("hideGrid"))->click();
        QVERIFY(grids.selected().isEmpty());
    }
    void mapViewDecorations()
    {
        QVERIFY(swak::mapScaleLegend({QSizeF(0, 0), {}, 12}).text.isEmpty());
        QVERIFY(swak::mapScaleLegend({QSizeF(800, 600), {0, 100}, 12}).text.isEmpty());
        const swak::Viewport view{QSizeF(800, 600), swak::project(59, 18), 12};
        const auto metric = swak::mapScaleLegend(view, QLocale(QLocale::Swedish, QLocale::Sweden));
        const auto imperial =
            swak::mapScaleLegend(view, QLocale(QLocale::English, QLocale::UnitedStates));
        QVERIFY(metric.pixels > 0 && metric.pixels <= 160);
        QVERIFY(metric.text.contains(QStringLiteral("m")));
        QVERIFY(imperial.text.contains(QStringLiteral("ft")) ||
                imperial.text.contains(QStringLiteral("mi")));
        auto closer = view;
        closer.zoom += 1;
        QVERIFY(swak::mapScaleLegend(closer).meters < swak::mapScaleLegend(view).meters);
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("map-view.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QCOMPARE(window.windowTitle(), QStringLiteral("SWAK"));
            auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainViews"));
            QCOMPARE(tabs->tabText(0), QStringLiteral("Map"));
            auto *map = window.findChild<QOpenGLWidget *>();
            QCOMPARE(tabs->widget(0), map);
            tabs->addTab(new QWidget(tabs), QStringLiteral("Test view"));
            tabs->setCurrentIndex(1);
            QVERIFY(!map->isVisible());
            tabs->setCurrentIndex(0);
            QVERIFY(map->isVisible());
            map->setFocus();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
            const auto before = settings->savePreferences().value("zoom").toInt();
            QTest::keyClick(map, Qt::Key_Q);
            QVERIFY(settings->savePreferences().value("zoom").toInt() > before);
            auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
            auto *mapButton = window.findChild<QToolButton *>(QStringLiteral("mapProviderButton"));
            auto *serverButton =
                window.findChild<QToolButton *>(QStringLiteral("serverProviderButton"));
            auto *dataButton =
                window.findChild<QToolButton *>(QStringLiteral("dataProviderButton"));
            QVERIFY(mapButton && serverButton && dataButton);
            QList<QWidget *> providerWidgets;
            for (auto *action : toolbar->actions()) {
                if (action->isSeparator()) {
                    break;
                }
                providerWidgets.append(toolbar->widgetForAction(action));
            }
            QCOMPARE(providerWidgets, (QList<QWidget *>{mapButton, serverButton, dataButton}));
            auto *gridAction = window.findChild<QAction *>(QStringLiteral("mapGridAction"));
            auto *grids = window.findChild<swak::GridTypes *>();
            QVERIFY(gridAction && grids && gridAction->isChecked());
            grids->select(QStringLiteral("utm"));
            gridAction->trigger();
            QVERIFY(grids->selected().isEmpty());
            QVERIFY(!gridAction->isChecked());
            gridAction->trigger();
            QCOMPARE(grids->selected(), QStringLiteral("utm"));
            gridAction->trigger();
            auto *scale = window.findChild<QAction *>(QStringLiteral("mapScaleAction"));
            auto *zoom = window.findChild<QAction *>(QStringLiteral("mapZoomAction"));
            QVERIFY(scale->isChecked() && zoom->isChecked());
            QCOMPARE(scale->text(), QStringLiteral("Scale"));
            QCOMPARE(zoom->text(), QStringLiteral("Zoom Level"));
            // Settings > Map uses the same names as the View menu.
            QStringList labels;
            for (const auto &field : settings->settingsFields()) {
                if (field.path == u"scaleLegend" || field.path == u"zoomIndicator") {
                    labels.append(field.label);
                }
            }
            QCOMPARE(labels, (QStringList{QStringLiteral("Scale"), QStringLiteral("Zoom Level")}));
            const swak::Viewport legendView{map->size(), {.5, .5}, 8};
            const auto legend = swak::mapZoomLegendRect(legendView);
            QVERIFY(legend.left() > map->width() / 2);
            QVERIFY(swak::mapZoomLegendRect({QSizeF(20, 20), {.5, .5}, 2}).isEmpty());
            const auto top = legend.topLeft().toPoint() + QPoint(10, 0);
            QTest::mouseDClick(map, Qt::LeftButton, Qt::NoModifier, top);
            QMouseEvent legendMove(QEvent::MouseMove,
                                   QPointF(top + QPoint(0, 20)),
                                   QPointF(map->mapToGlobal(top + QPoint(0, 20))),
                                   Qt::NoButton,
                                   Qt::LeftButton,
                                   Qt::NoModifier);
            QApplication::sendEvent(map, &legendMove);
            QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, top);
            QCOMPARE(settings->savePreferences().value("zoom").toInt(), 19);
            QTest::mouseClick(map,
                              Qt::LeftButton,
                              Qt::NoModifier,
                              QPoint(qRound(legend.center().x()), qRound(legend.top())));
            QCOMPARE(settings->savePreferences().value("zoom").toInt(), 19);
            auto *lock = window.findChild<QAction *>(QStringLiteral("mapNavigationLock"));
            lock->setChecked(true);
            QCOMPARE(lock->icon().pixmap(24, 24).toImage(),
                     swak::uiIcon(swak::UiIcon::Unlock).pixmap(24, 24).toImage());
            QTest::mouseClick(
                map, Qt::LeftButton, Qt::NoModifier, legend.bottomLeft().toPoint() + QPoint(10, 0));
            QCOMPARE(settings->savePreferences().value("zoom").toInt(), 19);
            lock->setChecked(false);
            QCOMPARE(lock->icon().pixmap(24, 24).toImage(),
                     swak::uiIcon(swak::UiIcon::Lock).pixmap(24, 24).toImage());
            QTest::mouseClick(
                map, Qt::LeftButton, Qt::NoModifier, legend.bottomLeft().toPoint() + QPoint(10, 0));
            QVERIFY(settings->savePreferences().value("zoom").toInt() < 19);

            QVERIFY(map->grabFramebuffer().save(QStringLiteral("map-decorations-frame.png")));
            QSignalSpy decorationFrames(map, &QOpenGLWidget::frameSwapped);
            map->update();
            QVERIFY(QTest::qWaitFor([&] {
                return !decorationFrames.isEmpty();
            }));
            QVERIFY(window.screen()
                        ->grabWindow(window.winId())
                        .save(QStringLiteral("map-tab-decorations.png")));
            scale->trigger();
            zoom->trigger();
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *gridAction = restored.findChild<QAction *>(QStringLiteral("mapGridAction"));
        auto *grids = restored.findChild<swak::GridTypes *>();
        QVERIFY(gridAction && grids && !gridAction->isChecked());
        gridAction->trigger();
        QCOMPARE(grids->selected(), QStringLiteral("utm"));
        for (const auto &id : grids->entries().keys()) {
            grids->remove(id);
        }
        QVERIFY(!gridAction->isEnabled());
        QVERIFY(!gridAction->isChecked());
        grids->setVisible(true);
        QVERIFY(grids->selected().isEmpty());
        QVERIFY(grids->put(QStringLiteral("only"), swak::GridTypes::defaults("mgrs")).isEmpty());
        QVERIFY(gridAction->isEnabled());
        gridAction->trigger();
        QCOMPARE(grids->selected(), QStringLiteral("only"));
        QVERIFY(!restored.findChild<QAction *>(QStringLiteral("mapScaleAction"))->isChecked());
        QVERIFY(!restored.findChild<QAction *>(QStringLiteral("mapZoomAction"))->isChecked());
    }
    void integerMapZoomAndBackground()
    {
        QTemporaryDir directory;
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        server.hold = true;
        swak::Window window(directory.filePath(QStringLiteral("zoom.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto *sources = window.findChild<swak::MapSources *>();
        QVERIFY(map && settings && sources);
        const auto zoom = [&] {
            return settings->savePreferences().value(QStringLiteral("zoom")).toDouble();
        };
        window.findChild<swak::GridTypes *>()->select({});
        const auto initial = map->grabFramebuffer();
        QCOMPARE(initial.pixelColor(20, 21), QColor(swak::MapBackgroundRgb));
        const auto id = QStringLiteral("org.swak.gdal.raster");
        auto source = sources->sources().value(id).toObject();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        source.insert(QStringLiteral("minZoom"), 3);
        source.insert(QStringLiteral("maxZoom"), 5);
        QVERIFY(sources->put(id, source).isEmpty());
        sources->select(id);
        QCOMPARE(zoom(), 3.0);
        bool bounded = false;
        for (const auto &field : settings->settingsFields()) {
            if (field.path == QStringLiteral("zoom")) {
                QCOMPARE(field.minimum, 3.0);
                QCOMPARE(field.maximum, 5.0);
                QCOMPARE(field.decimals, 0);
                bounded = true;
            }
        }
        QVERIFY(bounded);
        for (int i = 0; i < 5; ++i) {
            QTest::keyClick(map, Qt::Key_Plus);
        }
        QCOMPARE(zoom(), 5.0);
        QTest::keyClick(map, Qt::Key_Minus);
        QCOMPARE(zoom(), 4.0);
        const auto wheel = [&](int delta) {
            QWheelEvent event(QPointF(map->rect().center()),
                              QPointF(map->mapToGlobal(map->rect().center())),
                              {},
                              QPoint(0, delta),
                              Qt::NoButton,
                              Qt::NoModifier,
                              Qt::NoScrollPhase,
                              false);
            QApplication::sendEvent(map, &event);
        };
        wheel(60);
        QCOMPARE(zoom(), 4.0);
        wheel(60);
        QCOMPARE(zoom(), 5.0);
        wheel(120);
        QCOMPARE(zoom(), 5.0);
        wheel(-120);
        QCOMPARE(zoom(), 4.0);
        QTest::keyClick(map, Qt::Key_Home);
        QCOMPARE(zoom(), 3.0);
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("zoom"), 4.6);
        settings->restorePreferences(values);
        QCOMPARE(zoom(), 5.0);
        source.insert(QStringLiteral("minZoom"), 0);
        source.insert(QStringLiteral("maxZoom"), 2);
        QVERIFY(sources->put(id, source).isEmpty());
        QCOMPARE(
            zoom(),
            std::max(2.0, std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0))));
        for (int i = 0; i < 5; ++i) {
            QTest::keyClick(map, Qt::Key_Minus);
        }
        QCOMPARE(zoom(), std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0)));
        QTest::keyClick(map, Qt::Key_Plus);
        QCOMPARE(
            zoom(),
            std::max(2.0, std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0))));
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
            QVERIFY(legacy.save({{QStringLiteral("version"), 1},
                                 {QStringLiteral("components"),
                                  QJsonObject{{QStringLiteral("org.swak.maps"), catalog},
                                              {QStringLiteral("org.swak.map"),
                                               QJsonObject{{QStringLiteral("zoom"), 4.6}}}}}}));
        }
        swak::Window restored(legacyPath, QPluginLoader::staticInstances());
        auto *restoredMap =
            dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QOpenGLWidget *>());
        QVERIFY(restoredMap);
        QCOMPARE(restoredMap->savePreferences().value(QStringLiteral("zoom")).toDouble(), 5.0);
    }
    void takDrawingsLayer()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("drawings.json")),
                            QPluginLoader::staticInstances());
        swak::MapLayer *layer = nullptr;
        swak::MapLayer *points = nullptr;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            const QByteArray name(plugin->metaObject()->className());
            if (name.endsWith("TakDrawingsPlugin")) {
                layer = qobject_cast<swak::MapLayer *>(plugin);
            } else if (name.endsWith("TracksPlugin")) {
                points = qobject_cast<swak::MapLayer *>(plugin);
            }
        }
        auto *tools = window.findChild<swak::MapTools *>();
        auto *show = window.findChild<QAction *>(QStringLiteral("showTakDrawings"));
        swak::SettingsParticipant *settings = nullptr;
        for (auto *object : window.findChildren<QObject *>()) {
            auto *participant = dynamic_cast<swak::SettingsParticipant *>(object);
            if (participant &&
                participant->preferenceId() == QStringLiteral("org.swak.takdrawings")) {
                settings = participant;
            }
        }
        QVERIFY(layer && points && tools && show && settings);
        QVERIFY(show->isChecked());
        auto &store = window.store();
        const auto drawing = [](const QString &uid, const QString &type, const QString &detail) {
            swak::Event event;
            event.uid = uid;
            event.type = type;
            event.how = QStringLiteral("h-e");
            event.callsign = uid;
            event.latitude = 59;
            event.longitude = 18;
            event.origin = QStringLiteral("tak");
            event.time = event.start = QDateTime::currentDateTimeUtc();
            event.stale = event.time.addSecs(600);
            event.detail = detail;
            return event;
        };
        auto area =
            drawing(QStringLiteral("area"),
                    QStringLiteral("u-d-f"),
                    QStringLiteral("<link point=\"58.99,17.99\"/><link point=\"58.99,18.01\"/>"
                                   "<link point=\"59.01,18.01\"/><link point=\"59.01,17.99\"/>"
                                   "<link point=\"58.99,17.99\"/>"
                                   "<strokeColor value=\"-16777216\"/>"
                                   "<fillColor value=\"-65536\"/>"));
        QVERIFY(store.accept(area).isEmpty());
        QVERIFY(store
                    .accept(drawing(QStringLiteral("route"),
                                    QStringLiteral("b-m-r"),
                                    QStringLiteral("<link point=\"59.02,18.02\" relation=\"c\"/>"
                                                   "<link point=\"59.02,18.05\" relation=\"c\"/>")))
                    .isEmpty());
        QVERIFY(store
                    .accept(drawing(QStringLiteral("broken"),
                                    QStringLiteral("u-d-f"),
                                    QStringLiteral("<link point=\"59,18\"/>")))
                    .isEmpty());
        const swak::Viewport view{QSizeF(400, 400), swak::project(59, 18), 14};
        const auto render = [&](const swak::MapLayer &target) {
            QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            target.paint(painter, view, store);
            return image;
        };
        auto image = render(*layer);
        QVERIFY(image.save(QStringLiteral("tak-drawings.png")));
        auto center = image.pixelColor(200, 200);
        QVERIFY(center.alpha() == 255 && center.red() > 200 && center.green() < 60);
        // The CoT layer draws no point symbol for drawings.
        QCOMPARE(render(*points).pixelColor(200, 200).alpha(), 0);
        // A click inside the area or on the route selects the drawing.
        const auto ids = [&](QPointF position) {
            QStringList result;
            for (const auto &item : tools->itemsAt(view, position)) {
                if (item.owner == QStringLiteral("org.swak.events")) {
                    result.append(item.id);
                }
            }
            return result;
        };
        QVERIFY(ids({200, 200}).contains(QStringLiteral("area")));
        const auto routeStart = view.screen(59.02, 18.02);
        const auto routeEnd = view.screen(59.02, 18.05);
        QVERIFY(ids((routeStart + routeEnd) / 2 + QPointF(0, 3)).contains(QStringLiteral("route")));
        QVERIFY(ids({5, 395}).isEmpty());
        store.select(QStringLiteral("area"));
        QVERIFY(render(*layer).pixelColor(200, 200).red() > 200);
        // Opacity and visibility come from the settings.
        settings->restorePreferences(
            {{QStringLiteral("visible"), true}, {QStringLiteral("opacity"), 0.5}});
        center = render(*layer).pixelColor(200, 200);
        QVERIFY(center.alpha() > 100 && center.alpha() < 160);
        show->setChecked(false);
        QCOMPARE(render(*layer).pixelColor(200, 200).alpha(), 0);
        QVERIFY(ids({200, 200}).isEmpty());
        QVERIFY(!settings->savePreferences().value(QStringLiteral("visible")).toBool());
        settings->restorePreferences({});
        QVERIFY(show->isChecked());
        // A TAK spot marker uses its color element.
        auto spot = drawing(QStringLiteral("spot"),
                            QStringLiteral("b-m-p-s-m"),
                            QStringLiteral("<color argb=\"-16711936\"/>"));
        spot.latitude = 59.005;
        QVERIFY(store.accept(spot).isEmpty());
        const auto spotPoint = view.screen(59.005, 18).toPoint();
        auto spotColor = render(*points).pixelColor(spotPoint);
        QVERIFY(spotColor.green() > 200 && spotColor.red() < 80);
        spot.time = spot.time.addSecs(1);
        spot.detail = QStringLiteral("<color argb=\"-16776961\"/>");
        QVERIFY(store.accept(spot).isEmpty());
        spotColor = render(*points).pixelColor(spotPoint);
        QVERIFY(spotColor.blue() > 200 && spotColor.green() < 80);
        // An update moves the drawing.
        area.time = area.time.addSecs(1);
        area.detail.replace(QStringLiteral("58.99"), QStringLiteral("59.03"));
        area.detail.replace(QStringLiteral("59.01"), QStringLiteral("59.04"));
        QVERIFY(store.accept(area).isEmpty());
        QCOMPARE(render(*layer).pixelColor(200, 200).alpha(), 0);
    }
    void shareObjects()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("share.json")),
                            QPluginLoader::staticInstances());
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *tools = window.findChild<swak::MapTools *>();
        auto *sharing = window.findChild<swak::ObjectSharing *>(QStringLiteral("objectSharing"));
        auto *contacts = window.findChild<swak::Contacts *>(QStringLiteral("contacts"));
        QVERIFY(objects && tools && sharing && contacts);
        swak::UserObject marker;
        marker.id = QStringLiteral("m1");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Rally");
        marker.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.3}};
        QVERIFY(objects->put(marker).isEmpty());
        swak::MapItem item;
        item.owner = QStringLiteral("org.swak.markers");
        item.id = marker.id;
        const auto action = [&](const QString &name) -> QAction * {
            auto *menu = new QMenu(&window);
            tools->populateInformationMenu(item, *menu);
            return menu->findChild<QAction *>(name);
        };
        QVERIFY(!action(QStringLiteral("stopSharingObject")));
        action(QStringLiteral("shareObject"))->trigger();
        QVERIFY(sharing->isShared(marker.id));
        QVERIFY(!action(QStringLiteral("shareObject")));
        action(QStringLiteral("stopSharingObject"))->trigger();
        QVERIFY(!sharing->isShared(marker.id));
        // Without known users, the dialog explains how to find them and cannot share.
        bool empty = false;
        QTimer::singleShot(100, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("shareRecipientsDialog"));
            if (!dialog) {
                return;
            }
            auto *notice = dialog->findChild<QLabel *>(QStringLiteral("shareRecipientsEmpty"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            empty = notice && !notice->isHidden() && buttons &&
                    !buttons->button(QDialogButtonBox::Ok)->isEnabled();
            dialog->reject();
        });
        action(QStringLiteral("shareObjectWith"))->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return empty;
            },
            5000));
        QVERIFY(!sharing->isShared(marker.id));
        // A known user can receive the object.
        swak::Event user;
        user.uid = QStringLiteral("ANDROID-3");
        user.callsign = QStringLiteral("Echo");
        user.type = QStringLiteral("a-f-G-U-C");
        user.how = QStringLiteral("m-g");
        user.origin = QStringLiteral("tak");
        user.detail = QStringLiteral("<contact endpoint=\"*:-1:stcp\"/>");
        user.time = user.start = QDateTime::currentDateTimeUtc();
        user.stale = user.time.addSecs(600);
        QVERIFY(window.store().accept(user).isEmpty());
        contacts->rebuild();
        bool chosen = false;
        QTimer::singleShot(100, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("shareRecipientsDialog"));
            auto *list = dialog
                             ? dialog->findChild<QListWidget *>(QStringLiteral("shareRecipients"))
                             : nullptr;
            if (!list || list->count() != 1) {
                if (dialog) {
                    dialog->reject();
                }
                return;
            }
            list->item(0)->setCheckState(Qt::Checked);
            chosen = true;
            dialog->accept();
        });
        action(QStringLiteral("shareObjectWith"))->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return sharing->isShared(marker.id);
            },
            5000));
        QVERIFY(chosen);
        QCOMPARE(sharing->recipients(marker.id).size(), 1);
        QCOMPARE(sharing->recipients(marker.id).first().callsign, QStringLiteral("Echo"));
        // A share that fails shows the reason.
        auto *pending = action(QStringLiteral("stopSharingObject"));
        QVERIFY(pending);
        pending->trigger();
        auto *share = action(QStringLiteral("shareObject"));
        objects->remove(marker.id);
        QString reason;
        QTimer::singleShot(0, &window, [&] {
            if (auto *box = window.findChild<QMessageBox *>(QStringLiteral("shareError"))) {
                reason = box->text();
                box->accept();
            }
        });
        share->trigger();
        QVERIFY(!reason.isEmpty());
        QVERIFY(!sharing->isShared(marker.id));
        objects->undoStack().undo();
        // Received events and data features have no share actions.
        item.owner = QStringLiteral("org.swak.events");
        QVERIFY(!action(QStringLiteral("shareObjectWith")));
        // The plugin menu controls automatic sharing.
        auto *automatic = window.findChild<QAction *>(QStringLiteral("shareAutomatically"));
        QVERIFY(automatic && automatic->isCheckable() && !automatic->isChecked());
        automatic->trigger();
        QVERIFY(sharing->automatic());
        sharing->setAutomatic(false);
        QVERIFY(!automatic->isChecked());
    }
    void reportsPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("reports.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.reports.pane"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("reportStatus"));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(dock && status && servers);
        dock->show();
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        click(QStringLiteral("listReports"));
        QVERIFY(status->text().contains(QStringLiteral("REST API")));
        click(QStringLiteral("newReport"));
        QVERIFY(status->text().contains(QStringLiteral("REST API")));
        click(QStringLiteral("closeReport"));
        QCOMPARE(status->text(), QStringLiteral("Select a report."));
        click(QStringLiteral("reopenReport"));
        QCOMPARE(status->text(), QStringLiteral("Select a report."));
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("apiPort"), 9);
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        // New Report needs a title. A sent report waits for the server.
        const auto fill = [&](const QString &title) {
            QTimer::singleShot(0, &window, [&window, title] {
                auto *dialog = window.findChild<QDialog *>(QStringLiteral("newReportDialog"));
                if (!dialog) {
                    return;
                }
                dialog->findChild<QLineEdit *>(QStringLiteral("reportTitle"))->setText(title);
                dialog->findChild<QPlainTextEdit *>(QStringLiteral("reportDescription"))
                    ->setPlainText(QStringLiteral("Water over road"));
                dialog->accept();
            });
            click(QStringLiteral("newReport"));
        };
        fill(QStringLiteral("  "));
        QCOMPARE(status->text(), QStringLiteral("Type a report title."));
        fill(QStringLiteral("Bridge out"));
        QVERIFY(status->text().startsWith(QStringLiteral("Sending")) || !status->text().isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !status->text().startsWith(QStringLiteral("Sending"));
            },
            15000));
        click(QStringLiteral("listReports"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !status->text().contains(QStringLiteral("Reading"));
            },
            15000));
        auto *pane = dynamic_cast<swak::PreferenceParticipant *>(
            window.findChild<QWidget *>(QStringLiteral("reportsPane")));
        QVERIFY(pane);
        pane->restorePreferences(pane->savePreferences());
    }
    // Returns the open dialog with a name. A closed dialog can remain until its deletion.
    static QDialog *visibleDialog(const QWidget &window, const QString &name)
    {
        for (auto *dialog : window.findChildren<QDialog *>(name)) {
            if (dialog->isVisible()) {
                return dialog;
            }
        }
        return nullptr;
    }
    // Types a path into the next Qt file dialog that opens, and accepts it. An empty path cancels
    // the dialog. QFileDialog::selectFile does not change the name field while the field has
    // focus, so the timer sets the field. The timer stops with its context.
    static void chooseFile(QObject *context, const QString &path)
    {
        auto *timer = new QTimer(context);
        auto tries = std::make_shared<int>(0);
        QObject::connect(timer, &QTimer::timeout, timer, [timer, path, tries] {
            if (++*tries > 500) {
                timer->deleteLater();
                return;
            }
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *picker = qobject_cast<QFileDialog *>(widget);
                auto *name = picker ? picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))
                                    : nullptr;
                if (picker && picker->isVisible() && name) {
                    timer->deleteLater();
                    if (path.isEmpty()) {
                        picker->reject();
                        return;
                    }
                    name->setText(path);
                    static_cast<QDialog *>(picker)->accept();
                    return;
                }
            }
        });
        timer->start(20);
    }
    void editorFiles()
    {
        // The editor imports CoT files into the store and exports its XML preview.
        const QtDialogs dialogs;
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("editor.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"))->show();
        auto *label = window.findChild<QLabel *>(QStringLiteral("editorStatus"));
        auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("xmlPreview"));
        auto *importButton = window.findChild<QPushButton *>(QStringLiteral("importEvent"));
        auto *exportButton = window.findChild<QPushButton *>(QStringLiteral("exportEvent"));
        QVERIFY(label && preview && importButton && exportButton);
        const auto write = [&](const QString &name, const QByteArray &bytes) {
            const auto path = directory.filePath(name);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
                return QString{};
            }
            return path;
        };
        const auto import = [&](const QString &path) {
            chooseFile(&window, path);
            importButton->click();
        };
        swak::Event event;
        event.uid = QStringLiteral("imported-1");
        event.type = QStringLiteral("a-f-G-U-C");
        event.how = QStringLiteral("h-e");
        event.callsign = QStringLiteral("Kilo");
        event.latitude = 59.5;
        event.longitude = 18.25;
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(600);
        const auto valid = write(QStringLiteral("event.xml"), swak::encodeCot(event));
        QVERIFY(!valid.isEmpty());
        // Cancel changes nothing.
        const auto before = label->text();
        import({});
        QCOMPARE(label->text(), before);
        import(valid);
        QCOMPARE(label->text(),
                 QStringLiteral("The event was imported. Exports keep its detail fields."));
        QCOMPARE(window.store().rowCount(), 1);
        QVERIFY(preview->toPlainText().contains(QStringLiteral("imported-1")));
        QVERIFY(exportButton->isEnabled());
        // A file without an event, an unreadable file, and an event that the store refuses fail.
        import(write(QStringLiteral("other.xml"), "<kml/>"));
        QVERIFY(!label->text().isEmpty());
        QVERIFY(!label->text().startsWith(QStringLiteral("The event was imported")));
        const auto locked = write(QStringLiteral("locked.xml"), swak::encodeCot(event));
        QVERIFY(QFile::setPermissions(locked, {}));
        import(locked);
        QCOMPARE(label->text(), QStringLiteral("The file cannot be opened."));
        auto stale = event;
        stale.uid = QStringLiteral("stale-1");
        stale.stale = stale.time.addSecs(-60);
        import(write(QStringLiteral("stale.xml"), swak::encodeCot(stale)));
        QVERIFY(!label->text().startsWith(QStringLiteral("The event was imported")));
        QCOMPARE(window.store().rowCount(), 1);
        // Export writes the preview. A directory without write access fails.
        const auto saved = directory.filePath(QStringLiteral("saved.xml"));
        chooseFile(&window, saved);
        exportButton->click();
        QCOMPARE(label->text(), QStringLiteral("The XML preview was saved."));
        QFile output(saved);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(output.readAll()), preview->toPlainText());
        const auto readOnly = directory.filePath(QStringLiteral("read-only"));
        QVERIFY(QDir().mkdir(readOnly));
        QVERIFY(QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        chooseFile(&window, readOnly + QStringLiteral("/event.xml"));
        exportButton->click();
        QVERIFY(label->text().startsWith(QStringLiteral("The file cannot be saved: ")));
        chooseFile(&window, {});
        exportButton->click();
        QVERIFY(label->text().startsWith(QStringLiteral("The file cannot be saved: ")));
        QFile::setPermissions(
            readOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    void windowDialogs()
    {
        // Pane entries without a saved layout place the panes.
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("dialogs.json"));
        QFile profile(path);
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(
            QJsonDocument(
                QJsonObject{{QStringLiteral("version"), 1},
                            {QStringLiteral("panes"),
                             QJsonObject{{QStringLiteral("org.swak.editor.pane"),
                                          QJsonObject{{QStringLiteral("area"), 1},
                                                      {QStringLiteral("floating"), false},
                                                      {QStringLiteral("visible"), true}}},
                                         {QStringLiteral("org.swak.udp.pane"),
                                          QJsonObject{{QStringLiteral("area"), 99},
                                                      {QStringLiteral("visible"), false}}}}}})
                .toJson());
        profile.close();
        swak::Window window(path, QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *editor = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
        auto *udp = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
        QVERIFY(editor && udp);
        QCOMPARE(window.dockWidgetArea(editor), Qt::LeftDockWidgetArea);
        QVERIFY(!editor->isHidden());
        QVERIFY(udp->isHidden());
        // Apply in Identity checks and applies the values, and saves them.
        auto *identity = window.findChild<swak::Identity *>();
        QVERIFY(identity);
        bool applied = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = visibleDialog(window, QStringLiteral("identityDialog"));
            if (!dialog) {
                return;
            }
            dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/callsign"))
                ->setText(QStringLiteral("Lima"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
            applied = identity->savePreferences().value(QStringLiteral("callsign")).toString() ==
                      QStringLiteral("Lima");
            dialog->reject();
        });
        window.findChild<QAction *>(QStringLiteral("identityAction"))->trigger();
        QVERIFY(applied);
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().contains("\"Lima\""));
        // Apply in Servers saves the profile.
        bool serversShown = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<swak::SettingsDialog *>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            serversShown = dialog->windowTitle() == QStringLiteral("Servers");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
            dialog->reject();
        });
        window.findChild<QAction *>(QStringLiteral("serversAction"))->trigger();
        QVERIFY(serversShown);
        // A second request shows the open manager again instead of a new one.
        for (const auto *name :
             {"mapProvidersAction", "dataProvidersAction", "markerCollectionsAction"}) {
            auto *action = window.findChild<QAction *>(QString::fromLatin1(name));
            action->trigger();
            const auto before = window.findChildren<QDialog *>().size();
            for (auto *dialog : window.findChildren<QDialog *>()) {
                if (dialog->isVisible() && dialog->isWindow()) {
                    dialog->hide();
                }
            }
            action->trigger();
            QCOMPARE(window.findChildren<QDialog *>().size(), before);
        }
        for (auto *dialog : window.findChildren<QDialog *>()) {
            dialog->close();
        }
        // Pane settings detach a pane at a position, and move it to another edge.
        const auto paneSettings = [&](const std::function<void(swak::SettingsDialog *)> &change) {
            bool done = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                if (!dialog) {
                    return;
                }
                change(dialog);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
                done = true;
                dialog->reject();
            });
            window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
            return done;
        };
        const auto id = editor->objectName();
        QVERIFY(paneSettings([&](swak::SettingsDialog *dialog) {
            for (const auto *key : {"/floating", "/x", "/y", "/width", "/height", "/area"}) {
                QVERIFY2(dialog->findChild<QWidget *>(id + QString::fromLatin1(key)), key);
            }
            dialog->findChild<QCheckBox *>(id + QStringLiteral("/floating"))->setChecked(true);
            dialog->findChild<QDoubleSpinBox *>(id + QStringLiteral("/x"))->setValue(40);
            dialog->findChild<QDoubleSpinBox *>(id + QStringLiteral("/y"))->setValue(50);
            dialog->findChild<QDoubleSpinBox *>(id + QStringLiteral("/width"))->setValue(700);
            dialog->findChild<QDoubleSpinBox *>(id + QStringLiteral("/height"))->setValue(500);
        }));
        QVERIFY(editor->isFloating());
        // The pane keeps its minimum height.
        QCOMPARE(editor->pos(), QPoint(40, 50));
        QCOMPARE(editor->width(), 700);
        QVERIFY(paneSettings([&](swak::SettingsDialog *dialog) {
            dialog->findChild<QCheckBox *>(id + QStringLiteral("/floating"))->setChecked(false);
            auto *area = dialog->findChild<QComboBox *>(id + QStringLiteral("/area"));
            area->setCurrentIndex(area->findData(8));
        }));
        QVERIFY(!editor->isFloating());
        QCOMPARE(window.dockWidgetArea(editor), Qt::BottomDockWidgetArea);
    }
    void routePlannerDetails()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("routes.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *routePane = window.findChild<QWidget *>(QStringLiteral("routePlanner"));
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(routePane);
        auto *provider = dynamic_cast<swak::SettingsWidgetProvider *>(routePane);
        auto *routes = window.findChild<swak::RouteSources *>();
        auto *start = window.findChild<QComboBox *>(QStringLiteral("routeStart"));
        auto *destination = window.findChild<QComboBox *>(QStringLiteral("routeDestination"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("routeStatus"));
        QVERIFY(settings && provider && routes && start && destination && status);
        // Earlier versions saved four numbers. A zero pair was the default and means no point.
        settings->restorePreferences(
            {{QStringLiteral("points"), QJsonArray{20.0, 19.5, 21.0, 20.5}}});
        QCOMPARE(start->currentData().toString(), QStringLiteral("map"));
        QCOMPARE(destination->currentData().toString(), QStringLiteral("map"));
        const auto saved = settings->savePreferences();
        QCOMPARE(saved.value(QStringLiteral("start")).toObject().value(QStringLiteral("latitude")),
                 QJsonValue(20.0));
        QCOMPARE(saved.value(QStringLiteral("destination"))
                     .toObject()
                     .value(QStringLiteral("longitude")),
                 QJsonValue(20.5));
        settings->restorePreferences(
            {{QStringLiteral("points"), QJsonArray{0.0, 0.0, 21.0, 20.5}}});
        QCOMPARE(start->currentIndex(), 0);
        QCOMPARE(destination->currentData().toString(), QStringLiteral("map"));
        // A provider error shows in the status line.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 500 Server Error\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
                socket->disconnectFromHost();
            });
        });
        for (const auto &[id, name] : {std::pair{QStringLiteral("failing"), QStringLiteral("A")},
                                       std::pair{QStringLiteral("other"), QStringLiteral("B")}}) {
            QVERIFY(
                routes
                    ->put(id,
                          {{"name", name},
                           {"format", "org.swak.valhalla"},
                           {"endpoint",
                            QStringLiteral("http://127.0.0.1:%1/route").arg(server.serverPort())},
                           {"attribution", "Test route"}})
                    .isEmpty());
        }
        routes->select(QStringLiteral("failing"));
        settings->restorePreferences(
            {{QStringLiteral("points"), QJsonArray{20.0, 19.5, 21.0, 20.5}}});
        QSignalSpy finished(routes, &swak::RouteSources::completed);
        window.findChild<QPushButton *>(QStringLiteral("calculateRoute"))->click();
        QVERIFY(finished.wait(5000));
        QVERIFY(!routes->result().error.isEmpty());
        QCOMPARE(status->text(), routes->result().error);
        // The settings page selects the provider and sets the running speed.
        std::unique_ptr<QWidget> page(provider->createSettingsWidget(nullptr));
        page->show();
        auto *speed = page->findChild<QDoubleSpinBox *>();
        QVERIFY(speed);
        speed->setValue(12.5);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("runningSpeed")).toDouble(),
                 12.5);
        auto *other = page->findChild<QAbstractButton *>(QStringLiteral("activeProvider/other"));
        QVERIFY(other);
        other->click();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return routes->activeId() == QStringLiteral("other");
            },
            2000));
    }
    void shapeLabelHits()
    {
        // A click on a shape label selects the shape, also away from its geometry.
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("labels.json")),
                            QPluginLoader::staticInstances());
        auto *tools = window.findChild<swak::MapTools *>();
        auto *objects = window.findChild<swak::UserObjects *>();
        QVERIFY(tools && objects);
        const swak::Viewport view{QSizeF(800, 600), QPointF(0.5, 0.5), 12};
        const double pixel = 1 / view.scale();
        swak::UserObject ring;
        ring.id = QStringLiteral("ring");
        ring.type = QStringLiteral("range-circle");
        ring.title = QStringLiteral("A long circle title");
        // The radius is two pixels, so the title reaches far beyond the circle.
        ring.geometry = swak::polygonGeometry({QPointF(0.5, 0.5), QPointF(0.5 + 2 * pixel, 0.5)});
        const auto ringError = objects->put(ring);
        QVERIFY2(ringError.isEmpty(), qPrintable(ringError));
        swak::UserObject line;
        line.id = QStringLiteral("line");
        line.type = QStringLiteral("line");
        line.title = QStringLiteral("Line title");
        line.geometry = swak::polygonGeometry({QPointF(0.5 - 100 * pixel, 0.5 + 100 * pixel),
                                               QPointF(0.5 + 100 * pixel, 0.5 + 100 * pixel)});
        const auto lineError = objects->put(line);
        QVERIFY2(lineError.isEmpty(), qPrintable(lineError));
        const auto ids = [&](QPointF position) {
            QStringList result;
            for (const auto &item : tools->itemsAt(view, position)) {
                if (item.owner == QStringLiteral("org.swak.shapes")) {
                    result.append(item.id);
                }
            }
            return result;
        };
        // The circle is at (400, 300). Its title is centered there.
        QCOMPARE(ids({420, 300}), QStringList{QStringLiteral("ring")});
        // The line is at y = 400. Its label starts above the middle of the line.
        QCOMPARE(ids({420, 386}), QStringList{QStringLiteral("line")});
        QVERIFY(ids({400, 200}).isEmpty());
        QVERIFY(ids({700, 386}).isEmpty());
    }
    void objectEditorDetails()
    {
        swak::UserObjects objects;
        swak::MarkerCollections collections(QPluginLoader::staticInstances());
        swak::ObjectEditor editor(objects, collections, nullptr);
        editor.show();
        swak::UserObject marker;
        marker.id = QStringLiteral("m1");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Rally");
        marker.collection = editor.newCollection();
        marker.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.3}};
        editor.edit(marker);
        QVERIFY(editor.draft());
        // The description keeps at most 2048 characters.
        auto *description = editor.findChild<QPlainTextEdit *>(QStringLiteral("objectDescription"));
        description->setPlainText(QString(3000, QLatin1Char('x')));
        QCOMPARE(description->toPlainText().size(), 2048);
        // Remove property removes the selected row, and nothing without a selection.
        auto *properties = editor.findChild<QTableWidget *>(QStringLiteral("objectProperties"));
        auto *add = editor.findChild<QPushButton *>(QStringLiteral("addObjectProperty"));
        auto *remove = editor.findChild<QPushButton *>(QStringLiteral("removeObjectProperty"));
        QVERIFY(properties && add && remove);
        add->click();
        add->click();
        QCOMPARE(properties->rowCount(), 2);
        properties->setCurrentCell(0, 1);
        remove->click();
        QCOMPARE(properties->rowCount(), 1);
        properties->setCurrentCell(-1, -1);
        remove->click();
        QCOMPARE(properties->rowCount(), 1);
        // A change of the marker collections keeps the chosen collection and symbol.
        auto *collection = editor.findChild<QComboBox *>(QStringLiteral("objectCollection"));
        auto *type = editor.findChild<QComboBox *>(QStringLiteral("objectType"));
        QVERIFY(collection && type);
        const auto chosenCollection = collection->currentData();
        const auto chosenType = type->currentData();
        emit collections.changed();
        QCOMPARE(collection->currentData(), chosenCollection);
        QCOMPARE(type->currentData(), chosenType);
        QVERIFY(editor.draft());
    }
    void reportsPaneServer()
    {
        // The pane lists, sends with attachments, and updates reports of a server.
        const QtDialogs dialogs;
        HttpsHarness api;
        QVERIFY(api.load());
        int status = 200;
        api.respond = [&](const QByteArray &request) {
            QByteArray body;
            if (request.startsWith("GET /Marti/api/citrap?")) {
                body = "<reports><report id=\"r-1\" title=\"Bridge out\" type=\"Hazard\" "
                       "status=\"Open\" importance=\"High\" userCallsign=\"Echo\" "
                       "dateTime=\"2026-10-09T08:00:00.000Z\" location=\"59.5,18.25\">"
                       "<userDescription>Water over road</userDescription></report>"
                       "<report><id>r-2</id></report></reports>";
            } else if (request.startsWith("POST /Marti/api/citrap?")) {
                body = "{\"id\":\"server-7\"}";
            }
            return httpReply(status, body);
        };
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("reports.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.findChild<QDockWidget *>(QStringLiteral("org.swak.reports.pane"))->show();
        auto *label = window.findChild<QLabel *>(QStringLiteral("reportStatus"));
        auto *table = window.findChild<QTableWidget *>(QStringLiteral("citrapReports"));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(label && table && servers);
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        const auto waitFor = [&](const QString &text) {
            return QTest::qWaitFor(
                [&] {
                    return label->text().contains(text);
                },
                15000);
        };
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        click(QStringLiteral("listReports"));
        QVERIFY(waitFor(QStringLiteral("2 reports.")));
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Bridge out"));
        QCOMPARE(table->item(0, 0)->toolTip(), QStringLiteral("Water over road"));
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Echo"));
        QVERIFY(!table->item(0, 5)->text().isEmpty());
        // A report without a title shows its identifier and no time.
        QCOMPARE(table->item(1, 0)->text(), QStringLiteral("r-2"));
        QVERIFY(table->item(1, 5)->text().isEmpty());
        // Close and Reopen send the new status. A successful update reads the list again.
        table->selectRow(0);
        click(QStringLiteral("closeReport"));
        QCOMPARE(label->text(), QStringLiteral("Updating the report…"));
        QVERIFY(waitFor(QStringLiteral("2 reports.")));
        QVERIFY(std::any_of(api.requests.cbegin(), api.requests.cend(), [](const auto &request) {
            return request.startsWith("PUT /Marti/api/citrap/r-1?") &&
                   request.contains("status=\"Closed\"");
        }));
        // The dialog adds attachments through the file dialog. The pane reads each file.
        QTemporaryDir files;
        const auto photo = files.filePath(QStringLiteral("photo.jpg"));
        QFile output(photo);
        QVERIFY(output.open(QIODevice::WriteOnly) && output.write("jpeg") == 4);
        output.close();
        const auto gone = files.filePath(QStringLiteral("gone.jpg"));
        QVERIFY(QFile::copy(photo, gone));
        const auto send = [&](const QStringList &attachments) {
            QTimer::singleShot(0, &window, [&window, attachments] {
                auto *dialog = visibleDialog(window, QStringLiteral("newReportDialog"));
                if (!dialog) {
                    return;
                }
                dialog->findChild<QLineEdit *>(QStringLiteral("reportTitle"))
                    ->setText(QStringLiteral("Smoke"));
                for (const auto &path : attachments) {
                    chooseFile(dialog, path);
                    dialog->findChild<QPushButton *>(QStringLiteral("addReportFile"))->click();
                }
                dialog->accept();
            });
            click(QStringLiteral("newReport"));
        };
        send({photo});
        QCOMPARE(label->text(), QStringLiteral("Sending the report…"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(
                    api.requests.cbegin(), api.requests.cend(), [](const auto &request) {
                        return request.startsWith("POST /Marti/api/citrap/server-7/attachment?") &&
                               request.contains("filename=\"photo.jpg\"");
                    });
            },
            15000));
        // A sent report reads the list again.
        QVERIFY(waitFor(QStringLiteral("2 reports.")));
        // A file that disappears before Send stops the report with the read error.
        const auto requests = api.requests.size();
        QTimer::singleShot(0, &window, [&window, gone] {
            auto *dialog = visibleDialog(window, QStringLiteral("newReportDialog"));
            if (!dialog) {
                return;
            }
            dialog->findChild<QLineEdit *>(QStringLiteral("reportTitle"))
                ->setText(QStringLiteral("Smoke"));
            chooseFile(dialog, gone);
            dialog->findChild<QPushButton *>(QStringLiteral("addReportFile"))->click();
            QFile::remove(gone);
            dialog->accept();
        });
        click(QStringLiteral("newReport"));
        QCOMPARE(label->text(), QStringLiteral("The file cannot be opened."));
        QCOMPARE(api.requests.size(), requests);
        // A refused update shows the server error and does not read the list.
        status = 403;
        table->selectRow(0);
        click(QStringLiteral("reopenReport"));
        QVERIFY(waitFor(QStringLiteral("does not allow")));
        QCOMPARE(table->rowCount(), 2);
    }
    void checklistsPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("checklists.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.checklists.pane"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("checklistStatus"));
        auto *serverChoice = window.findChild<QComboBox *>(QStringLiteral("checklistServer"));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(dock && status && serverChoice && servers);
        dock->show();
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        click(QStringLiteral("listChecklists"));
        QVERIFY(status->text().contains(QStringLiteral("REST API")));
        click(QStringLiteral("completeTask"));
        QCOMPARE(status->text(), QStringLiteral("Select a task."));
        click(QStringLiteral("pendingTask"));
        QCOMPARE(status->text(), QStringLiteral("Select a task."));
        click(QStringLiteral("startChecklist"));
        QVERIFY(status->text().startsWith(QStringLiteral("Refresh first")));
        click(QStringLiteral("checklistMission"));
        QCOMPARE(status->text(), QStringLiteral("Select a checklist."));
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("apiPort"), 9);
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        QCOMPARE(serverChoice->count(), 1);
        click(QStringLiteral("listChecklists"));
        QVERIFY(status->text().contains(QStringLiteral("Reading")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !status->text().contains(QStringLiteral("Reading"));
            },
            15000));
        QVERIFY(!status->text().isEmpty());
        auto *pane = dynamic_cast<swak::PreferenceParticipant *>(
            window.findChild<QWidget *>(QStringLiteral("checklistPane")));
        QVERIFY(pane);
        pane->restorePreferences(pane->savePreferences());
    }
    void checklistsPaneServer()
    {
        // The pane lists, opens, starts, and updates checklists of a server.
        HttpsHarness api;
        QVERIFY(api.load());
        const QByteArray checklist =
            "<checklist><checklistDetails><name>Patrol</name><uid>c-1</uid>"
            "<description>Night</description><templateName>Base</templateName>"
            "</checklistDetails><checklistColumns><checklistColumn><columnName>Step</columnName>"
            "</checklistColumn></checklistColumns><checklistTasks>"
            "<checklistTask><uid>t-1</uid><value>Radio check</value><status>Pending</status>"
            "</checklistTask><checklistTask><uid>t-2</uid><value>Return</value>"
            "<status>Complete</status></checklistTask></checklistTasks></checklist>";
        int status = 200;
        api.respond = [&](const QByteArray &request) {
            QByteArray body;
            if (request.startsWith("GET /Marti/api/excheck/checklist/active?")) {
                body = "<checklists>" + checklist + "</checklists>";
            } else if (request.startsWith("GET /Marti/api/missions/exchecktemplates ")) {
                body = "{\"data\":[{\"contents\":[{\"data\":{\"uid\":\"tpl-1\",\"name\":\"Base\"}}]"
                       "}]}";
            } else if (request.startsWith("GET /Marti/api/excheck/checklist/c-1?")) {
                body = checklist;
            } else if (request.startsWith("PUT /Marti/api/missions/c-1/subscription?")) {
                body = "{\"data\":{\"role\":{\"type\":\"MISSION_SUBSCRIBER\"}}}";
            }
            return httpReply(status, body);
        };
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("checklists.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.findChild<QDockWidget *>(QStringLiteral("org.swak.checklists.pane"))->show();
        auto *label = window.findChild<QLabel *>(QStringLiteral("checklistStatus"));
        auto *list = window.findChild<QListWidget *>(QStringLiteral("checklistList"));
        auto *tasks = window.findChild<QTableWidget *>(QStringLiteral("checklistTasks"));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(label && list && tasks && servers);
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        const auto waitFor = [&](const QString &text) {
            return QTest::qWaitFor(
                [&] {
                    return label->text() == text;
                },
                15000);
        };
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        click(QStringLiteral("listChecklists"));
        QVERIFY(waitFor(QStringLiteral("1 checklists.")));
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Patrol"));
        QCOMPARE(list->item(0)->toolTip(), QStringLiteral("Night"));
        // Selecting a checklist reads its tasks. The status column follows the task columns.
        list->setCurrentRow(0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return tasks->rowCount() == 2;
            },
            15000));
        QCOMPARE(tasks->columnCount(), 2);
        QCOMPARE(tasks->horizontalHeaderItem(0)->text(), QStringLiteral("Step"));
        QCOMPARE(tasks->item(0, 0)->text(), QStringLiteral("Radio check"));
        QCOMPARE(tasks->item(1, 1)->text(), QStringLiteral("Complete"));
        QVERIFY(label->isHidden());
        // A task update reads the checklist again.
        tasks->selectRow(0);
        click(QStringLiteral("completeTask"));
        QVERIFY(waitFor(QStringLiteral("The task is updated.")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return api.requests.last().startsWith("GET /Marti/api/excheck/checklist/c-1?");
            },
            15000));
        QVERIFY(std::any_of(api.requests.cbegin(), api.requests.cend(), [](const auto &request) {
            return request.startsWith("PUT /Marti/api/excheck/checklist/c-1/task/t-1?") &&
                   request.contains("<status>Complete</status>");
        }));
        // The start dialog sends the template, name, and description. Cancel sends nothing.
        const auto start = [&](bool accept) {
            QTimer::singleShot(0, &window, [&window, accept] {
                auto *dialog = window.findChild<QDialog *>(QStringLiteral("startChecklistDialog"));
                if (!dialog) {
                    return;
                }
                dialog->findChild<QLineEdit *>(QStringLiteral("checklistName"))
                    ->setText(QStringLiteral("Night 2"));
                dialog->findChild<QLineEdit *>(QStringLiteral("checklistDescription"))
                    ->setText(QStringLiteral("Go"));
                accept ? dialog->accept() : dialog->reject();
            });
            click(QStringLiteral("startChecklist"));
        };
        const auto sent = api.requests.size();
        start(false);
        QCOMPARE(api.requests.size(), sent);
        start(true);
        QVERIFY(waitFor(QStringLiteral("The checklist is started.")));
        QVERIFY(std::any_of(api.requests.cbegin(), api.requests.cend(), [](const auto &request) {
            return request.startsWith("POST /Marti/api/excheck/tpl-1/start?name=Night%202&"
                                      "description=Go&");
        }));
        // A started checklist reads the list again.
        QVERIFY(waitFor(QStringLiteral("1 checklists.")));
        list->setCurrentRow(0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return tasks->rowCount() == 2;
            },
            15000));
        click(QStringLiteral("checklistMission"));
        QCOMPARE(label->text(),
                 QStringLiteral("Subscribing. Missions shows the checklist mission."));
        // Server errors show in the status line. A failed update does not read the checklist.
        status = 403;
        tasks->selectRow(1);
        click(QStringLiteral("pendingTask"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return label->text().contains(QStringLiteral("does not allow"));
            },
            15000));
        start(true);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return label->text().contains(QStringLiteral("does not allow"));
            },
            15000));
        click(QStringLiteral("listChecklists"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return label->text().contains(QStringLiteral("does not allow"));
            },
            15000));
        QCOMPARE(list->count(), 0);
        QCOMPARE(tasks->rowCount(), 0);
    }
    void videoPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("video.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.video.pane"));
        auto *list = window.findChild<QListWidget *>(QStringLiteral("videoSources"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("videoStatus"));
        auto *sources = window.findChild<swak::VideoSources *>(QStringLiteral("videoSources"));
        QVERIFY(dock && list && status && sources);
        dock->show();
        dock->raise();
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        // The pane loads the media backend only at the first Play.
        QVERIFY(!window.findChild<QObject *>(QStringLiteral("videoPlayer")));
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("videoPlaceholder")));
        click(QStringLiteral("playVideo"));
        QCOMPARE(status->text(), QStringLiteral("Select a source."));
        click(QStringLiteral("refreshVideo"));
        QVERIFY(status->text().contains(QStringLiteral("REST API")));
        // A video event adds a source.
        swak::Event camera;
        camera.uid = QStringLiteral("camera-1");
        camera.callsign = QStringLiteral("Gate");
        camera.type = QStringLiteral("b-i-v");
        camera.how = QStringLiteral("h-e");
        camera.detail = QStringLiteral("<__video url=\"rtsp://127.0.0.1:9/none\"/>");
        camera.time = camera.start = QDateTime::currentDateTimeUtc();
        camera.stale = camera.time.addSecs(600);
        QVERIFY(window.store().accept(camera).isEmpty());
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Gate · RTSP"));
        // A stream that does not answer shows as unavailable.
        list->setCurrentRow(0);
        click(QStringLiteral("playVideo"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return status->text().startsWith(QStringLiteral("Unavailable"));
            },
            20000));
        // Add URL rejects other schemes and adds stream URLs.
        const auto addUrl = [&](const QString &text) {
            QTimer::singleShot(0, &window, [&window, text] {
                if (auto *dialog =
                        window.findChild<QInputDialog *>(QStringLiteral("videoUrlDialog"))) {
                    dialog->setTextValue(text);
                    dialog->accept();
                }
            });
            click(QStringLiteral("addVideo"));
        };
        addUrl(QStringLiteral("file:///etc/passwd"));
        QVERIFY(status->text().contains(QStringLiteral("RTSP")));
        // A local MPEG-TS stream over UDP plays inside SWAK.
        const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) {
            QSKIP("ffmpeg is not installed, so the UDP playback check cannot run.");
        }
        QUdpSocket probe;
        QVERIFY(probe.bind(QHostAddress::LocalHost, 0));
        const auto port = probe.localPort();
        probe.close();
        addUrl(QStringLiteral("udp://127.0.0.1:%1").arg(port));
        QCOMPARE(list->count(), 2);
        list->setCurrentRow(1);
        click(QStringLiteral("playVideo"));
        QProcess stream;
        stream.start(ffmpeg,
                     {QStringLiteral("-hide_banner"),
                      QStringLiteral("-loglevel"),
                      QStringLiteral("error"),
                      QStringLiteral("-re"),
                      QStringLiteral("-f"),
                      QStringLiteral("lavfi"),
                      QStringLiteral("-i"),
                      QStringLiteral("testsrc=size=160x120:rate=15"),
                      QStringLiteral("-t"),
                      QStringLiteral("40"),
                      QStringLiteral("-f"),
                      QStringLiteral("mpegts"),
                      QStringLiteral("udp://127.0.0.1:%1?pkt_size=1316").arg(port)});
        QVERIFY(stream.waitForStarted(5000));
        const auto stopStream = qScopeGuard([&stream] {
            stream.kill();
            stream.waitForFinished(5000);
        });
        const bool played = QTest::qWaitFor(
            [&] {
                return status->text().startsWith(QStringLiteral("Playing"));
            },
            30000);
        const auto text = status->text();
        QVERIFY(window.grab().save(QStringLiteral("video-pane.png")));
        click(QStringLiteral("stopVideo"));
        QCOMPARE(status->text(), QStringLiteral("Stopped."));
        // Hiding the pane stops a stream.
        click(QStringLiteral("playVideo"));
        dock->hide();
        QVERIFY2(played, qPrintable(text));
        QVERIFY2(status->text().contains(QStringLiteral("hidden")), qPrintable(status->text()));
        auto *pane = dynamic_cast<swak::PreferenceParticipant *>(
            window.findChild<QWidget *>(QStringLiteral("videoPane")));
        QVERIFY(pane);
        pane->restorePreferences(pane->savePreferences());
        // SRT plays from ffmpeg in listener mode, and HLS from a local HTTP server. The ffmpeg
        // RTMP listener accepts one client only, so it cannot check RTMP playback.
        dock->show();
        const QStringList source{QStringLiteral("-hide_banner"),
                                 QStringLiteral("-loglevel"),
                                 QStringLiteral("error"),
                                 QStringLiteral("-re"),
                                 QStringLiteral("-f"),
                                 QStringLiteral("lavfi"),
                                 QStringLiteral("-i"),
                                 QStringLiteral("testsrc=size=160x120:rate=15"),
                                 QStringLiteral("-t"),
                                 QStringLiteral("30"),
                                 QStringLiteral("-pix_fmt"),
                                 QStringLiteral("yuv420p")};
        const auto play = [&](const QString &url, const QStringList &output) -> QString {
            const auto before = list->count();
            addUrl(url);
            if (list->count() != before + 1) {
                return QStringLiteral("not added: ") + status->text();
            }
            QProcess server;
            if (!output.isEmpty()) {
                server.start(ffmpeg, source + output);
                if (!server.waitForStarted(5000)) {
                    return QStringLiteral("ffmpeg did not start");
                }
                QTest::qWait(1500);
            }
            list->setCurrentRow(before);
            click(QStringLiteral("playVideo"));
            const bool ok = QTest::qWaitFor(
                [&] {
                    return status->text().startsWith(QStringLiteral("Playing")) ||
                           status->text().startsWith(QStringLiteral("Unavailable"));
                },
                20000);
            const auto result = ok ? status->text() : QStringLiteral("timeout: ") + status->text();
            click(QStringLiteral("stopVideo"));
            server.kill();
            server.waitForFinished(5000);
            return result;
        };
        QUdpSocket srtProbe;
        QVERIFY(srtProbe.bind(QHostAddress::LocalHost, 0));
        const auto srtPort = srtProbe.localPort();
        srtProbe.close();
        const auto srt = play(QStringLiteral("srt://127.0.0.1:%1").arg(srtPort),
                              {QStringLiteral("-f"),
                               QStringLiteral("mpegts"),
                               QStringLiteral("srt://127.0.0.1:%1?mode=listener").arg(srtPort)});
        // HLS: write a short live playlist, as TAK feeds use, then serve the folder over HTTP.
        QTemporaryDir hls;
        QProcess segmenter;
        segmenter.start(ffmpeg,
                        {QStringLiteral("-hide_banner"),
                         QStringLiteral("-loglevel"),
                         QStringLiteral("error"),
                         QStringLiteral("-f"),
                         QStringLiteral("lavfi"),
                         QStringLiteral("-i"),
                         QStringLiteral("testsrc=size=160x120:rate=15"),
                         QStringLiteral("-t"),
                         QStringLiteral("6"),
                         QStringLiteral("-pix_fmt"),
                         QStringLiteral("yuv420p"),
                         QStringLiteral("-c:v"),
                         QStringLiteral("libx264"),
                         QStringLiteral("-preset"),
                         QStringLiteral("ultrafast"),
                         QStringLiteral("-f"),
                         QStringLiteral("hls"),
                         QStringLiteral("-hls_time"),
                         QStringLiteral("2"),
                         QStringLiteral("-hls_list_size"),
                         QStringLiteral("0"),
                         QStringLiteral("-hls_flags"),
                         QStringLiteral("omit_endlist"),
                         hls.filePath(QStringLiteral("live.m3u8"))});
        QVERIFY(segmenter.waitForFinished(30000));
        QTcpServer http;
        QVERIFY(http.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&http, &QTcpServer::newConnection, &http, [&http, &hls] {
            while (auto *socket = http.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &hls] {
                    const auto request = socket->readAll();
                    const auto path = QString::fromLatin1(request.split(' ').value(1));
                    QFile file(hls.filePath(QFileInfo(path).fileName()));
                    const auto body =
                        file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
                    socket->write((body.isEmpty() ? QByteArray("HTTP/1.1 404 X\r\n")
                                                  : QByteArray("HTTP/1.1 200 OK\r\n")) +
                                  "Content-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        const auto hlsResult =
            play(QStringLiteral("http://127.0.0.1:%1/live.m3u8").arg(http.serverPort()), {});
        QVERIFY2(srt.startsWith(QStringLiteral("Playing")),
                 qPrintable(QStringLiteral("SRT: ") + srt));
        QVERIFY2(hlsResult.startsWith(QStringLiteral("Playing")),
                 qPrintable(QStringLiteral("HLS: ") + hlsResult));
    }
    void historyAndIcons()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("history.json")),
                            QPluginLoader::staticInstances());
        swak::MapLayer *historyLayer = nullptr;
        swak::MapLayer *points = nullptr;
        for (auto *plugin : QPluginLoader::staticInstances()) {
            const QByteArray name(plugin->metaObject()->className());
            if (name.endsWith("HistoryPlugin")) {
                historyLayer = qobject_cast<swak::MapLayer *>(plugin);
            } else if (name.endsWith("TracksPlugin")) {
                points = qobject_cast<swak::MapLayer *>(plugin);
            }
        }
        auto *history = window.findChild<swak::TrackHistory *>(QStringLiteral("trackHistory"));
        auto *icons = window.findChild<swak::IconCache *>(QStringLiteral("iconCache"));
        auto *tools = window.findChild<swak::MapTools *>();
        auto *servers = window.findChild<swak::Servers *>();
        auto *trace = dynamic_cast<swak::PreferenceParticipant *>(
            window.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(historyLayer && points && history && icons && tools && servers && trace);
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("apiPort"), 9);
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        swak::Event user;
        user.uid = QStringLiteral("ANDROID-9");
        user.callsign = QStringLiteral("Hotel");
        user.type = QStringLiteral("a-f-G-U-C");
        user.how = QStringLiteral("m-g");
        user.latitude = 59;
        user.longitude = 18;
        user.origin = QStringLiteral("tak");
        user.detail = QStringLiteral("<usericon iconsetpath=\"set/group/hiker.png\"/>");
        user.time = user.start = QDateTime::currentDateTimeUtc();
        user.stale = user.time.addSecs(600);
        QVERIFY(window.store().accept(user).isEmpty());
        // Load History asks the server of the object.
        swak::MapItem item;
        item.owner = QStringLiteral("org.swak.events");
        item.id = user.uid;
        QMenu menu;
        tools->populateContextMenu(item, menu);
        auto *load = menu.findChild<QMenu *>(QStringLiteral("loadHistory"));
        QVERIFY(load && load->actions().size() == 4);
        QVERIFY(!menu.findChild<QAction *>(QStringLiteral("clearHistory")));
        QSignalSpy loaded(history, &swak::TrackHistory::loaded);
        load->actions().first()->trigger();
        QVERIFY(loaded.wait(15000));
        QVERIFY(!loaded.last().at(1).toString().isEmpty());
        // A track shows on the map and Trace continues it.
        const auto now = QDateTime::currentDateTimeUtc();
        history->add({user.uid,
                      QStringLiteral("Hotel"),
                      QStringLiteral("tak"),
                      now.addSecs(-600),
                      now,
                      {{now.addSecs(-600), swak::project(59, 17.99)},
                       {now.addSecs(-300), swak::project(59, 17.995)},
                       {now.addSecs(-60), swak::project(59, 17.999)}}});
        const swak::Viewport view{QSizeF(400, 400), swak::project(59, 17.995), 14};
        const auto render = [&](const swak::MapLayer &layer) {
            QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            layer.paint(painter, view, window.store());
            return image;
        };
        const auto image = render(*historyLayer);
        int painted = 0;
        for (int x = 0; x < 400; ++x) {
            painted += image.pixelColor(x, 200).alpha() > 0 ? 1 : 0;
        }
        QVERIFY(painted > 50);
        const auto targets = trace->savePreferences().value(QStringLiteral("targets")).toArray();
        QVERIFY(std::any_of(targets.cbegin(), targets.cend(), [&user](const QJsonValue &value) {
            return value.toObject().value(QStringLiteral("id")).toString() == user.uid;
        }));
        QMenu second;
        tools->populateContextMenu(item, second);
        auto *clear = second.findChild<QAction *>(QStringLiteral("clearHistory"));
        QVERIFY(clear);
        clear->trigger();
        QVERIFY(history->tracks().isEmpty());
        QCOMPARE(render(*historyLayer).pixelColor(200, 200).alpha(), 0);
        history->add({user.uid, {}, {}, now, now, {{now, swak::project(59, 17.995)}}});
        window.findChild<QAction *>(QStringLiteral("clearAllHistory"))->trigger();
        QVERIFY(history->tracks().isEmpty());
        // A server icon replaces the symbol once it is available.
        const swak::Viewport iconView{QSizeF(400, 400), swak::project(59, 18), 14};
        const auto renderPoints = [&] {
            QImage result(400, 400, QImage::Format_ARGB32_Premultiplied);
            result.fill(Qt::transparent);
            QPainter painter(&result);
            points->paint(painter, iconView, window.store());
            return result;
        };
        QVERIFY(renderPoints().pixelColor(200, 200).alpha() > 0);
        QImage blue(16, 16, QImage::Format_ARGB32);
        blue.fill(Qt::blue);
        icons->insert(QStringLiteral("tak"), QStringLiteral("set/group/hiker.png"), blue);
        const auto withIcon = renderPoints().pixelColor(200, 200);
        QVERIFY(withIcon.blue() > 200 && withIcon.red() < 60);
        window.store().select(user.uid);
        QVERIFY(renderPoints().pixelColor(200, 200).blue() > 200);
    }
    void missionsPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("missions.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.missions.view.pane"));
        auto *serverChoice = window.findChild<QComboBox *>(QStringLiteral("missionServer"));
        auto *subscriptions =
            window.findChild<QListWidget *>(QStringLiteral("missionSubscriptions"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("missionsStatus"));
        auto *missions = window.findChild<swak::Missions *>(QStringLiteral("missions"));
        auto *servers = window.findChild<swak::Servers *>();
        auto *tools = window.findChild<swak::MapTools *>();
        auto *objects = window.findChild<swak::UserObjects *>();
        QVERIFY(dock && serverChoice && subscriptions && status && missions && servers && tools &&
                objects);
        dock->show();
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        click(QStringLiteral("listMissions"));
        QVERIFY(status->text().contains(QStringLiteral("REST API")));
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("apiPort"), 9);
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        QCOMPARE(serverChoice->count(), 1);
        click(QStringLiteral("listMissions"));
        QVERIFY(status->text().contains(QStringLiteral("Reading")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !status->text().contains(QStringLiteral("Reading"));
            },
            15000));
        click(QStringLiteral("subscribeMission"));
        QCOMPARE(status->text(), QStringLiteral("Select a mission."));
        // A restored subscription shows even when the server cannot answer.
        missions->restorePreferences(
            {{QStringLiteral("subscriptions"),
              QJsonArray{QJsonObject{{QStringLiteral("server"), QStringLiteral("tak")},
                                     {QStringLiteral("name"), QStringLiteral("Ops")},
                                     {QStringLiteral("visible"), true}}}}});
        QCOMPARE(subscriptions->count(), 1);
        QVERIFY(subscriptions->item(0)->text().startsWith(QStringLiteral("Ops")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !subscriptions->item(0)->text().contains(QStringLiteral("Subscribing"));
            },
            15000));
        subscriptions->item(0)->setCheckState(Qt::Unchecked);
        QVERIFY(!missions->subscription(QStringLiteral("tak"), QStringLiteral("Ops"))->visible);
        click(QStringLiteral("downloadMissionFile"));
        QCOMPARE(status->text(), QStringLiteral("Select a mission file."));
        window.findChild<QLineEdit *>(QStringLiteral("missionLogInput"))
            ->setText(QStringLiteral("Note"));
        click(QStringLiteral("addMissionLog"));
        QVERIFY(status->isHidden());
        click(QStringLiteral("exportMission"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !status->text().contains(QStringLiteral("Downloading"));
            },
            15000));
        QVERIFY(!status->text().isEmpty());
        click(QStringLiteral("reloadMission"));
        // Map menus add local objects to missions and remove mission objects.
        swak::UserObject marker;
        marker.id = QStringLiteral("m1");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Rally");
        marker.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.3}};
        QVERIFY(objects->put(marker).isEmpty());
        swak::MapItem item;
        item.owner = QStringLiteral("org.swak.markers");
        item.id = marker.id;
        QMenu menu;
        tools->populateContextMenu(item, menu);
        auto *add = menu.findChild<QMenu *>(QStringLiteral("addToMission"));
        QVERIFY(add && add->actions().size() == 1);
        QCOMPARE(add->actions().first()->text(), QStringLiteral("Ops"));
        add->actions().first()->trigger();
        swak::Event inMission;
        inMission.uid = QStringLiteral("mission-object");
        inMission.type = QStringLiteral("a-f-G");
        inMission.how = QStringLiteral("h-e");
        inMission.origin = swak::missionOrigin(QStringLiteral("tak"), QStringLiteral("Ops"));
        inMission.time = inMission.start = QDateTime::currentDateTimeUtc();
        inMission.stale = inMission.time.addSecs(600);
        QVERIFY(window.store().accept(inMission).isEmpty());
        item.owner = QStringLiteral("org.swak.events");
        item.id = inMission.uid;
        QMenu eventMenu;
        tools->populateContextMenu(item, eventMenu);
        auto *remove = eventMenu.findChild<QAction *>(QStringLiteral("removeFromMission"));
        QVERIFY(remove);
        remove->trigger();
        item.id = QStringLiteral("missing");
        QMenu emptyMenu;
        tools->populateContextMenu(item, emptyMenu);
        QVERIFY(!emptyMenu.findChild<QAction *>(QStringLiteral("removeFromMission")));
        // An invitation shows in the status line.
        swak::Event invitation = inMission;
        invitation.uid = QStringLiteral("invite");
        invitation.type = QStringLiteral("t-x-m-i");
        invitation.origin = QStringLiteral("tak");
        invitation.detail = QStringLiteral("<mission name=\"Plans\"/>");
        QVERIFY(window.store().accept(invitation).isEmpty());
        QVERIFY(status->text().contains(QStringLiteral("Plans")));
        dock->raise();
        QVERIFY(window.grab().save(QStringLiteral("missions-pane.png")));
        click(QStringLiteral("unsubscribeMission"));
        QCOMPARE(subscriptions->count(), 0);
        auto *pane = dynamic_cast<swak::PreferenceParticipant *>(
            window.findChild<QWidget *>(QStringLiteral("missionsPane")));
        QVERIFY(pane);
        pane->restorePreferences(pane->savePreferences());
    }
    void missionsPaneServer()
    {
        // The pane lists and subscribes to missions, and shows their files and logs.
        const QtDialogs dialogs;
        HttpsHarness api;
        QVERIFY(api.load());
        bool archiveFails = false;
        api.respond = [&](const QByteArray &request) {
            const auto line = request.left(request.indexOf("\r\n"));
            QByteArray body = "{}";
            int status = 200;
            if (line.startsWith("GET /Marti/api/missions?passwordProtected=true ")) {
                body = "{\"data\":[{\"name\":\"Ops\",\"description\":\"Ops room\","
                       "\"creatorUid\":\"ANDROID-1\"},{\"name\":\"Secret\","
                       "\"passwordProtected\":true},{\"name\":\"Team\"}]}";
            } else if (line.startsWith("PUT /Marti/api/missions/Ops/subscription?")) {
                body = "{\"data\":{\"role\":{\"type\":\"MISSION_OWNER\"}}}";
            } else if (line.startsWith("PUT /Marti/api/missions/Team/subscription?")) {
                body = "{\"data\":{\"role\":{\"type\":\"MISSION_SUBSCRIBER\"}}}";
            } else if (line.startsWith("PUT /Marti/api/missions/Secret/subscription?")) {
                status = line.contains("password=pw") ? 200 : 403;
                body = "{\"data\":{\"role\":{\"type\":\"MISSION_READONLY_SUBSCRIBER\"}}}";
            } else if (line.contains("/cot ")) {
                body = "<events></events>";
            } else if (line.startsWith("GET /Marti/api/missions/Ops/log ")) {
                body = "{\"data\":[{\"id\":\"1\",\"content\":\"Started\",\"creatorUid\":"
                       "\"ANDROID-1\",\"dtg\":\"2026-10-08T10:00:00.000Z\"}]}";
            } else if (line.startsWith("GET /Marti/api/missions/Ops/archive ")) {
                status = archiveFails ? 500 : 200;
                body = "PK archive";
            } else if (line.startsWith("GET /Marti/api/missions/Ops ")) {
                body = "{\"data\":[{\"name\":\"Ops\",\"contents\":[{\"data\":{\"hash\":\"" +
                       QByteArray(64, 'c') +
                       "\",\"name\":\"map.kmz\",\"size\":2048,\"submitter\":\"Echo\"}},"
                       "{\"data\":{\"hash\":\"" +
                       QByteArray(64, 'd') + "\",\"name\":\"notes.txt\"}}]}]}";
            } else if (line.contains("/log ")) {
                body = "{\"data\":[]}";
            } else if (line.startsWith("GET /Marti/api/missions/")) {
                const auto name = line.mid(24, line.indexOf(' ', 24) - 24);
                body = "{\"data\":[{\"name\":\"" + name + "\"}]}";
            }
            return httpReply(status, body);
        };
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("missions.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.findChild<QDockWidget *>(QStringLiteral("org.swak.missions.view.pane"))->show();
        auto *label = window.findChild<QLabel *>(QStringLiteral("missionsStatus"));
        auto *available = window.findChild<QTableWidget *>(QStringLiteral("missionList"));
        auto *subscriptions =
            window.findChild<QListWidget *>(QStringLiteral("missionSubscriptions"));
        auto *files = window.findChild<QListWidget *>(QStringLiteral("missionFiles"));
        auto *logs = window.findChild<QListWidget *>(QStringLiteral("missionLogs"));
        auto *logInput = window.findChild<QLineEdit *>(QStringLiteral("missionLogInput"));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(label && available && subscriptions && files && logs && logInput && servers);
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        const auto waitFor = [&](const QString &text) {
            return QTest::qWaitFor(
                [&] {
                    return label->text().contains(text);
                },
                15000);
        };
        const auto row = [&](const QString &name) {
            for (int index = 0; index < subscriptions->count(); ++index) {
                if (subscriptions->item(index)->text().startsWith(name + QStringLiteral(" · "))) {
                    return index;
                }
            }
            return -1;
        };
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        click(QStringLiteral("listMissions"));
        QVERIFY(waitFor(QStringLiteral("3 missions.")));
        QCOMPARE(available->rowCount(), 3);
        QCOMPARE(available->item(0, 1)->text(), QStringLiteral("Ops room"));
        QCOMPARE(available->item(0, 2)->text(), QStringLiteral("ANDROID-1"));
        QCOMPARE(available->item(0, 3)->text(), QStringLiteral("No"));
        QCOMPARE(available->item(1, 3)->text(), QStringLiteral("Yes"));
        // The subscription shows the role, the files, and the log of the mission.
        available->selectRow(0);
        click(QStringLiteral("subscribeMission"));
        QCOMPARE(label->text(), QStringLiteral("Subscribing to Ops…"));
        QVERIFY(waitFor(QStringLiteral("Subscribed to Ops.")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return row(QStringLiteral("Ops")) >= 0 && files->count() == 2 && logs->count() == 1;
            },
            15000));
        QVERIFY(subscriptions->item(row(QStringLiteral("Ops")))
                    ->text()
                    .contains(QStringLiteral(" · owner · ")));
        QVERIFY(files->item(0)->text().startsWith(QStringLiteral("map.kmz (")));
        QCOMPARE(files->item(0)->toolTip(), QStringLiteral("Echo"));
        QCOMPARE(files->item(1)->text(), QStringLiteral("notes.txt (unknown size)"));
        QVERIFY(logs->item(0)->text().contains(QStringLiteral("ANDROID-1")));
        QVERIFY(logs->item(0)->text().endsWith(QStringLiteral("\nStarted")));
        QVERIFY(logInput->isEnabled());
        // A log entry goes to the server, and the input clears.
        logInput->setText(QStringLiteral("Checkpoint"));
        QTest::keyClick(logInput, Qt::Key_Return);
        QVERIFY(logInput->text().isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return std::any_of(
                    api.requests.cbegin(), api.requests.cend(), [](const auto &request) {
                        return request.startsWith("POST /Marti/api/missions/logs/entries ") &&
                               request.contains("Checkpoint");
                    });
            },
            15000));
        // A selected file downloads through the file shares.
        files->setCurrentRow(0);
        click(QStringLiteral("downloadMissionFile"));
        QVERIFY(label->text().startsWith(QStringLiteral("Downloading map.kmz.")));
        // Export saves the archive in the selected file. A failed save or download shows an
        // error.
        const auto archive = directory.filePath(QStringLiteral("ops.zip"));
        chooseFile(&window, archive);
        click(QStringLiteral("exportMission"));
        QCOMPARE(label->text(), QStringLiteral("Downloading the mission archive…"));
        QVERIFY(waitFor(QStringLiteral("The mission archive is saved.")));
        QFile saved(archive);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("PK archive"));
        const auto locked = directory.filePath(QStringLiteral("locked"));
        QVERIFY(QDir().mkdir(locked));
        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        chooseFile(&window, locked + QStringLiteral("/ops.zip"));
        click(QStringLiteral("exportMission"));
        QVERIFY(waitFor(QStringLiteral("Cannot save the mission archive.")));
        archiveFails = true;
        click(QStringLiteral("exportMission"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !label->text().startsWith(QStringLiteral("Downloading"));
            },
            15000));
        QVERIFY(!label->text().contains(QStringLiteral("saved")));
        QFile::setPermissions(
            locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        // A password mission asks for the password. Cancel sends nothing.
        const auto password = [&](bool accept, const QString &text) {
            QTimer::singleShot(0, &window, [&window, accept, text] {
                auto *dialog = qobject_cast<QInputDialog *>(
                    visibleDialog(window, QStringLiteral("missionPassword")));
                if (!dialog) {
                    return;
                }
                dialog->setTextValue(text);
                accept ? dialog->accept() : dialog->reject();
            });
            available->selectRow(1);
            click(QStringLiteral("subscribeMission"));
        };
        const auto sent = api.requests.size();
        password(false, QStringLiteral("pw"));
        QCOMPARE(api.requests.size(), sent);
        password(true, QStringLiteral("wrong"));
        QVERIFY(waitFor(QStringLiteral("does not allow")));
        // The refused subscription stays with the error as its status, so the user can retry.
        QVERIFY(row(QStringLiteral("Secret")) >= 0);
        QVERIFY(subscriptions->item(row(QStringLiteral("Secret")))
                    ->text()
                    .contains(QStringLiteral("does not allow")));
        password(true, QStringLiteral("pw"));
        QVERIFY(waitFor(QStringLiteral("Subscribed to Secret.")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return row(QStringLiteral("Secret")) >= 0;
            },
            15000));
        QVERIFY(subscriptions->item(row(QStringLiteral("Secret")))
                    ->text()
                    .contains(QStringLiteral(" · read-only · ")));
        // A read-only mission does not accept log entries.
        subscriptions->setCurrentRow(row(QStringLiteral("Secret")));
        QVERIFY(!logInput->isEnabled());
        // A double click subscribes too.
        available->scrollToItem(available->item(2, 0));
        const auto cell = available->visualItemRect(available->item(2, 0)).center();
        QVERIFY(available->viewport()->rect().contains(cell));
        QTest::mouseClick(available->viewport(), Qt::LeftButton, Qt::NoModifier, cell);
        QTest::mouseDClick(available->viewport(), Qt::LeftButton, Qt::NoModifier, cell);
        QVERIFY(waitFor(QStringLiteral("Subscribed to Team.")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return row(QStringLiteral("Team")) >= 0;
            },
            15000));
        QVERIFY(subscriptions->item(row(QStringLiteral("Team")))
                    ->text()
                    .contains(QStringLiteral(" · subscriber · ")));
        // The selection stays on its mission when the list changes.
        subscriptions->setCurrentRow(row(QStringLiteral("Secret")));
        click(QStringLiteral("reloadMission"));
        QVERIFY(subscriptions->currentItem()->text().startsWith(QStringLiteral("Secret · ")));
    }
    void filesPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("files.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.files.pane"));
        auto *offers = window.findChild<QTableWidget *>(QStringLiteral("fileOffers"));
        auto *serverFiles = window.findChild<QTableWidget *>(QStringLiteral("serverFiles"));
        auto *serverChoice = window.findChild<QComboBox *>(QStringLiteral("fileServer"));
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("filesTabs"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("filesStatus"));
        auto *files = window.findChild<swak::FileShares *>(QStringLiteral("fileShares"));
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *data = window.findChild<swak::DataSources *>(QStringLiteral("dataSources"));
        auto *maps = window.findChild<swak::MapSources *>();
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(dock && offers && serverFiles && serverChoice && tabs && status && files &&
                objects && data && maps && servers);
        dock->show();
        files->setDirectory(directory.filePath(QStringLiteral("packages")));
        // A package import adds objects, data feeds, and maps through the window services.
        swak::Event marker;
        marker.uid = QStringLiteral("spot-1");
        marker.type = QStringLiteral("b-m-p-s-m");
        marker.how = QStringLiteral("h-e");
        marker.callsign = QStringLiteral("Rally");
        marker.latitude = 59.5;
        marker.longitude = 18.25;
        marker.time = marker.start = QDateTime::currentDateTimeUtc();
        marker.stale = marker.time.addSecs(600);
        swak::DataPackage package;
        package.name = QStringLiteral("Patrol");
        package.entries = {
            {QStringLiteral("m.cot"), swak::encodeCot(marker)},
            {QStringLiteral("roads.kml"), "<kml xmlns=\"http://www.opengis.net/kml/2.2\"/>"},
            {QStringLiteral("map.tif"), "placeholder"}};
        const auto result = swak::importDataPackage(package,
                                                    QStringLiteral("feedfacefeedface"),
                                                    directory.filePath(QStringLiteral("imported")),
                                                    objects,
                                                    data,
                                                    maps);
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QLatin1Char(' '))));
        QCOMPARE(result.objects, 1);
        QCOMPARE(result.feeds.size(), 1);
        QCOMPARE(result.maps.size(), 1);
        QVERIFY(data->sources().contains(result.feeds.first()));
        QVERIFY(maps->sources().contains(result.maps.first()));
        QCOMPARE(data->sources()
                     .value(result.feeds.first())
                     .toObject()
                     .value(QStringLiteral("name"))
                     .toString(),
                 QStringLiteral("Patrol: roads.kml"));
        // A server entry with a REST API lists offers and files.
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("apiPort"), 9);
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        QCOMPARE(serverChoice->count(), 1);
        QCOMPARE(serverChoice->currentText(), QStringLiteral("Test TAK"));
        swak::Event sender;
        sender.uid = QStringLiteral("ANDROID-1");
        sender.callsign = QStringLiteral("Bravo");
        auto offer = swak::fileOfferEvent(sender,
                                          {},
                                          QStringLiteral("Patrol"),
                                          QStringLiteral("patrol.zip"),
                                          QString(64, QLatin1Char('a')),
                                          2048,
                                          {},
                                          QDateTime::currentDateTimeUtc());
        offer.origin = QStringLiteral("tak");
        QVERIFY(window.store().accept(offer).isEmpty());
        QCOMPARE(offers->rowCount(), 1);
        QCOMPARE(offers->item(0, 0)->text(), QStringLiteral("Patrol"));
        QCOMPARE(offers->item(0, 1)->text(), QStringLiteral("Bravo"));
        QVERIFY(offers->item(0, 3)->text().contains(QStringLiteral("Test TAK")));
        // Download asks first. Cancel keeps the offer, and Download starts it.
        auto *download = window.findChild<QPushButton *>(QStringLiteral("downloadOffer"));
        download->click();
        QVERIFY(!status->isHidden());
        offers->selectRow(0);
        const auto answer = [&](bool accept) {
            QTimer::singleShot(0, &window, [&window, accept] {
                auto *box = window.findChild<QMessageBox *>(QStringLiteral("actionConfirmation"));
                if (!box) {
                    return;
                }
                if (accept) {
                    for (auto *button : box->buttons()) {
                        if (button->text() == QStringLiteral("Download")) {
                            button->click();
                        }
                    }
                } else {
                    box->reject();
                }
            });
        };
        answer(false);
        download->click();
        QCOMPARE(files->offers().first().state, swak::FileOffer::State::Offered);
        answer(true);
        download->click();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return files->offers().first().state == swak::FileOffer::State::Failed;
            },
            15000));
        QVERIFY(!offers->item(0, 3)->text().contains(QStringLiteral("Offered")));
        offers->selectRow(0);
        window.findChild<QPushButton *>(QStringLiteral("dismissOffer"))->click();
        QCOMPARE(offers->rowCount(), 0);
        // The server tab reports errors and needs a selection.
        tabs->setCurrentIndex(1);
        window.findChild<QPushButton *>(QStringLiteral("searchServerFiles"))->click();
        QVERIFY(status->text().contains(QStringLiteral("Reading")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !status->text().contains(QStringLiteral("Reading"));
            },
            15000));
        QCOMPARE(serverFiles->rowCount(), 0);
        window.findChild<QPushButton *>(QStringLiteral("downloadServerFile"))->click();
        QCOMPARE(status->text(), QStringLiteral("Select a file."));
        window.findChild<QPushButton *>(QStringLiteral("deleteServerFile"))->click();
        QCOMPARE(status->text(), QStringLiteral("Select a file."));
        // Send Package lists local objects and needs recipients.
        bool listed = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("sendPackageDialog"));
            auto *list = dialog ? dialog->findChild<QListWidget *>(QStringLiteral("packageObjects"))
                                : nullptr;
            if (!list) {
                return;
            }
            listed = list->count() == 1;
            list->item(0)->setCheckState(Qt::Checked);
            dialog->accept();
        });
        window.findChild<QPushButton *>(QStringLiteral("sendPackage"))->click();
        QVERIFY(listed);
        QVERIFY(status->text().contains(QStringLiteral("recipients")));
        auto *pane = dynamic_cast<swak::PreferenceParticipant *>(
            window.findChild<QWidget *>(QStringLiteral("filesPane")));
        QVERIFY(pane);
        QCOMPARE(pane->savePreferences().value(QStringLiteral("tab")).toInt(), 1);
        pane->restorePreferences({{QStringLiteral("tab"), 0}});
        QCOMPARE(tabs->currentIndex(), 0);
        dock->raise();
        QVERIFY(window.grab().save(QStringLiteral("files-pane.png")));
    }
    void filesPaneServer()
    {
        // The server tab lists, downloads, and deletes server files. Send Package uploads.
        HttpsHarness api;
        QVERIFY(api.load());
        const QByteArray hash(64, 'a');
        int deleteStatus = 200;
        int uploadStatus = 200;
        api.respond = [&](const QByteArray &request) {
            QByteArray body;
            int status = 200;
            if (request.startsWith("GET /Marti/sync/search ")) {
                body = "{\"resultCount\":2,\"results\":[{\"hash\":\"" + hash +
                       "\",\"name\":\"Patrol\",\"filename\":\"patrol.zip\",\"submitter\":"
                       "\"alice\",\"size\":1234,\"submissionTime\":\"2026-10-08T10:00:00.000Z\"},"
                       "{\"hash\":\"" +
                       QByteArray(64, 'b') + "\",\"filename\":\"roads.kml\"}]}";
            } else if (request.startsWith("DELETE ")) {
                status = deleteStatus;
            } else if (request.startsWith("POST ")) {
                status = uploadStatus;
            } else {
                status = 404;
            }
            return httpReply(status, body);
        };
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("files.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.findChild<QDockWidget *>(QStringLiteral("org.swak.files.pane"))->show();
        auto *serverFiles = window.findChild<QTableWidget *>(QStringLiteral("serverFiles"));
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("filesTabs"));
        auto *label = window.findChild<QLabel *>(QStringLiteral("filesStatus"));
        auto *files = window.findChild<swak::FileShares *>(QStringLiteral("fileShares"));
        auto *contacts = window.findChild<swak::Contacts *>(QStringLiteral("contacts"));
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(serverFiles && tabs && label && files && contacts && objects && servers);
        files->setDirectory(directory.filePath(QStringLiteral("packages")));
        const auto click = [&window](const QString &name) {
            window.findChild<QPushButton *>(name)->click();
        };
        const auto waitFor = [&](const QString &text) {
            return QTest::qWaitFor(
                [&] {
                    return label->text().contains(text);
                },
                15000);
        };
        const auto answer = [&](const QString &button) {
            QTimer::singleShot(0, &window, [&window, button] {
                auto *box = window.findChild<QMessageBox *>(QStringLiteral("actionConfirmation"));
                if (!box) {
                    return;
                }
                for (auto *candidate : box->buttons()) {
                    if (candidate->text() == button) {
                        candidate->click();
                        return;
                    }
                }
                box->reject();
            });
        };
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("Test TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        tabs->setCurrentIndex(1);
        click(QStringLiteral("searchServerFiles"));
        QVERIFY(waitFor(QStringLiteral("2 files.")));
        QCOMPARE(serverFiles->rowCount(), 2);
        QCOMPARE(serverFiles->item(0, 1)->text(), QStringLiteral("alice"));
        QVERIFY(!serverFiles->item(0, 2)->text().isEmpty());
        QVERIFY(!serverFiles->item(0, 3)->text().isEmpty());
        QVERIFY(serverFiles->item(1, 3)->text().isEmpty());
        // Download asks first. Cancel sends nothing.
        serverFiles->selectRow(0);
        auto sent = api.requests.size();
        answer({});
        click(QStringLiteral("downloadServerFile"));
        QCOMPARE(api.requests.size(), sent);
        answer(QStringLiteral("Download"));
        click(QStringLiteral("downloadServerFile"));
        QVERIFY(label->text().startsWith(QStringLiteral("Downloading ")));
        QVERIFY(label->text().endsWith(QStringLiteral(". Received shows the result.")));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return api.requests.size() > sent &&
                       api.requests.last().startsWith("GET /Marti/sync/content?hash=" + hash);
            },
            15000));
        // Delete asks first. A refused delete keeps the row, and a deletion removes it.
        sent = api.requests.size();
        answer({});
        click(QStringLiteral("deleteServerFile"));
        QCOMPARE(api.requests.size(), sent);
        deleteStatus = 403;
        serverFiles->selectRow(0);
        answer(QStringLiteral("Delete"));
        click(QStringLiteral("deleteServerFile"));
        QVERIFY(waitFor(QStringLiteral("does not allow")));
        QCOMPARE(serverFiles->rowCount(), 2);
        deleteStatus = 200;
        serverFiles->selectRow(0);
        answer(QStringLiteral("Delete"));
        click(QStringLiteral("deleteServerFile"));
        QVERIFY(waitFor(QStringLiteral("The file is deleted.")));
        QCOMPARE(serverFiles->rowCount(), 1);
        QVERIFY(api.requests.last().startsWith("DELETE /Marti/sync/delete?hash=" + hash));
        // Send Package uploads the checked objects and offers them to the checked contacts.
        swak::UserObject marker;
        marker.id = QStringLiteral("m1");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Rally");
        marker.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.3}};
        QVERIFY(objects->put(marker).isEmpty());
        swak::Event user;
        user.uid = QStringLiteral("ANDROID-8");
        user.callsign = QStringLiteral("Golf");
        user.type = QStringLiteral("a-f-G-U-C");
        user.how = QStringLiteral("m-g");
        user.origin = QStringLiteral("tak");
        user.detail = QStringLiteral("<contact endpoint=\"*:-1:stcp\"/>");
        user.time = user.start = QDateTime::currentDateTimeUtc();
        user.stale = user.time.addSecs(600);
        QVERIFY(window.store().accept(user).isEmpty());
        contacts->rebuild();
        const auto send = [&](bool accept) {
            QTimer::singleShot(0, &window, [&window, accept] {
                auto *dialog = visibleDialog(window, QStringLiteral("sendPackageDialog"));
                if (!dialog) {
                    return;
                }
                dialog->findChild<QListWidget *>(QStringLiteral("packageObjects"))
                    ->item(0)
                    ->setCheckState(Qt::Checked);
                auto *people =
                    dialog->findChild<QListWidget *>(QStringLiteral("packageRecipients"));
                QCOMPARE(people->count(), 1);
                QCOMPARE(people->item(0)->text(), QStringLiteral("Golf"));
                people->item(0)->setCheckState(Qt::Checked);
                accept ? dialog->accept() : dialog->reject();
            });
            click(QStringLiteral("sendPackage"));
        };
        sent = api.requests.size();
        send(false);
        QCOMPARE(api.requests.size(), sent);
        uploadStatus = 500;
        send(true);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return api.requests.size() > sent && api.requests.last().startsWith("POST ");
            },
            15000));
        QVERIFY(waitFor(QStringLiteral("500")));
        // The server entry has no connection, so the offer cannot go out after the upload.
        uploadStatus = 200;
        send(true);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !label->text().contains(QStringLiteral("500")) &&
                       !label->text().contains(QStringLiteral("…"));
            },
            15000));
        QVERIFY(!label->text().contains(QStringLiteral("uploaded and offered")));
    }
    void chatPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("chat.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.chat.pane"));
        auto *rooms = window.findChild<QListWidget *>(QStringLiteral("chatRooms"));
        auto *messages = window.findChild<QListWidget *>(QStringLiteral("chatMessages"));
        auto *input = window.findChild<QLineEdit *>(QStringLiteral("chatInput"));
        auto *send = window.findChild<QPushButton *>(QStringLiteral("sendChat"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("chatStatus"));
        auto *newChat = window.findChild<QPushButton *>(QStringLiteral("newChat"));
        auto *chat = window.findChild<swak::Chat *>(QStringLiteral("chat"));
        auto *contacts = window.findChild<swak::Contacts *>(QStringLiteral("contacts"));
        QVERIFY(dock && rooms && messages && input && send && status && newChat && chat &&
                contacts);
        dock->show();
        QCOMPARE(rooms->count(), 2);
        QCOMPARE(rooms->currentItem()->text(), swak::AllChatRooms);
        // An empty message shows the reason. A message waits without a connection.
        send->click();
        QVERIFY(!status->isHidden());
        input->setText(QStringLiteral("Hello all"));
        QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(status->isHidden());
        QVERIFY(input->text().isEmpty());
        QCOMPARE(messages->count(), 1);
        QVERIFY(messages->item(0)->text().contains(QStringLiteral("Waiting for a connection")));
        QVERIFY(messages->item(0)->text().endsWith(QStringLiteral("Hello all")));
        // A received message in the shown room is read at once.
        const auto message = [](const QString &id, const QString &room, const QString &text) {
            swak::Event event;
            event.uid = QStringLiteral("GeoChat.ANDROID-7.") + id;
            event.type = QStringLiteral("b-t-f");
            event.how = QStringLiteral("h-e");
            event.time = event.start = QDateTime::currentDateTimeUtc();
            event.stale = event.time.addSecs(600);
            event.detail =
                QStringLiteral(
                    "<__chat parent=\"RootContactGroup\" messageId=\"%1\" chatroom=\"%2\" "
                    "id=\"%2\" senderCallsign=\"Foxtrot\"/><link uid=\"ANDROID-7\" "
                    "type=\"a-f-G-U-C\" relation=\"p-p\"/><remarks>%3</remarks>")
                    .arg(id, room, text);
            return event;
        };
        QVERIFY(
            window.store()
                .accept(message(QStringLiteral("a-1"), swak::AllChatRooms, QStringLiteral("Hi")))
                .isEmpty());
        QCOMPARE(messages->count(), 2);
        QCOMPARE(chat->room(swak::AllChatRooms)->unread, 0);
        // A direct message adds a bold room with an unread count until the user opens it.
        swak::Event self;
        window.findChild<swak::Identity *>()->apply(self);
        QVERIFY(window.store()
                    .accept(message(QStringLiteral("d-1"), self.uid, QStringLiteral("Private")))
                    .isEmpty());
        QCOMPARE(rooms->count(), 3);
        QCOMPARE(rooms->item(2)->text(), QStringLiteral("Foxtrot (1)"));
        QVERIFY(rooms->item(2)->font().bold());
        rooms->setCurrentRow(2);
        QCOMPARE(chat->room(QStringLiteral("ANDROID-7"))->unread, 0);
        QCOMPARE(rooms->item(2)->text(), QStringLiteral("Foxtrot"));
        QCOMPARE(messages->count(), 1);
        // Copy puts the message text on the clipboard.
        QTimer::singleShot(0, &window, [&] {
            if (auto *menu = window.findChild<QMenu *>(QStringLiteral("chatMessageMenu"))) {
                if (auto *copy = menu->findChild<QAction *>(QStringLiteral("copyChatMessage"))) {
                    copy->trigger();
                }
                menu->close();
            }
        });
        messages->setCurrentRow(0);
        emit messages->customContextMenuRequested(QPoint(-5, -5));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Private"));
        // New Chat lists known users.
        auto *people = newChat->menu();
        emit people->aboutToShow();
        auto *none = people->findChild<QAction *>(QStringLiteral("noChatContacts"));
        QVERIFY(none && !none->isEnabled());
        swak::Event user;
        user.uid = QStringLiteral("ANDROID-8");
        user.callsign = QStringLiteral("Golf");
        user.type = QStringLiteral("a-f-G-U-C");
        user.how = QStringLiteral("m-g");
        user.origin = QStringLiteral("tak");
        user.detail = QStringLiteral("<contact endpoint=\"*:-1:stcp\"/>");
        user.time = user.start = QDateTime::currentDateTimeUtc();
        user.stale = user.time.addSecs(600);
        QVERIFY(window.store().accept(user).isEmpty());
        contacts->rebuild();
        emit people->aboutToShow();
        QCOMPARE(people->actions().size(), 1);
        people->actions().first()->trigger();
        QCOMPARE(rooms->currentItem()->text(), QStringLiteral("Golf"));
        // Chat with in Contacts opens the room and shows the pane.
        rooms->setCurrentRow(0);
        dock->hide();
        auto *table = window.findChild<QTableView *>(QStringLiteral("contactsTable"));
        QVERIFY(table && table->model()->rowCount() == 1);
        table->setCurrentIndex(table->model()->index(0, 0));
        bool chatted = false;
        QTimer::singleShot(0, &window, [&] {
            if (auto *menu = window.findChild<QMenu *>(QStringLiteral("contactMenu"))) {
                if (auto *talk = menu->findChild<QAction *>(QStringLiteral("chatWithContact"))) {
                    talk->trigger();
                    chatted = true;
                }
                menu->close();
            }
        });
        emit table->customContextMenuRequested(QPoint(-5, -5));
        QVERIFY(chatted);
        QVERIFY(!dock->isHidden());
        QCOMPARE(rooms->currentItem()->text(), QStringLiteral("Golf"));
        QVERIFY(window.grab().save(QStringLiteral("chat-pane.png")));
    }
    void contactsPane()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("contacts.json")),
                            QPluginLoader::staticInstances());
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.contacts.pane"));
        auto *table = window.findChild<QTableView *>(QStringLiteral("contactsTable"));
        auto *refresh = window.findChild<QPushButton *>(QStringLiteral("refreshContacts"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("contactsStatus"));
        auto *contacts = window.findChild<swak::Contacts *>(QStringLiteral("contacts"));
        QVERIFY(dock && table && refresh && status && contacts);
        dock->show();
        swak::Event event;
        event.uid = QStringLiteral("ANDROID-7");
        event.callsign = QStringLiteral("Delta");
        event.type = QStringLiteral("a-f-G-U-C");
        event.how = QStringLiteral("m-g");
        event.latitude = 59.3;
        event.longitude = 18.1;
        event.origin = QStringLiteral("tak");
        event.detail = QStringLiteral("<contact endpoint=\"*:-1:stcp\"/>");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(600);
        QVERIFY(window.store().accept(event).isEmpty());
        // A local object without an origin is not a contact.
        event.uid = QStringLiteral("local");
        event.origin.clear();
        QVERIFY(window.store().accept(event).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [table] {
                return table->model()->rowCount() == 1;
            },
            5000));
        QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Delta"));
        // Without TAK REST entries, Refresh ends at once and counts the contacts.
        refresh->click();
        QVERIFY(status->text().contains(QStringLiteral("1")));
        const auto rect = table->visualRect(table->model()->index(0, 0));
        emit table->doubleClicked(table->model()->index(0, 0));
        QCOMPARE(window.store().selected(), QStringLiteral("ANDROID-7"));
        window.store().select({});
        bool shown = false;
        QTimer::singleShot(0, &window, [&] {
            auto *menu = window.findChild<QMenu *>(QStringLiteral("contactMenu"));
            auto *action =
                menu ? menu->findChild<QAction *>(QStringLiteral("showContact")) : nullptr;
            shown = action && action->isEnabled();
            if (action) {
                action->trigger();
            }
            if (menu) {
                menu->close();
            }
        });
        table->setCurrentIndex(table->model()->index(0, 0));
        emit table->customContextMenuRequested(rect.center());
        QVERIFY(shown);
        QCOMPARE(window.store().selected(), QStringLiteral("ANDROID-7"));
        // An expired position removes the contact. An empty row opens no menu.
        window.store().expire(event.stale.addSecs(1));
        contacts->rebuild();
        QCOMPARE(table->model()->rowCount(), 0);
        emit table->customContextMenuRequested(rect.center());
    }
    void selectionExpiryAndNumericSort()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("selection.json")),
                            QPluginLoader::staticInstances());
        auto *table = window.findChild<QTableView *>(QStringLiteral("eventsTable"));
        auto &store = window.store();
        QAbstractItemModelTester tester(&store,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        swak::Event event;
        event.type = QStringLiteral("a-f-G");
        event.how = QStringLiteral("h-e");
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        const auto now = event.time;
        for (int n : {2, 10, -2, -10}) {
            event.uid = QString::number(n);
            event.latitude = n;
            event.longitude = n;
            event.stale = now.addSecs(n == 2 ? 1 : 60);
            QVERIFY(store.accept(event, now).isEmpty());
        }
        for (int column : {2, 3}) {
            table->sortByColumn(column, Qt::AscendingOrder);
            QCOMPARE(table->model()->index(0, 0).data(Qt::UserRole).toString(),
                     QStringLiteral("-10"));
            QCOMPARE(table->model()->index(3, 0).data(Qt::UserRole).toString(),
                     QStringLiteral("10"));
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
        sources.insert(QStringLiteral("custom"),
                       sources.value(QStringLiteral("org.swak.gdal.raster")));
        auto invalid = sources.value(QStringLiteral("org.swak.wms")).toObject();
        invalid.insert(QStringLiteral("endpoint"), QStringLiteral("invalid"));
        sources.insert(QStringLiteral("org.swak.wms"), invalid);
        values.insert(QStringLiteral("sources"), sources);
        {
            swak::Preferences profile(path);
            QVERIFY(profile.save(
                {{QStringLiteral("version"), 1},
                 {QStringLiteral("components"), QJsonObject{{seed.preferenceId(), values}}}}));
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QVERIFY(window.savePreferences());
        }
        swak::Preferences locked(path);
        QCOMPARE(locked.document()
                     .value(QStringLiteral("components"))
                     .toObject()
                     .value(seed.preferenceId())
                     .toObject(),
                 values);
        swak::Window second(path, QPluginLoader::staticInstances());
        second.show();
        auto *activity = second.findChild<swak::ActivityModel *>();
        activity->clear();
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return activity->rowCount() > 0;
            },
            1800));
        second.findChild<QAction *>(QStringLiteral("mapProvidersAction"))->trigger();
        auto *manager = second.findChild<swak::MapManager *>();
        bool tested = false;
        QTimer::singleShot(0, manager, [&] {
            auto *dialog = manager->findChild<swak::SettingsDialog *>();
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] {
                dialog->reject();
            });
            for (auto *line : dialog->findChildren<QLineEdit *>()) {
                if (line->objectName().endsWith(QStringLiteral("/name"))) {
                    line->setText(QStringLiteral("Edited"));
                }
            }
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(dialog->result(), 0);
            QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("settingsStatus"))
                        ->text()
                        .contains(QStringLiteral("saving failed")));
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
            for (int i = 0; i < activity->rowCount(); ++i) {
                if (activity->index(i, swak::ActivityModel::SourceColumn).data().toString() ==
                    QStringLiteral("swak.preferences")) {
                    ++count;
                }
            }
            return count;
        };
        QVERIFY(!window.savePreferences());
        QVERIFY(!window.savePreferences());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return failures() == 1;
            },
            2000));
        QVERIFY(!QTest::qWaitFor(
            [&] {
                return failures() > 1;
            },
            1800));
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
        for (auto *action : root->actions()) {
            if (action->menu() && action->text() == QStringLiteral("CoT Messages")) {
                events = action->menu();
            }
        }
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
            const auto close = qScopeGuard([dialog] {
                dialog->reject();
            });
            auto *width = dialog->findChild<QDoubleSpinBox *>(editor->objectName() +
                                                              QStringLiteral("/width"));
            QVERIFY(width);
            width->setValue(width->value() + 1);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
            QVERIFY(window.tabifiedDockWidgets(editor).contains(udp));
            tested = true;
        });
        window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
        QVERIFY(tested);
    }
    void traceFovAndPredictSettings()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("aids.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        bool tested = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<swak::SettingsDialog *>();
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] {
                dialog->reject();
            });
            // FOV, Trace, and Predict each have a Settings page with a visibility switch.
            for (const auto *name : {"org.swak.fov/headingConeEnabled",
                                     "org.swak.fov/headingConeOutline",
                                     "org.swak.trace/visible",
                                     "org.swak.trace/positions",
                                     "org.swak.trace/opacity",
                                     "org.swak.trace/width",
                                     "org.swak.predict/visible",
                                     "org.swak.predict/historySeconds",
                                     "org.swak.predict/opacity",
                                     "org.swak.predict/width"}) {
                QVERIFY2(dialog->findChild<QWidget *>(QString::fromLatin1(name)), name);
            }
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
            QVERIFY(window.findChild<QLabel *>(QStringLiteral("cotEventCount"))
                        ->text()
                        .startsWith(QStringLiteral("CoT Messages:")));
            QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("statusSeparator")).size(), 3);
            QVERIFY(!window.findChild<QLabel *>(QStringLiteral("latestActivity")));
            history->clear();
            history->restorePreferences({{QStringLiteral("capacity"), 2}});
            swak::logMessage(swak::Severity::Error,
                             swak::Urgency::Immediate,
                             QStringLiteral("swak.test"),
                             QStringLiteral("Example failure"));
            QVERIFY(QTest::qWaitFor([&] {
                return history->rowCount() > 0;
            }));
            QCOMPARE(status->text(), QStringLiteral("Events: %1").arg(history->rowCount()));
            // A long event opens in full, and Copy puts one event on the clipboard.
            const auto longText = QStringLiteral("Long event ") + QString(1500, 'y');
            swak::logMessage(swak::Severity::Warning,
                             swak::Urgency::Routine,
                             QStringLiteral("swak.long"),
                             longText);
            QVERIFY(QTest::qWaitFor([&] {
                return table->model()->rowCount() > 0 &&
                       table->model()
                               ->index(0, swak::ActivityModel::MessageColumn)
                               .data()
                               .toString() == longText;
            }));
            QCOMPARE(table->model()->columnCount(), 4);
            table->setCurrentIndex(table->model()->index(0, 0));
            QGuiApplication::clipboard()->clear();
            table->findChild<QAction *>(QStringLiteral("copyActivityEvent"))->trigger();
            const auto copied = QGuiApplication::clipboard()->text();
            QVERIFY(copied.contains(QStringLiteral("\tWarning\tswak.long\t") + longText));
            QVERIFY(!copied.contains('\n'));
            QString shown;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                QCOMPARE(dialog->objectName(), QStringLiteral("activityDetails"));
                shown = dialog->findChild<QPlainTextEdit *>(QStringLiteral("activityDetailsText"))
                            ->toPlainText();
                QGuiApplication::clipboard()->clear();
                dialog->findChild<QPushButton *>(QStringLiteral("activityDetailsCopy"))->click();
                QVERIFY(dialog->grab().save(QStringLiteral("activity-details.png")));
                dialog->reject();
            });
            table->findChild<QAction *>(QStringLiteral("viewActivityEvent"))->trigger();
            QCOMPARE(shown, longText);
            QCOMPARE(QGuiApplication::clipboard()->text(), copied);
            QVERIFY(!table->isVisible());
            swak::logMessage(swak::Severity::Info,
                             swak::Urgency::Routine,
                             QStringLiteral("swak.test"),
                             QStringLiteral("Recovered"));
            QVERIFY(QTest::qWaitFor([&] {
                return history->rowCount() == 2;
            }));
            QCOMPARE(status->text(), QStringLiteral("Events: 2"));
            QVERIFY(!table->isVisible());
            auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
            QMenu *historyMenu = nullptr;
            for (auto *action : plugins->actions()) {
                if (action->text() == QStringLiteral("Events")) {
                    historyMenu = action->menu();
                }
            }
            QVERIFY(historyMenu);
            historyMenu->menuAction()->trigger();
            QVERIFY(table->isVisible());
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QTest::qWait(50);
            QVERIFY(window.screen()
                        ->grabWindow(window.winId())
                        .save(QStringLiteral("internal-events-smoke.png")));
            const auto countBeforeClear = history->rowCount();
            answerAction(Qt::Key_Escape);
            historyMenu->findChild<QAction *>(QStringLiteral("clearActivityAction"))->trigger();
            QCOMPARE(history->rowCount(), countBeforeClear);
            answerAction();
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
            QAction *none = nullptr, *provider = nullptr, *manage = nullptr;
            for (auto *action : menu->actions()) {
                if (action->text() == QStringLiteral("Disable")) {
                    none = action;
                } else if (action->text() == QStringLiteral("More ...")) {
                    manage = action;
                } else if (action->isEnabled() && action->isCheckable()) {
                    provider = action;
                }
            }
            QVERIFY(none && provider && manage);
            // Disable comes directly before More ... at the end of the menu.
            QCOMPARE(menu->actions().last(), manage);
            QCOMPARE(menu->actions().at(menu->actions().size() - 2), none);
            QVERIFY(menu->actions().at(menu->actions().size() - 3)->isSeparator());
            QVERIFY(none->isChecked());
            QCOMPARE(button->toolTip(), QStringLiteral("Map provider: Disabled"));
            provider->trigger();
            QVERIFY(!maps->activeSource().isEmpty());
            QCOMPARE(button->text(), maps->activeSource().value(QStringLiteral("name")).toString());
            none->trigger();
            QVERIFY(maps->activeSource().isEmpty());
            manage->trigger();
            QVERIFY(window.findChild<swak::MapManager *>());
            window.findChild<swak::MapManager *>()->close();
            auto values = maps->savePreferences();
            auto sources = values.value(QStringLiteral("sources")).toObject();
            sources.insert(QStringLiteral("absent"),
                           QJsonObject{{QStringLiteral("name"), QStringLiteral("Absent provider")},
                                       {QStringLiteral("format"), QStringLiteral("absent")}});
            values.insert(QStringLiteral("sources"), sources);
            maps->restorePreferences(values);
            QMetaObject::invokeMethod(menu, "aboutToShow");
            QAction *absent = nullptr;
            for (auto *entry : menu->actions()) {
                if (entry->data().toString() == QStringLiteral("absent")) {
                    absent = entry;
                }
            }
            QVERIFY(absent);
            QVERIFY(!absent->isEnabled());
            QVERIFY(window.savePreferences());
            QVERIFY(window.grab().save(QStringLiteral("activity-toolbar-smoke.png")));
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<swak::ActivityModel *>()
                     ->savePreferences()
                     .value(QStringLiteral("capacity"))
                     .toInt(),
                 2);
        QCOMPARE(restored.findChild<QTableView *>(QStringLiteral("activityTable"))
                     ->horizontalHeader()
                     ->sectionSize(0),
                 250);
    }

    void vectorMapAndInformation()
    {
        QTemporaryDir directory;
        TileServer server;
        server.body = vector_fixture::sample();
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const auto path = directory.filePath(QStringLiteral("vector.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *sources = window.findChild<swak::MapSources *>();
            const auto id = QStringLiteral("org.swak.mvt");
            auto source = sources->sources().value(id).toObject();
            source.insert(
                QStringLiteral("endpoint"),
                QStringLiteral("https://localhost:%1/{z}/{x}/{y}.pbf").arg(server.serverPort()));
            source.insert(QStringLiteral("ignoreTls"), true);
            QVERIFY(sources->put(id, source).isEmpty());
            sources->select(id);
            auto *map = window.findChild<QOpenGLWidget *>();
            QVERIFY(map);
            auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map);
            QVERIFY(preferences);
            QCOMPARE(preferences->savePreferences().value(QStringLiteral("tileCacheMiB")).toInt(),
                     256);
            preferences->restorePreferences(
                QJsonObject{{QStringLiteral("zoom"), 0}, {QStringLiteral("tileCacheMiB"), 128}});
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(QTest::qWaitFor(
                [&] {
                    const auto frame = map->grabFramebuffer();
                    return !frame.isNull() &&
                           frame.pixelColor(frame.width() / 2 - 70, frame.height() / 2 - 70) ==
                               QColor("#92bfd0");
                },
                5000));
            QVERIFY(map->grabFramebuffer().save(QStringLiteral("vector-map-smoke.png")));
            withObjectContext(map, QStringLiteral("Test place"), [](QMenu *menu) {
                auto *action = menu->findChild<QAction *>(QStringLiteral("mapInformation"));
                QVERIFY(action);
                const bool enabled = action->isEnabled();
                action->trigger();
                menu->close();
                QVERIFY(enabled);
            });
            const QPoint fixturePoint = map->rect().center() + QPoint(-128, -128);
            QContextMenuEvent information(
                QContextMenuEvent::Mouse, fixturePoint, map->mapToGlobal(fixturePoint));
            QApplication::sendEvent(map, &information);
            auto *dialog = map->findChild<QDialog *>(QStringLiteral("mapInformationDialog"));
            QVERIFY(dialog);
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            const auto details =
                dialog->findChild<QPlainTextEdit *>(QStringLiteral("mapFeatureProperties"))
                    ->toPlainText();
            QVERIFY(details.contains(QStringLiteral("Test place")));
            QVERIFY(details.contains(QStringLiteral("cafe")));
            QVERIFY(dialog->grab().save(QStringLiteral("vector-information-smoke.png")));
            dialog->close();
            withObjectContext(map, {}, [](QMenu *menu) {
                bool changed = false;
                for (auto *action : menu->actions()) {
                    if (auto *layers = action->menu()) {
                        for (auto *layer : layers->actions()) {
                            if (layer->text() == QStringLiteral("poi")) {
                                QVERIFY(layer->isChecked());
                                layer->trigger();
                                changed = true;
                            }
                        }
                    }
                }
                QVERIFY(changed);
                menu->close();
            });
            QContextMenuEvent layers(QContextMenuEvent::Keyboard,
                                     map->rect().center(),
                                     map->mapToGlobal(map->rect().center()));
            QApplication::sendEvent(map, &layers);
            QVERIFY(!sources->activeSource()
                         .value(QStringLiteral("visibleLayers"))
                         .toObject()
                         .value(QStringLiteral("poi"))
                         .toBool(true));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *restoredMap =
            dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>());
        QVERIFY(restoredMap);
        QCOMPARE(restoredMap->savePreferences().value(QStringLiteral("tileCacheMiB")).toInt(), 128);
        const auto source = restored.findChild<swak::MapSources *>()->activeSource();
        QVERIFY(!source.value(QStringLiteral("visibleLayers"))
                     .toObject()
                     .value(QStringLiteral("poi"))
                     .toBool(true));
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
        auto owner = std::make_unique<swak::Window>(directory.filePath(QStringLiteral("maps.json")),
                                                    plugins);
        auto &window = *owner;
        auto *sources = window.findChild<swak::MapSources *>();
        QVERIFY(sources);
        QCOMPARE(sources->providers().size(), 4);
        QVERIFY(!sources->networkEnabled());
        const auto id = QStringLiteral("org.swak.gdal.raster");
        auto source = sources->sources().value(id).toObject();
        source.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("https://localhost:%1/{z}/{x}/{y}.png").arg(server.serverPort()));
        source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(sources->put(id, source).isEmpty());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        sources->select(id);
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto frame = map->grabFramebuffer();
                if (frame.isNull()) {
                    return false;
                }
                for (int dx : {-100, 100}) {
                    for (int dy : {-100, 100}) {
                        if (frame.pixelColor(frame.width() / 2 + dx, frame.height() / 2 + dy) !=
                            QColor("#eb426a")) {
                            return false;
                        }
                    }
                }
                return true;
            },
            15000));
        auto *credit = map->findChild<QLabel *>(QStringLiteral("mapAttribution"));
        QVERIFY(credit);
        auto *cache = window.statusBar()->findChild<QLabel *>(QStringLiteral("mapStatus"));
        QVERIFY(cache);
        QVERIFY(QTest::qWaitFor([&] {
            return !cache->text().contains(QStringLiteral("inactive")) &&
                   !cache->text().contains(QStringLiteral("0 bytes"));
        }));
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
        for (auto *label : {cache, zoom, pointer, tls, tileStatus}) {
            QVERIFY(!label->text().contains('|'));
        }
        QCOMPARE(window.findChildren<QFrame *>(QStringLiteral("mapStatusSeparator")).size(), 5);
        source.insert(QStringLiteral("ignoreTls"), false);
        QVERIFY(sources->put(id, source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] {
            return tls->parentWidget()->isHidden();
        }));
        source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(sources->put(id, source).isEmpty());
        QVERIFY(QTest::qWaitFor([&] {
            return tls->isVisible();
        }));
        QVERIFY(QTest::qWaitFor([&] {
            return cache->toolTip() == cache->text();
        }));
        QVERIFY(cache->width() > 0);
        QVERIFY(
            window.screen()->grabWindow(window.winId()).save(QStringLiteral("map-status-bar.png")));
        QVERIFY(!credit->text().contains(QStringLiteral("HTTPS certificate")));
        QCOMPARE(credit->alignment(), Qt::AlignRight);
        QVERIFY(credit->text().contains(QStringLiteral("href=")));
        QVERIFY(!credit->text().contains(QStringLiteral("| https")));
        QVERIFY(credit->styleSheet().contains(QStringLiteral("140")));
        BrowserCapture browser;
        QDesktopServices::setUrlHandler(QStringLiteral("https"), &browser, "open");
        const auto resetHandler = qScopeGuard([] {
            QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
        });
        credit->setFocus();
        QTest::keyClick(credit, Qt::Key_Tab);
        QTest::keyClick(credit, Qt::Key_Return);
        QCOMPARE(browser.opened, QUrl(QStringLiteral("https://www.openstreetmap.org/copyright")));
        source.insert(QStringLiteral("attribution"), QStringLiteral("https://example.org/license"));
        QVERIFY(sources->put(id, source).isEmpty());
        map->grabFramebuffer();
        QVERIFY(credit->text().contains(QStringLiteral(">Map attribution</a>")));
        source.insert(
            QStringLiteral("attribution"),
            QStringLiteral("Example (https://example.org/license). | https://example.org/second"));
        QVERIFY(sources->put(id, source).isEmpty());
        map->grabFramebuffer();
        QCOMPARE(credit->text().count(QStringLiteral("href=")), 2);
        QVERIFY(credit->text().contains(QStringLiteral("href=\"https://example.org/license\"")));
        QVERIFY(window.screen()
                    ->grabWindow(window.winId())
                    .save(QStringLiteral("map-attribution-smoke.png")));
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
        QVERIFY(QTest::qWaitFor([&] {
            return sources->activeId() == id;
        }));
        QVERIFY(!dialog->findChild<QComboBox *>());
        QTimer::singleShot(0, dialog, [dialog] {
            auto *editor = dialog->findChild<swak::SettingsDialog *>();
            QVERIFY(editor);
            const auto close = qScopeGuard([editor] {
                editor->reject();
            });
            QVERIFY(!editor->findChild<QTabWidget *>());
            for (auto *field : editor->findChildren<QLineEdit *>()) {
                if (field->objectName().contains(QLatin1Char('/'))) {
                    QVERIFY(field->text().isEmpty());
                }
            }
            editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(editor->isVisible());
            QVERIFY(
                !editor->findChild<QLabel *>(QStringLiteral("settingsStatus"))->text().isEmpty());
            QVERIFY(QTest::qWaitForWindowExposed(editor));
            QVERIFY(editor->grab().save(QStringLiteral("add-provider-smoke.png")));
        });
        dialog->findChild<QPushButton *>(QStringLiteral("addMapProvider"))->click();
        QCOMPARE(list->rowCount(), originalCount);
        QTimer::singleShot(0, dialog, [dialog, id, source] {
            auto *editor = dialog->findChild<swak::SettingsDialog *>();
            QVERIFY(editor);
            const auto close = qScopeGuard([editor] {
                if (editor->isVisible()) {
                    editor->reject();
                }
            });
            auto *formats = editor->findChild<QComboBox *>(QStringLiteral("mapProviderFormats"));
            for (int i = 0; i < formats->count(); ++i) {
                formats->setCurrentIndex(i);
                if (editor->findChild<QLineEdit *>(id + QStringLiteral("/name"))->isVisible()) {
                    break;
                }
            }
            for (const auto &key : {QStringLiteral("name"),
                                    QStringLiteral("endpoint"),
                                    QStringLiteral("attribution")}) {
                editor->findChild<QLineEdit *>(id + '/' + key)
                    ->setText(source.value(key).toString());
            }
            editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        dialog->findChild<QPushButton *>(QStringLiteral("addMapProvider"))->click();
        QCOMPARE(list->rowCount(), originalCount + 1);
        answerDeletion(Qt::Key_Escape);
        dialog->findChild<QPushButton *>(QStringLiteral("removeMapProvider"))->click();
        QCOMPARE(list->rowCount(), originalCount + 1);
        answerDeletion();
        dialog->findChild<QPushButton *>(QStringLiteral("removeMapProvider"))->click();
        QCOMPARE(list->rowCount(), originalCount);
        for (int i = 0; i < list->rowCount(); ++i) {
            if (list->item(i, 1)->data(Qt::UserRole).toString() == id) {
                list->setCurrentCell(i, 1);
            }
        }
        QTimer::singleShot(0, dialog, [dialog, id] {
            auto *editor = dialog->findChild<swak::SettingsDialog *>();
            if (!editor) {
                return;
            }
            const auto close = qScopeGuard([editor] {
                if (editor->isVisible()) {
                    editor->reject();
                }
            });
            QVERIFY(!editor->findChild<QTabWidget *>());
            for (auto *label : editor->findChildren<QLabel *>()) {
                if (label->text() == QStringLiteral("Attribution and license URL")) {
                    QVERIFY(!label->wordWrap());
                }
            }
            QVERIFY(QTest::qWaitForWindowExposed(editor));
            QVERIFY(QTest::qWaitFor([editor] {
                return editor->findChild<QScrollArea *>()->horizontalScrollBar()->maximum() == 0;
            }));
            QVERIFY(editor->grab().save(QStringLiteral("edit-provider-smoke.png")));
            editor->findChild<QLineEdit *>(id + QStringLiteral("/name"))
                ->setText(QStringLiteral("Local test map"));
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
        QCOMPARE(missing.findChild<QLabel *>(QStringLiteral("mapManagerStatus"))->text(),
                 QStringLiteral("Map provider: Disabled"));
        missing.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
    }
    void mapInformationPane()
    {
        QTemporaryDir directory;
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body =
            "<kml "
            "xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark><name>Station</"
            "name><description>Read only</description><ExtendedData><Data "
            "name=\"status\"><value>Ready</value></Data></ExtendedData><Point><coordinates>0,0</"
            "coordinates></Point></Placemark></kml>";
        swak::Window window(directory.filePath(QStringLiteral("info.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *feeds = window.findChild<swak::DataSources *>();
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(map && feeds && selection);
        auto *pane = window.findChild<QWidget *>(QStringLiteral("informationPane"));
        QVERIFY(pane);
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        QVERIFY(dock);
        QVERIFY(!dock->isVisible());
        auto source = swak::dataDefaults();
        source.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
        source.insert(QStringLiteral("name"), QStringLiteral("Fixture"));
        source.insert(QStringLiteral("attribution"), QStringLiteral("Fixture"));
        source.insert(QStringLiteral("endpoint"),
                      QStringLiteral("https://localhost:%1/feed.kml").arg(server.serverPort()));
        source.insert(QStringLiteral("enabled"), true);
        source.insert(QStringLiteral("ignoreTls"), true);
        QVERIFY(feeds->put(QStringLiteral("test"), source).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return feeds->shapeCount(QStringLiteral("test")) == 1;
            },
            5000));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(selection->item());
        QVERIFY(dock->isVisible());
        auto *title = pane->findChild<QLabel *>(QStringLiteral("informationTitle"));
        QCOMPARE(title->text(), QStringLiteral("Station"));
        auto *description = pane->findChild<QPlainTextEdit *>();
        QVERIFY(description->isReadOnly());
        QCOMPARE(description->toPlainText(), QStringLiteral("Read only"));
        auto *table = pane->findChild<QTableWidget *>();
        QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
        bool copied = false;
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 0)->text() == QStringLiteral("status")) {
                table->scrollToItem(table->item(row, 1));
                QTest::mouseClick(table->viewport(),
                                  Qt::LeftButton,
                                  Qt::NoModifier,
                                  table->visualItemRect(table->item(row, 1)).center());
                QTest::mouseDClick(table->viewport(),
                                   Qt::LeftButton,
                                   Qt::NoModifier,
                                   table->visualItemRect(table->item(row, 1)).center());
                QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Ready"));
                copied = true;
            }
        }
        QVERIFY(copied);
        QTest::mouseDClick(title, Qt::LeftButton);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Station"));
        QTest::mouseDClick(description->viewport(), Qt::LeftButton);
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("Read only"));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        const auto selectedImage = map->grabFramebuffer();
        const QPoint imageCenter(selectedImage.width() / 2, selectedImage.height() / 2);
        const int outline = qRound(7 * map->devicePixelRatioF());
        QVERIFY(selectedImage.pixelColor(imageCenter + QPoint(outline, 0)).lightness() > 220);
        QVERIFY(window.screen()
                    ->grabWindow(window.winId())
                    .save(QStringLiteral("information-pane.png")));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(40, 40));
        QVERIFY(!selection->item());
        QVERIFY(!dock->isVisible());
        window.findChild<QPushButton *>(QStringLiteral("generateEvent"))->click();
        chooseMapObject(map, QStringLiteral("SWAK"));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->label, QStringLiteral("SWAK"));
        QVERIFY(selection->item()->properties.contains(QStringLiteral("UID")));
        QTest::keyClick(map, Qt::Key_Escape);
        QVERIFY(!selection->item());
        QVERIFY(window.store().selected().isEmpty());
        QVERIFY(!dock->isVisible());
        chooseMapObject(map, QStringLiteral("SWAK"));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(selection->item());
        window.store().clear();
        QVERIFY(!selection->item());
        withObjectContext(map, QStringLiteral("Station"), [](QMenu *menu) {
            auto *action = menu->findChild<QAction *>(QStringLiteral("mapItemInformation"));
            QVERIFY(action);
            action->trigger();
        });
        QContextMenuEvent context(
            QContextMenuEvent::Mouse, map->rect().center(), map->mapToGlobal(map->rect().center()));
        QApplication::sendEvent(map, &context);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->label, QStringLiteral("Station"));
        auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map);
        QVERIFY(preferences);
        preferences->restorePreferences({{QStringLiteral("zoom"), 0},
                                         {QStringLiteral("centerX"), 0},
                                         {QStringLiteral("centerY"), 1}});
        const auto values = preferences->savePreferences();
        const double scale = 256 * std::exp2(values.value(QStringLiteral("zoom")).toDouble());
        QVERIFY(scale >= map->width());
        QVERIFY(scale >= map->height());
        QVERIFY(values.value(QStringLiteral("centerX")).toDouble() * scale >=
                map->width() / 2.0 - 0.01);
        QVERIFY((1 - values.value(QStringLiteral("centerY")).toDouble()) * scale >=
                map->height() / 2.0 - 0.01);
        window.close();
    }
    void dataProviders()
    {
        QTemporaryDir directory;
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body = "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark><name>Feed "
                      "geometry</name><MultiGeometry>"
                      "<Point><coordinates>0,0</coordinates></"
                      "Point><LineString><coordinates>-25,15 25,15</coordinates></LineString>"
                      "<Polygon><outerBoundaryIs><LinearRing><coordinates>-30,-25 30,-25 30,-5 "
                      "-30,-5 -30,-25</coordinates></LinearRing></outerBoundaryIs></Polygon>"
                      "</MultiGeometry></Placemark></kml>";
        const auto profile = directory.filePath(QStringLiteral("data.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *feeds = window.findChild<swak::DataSources *>();
            QVERIFY(feeds);
            QCOMPARE(feeds->sources().size(), 2);
            window.findChild<QAction *>(QStringLiteral("dataProvidersAction"))->trigger();
            auto *manager = window.findChild<swak::DataManager *>();
            QVERIFY(manager);
            auto *list = manager->findChild<QTableWidget *>(QStringLiteral("dataProviderList"));
            QVERIFY(list);
            QCOMPARE(list->rowCount(), 2);
            QTimer::singleShot(0, manager, [manager, &server] {
                auto *dialog = manager->findChild<swak::SettingsDialog *>();
                QVERIFY(dialog);
                const auto close = qScopeGuard([dialog] {
                    if (dialog->isVisible()) {
                        dialog->reject();
                    }
                });
                QVERIFY(!dialog->findChild<QTabWidget *>());
                for (auto *field : dialog->findChildren<QLineEdit *>()) {
                    if (field->objectName().endsWith(QStringLiteral("/name"))) {
                        QVERIFY(field->text().isEmpty());
                        field->setText(QStringLiteral("A test feed"));
                    }
                    if (field->objectName().endsWith(QStringLiteral("/endpoint"))) {
                        QVERIFY(field->text().isEmpty());
                        field->setText(QStringLiteral("https://127.0.0.1:%1/feed.kml")
                                           .arg(server.serverPort()));
                    }
                    if (field->objectName().endsWith(QStringLiteral("/attribution"))) {
                        field->setText(QStringLiteral("Test data"));
                    }
                }
                for (auto *field : dialog->findChildren<QCheckBox *>()) {
                    if (field->objectName().endsWith(QStringLiteral("/ignoreTls"))) {
                        field->setChecked(true);
                    }
                }
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            manager->findChild<QPushButton *>(QStringLiteral("addDataProvider"))->click();
            QCOMPARE(list->rowCount(), 3);
            QCOMPARE(list->item(0, 1)->text(), QStringLiteral("A test feed"));
            const auto id = list->item(0, 1)->data(Qt::UserRole).toString();
            manager->findChild<QCheckBox *>(QStringLiteral("activeProvider/") + id)->click();
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return feeds->shapeCount(id) == 3;
                },
                5000));
            QVERIFY(QTest::qWaitForWindowExposed(manager));
            QVERIFY(manager->grab().save(QStringLiteral("data-providers-smoke.png")));
            manager->hide();
            auto *map = window.findChild<QOpenGLWidget *>();
            QVERIFY(map);
            QVERIFY(map->isValid());
            const auto image = map->grabFramebuffer();
            const auto center = image.pixelColor(image.width() / 2, image.height() / 2);
            QVERIFY(center.red() > 120);
            QVERIFY(image.save(QStringLiteral("data-layer-smoke.png")));
            QVERIFY(map->grab().save(QStringLiteral("data-attribution-smoke.png")));
            manager->show();
            QTimer::singleShot(0, manager, [manager] {
                auto *dialog = manager->findChild<swak::SettingsDialog *>();
                QVERIFY(dialog);
                for (auto *field : dialog->findChildren<QDoubleSpinBox *>()) {
                    if (field->objectName().endsWith(QStringLiteral("/interval"))) {
                        field->setValue(600);
                    }
                }
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            manager->findChild<QPushButton *>(QStringLiteral("editDataProvider"))->click();
            QCOMPARE(
                feeds->sources().value(id).toObject().value(QStringLiteral("interval")).toInt(),
                600);
            auto values = feeds->sources().value(id).toObject();
            values.insert(QStringLiteral("enabled"), false);
            QVERIFY(feeds->put(id, values).isEmpty());
            QVERIFY(window.savePreferences());
            manager->close();
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *feeds = restored.findChild<swak::DataSources *>();
        QCOMPARE(feeds->sources().size(), 3);
        restored.findChild<QAction *>(QStringLiteral("dataProvidersAction"))->trigger();
        auto *manager = restored.findChild<swak::DataManager *>();
        manager->findChild<QTableWidget *>(QStringLiteral("dataProviderList"))
            ->setCurrentCell(0, 1);
        const auto countBeforeDelete = feeds->sources().size();
        answerDeletion(Qt::Key_Escape);
        manager->findChild<QPushButton *>(QStringLiteral("removeDataProvider"))->click();
        QCOMPARE(feeds->sources().size(), countBeforeDelete);
        answerDeletion();
        manager->findChild<QPushButton *>(QStringLiteral("removeDataProvider"))->click();
        QCOMPARE(feeds->sources().size(), 2);
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
            toolbar->setObjectName(QStringLiteral("mainToolbar"));
            toolbar->hide();
            swak::Preferences preferences(path);
            QVERIFY(preferences.save(
                {{QStringLiteral("window"),
                  QJsonObject{{QStringLiteral("layout"),
                               QString::fromLatin1(legacy.saveState(1).toBase64())}}}}));
        }
        swak::Window window(path, QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *editor = window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
        auto *udp = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
        QCOMPARE(window.dockWidgetArea(editor), Qt::LeftDockWidgetArea);
        QVERIFY(window.tabifiedDockWidgets(editor).contains(udp));
        QVERIFY(window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))->isHidden());
        auto *map = window.findChild<QOpenGLWidget *>();
        const auto bounds = map->geometry();
        editor->hide();
        udp->hide();
        QCoreApplication::processEvents();
        QCOMPARE(map->geometry(), bounds);
        QCOMPARE(map->width(), window.contentsRect().width());
        QVERIFY(window.savePreferences());
    }
    void mapOverlayPanels()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("overlay.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *panels = window.findChild<QMainWindow *>(QStringLiteral("mapPanels"));
        QVERIFY(map && panels);
        QVERIFY(!panels->isWindow());
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(settings);
        const QRect bounds = map->geometry();
        const auto view = settings->savePreferences();
        for (auto *pane : window.findChildren<QDockWidget *>()) {
            pane->show();
            pane->raise();
            QVERIFY(QTest::qWaitFor([&] {
                return panels->mask().contains(pane->mapTo(panels, pane->rect().center()));
            }));
            QCOMPARE(map->geometry(), bounds);
            QCOMPARE(settings->savePreferences().value(QStringLiteral("zoom")),
                     view.value(QStringLiteral("zoom")));
            QCOMPARE(settings->savePreferences().value(QStringLiteral("centerX")),
                     view.value(QStringLiteral("centerX")));
            QCOMPARE(settings->savePreferences().value(QStringLiteral("centerY")),
                     view.value(QStringLiteral("centerY")));
            pane->hide();
        }
        QVERIFY(QTest::qWaitFor([&] {
            return !panels->mask().contains(panels->rect().center());
        }));
        QCOMPARE(QApplication::widgetAt(map->mapToGlobal(map->rect().center())), map);
        auto *info = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        info->show();
        info->raise();
        QVERIFY(QTest::qWaitFor([&] {
            return panels->mask().contains(info->mapTo(panels, info->rect().center()));
        }));
        QCOMPARE(map->geometry(), bounds);
        QVERIFY(QTest::qWaitFor([&] {
            return !panels->mask().contains(QPoint(100, panels->height() - 20));
        }));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(
            window.screen()->grabWindow(window.winId()).save(QStringLiteral("map-overlays.png")));
    }
    void markerCollectionArchiveDialogs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QtDialogs dialogs;
        swak::MarkerCollections collections(QPluginLoader::staticInstances());
        collections.setDirectory(directory.path());
        auto value = swak::coreMarkerCollection();
        const auto id = QStringLiteral("archive.ui");
        value.insert(QStringLiteral("id"), id);
        value.insert(QStringLiteral("name"), QStringLiteral("Custom"));
        value.insert(QStringLiteral("description"),
                     QStringLiteral("<img src='file:///private' />"));
        QVERIFY(collections.put(value).isEmpty());
        int saves = 0;
        swak::MarkerManager manager(collections, nullptr, [&] {
            ++saves;
            return QString{};
        });
        manager.show();
        QVERIFY(QTest::qWaitForWindowExposed(&manager));
        auto *table = manager.findChild<QTableWidget *>(QStringLiteral("markerCollectionList"));
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 1)->data(Qt::UserRole).toString() == id) {
                table->setCurrentCell(row, 1);
                QVERIFY(table->item(row, 1)->toolTip().contains(QStringLiteral("&lt;img")));
            }
        }
        const auto chooseFile = [&](const QString &button, const QString &path) {
            bool found = false;
            QTimer::singleShot(0, &manager, [&] {
                auto *dialog = manager.findChild<QFileDialog *>();
                QVERIFY(dialog);
                found = true;
                if (path.isEmpty()) {
                    dialog->reject();
                } else {
                    dialog->selectFile(path);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            manager.findChild<QPushButton *>(button)->click();
            QVERIFY(found);
        };
        const auto file = directory.filePath(QStringLiteral("symbols.zip"));
        chooseFile(QStringLiteral("exportMarkerCollection"), file.chopped(4));
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(saves, 1);
        chooseFile(QStringLiteral("exportMarkerCollection"), {});
        QCOMPARE(saves, 1);
        answerDeletion(Qt::Key_Escape);
        manager.findChild<QPushButton *>(QStringLiteral("deleteMarkerCollection"))->click();
        QVERIFY(collections.collections().contains(id));
        answerDeletion();
        manager.findChild<QPushButton *>(QStringLiteral("deleteMarkerCollection"))->click();
        QVERIFY(!collections.collections().contains(id));
        chooseFile(QStringLiteral("importMarkerCollection"), file);
        QVERIFY(collections.collections().contains(id));
        const auto before = collections.collections();
        chooseFile(QStringLiteral("importMarkerCollection"), file);
        QCOMPARE(collections.collections(), before);
        QVERIFY(!manager.findChild<QLabel *>(QStringLiteral("markerCollectionStatus"))
                     ->text()
                     .isEmpty());
        chooseFile(QStringLiteral("importMarkerCollection"), {});
        QCOMPARE(collections.collections(), before);
        QCOMPARE(collections.directory(), directory.path());
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 1)->data(Qt::UserRole).toString() == swak::CoreMarkerCollection) {
                table->setCurrentCell(row, 1);
            }
        }
        manager.findChild<QPushButton *>(QStringLiteral("editMarkerCollection"))->click();
        QVERIFY(manager.findChild<QLabel *>(QStringLiteral("markerCollectionStatus"))
                    ->text()
                    .contains(QStringLiteral("read-only")));
        answerDeletion();
        manager.findChild<QPushButton *>(QStringLiteral("deleteMarkerCollection"))->click();
        QVERIFY(collections.collections().contains(swak::CoreMarkerCollection));
    }
    void markerCollectionWorkflow()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("collections.json"));
        QString collectionId, markerId;
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *collections = window.findChild<swak::MarkerCollections *>();
            QVERIFY(collections);
            window.findChild<QAction *>(QStringLiteral("markerCollectionsAction"))->trigger();
            auto *manager = window.findChild<swak::MarkerManager *>();
            QVERIFY(manager);
            auto *table =
                manager->findChild<QTableWidget *>(QStringLiteral("markerCollectionList"));
            QVERIFY(table);
            QCOMPARE(table->rowCount(), 2);
            bool edited = false;
            QTimer::singleShot(0, manager, [&] {
                auto *dialog =
                    manager->findChild<QDialog *>(QStringLiteral("editMarkerCollectionDialog"));
                QVERIFY(dialog);
                dialog->findChild<QLineEdit *>(QStringLiteral("collectionName"))
                    ->setText(QStringLiteral("Custom units"));
                auto *source = dialog->findChild<QComboBox *>(QStringLiteral("collectionTemplate"));
                source->setCurrentIndex(source->findData(swak::MilitaryMarkerCollection));
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                edited = true;
            });
            manager->findChild<QPushButton *>(QStringLiteral("addMarkerCollection"))->click();
            QVERIFY(edited);
            QCOMPARE(table->rowCount(), 3);
            const auto entries = collections->collections();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (!collections->builtIn(it.key())) {
                    collectionId = it.key();
                }
            }
            QVERIFY(!collectionId.isEmpty());
            QTimer::singleShot(0, manager, [&] {
                auto *dialog =
                    manager->findChild<QDialog *>(QStringLiteral("editMarkerCollectionDialog"));
                QVERIFY(dialog);
                dialog->findChild<QLineEdit *>(QStringLiteral("collectionDescription"))
                    ->setText(QStringLiteral("Portable symbols"));
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            manager->findChild<QPushButton *>(QStringLiteral("editMarkerCollection"))->click();
            QCOMPARE(collections->collections()
                         .value(collectionId)
                         .toObject()
                         .value(QStringLiteral("description"))
                         .toString(),
                     QStringLiteral("Portable symbols"));
            auto *toggle = manager->findChild<QAbstractButton *>(QStringLiteral("activeProvider/") +
                                                                 collectionId);
            QVERIFY(toggle);
            toggle->click();
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return !collections->collections()
                                .value(collectionId)
                                .toObject()
                                .value(QStringLiteral("enabled"))
                                .toBool();
                },
                5000));
            toggle = manager->findChild<QAbstractButton *>(QStringLiteral("activeProvider/") +
                                                           collectionId);
            toggle->click();
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return collections->collections()
                        .value(collectionId)
                        .toObject()
                        .value(QStringLiteral("enabled"))
                        .toBool();
                },
                5000));
            QTimer::singleShot(0, manager, [&] {
                auto *dialog =
                    manager->findChild<QDialog *>(QStringLiteral("editMarkerCollectionDialog"));
                QVERIFY(dialog);
                dialog->reject();
            });
            manager->findChild<QPushButton *>(QStringLiteral("addMarkerCollection"))->click();
            QCOMPARE(table->rowCount(), 3);
            QVERIFY(
                collections
                    ->exportArchive(collectionId, directory.filePath(QStringLiteral("custom.zip")))
                    .isEmpty());
            QVERIFY(manager->grab().save(QStringLiteral("marker-collections.png")));
            manager->close();
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            auto *objects = window.findChild<swak::UserObjects *>();
            auto *selection = window.findChild<swak::MapSelection *>();
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            auto *collection = editor->findChild<QComboBox *>(QStringLiteral("objectCollection"));
            auto *type = editor->findChild<QComboBox *>(QStringLiteral("objectType"));
            QCOMPARE(collection->currentData().toString(), swak::CoreMarkerCollection);
            QCOMPARE(type->count(), 1);
            collection->setCurrentIndex(collection->findData(collectionId));
            QCOMPARE(type->count(), 16);
            type->setCurrentIndex(type->findData(QStringLiteral("a-h-G-U-C-I")));
            QVERIFY(editor->grab().save(QStringLiteral("marker-collection-editor.png")));
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
            markerId = objects->items().firstKey();
            QCOMPARE(objects->find(markerId)->collection, collectionId);
            QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
            map->update();
            QVERIFY(frames.wait(2000));
            QVERIFY(window.screen()
                        ->grabWindow(window.winId())
                        .save(QStringLiteral("marker-collection-map.png")));
            const auto outline = selection->item()->markerOutline;
            QVERIFY(!outline.isEmpty());
            QVERIFY(collections->setEnabled(collectionId, false).isEmpty());
            QCOMPARE(selection->item()->markerOutline, outline);
            editor->edit(*objects->find(markerId));
            QCOMPARE(collection->currentData().toString(), collectionId);
            editor->cancel();
            QVERIFY(collections->remove(collectionId).isEmpty());
            QCOMPARE(selection->item()->type, QStringLiteral("Unavailable marker"));
            editor->edit(*objects->find(markerId));
            QCOMPARE(collection->currentData().toString(), collectionId);
            QCOMPARE(type->currentData().toString(), QStringLiteral("a-h-G-U-C-I"));
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
            QCOMPARE(objects->find(markerId)->collection, collectionId);
            QVERIFY(collections->importArchive(directory.filePath(QStringLiteral("custom.zip")))
                        .isEmpty());
            QCOMPARE(selection->item()->markerOutline, outline);
            objects->remove(markerId);
            objects->undoStack().undo();
            QCOMPARE(objects->find(markerId)->collection, collectionId);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored));
        auto *collections = restored.findChild<swak::MarkerCollections *>();
        QVERIFY(collections->collections().contains(collectionId));
        auto *objects = restored.findChild<swak::UserObjects *>();
        QCOMPARE(objects->find(markerId)->collection, collectionId);
        auto *editor = restored.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
        QCOMPARE(editor->newCollection(), collectionId);
        QCOMPARE(editor->newSymbol(), QStringLiteral("a-h-G-U-C-I"));
        for (const auto &id : collections->collections().keys()) {
            QVERIFY(collections->setEnabled(id, false).isEmpty());
        }
        QVERIFY(editor->newCollection().isEmpty());
        auto *map = restored.findChild<QOpenGLWidget *>();
        map->setFocus();
        QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(!editor->draft());
        QCOMPARE(objects->items().size(), 1);
        QVERIFY(restored.findChild<QLabel *>(QStringLiteral("mapToolStatus"))->text().isEmpty());
        QTest::keyClick(map, Qt::Key_Escape);
    }
    void militaryMarkerWorkflow()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("symbols.json"));
        QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            auto *objects = window.findChild<swak::UserObjects *>();
            auto *selection = window.findChild<swak::MapSelection *>();
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            auto *type = editor->findChild<QComboBox *>(QStringLiteral("objectType"));
            auto *collection = editor->findChild<QComboBox *>(QStringLiteral("objectCollection"));
            QVERIFY(collection);
            collection->setCurrentIndex(collection->findData(swak::MilitaryMarkerCollection));
            for (const auto &identity : {QStringLiteral("f"),
                                         QStringLiteral("h"),
                                         QStringLiteral("n"),
                                         QStringLiteral("u")}) {
                const auto symbol = QStringLiteral("a-%1-G-U").arg(identity);
                type->setCurrentIndex(type->findData(symbol));
                auto *group = editor->findChild<QButtonGroup *>();
                QVERIFY(group);
                auto *color = group->checkedButton();
                QVERIFY(color);
                auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject"));
                apply->click();
                const auto record = objects->items().first();
                QCOMPARE(record.color, color->property("markerColor").toString());
                editor->edit(record);
                QVERIFY(color->isChecked());
            }
            const auto code = QStringLiteral("a-h-G-U-C-I");
            type->setCurrentIndex(type->findData(code));
            QVERIFY(editor->grab().save(QStringLiteral("military-marker-editor.png")));
            auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject"));
            apply->click();
            id = objects->items().firstKey();
            QCOMPARE(objects->find(id)->symbol, code);
            QCOMPARE(objects->find(id)->color, QStringLiteral("#ff8080"));
            QCOMPARE(selection->item()->symbol, code);
            QVERIFY(selection->item()->type.contains(QStringLiteral("infantry")));
            objects->remove(id);
            objects->undoStack().undo();
            QCOMPARE(objects->find(id)->symbol, code);
            editor->edit(*objects->find(id));
            QCOMPARE(type->currentData().toString(), code);
            type->setCurrentIndex(0);
            editor->cancel();
            QCOMPARE(objects->find(id)->symbol, code);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<swak::UserObjects *>()->find(id)->symbol,
                 QStringLiteral("a-h-G-U-C-I"));
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored));
        auto *map = restored.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(settings);
        const auto values = settings->savePreferences();
        const auto position =
            swak::unproject(QPointF(values.value(QStringLiteral("centerX")).toDouble(),
                                    values.value(QStringLiteral("centerY")).toDouble()));
        swak::Event event;
        event.uid = QStringLiteral("received-symbol");
        event.type = QStringLiteral("a-n-G-U-C-A");
        event.how = QStringLiteral("h-e");
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addSecs(60);
        event.latitude = position.y();
        event.longitude = position.x();
        QVERIFY(restored.store().accept(event).isEmpty());
        restored.findChild<swak::UserObjects *>()->remove(id);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        auto *selection = restored.findChild<swak::MapSelection *>();
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->symbol, event.type);
    }
    void markerAppearance()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("colors.json"));
        QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            auto *objects = window.findChild<swak::UserObjects *>();
            auto *selection = window.findChild<swak::MapSelection *>();
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            auto *type = editor->findChild<QComboBox *>(QStringLiteral("objectType"));
            QVERIFY(type);
            QCOMPARE(type->count(), 1);
            QCOMPARE(type->currentText(), QStringLiteral("Generic marker"));
            QVERIFY(!type->itemIcon(0).isNull());
            auto *orange = editor->findChild<QToolButton *>(QStringLiteral("objectColor1"));
            QVERIFY(orange && orange->isChecked());
            for (int i = 0; i < 12; ++i) {
                QVERIFY(editor->findChild<QToolButton *>(QStringLiteral("objectColor%1").arg(i)));
            }
            auto *blue = editor->findChild<QToolButton *>(QStringLiteral("objectColor5"));
            blue->click();
            QVERIFY(blue->isChecked());
            QVERIFY(!orange->isChecked());
            QVERIFY(editor->grab().save(QStringLiteral("marker-colors.png")));
            auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject"));
            apply->click();
            id = objects->items().firstKey();
            QCOMPARE(objects->find(id)->color, QStringLiteral("#1e88e5"));
            QCOMPARE(selection->item()->color, QColor("#1e88e5"));
            objects->remove(id);
            objects->undoStack().undo();
            QCOMPARE(objects->find(id)->color, QStringLiteral("#1e88e5"));
            editor->edit(*objects->find(id));
            QVERIFY(blue->isChecked());
            editor->findChild<QToolButton *>(QStringLiteral("objectColor0"))->click();
            editor->cancel();
            QCOMPARE(objects->find(id)->color, QStringLiteral("#1e88e5"));
            editor->edit(*objects->find(id));
            editor->findChild<QToolButton *>(QStringLiteral("objectColor7"))->click();
            apply->click();
            QCOMPARE(selection->item()->color, QColor("#d81b60"));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<swak::UserObjects *>()->find(id)->color,
                 QStringLiteral("#d81b60"));
    }
    void undoMenuHistory()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("undo.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *objects = window.findChild<swak::UserObjects *>();
        QVERIFY(objects);
        auto *undo = window.findChild<QAction *>(QStringLiteral("undoAction"));
        QVERIFY(undo);
        auto *menu = window.findChild<QMenu *>(QStringLiteral("editMenu"));
        QVERIFY(menu);
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        QVERIFY(menu->actions().contains(undo));
        QVERIFY(!undo->isEnabled());
        for (int i = 0; i < 3; ++i) {
            swak::UserObject item;
            item.id = QString::number(i);
            item.type = QStringLiteral("circle");
            item.title = item.id;
            item.geometry = {{QStringLiteral("x"), 0.5}, {QStringLiteral("y"), 0.5}};
            QVERIFY(objects->put(item).isEmpty());
            objects->remove(item.id);
        }
        for (int remaining = 3; remaining > 0; --remaining) {
            QVERIFY(undo->isEnabled());
            if (remaining == 2) {
                map->setFocus();
                QTest::keyClick(map, Qt::Key_Z, Qt::ControlModifier);
            } else {
                menu->popup(window.mapToGlobal(QPoint(30, 30)));
                QVERIFY(QTest::qWaitForWindowExposed(menu));
                menu->setActiveAction(undo);
                QTest::keyClick(menu, Qt::Key_Return);
            }
            QCOMPARE(objects->undoStack().index(), remaining - 1);
            QCOMPARE(undo->isEnabled(), remaining > 1);
        }
        QCOMPARE(objects->items().size(), 3);
        menu->popup(window.mapToGlobal(QPoint(30, 30)));
        QVERIFY(QTest::qWaitForWindowExposed(menu));
        QVERIFY(!undo->isEnabled());
        QVERIFY(menu->grab().save(QStringLiteral("undo-empty-menu.png")));
        menu->setActiveAction(undo);
        QTest::keyClick(menu, Qt::Key_Return);
        menu->close();
        map->setFocus();
        QTest::keyClick(map, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(objects->items().size(), 3);
        QCOMPARE(objects->undoStack().index(), 0);
        objects->remove(QStringLiteral("0"));
        QVERIFY(undo->isEnabled());
        objects->undoStack().clear();
        QVERIFY(!undo->isEnabled());
    }
    void markerFocusAfterApply_data()
    {
        QTest::addColumn<bool>("floatingEditor");
        QTest::addColumn<bool>("floatingInformation");
        for (bool editor : {false, true}) {
            for (bool information : {false, true}) {
                QTest::newRow(qPrintable(
                    QStringLiteral("editor-%1-information-%2").arg(editor).arg(information)))
                    << editor << information;
            }
        }
    }
    void markerFocusAfterApply()
    {
        QFETCH(bool, floatingEditor);
        QFETCH(bool, floatingInformation);
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("focus.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *editorDock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.markers.pane"));
        auto *infoDock =
            window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        QVERIFY(map && editor && objects && editorDock && infoDock);
        auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"));
        auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("mapToolStatus"));
        QVERIFY(title && apply && status);
        editorDock->setFloating(floatingEditor);
        infoDock->setFloating(floatingInformation);
        editorDock->hide();
        infoDock->hide();
        window.activateWindow();
        map->setFocus();
        QVERIFY(QTest::qWaitFor([&] {
            return map->hasFocus();
        }));
        for (int cycle = 0; cycle < 6; ++cycle) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_M, Qt::ShiftModifier);
            QCOMPARE(status->text(), QStringLiteral("M"));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            QVERIFY(editor->draft());
            editor->window()->activateWindow();
            QTest::mouseClick(title, Qt::LeftButton);
            QVERIFY(QTest::qWaitFor([&] {
                return title->hasFocus();
            }));
            QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Marker M"));
            QVERIFY(status->text().isEmpty());
            if (cycle % 2 == 0) {
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
            } else {
                QTest::mouseClick(apply, Qt::LeftButton);
            }
            QCOMPARE(objects->items().size(), cycle + 1);
            QVERIFY(!editor->draft());
            QVERIFY(infoDock->isVisible());
            QVERIFY2(QTest::qWaitFor([&] {
                         return map->hasFocus();
                     }),
                     "Saving must return keyboard focus to the map without another user action");
        }
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_M, Qt::ShiftModifier);
        QCOMPARE(status->text(), QStringLiteral("M"));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        QVERIFY(status->text().isEmpty());
    }
    void markerPointerLatency()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("pointer.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (auto *pane : window.findChildren<QDockWidget *>()) {
            pane->hide();
        }
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *overlay = map->findChild<QWidget *>(QStringLiteral("mapToolOverlay"));
        QVERIFY(overlay);
        map->setFocus();
        QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
        const QPoint current(210, 180);
        QCursor::setPos(map->mapToGlobal(current));
        QVERIFY(QTest::qWaitFor([&] {
            return map->mapFromGlobal(QCursor::pos()) == current;
        }));
        QTest::qWait(80);
        struct PaintCounter final : QObject
        {
            int count = 0;
            bool eventFilter(QObject *, QEvent *event) override
            {
                if (event->type() == QEvent::Paint) {
                    ++count;
                }
                return false;
            }
        } overlayPaints, mapPaints;
        overlay->installEventFilter(&overlayPaints);
        map->installEventFilter(&mapPaints);
        const QPoint stale(150, 120);
        QMouseEvent motion(QEvent::MouseMove,
                           stale,
                           map->mapToGlobal(stale),
                           Qt::NoButton,
                           Qt::NoButton,
                           Qt::NoModifier);
        QApplication::sendEvent(map, &motion);
        QVERIFY2(QTest::qWaitFor(
                     [&] {
                         return overlayPaints.count > 0;
                     },
                     200),
                 "Guide painting did not complete promptly");
        QCOMPARE(mapPaints.count, 0);
        const auto frame = overlay->grab().toImage();
        const qreal scale = overlay->devicePixelRatioF();
        const auto at = [&](int x, int y) {
            return frame.pixelColor(qRound(x * scale), qRound(y * scale));
        };
        QVERIFY(at(current.x(), 40) != at(current.x() + 10, 40));
        QCOMPARE(at(stale.x(), 40), at(stale.x() + 10, 40));
        QTest::keyClick(map, Qt::Key_Escape);
        // Sanitizer builds can deliver the Escape paint after 80 ms. Wait until paints stop.
        for (int previous = -1, i = 0; i < 25 && overlayPaints.count != previous; ++i) {
            previous = overlayPaints.count;
            QTest::qWait(80);
        }
        overlayPaints.count = 0;
        QTest::qWait(80);
        QCOMPARE(overlayPaints.count, 0);
    }
    void editableMarkers()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("markers.json"));
        QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *objects = window.findChild<swak::UserObjects *>();
            QVERIFY(map && objects);
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
            QVERIFY(editor);
            auto *selection = window.findChild<swak::MapSelection *>();
            auto *pane = window.findChild<QDockWidget *>(QStringLiteral("org.swak.markers.pane"));
            QVERIFY(pane);
            auto *regular =
                window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
            QVERIFY(regular);
            auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"));
            auto *description =
                editor->findChild<QPlainTextEdit *>(QStringLiteral("objectDescription"));
            auto *table = editor->findChild<QTableWidget *>(QStringLiteral("objectProperties"));
            auto *apply = editor->findChild<QPushButton *>(QStringLiteral("applyObject"));
            QVERIFY(title && description && table && apply);
            map->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return map->hasFocus();
            }));
            auto *modeTools = window.findChild<swak::MapTools *>();
            QVERIFY(modeTools);
            QVERIFY(!modeTools->guidesActive());
            auto *modeStatus = window.findChild<QLabel *>(QStringLiteral("mapToolStatus"));
            QVERIFY(modeStatus);
            QVERIFY(modeStatus->text().isEmpty());
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QKeyEvent repeat(
                QEvent::KeyPress, Qt::Key_M, Qt::ShiftModifier, QStringLiteral("M"), true);
            QApplication::sendEvent(map, &repeat);
            QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QVERIFY(modeStatus->text().isEmpty());
            QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            QVERIFY(!modeTools->guidesActive());
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QVERIFY(modeTools->guidesActive());
            auto *tools = window.findChild<swak::MapTools *>();
            QVERIFY(tools);
            QSignalSpy sceneChanges(tools, &swak::MapTools::changed);
            QSignalSpy overlayChanges(tools, &swak::MapTools::overlayChanged);
            const QPoint point = map->rect().center();
            QTest::mouseMove(map, point);
            QCOMPARE(sceneChanges.count(), 0);
            QVERIFY(overlayChanges.count() > 0);
            QImage guides(map->size(), QImage::Format_ARGB32_Premultiplied);
            guides.fill(Qt::transparent);
            {
                QPainter painter(&guides);
                swak::Viewport view;
                view.size = map->size();
                tools->paintOverlay(painter, view, point);
            }
            const auto guide = guides.pixelColor(point.x(), 30);
            QCOMPARE(guide.red(), 0);
            QCOMPARE(guide.alpha(), 128);
            QVERIFY(map->findChild<QWidget *>(QStringLiteral("mapToolOverlay")));
            struct PaintCounter final : QObject
            {
                int count = 0;
                bool eventFilter(QObject *, QEvent *event) override
                {
                    if (event->type() == QEvent::Paint) {
                        ++count;
                    }
                    return false;
                }
            } paints;
            map->installEventFilter(&paints);
            QTest::qWait(80);
            paints.count = 0;
            for (int i = 1; i <= 20; ++i) {
                QTest::mouseMove(map, point + QPoint(i, i));
                QTest::qWait(2);
            }
            QVERIFY2(
                paints.count <= 2,
                qPrintable(
                    QStringLiteral("Pointer motion repainted the map %1 times").arg(paints.count)));
            QSignalSpy guideFrames(map, &QOpenGLWidget::frameSwapped);
            QTest::mouseMove(map, point);
            QVERIFY(guideFrames.count() > 0 || guideFrames.wait(2000));
            map->removeEventFilter(&paints);

            QVERIFY(QTest::qWaitFor([&] {
                return map->isValid();
            }));
            QVERIFY(window.screen()
                        ->grabWindow(window.winId())
                        .save(QStringLiteral("marker-crosshair.png")));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
            QVERIFY(modeStatus->text().isEmpty());
            QVERIFY(!modeTools->guidesActive());
            QVERIFY(pane->isVisible());
            QVERIFY(editor->draft());
            QVERIFY(objects->items().isEmpty());
            QCOMPARE(title->placeholderText(), QStringLiteral("New Marker"));
            QVERIFY(editor->grab().save(QStringLiteral("marker-editor.png")));
            title->setText(QStringLiteral("Observation post"));
            description->setPlainText(QStringLiteral("Local marker description"));
            table->setRowCount(2);
            for (int row = 0; row < 2; ++row) {
                table->setItem(row, 0, new QTableWidgetItem(QStringLiteral("Unit")));
                table->setItem(row, 1, new QTableWidgetItem(QStringLiteral("Alpha")));
            }
            QTest::keyClick(title, Qt::Key_Return);
            QVERIFY(objects->items().isEmpty());
            QVERIFY(editor->draft());
            table->removeRow(1);
            QTest::keyClick(title, Qt::Key_Return);
            QCOMPARE(objects->items().size(), 1);
            QVERIFY(!editor->draft());
            id = objects->items().firstKey();
            QCOMPARE(objects->find(id)->title, QStringLiteral("Observation post"));
            QVERIFY(selection->item());
            QCOMPARE(selection->item()->id, id);
            QVERIFY(regular->isVisible());
            QVERIFY(!pane->isVisible());
            QCOMPARE(selection->item()->properties.value(QStringLiteral("Unit")).toString(),
                     QStringLiteral("Alpha"));
            const auto position = [&] {
                const auto value =
                    dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
                const double scale =
                    256 * std::exp2(value.value(QStringLiteral("zoom")).toDouble());
                const auto geometry = objects->find(id)->geometry;
                return QPointF((geometry.value(QStringLiteral("x")).toDouble() -
                                value.value(QStringLiteral("centerX")).toDouble()) *
                                       scale +
                                   map->width() / 2.0,
                               (geometry.value(QStringLiteral("y")).toDouble() -
                                value.value(QStringLiteral("centerY")).toDouble()) *
                                       scale +
                                   map->height() / 2.0)
                    .toPoint();
            };
            const auto context = [&](const QString &actionName) {
                QTimer::singleShot(0, map, [actionName] {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                    QVERIFY(menu);
                    auto *action = menu->findChild<QAction *>(actionName);
                    QVERIFY(action);
                    if (actionName.startsWith(QStringLiteral("delete"))) {
                        QCOMPARE(menu->actions().at(0)->objectName(), QStringLiteral("editMarker"));
                        QCOMPARE(menu->actions().at(1), action);
                        QCOMPARE(action->text(), QStringLiteral("Delete ..."));
                        answerDeletion();
                    }
                    action->trigger();
                    menu->close();
                });
                const auto hitPoint = position();
                QContextMenuEvent event(
                    QContextMenuEvent::Mouse, hitPoint, map->mapToGlobal(hitPoint));
                QApplication::sendEvent(map, &event);
            };
            context(QStringLiteral("editMarker"));
            QVERIFY(editor->draft());
            QCOMPARE(title->text(), QStringLiteral("Observation post"));
            title->setText(QStringLiteral("Discarded"));
            QTest::keyClick(title, Qt::Key_Escape);
            QVERIFY(!editor->draft());
            QCOMPARE(objects->find(id)->title, QStringLiteral("Observation post"));
            context(QStringLiteral("editMarker"));
            title->setText(QStringLiteral("Updated marker"));
            apply->click();
            QCOMPARE(objects->find(id)->title, QStringLiteral("Updated marker"));
            const auto original = objects->find(id)->geometry;
            context(QStringLiteral("moveMarker"));
            QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            QTest::mouseMove(map, position() + QPoint(70, 40));
            QCOMPARE(objects->find(id)->geometry, original);
            QTest::keyClick(map, Qt::Key_Escape);
            QCOMPARE(objects->find(id)->geometry, original);
            QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            QVERIFY(modeStatus->text().isEmpty());
            context(QStringLiteral("moveMarker"));
            QCOMPARE(modeStatus->text(), QStringLiteral("M"));
            QTest::mouseMove(map, position() + QPoint(40, 30));
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QCOMPARE(objects->find(id)->geometry, original);
            QVERIFY(modeStatus->text().isEmpty());
            context(QStringLiteral("moveMarker"));
            const auto destination = position() + QPoint(70, 40);
            QTest::mouseMove(map, destination);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, destination);
            QVERIFY(objects->find(id)->geometry != original);
            auto *undo = window.findChild<QAction *>(QStringLiteral("undoAction"));
            QVERIFY(undo);
            QVERIFY(!undo->isEnabled());
            context(QStringLiteral("deleteMarker"));
            QVERIFY(objects->items().isEmpty());
            QVERIFY(!selection->item());
            QVERIFY(undo->isEnabled());
            map->setFocus();
            QTest::keyClick(map, Qt::Key_Z, Qt::ControlModifier);
            QCOMPARE(objects->items().size(), 1);
            QCOMPARE(objects->find(id)->title, QStringLiteral("Updated marker"));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, position());
            QVERIFY(selection->item());
            answerDeletion(Qt::Key_Escape);
            QTest::keyClick(map, Qt::Key_Backspace);
            QCOMPARE(objects->items().size(), 1);
            answerDeletion();
            QTest::keyClick(map, Qt::Key_Backspace);
            QVERIFY(objects->items().isEmpty());
            undo->trigger();
            QCOMPARE(objects->items().size(), 1);
            auto *readOnly = regular->findChild<QPlainTextEdit *>();
            QVERIFY(readOnly);
            answerDeletion();
            QTest::keyClick(readOnly, Qt::Key_Delete);
            QVERIFY(objects->items().isEmpty());
            undo->trigger();
            QCOMPARE(objects->items().size(), 1);
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QTest::keyClick(map, Qt::Key_Escape);
            QVERIFY(!editor->draft());
            QCOMPARE(objects->items().size(), 1);
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            QVERIFY(editor->draft());
            QTest::keyClick(title, Qt::Key_Escape);
            QVERIFY(!editor->draft());
            QCOMPARE(objects->items().size(), 1);
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
            QTest::keyClick(title, Qt::Key_Return);
            QCOMPARE(objects->items().size(), 2);
            QCOMPARE(selection->item()->label, QStringLiteral("New Marker"));
            const auto temporaryId = selection->item()->id;
            objects->remove(temporaryId);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, position());
            QVERIFY(selection->item());
            QCOMPARE(selection->item()->id, id);
            regular->setFloating(true);
            regular->show();
            regular->activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(regular));
            readOnly->setFocus();
            answerDeletion();
            QTest::keyClick(readOnly, Qt::Key_Delete);
            QVERIFY(!objects->find(id));
            QTest::keyClick(readOnly, Qt::Key_Z, Qt::ControlModifier);
            QVERIFY(objects->find(id));
            regular->setFloating(false);
            regular->show();
            window.activateWindow();
            map->setFocus();
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, position());
            QVERIFY(QTest::qWaitFor([&] {
                const auto frame = map->grabFramebuffer();
                const auto pixel = position() * map->devicePixelRatioF();
                return !frame.isNull() && frame.rect().contains(pixel) &&
                       frame.pixelColor(pixel) == QColor("#ffc857");
            }));
            QVERIFY(QTest::qWaitFor([&] {
                return regular->isVisible();
            }));
            QVERIFY(window.screen()
                        ->grabWindow(window.winId())
                        .save(QStringLiteral("marker-information.png")));
            auto *configuration = dynamic_cast<swak::SettingsParticipant *>(editor);
            QVERIFY(configuration);
            auto options = configuration->savePreferences();
            options.insert(QStringLiteral("createKey"), QStringLiteral("N"));
            options.insert(QStringLiteral("radius"), 9);
            QVERIFY(configuration->validateSettings(options).isEmpty());
            configuration->restorePreferences(options);
            map->setFocus();
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            QTest::keyClick(map, Qt::Key_N);
            QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            QTest::keyClick(map, Qt::Key_Escape);
            options.insert(QStringLiteral("color"), QStringLiteral("invalid"));
            QVERIFY(!configuration->validateSettings(options).isEmpty());
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *objects = restored.findChild<swak::UserObjects *>();
        QCOMPARE(objects->items().size(), 1);
        QCOMPARE(objects->find(id)->title, QStringLiteral("Updated marker"));
        QCOMPARE(objects->find(id)->description, QStringLiteral("Local marker description"));
        QVERIFY(!objects->undoStack().canUndo());
        const auto configuration =
            dynamic_cast<swak::SettingsParticipant *>(
                restored.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")))
                ->savePreferences();
        QCOMPARE(configuration.value(QStringLiteral("createKey")).toString(), QStringLiteral("N"));
        QCOMPARE(configuration.value(QStringLiteral("radius")).toInt(), 9);
    }
    void keybindingsAndDismissal()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("keys.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
            QVERIFY(settings);
            settings->restorePreferences({{QStringLiteral("zoom"), 4},
                                          {QStringLiteral("panNorthKey"), QStringLiteral("Q")}});
            QCOMPARE(settings->savePreferences().value(QStringLiteral("panNorthKey")).toString(),
                     QStringLiteral("Q"));
            QVERIFY(settings->savePreferences()
                        .value(QStringLiteral("zoomInKey"))
                        .toString()
                        .isEmpty());
            settings->restorePreferences({{QStringLiteral("zoom"), 4}});
            const auto zoom = [&] {
                return settings->savePreferences().value(QStringLiteral("zoom")).toInt();
            };
            map->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return map->hasFocus();
            }));
            QTest::keyClick(map, Qt::Key_Q);
            QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_E);
            QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_Plus, Qt::ShiftModifier);
            QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_Minus);
            QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_Equal);
            QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_Minus);
            QCOMPARE(zoom(), 4);
            const auto center = [&] {
                const auto v = settings->savePreferences();
                return QPointF(v.value(QStringLiteral("centerX")).toDouble(),
                               v.value(QStringLiteral("centerY")).toDouble());
            };
            for (const auto key : {Qt::Key_A, Qt::Key_D, Qt::Key_W, Qt::Key_S}) {
                const auto origin = center();
                QTest::keyPress(map, key);
                QVERIFY(QTest::qWaitFor(
                    [&] {
                        return center() != origin;
                    },
                    1000));
                QTest::keyRelease(map, key);
                if (key == Qt::Key_A) {
                    QVERIFY(center().x() < origin.x());
                }
                if (key == Qt::Key_D) {
                    QVERIFY(center().x() > origin.x());
                }
                if (key == Qt::Key_W) {
                    QVERIFY(center().y() < origin.y());
                }
                if (key == Qt::Key_S) {
                    QVERIFY(center().y() > origin.y());
                }
            }
            QTest::keyClick(map, Qt::Key_E, Qt::ControlModifier);
            QCOMPARE(zoom(), 4);
            auto *selection = window.findChild<swak::MapSelection *>();
            QVERIFY(selection);
            auto *pane = window.findChild<QWidget *>(QStringLiteral("informationPane"));
            auto *dock =
                window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
            QVERIFY(pane && dock);
            swak::MapItem item;
            item.label = QStringLiteral("Selected point");
            selection->select(item);
            QVERIFY(dock->isVisible());
            QTest::keyClick(map, Qt::Key_Escape);
            QVERIFY(!selection->item());
            QVERIFY(!dock->isVisible());
            selection->select(item);
            dock->setFloating(true);
            dock->show();
            dock->activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(dock));
            auto *description = pane->findChild<QPlainTextEdit *>();
            description->setFocus();
            QTest::keyClick(description, Qt::Key_Q);
            QCOMPARE(zoom(), 4);
            QTest::keyClick(description, Qt::Key_Escape);
            QVERIFY(!selection->item());
            QVERIFY(!dock->isVisible());
            dock->show();
            QTest::keyClick(description, Qt::Key_Escape);
            QVERIFY(!dock->isVisible());
            selection->select(item);
            {
                QDialog modal(&window);
                modal.setModal(true);
                modal.show();
                QVERIFY(QTest::qWaitForWindowExposed(&modal));
                QTest::keyClick(&modal, Qt::Key_Escape);
                QVERIFY(selection->item());
                QVERIFY(dock->isVisible());
            }
            auto *info = dynamic_cast<swak::SettingsParticipant *>(pane);
            QVERIFY(info);
            swak::SettingsDialog dialog(
                {{settings->preferenceId(),
                  settings->settingsTitle(),
                  settings->settingsFields(),
                  settings->savePreferences(),
                  [settings](const QJsonObject &v) {
                      return settings->validateSettings(v);
                  },
                  [settings](const QJsonObject &v) {
                      settings->restorePreferences(v);
                  }},
                 {info->preferenceId(),
                  info->settingsTitle(),
                  info->settingsFields(),
                  info->savePreferences(),
                  [info](const QJsonObject &v) {
                      return info->validateSettings(v);
                  },
                  [info](const QJsonObject &v) {
                      info->restorePreferences(v);
                  }}},
                [] {
                    return QString{};
                },
                &window);
            dialog.show();
            dialog.findChild<QTabWidget *>()->setCurrentIndex(2);
            auto *keys =
                dialog.findChild<QListWidget *>(QStringLiteral("keybindingSettingsSections"));
            QVERIFY(keys);
            QCOMPARE(keys->count(), 2);
            keys->setCurrentRow(1);
            auto *in =
                dialog.findChild<QKeySequenceEdit *>(QStringLiteral("org.swak.map/zoomInKey"));
            QVERIFY(in);
            auto *out =
                dialog.findChild<QKeySequenceEdit *>(QStringLiteral("org.swak.map/zoomOutKey"));
            QVERIFY(out);
            auto *close = dialog.findChild<QKeySequenceEdit *>(
                QStringLiteral("org.swak.information/dismissKey"));
            QVERIFY(close);
            in->setKeySequence(QKeySequence(Qt::Key_R));
            out->setKeySequence(QKeySequence(Qt::Key_R));
            auto *apply = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
            apply->click();
            QCOMPARE(settings->savePreferences().value(QStringLiteral("zoomInKey")).toString(),
                     QStringLiteral("Q"));
            out->setKeySequence(QKeySequence(Qt::Key_T));
            close->setKeySequence(QKeySequence(Qt::Key_F12));
            apply->click();
            QCOMPARE(info->savePreferences().value(QStringLiteral("dismissKey")).toString(),
                     QStringLiteral("F12"));
            QVERIFY(QTest::qWaitForWindowExposed(&dialog));
            QVERIFY(dialog.screen()
                        ->grabWindow(dialog.winId())
                        .save(QStringLiteral("keybindings-settings.png")));
            const auto navigation = dialog.navigation();
            QCOMPARE(navigation.value(QStringLiteral("tab")).toString(),
                     QStringLiteral("keybindings"));
            dialog.close();
            window.activateWindow();
            map->setFocus();
            QTest::keyClick(map, Qt::Key_Q);
            QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_R);
            QCOMPARE(zoom(), 5);
            QTest::keyClick(map, Qt::Key_T);
            QCOMPARE(zoom(), 4);
            QTest::keyClick(map, Qt::Key_Escape);
            QVERIFY(selection->item());
            QTest::keyClick(map, Qt::Key_F12);
            QVERIFY(!selection->item());
            QVERIFY(!dock->isVisible());
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *map =
            dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QOpenGLWidget *>());
        QCOMPARE(map->savePreferences().value(QStringLiteral("zoomInKey")).toString(),
                 QStringLiteral("R"));
        auto *info = dynamic_cast<swak::SettingsParticipant *>(
            restored.findChild<QWidget *>(QStringLiteral("informationPane")));
        QCOMPARE(info->savePreferences().value(QStringLiteral("dismissKey")).toString(),
                 QStringLiteral("F12"));
    }
    void deleteConfirmationKeys()
    {
        QWidget parent;
        parent.show();
        answerDeletion(Qt::Key_Escape);
        QVERIFY(!swak::confirmDeletion(&parent, QStringLiteral("<b>Area</b>")));
        QTimer::singleShot(0, &parent, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QVERIFY(dialog->grab().save(QStringLiteral("delete-confirmation.png")));
            QTest::keyClick(dialog, Qt::Key_Return);
        });
        QVERIFY(swak::confirmDeletion(&parent, QStringLiteral("Area")));
    }
    void modalParentDestroyed()
    {
        // Each case deletes the parent inside the nested loop. A stack dialog got a second delete.
        auto *owner = new QWidget;
        owner->show();
        swak::Modal<QDialog> dialog(owner);
        QTimer::singleShot(0, owner, [owner] {
            delete owner;
        });
        QCOMPARE(dialog->exec(), int(QDialog::Rejected));
        QVERIFY(!dialog);

        auto *menuOwner = new QWidget;
        menuOwner->show();
        swak::Modal<QMenu> menu(menuOwner);
        menu->addAction(QStringLiteral("Item"));
        QTimer::singleShot(0, menuOwner, [menuOwner] {
            delete menuOwner;
        });
        QVERIFY(!menu->exec(QPoint(10, 10)));
        QVERIFY(!menu);

        QWidget top;
        auto *pane = new QWidget(&top);
        top.show();
        QTimer::singleShot(0, &top, [pane] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            delete pane;
            QVERIFY(box);
            QTest::keyClick(box, Qt::Key_Return);
        });
        QVERIFY(!swak::confirmDeletion(pane, QStringLiteral("Area")));

        QTemporaryDir directory;
        QPointer<swak::Window> window = new swak::Window(
            directory.filePath(QStringLiteral("modal.json")), QPluginLoader::staticInstances());
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *settings = window->findChild<QAction *>(QStringLiteral("settingsAction"));
        QVERIFY(settings);
        QTimer::singleShot(0, window, [window] {
            QVERIFY(qobject_cast<swak::SettingsDialog *>(QApplication::activeModalWidget()));
            delete window.data();
        });
        settings->trigger();
        QVERIFY(!window);
    }
    void navigationLock()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("locked.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
            auto *lock = window.findChild<QAction *>(QStringLiteral("mapNavigationLock"));
            QVERIFY(lock && settings);
            settings->restorePreferences({{QStringLiteral("zoom"), 5}});
            map->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return map->hasFocus();
            }));
            auto *toolbar = window.findChild<QToolBar *>();
            QCOMPARE(toolbar->actions().last(), lock);
            swak::arrangeToolbarSections(*toolbar);
            QCOMPARE(toolbar->actions().last(), lock);
            QCOMPARE(
                toolbar->findChildren<QWidget *>(QStringLiteral("toolbarTrailingSpace")).size(), 1);
            const auto icon = lock->icon().pixmap(24, 24).toImage();
            QTest::keyPress(map, Qt::Key_D);
            QTest::qWait(40);
            lock->trigger();
            QVERIFY(lock->isChecked());
            QVERIFY(lock->icon().pixmap(24, 24).toImage() != icon);
            const auto before = settings->savePreferences();
            QTest::qWait(80);
            QTest::keyRelease(map, Qt::Key_D);
            for (auto key : {Qt::Key_A, Qt::Key_D, Qt::Key_W, Qt::Key_S, Qt::Key_Q, Qt::Key_E}) {
                QTest::keyPress(map, key);
                QTest::qWait(20);
                QTest::keyRelease(map, key);
            }
            QTest::mousePress(map, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
            QTest::mouseMove(map, QPoint(180, 180));
            QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, QPoint(180, 180));
            QTest::mouseDClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(50, 50));
            const auto wheel = [&] {
                QWheelEvent event(QPointF(100, 100),
                                  map->mapToGlobal(QPoint(100, 100)),
                                  {},
                                  QPoint(0, 120),
                                  Qt::NoButton,
                                  Qt::NoModifier,
                                  Qt::NoScrollPhase,
                                  false);
                QApplication::sendEvent(map, &event);
            };
            wheel();
            QCOMPARE(settings->savePreferences(), before);
            lock->trigger();
            wheel();
            QVERIFY(settings->savePreferences().value(QStringLiteral("zoom")) !=
                    before.value(QStringLiteral("zoom")));
            auto *pane =
                window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
            QVERIFY(pane);
            pane->show();
            pane->raise();
            auto *table = pane->findChild<QTableWidget *>();
            QVERIFY(table);
            table->setRowCount(1);
            table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Value")));
            table->setCurrentCell(0, 0);
            table->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return !map->hasFocus();
            }));
            const auto focused = settings->savePreferences();
            for (auto key : {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down}) {
                QTest::keyPress(table, key);
                QTest::qWait(50);
                QTest::keyRelease(table, key);
            }
            QCOMPARE(settings->savePreferences(), focused);
            lock->trigger();
            QVERIFY(window.savePreferences());
            QVERIFY(window.grab().save(QStringLiteral("navigation-lock.png")));
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        QVERIFY(restored.findChild<QAction *>(QStringLiteral("mapNavigationLock"))->isChecked());
    }
    void keyboardStepKeys()
    {
        // The step keys have no default. When bound, each press moves the view by 64 pixels.
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("steps.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(settings);
        map->setFocus();
        QVERIFY(QTest::qWaitFor([&] {
            return map->hasFocus();
        }));
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("zoom"), 4);
        values.insert(QStringLiteral("stepWestKey"), QStringLiteral("J"));
        values.insert(QStringLiteral("stepEastKey"), QStringLiteral("L"));
        values.insert(QStringLiteral("stepNorthKey"), QStringLiteral("I"));
        values.insert(QStringLiteral("stepSouthKey"), QStringLiteral("K"));
        settings->restorePreferences(values);
        const auto center = [&] {
            const auto state = settings->savePreferences();
            return QPointF(state.value(QStringLiteral("centerX")).toDouble(),
                           state.value(QStringLiteral("centerY")).toDouble());
        };
        // At zoom 4 the world is 4096 pixels wide.
        const double step = 64.0 / 4096.0;
        for (const auto &[key, offset] : {std::pair{Qt::Key_J, QPointF(-step, 0)},
                                          std::pair{Qt::Key_L, QPointF(step, 0)},
                                          std::pair{Qt::Key_I, QPointF(0, -step)},
                                          std::pair{Qt::Key_K, QPointF(0, step)}}) {
            const auto before = center();
            QTest::keyClick(map, key);
            const auto moved = center() - before;
            QVERIFY2(std::abs(moved.x() - offset.x()) < 1e-9 &&
                         std::abs(moved.y() - offset.y()) < 1e-9,
                     qPrintable(QStringLiteral("%1 %2").arg(moved.x()).arg(moved.y())));
        }
        // The minus key zooms out.
        QTest::keyClick(map, Qt::Key_Minus);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("zoom")).toDouble(), 3.0);
    }
    void keyboardPanAndWheelSettings()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("navigation.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
            QVERIFY(settings);
            map->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return map->hasFocus();
            }));
            QJsonObject values{{QStringLiteral("zoom"), 4}, {QStringLiteral("panSpeed"), 320}};
            settings->restorePreferences(values);
            const auto center = [&] {
                const auto state = settings->savePreferences();
                return QPointF(state.value(QStringLiteral("centerX")).toDouble(),
                               state.value(QStringLiteral("centerY")).toDouble());
            };
            const auto zoom = [&] {
                return settings->savePreferences().value(QStringLiteral("zoom")).toDouble();
            };
            const auto origin = center();
            QElapsedTimer clock;
            clock.start();
            QTest::keyPress(map, Qt::Key_W);
            QTest::qWait(240);
            QKeyEvent repeat(
                QEvent::KeyPress, Qt::Key_W, Qt::NoModifier, QStringLiteral("w"), true);
            QApplication::sendEvent(map, &repeat);
            QKeyEvent repeatRelease(
                QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier, QStringLiteral("w"), true);
            QApplication::sendEvent(map, &repeatRelease);
            QTest::qWait(240);
            QTest::keyRelease(map, Qt::Key_W);
            const double pixels = (origin.y() - center().y()) * 4096;
            const double expected = static_cast<double>(clock.elapsed()) * 0.32;
            QVERIFY(pixels > expected * 0.65 && pixels < expected * 1.2);
            QCOMPARE(center().x(), origin.x());
            auto stopped = center();
            QTest::qWait(80);
            QCOMPARE(center(), stopped);
            for (const auto key : {Qt::Key_A, Qt::Key_S, Qt::Key_D}) {
                const auto before = center();
                QTest::keyPress(map, key);
                QVERIFY(QTest::qWaitFor(
                    [&] {
                        return center() != before;
                    },
                    1000));
                QTest::keyRelease(map, key);
                if (key == Qt::Key_A) {
                    QVERIFY(center().x() < before.x());
                }
                if (key == Qt::Key_S) {
                    QVERIFY(center().y() > before.y());
                }
                if (key == Qt::Key_D) {
                    QVERIFY(center().x() > before.x());
                }
            }
            QTest::keyPress(map, Qt::Key_W);
            QTest::keyPress(map, Qt::Key_S);
            stopped = center();
            QTest::qWait(80);
            QCOMPARE(center(), stopped);
            QTest::keyRelease(map, Qt::Key_W);
            QTest::keyRelease(map, Qt::Key_S);
            QTest::keyPress(map, Qt::Key_D);
            auto *editorPane =
                window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"));
            QVERIFY(editorPane);
            editorPane->show();
            editorPane->raise();
            auto *editor = editorPane->findChild<QLineEdit *>();
            QVERIFY(editor);
            editor->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return !map->hasFocus();
            }));
            stopped = center();
            QTest::qWait(80);
            QCOMPARE(center(), stopped);
            QTest::keyRelease(map, Qt::Key_D);
            values.insert(QStringLiteral("panNorthKey"), QStringLiteral("I"));
            values.insert(QStringLiteral("panWestKey"), QStringLiteral("J"));
            values.insert(QStringLiteral("panSouthKey"), QStringLiteral("K"));
            values.insert(QStringLiteral("panEastKey"), QStringLiteral("L"));
            QVERIFY(settings->validateSettings(values).isEmpty());
            auto invalid = values;
            invalid.insert(QStringLiteral("panEastKey"), QStringLiteral("I"));
            QVERIFY(!settings->validateSettings(invalid).isEmpty());
            invalid = values;
            invalid.insert(QStringLiteral("panEastKey"), QStringLiteral("Ctrl+L"));
            QVERIFY(!settings->validateSettings(invalid).isEmpty());
            values.insert(QStringLiteral("wheelZoom"), false);
            settings->restorePreferences(values);
            const auto wheel = [&](QPointF position) {
                QWheelEvent event(position,
                                  map->mapToGlobal(position.toPoint()),
                                  QPoint{},
                                  QPoint(0, 120),
                                  Qt::NoButton,
                                  Qt::NoModifier,
                                  Qt::NoScrollPhase,
                                  false);
                QApplication::sendEvent(map, &event);
            };
            wheel(map->rect().center());
            QCOMPARE(zoom(), 4.0);
            values.insert(QStringLiteral("wheelZoom"), true);
            values.insert(QStringLiteral("wheelReverse"), true);
            values.insert(QStringLiteral("wheelSteps"), 2);
            settings->restorePreferences(values);
            wheel(QPointF(-10, -10));
            QCOMPARE(zoom(), 4.0);
            wheel(map->rect().center());
            QCOMPARE(
                zoom(),
                std::max(2.0, std::ceil(std::log2(std::max(map->width(), map->height()) / 256.0))));
            map->setFocus();
            QVERIFY(QTest::qWaitFor([&] {
                return map->hasFocus();
            }));
            QTest::keyPress(map, Qt::Key_I);
            QVERIFY(QTest::qWaitFor([&] {
                return center().y() < 0.5;
            }));
            QTest::keyRelease(map, Qt::Key_I);
            QTest::keyPress(map, Qt::Key_L);
            map->hide();
            stopped = center();
            QTest::qWait(80);
            QCOMPARE(center(), stopped);
            map->show();
            QTest::keyRelease(map, Qt::Key_L);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        const auto state =
            dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>())
                ->savePreferences();
        QCOMPARE(state.value(QStringLiteral("panNorthKey")).toString(), QStringLiteral("I"));
        QCOMPARE(state.value(QStringLiteral("panSpeed")).toDouble(), 320.0);
        QCOMPARE(state.value(QStringLiteral("wheelSteps")).toInt(), 2);
        QVERIFY(state.value(QStringLiteral("wheelReverse")).toBool());
    }
    void selectionStrokeAtZoom_data()
    {
        QTest::addColumn<int>("zoom");
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("feed");
        for (int zoom : {2, 12, 19}) {
            for (int kind : {0, 1, 2}) {
                for (bool feed : {false, true}) {
                    if (feed && kind == 2) {
                        continue;
                    }
                    QTest::newRow(qPrintable(
                        QStringLiteral("zoom-%1-kind-%2-feed-%3").arg(zoom).arg(kind).arg(feed)))
                        << zoom << kind << feed;
                }
            }
        }
    }
    void selectionStrokeAtZoom()
    {
        QFETCH(int, zoom);
        QFETCH(int, kind);
        QFETCH(bool, feed);
        TileServer server;
        swak::DataSources sources(QPluginLoader::staticInstances());
        SelectionCanvas canvas;
        canvas.resize(320, 240);
        canvas.view = {QSizeF(320, 240), QPointF(0.63, 0.37), static_cast<double>(zoom)};
        QPainterPath path;
        if (kind == 0) {
            path.moveTo(50, 70);
            path.lineTo(270, 170);
        } else {
            path.addRect(QRectF(50, 50, 220, 140));
        }
        QTransform world;
        world.translate(canvas.view.center.x(), canvas.view.center.y());
        world.scale(1 / canvas.view.scale(), 1 / canvas.view.scale());
        world.translate(-160, -120);
        swak::MapItem item;
        item.kind = kind == 0 ? swak::MapItem::Kind::Line : swak::MapItem::Kind::Polygon;
        item.color = Qt::black;
        item.lineWidth = 2;
        item.path = world.map(path);
        item.outlineStyle = kind == 2 ? Qt::DashLine : Qt::SolidLine;
        if (feed) {
            QVERIFY(server.load(QFINDTESTDATA("fixtures")));
            QVERIFY(server.listen(QHostAddress::LocalHost));
            QStringList coordinates;
            for (int i = 0; i < item.path.elementCount(); ++i) {
                const auto point = item.path.elementAt(i);
                const auto coordinate = swak::unproject({point.x, point.y});
                coordinates.append(
                    QStringLiteral("%1,%2").arg(QString::number(coordinate.x(), 'g', 17),
                                                QString::number(coordinate.y(), 'g', 17)));
            }
            const auto geometry =
                kind == 0 ? QStringLiteral("<LineString><coordinates>%1</coordinates></LineString>")
                          : QStringLiteral("<Polygon><outerBoundaryIs><LinearRing><coordinates>%1</"
                                           "coordinates></LinearRing></outerBoundaryIs></Polygon>");
            server.body =
                (QStringLiteral("<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Placemark>") +
                 geometry.arg(coordinates.join(' ')) + QStringLiteral("</Placemark></kml>"))
                    .toUtf8();
            auto source = swak::dataDefaults();
            source.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
            source.insert(QStringLiteral("name"), QStringLiteral("Stroke test"));
            source.insert(QStringLiteral("attribution"), QStringLiteral("Test fixture"));
            source.insert(
                QStringLiteral("endpoint"),
                QStringLiteral("https://127.0.0.1:%1/stroke.kml").arg(server.serverPort()));
            source.insert(QStringLiteral("enabled"), true);
            source.insert(QStringLiteral("ignoreTls"), true);
            source.insert(QStringLiteral("color"), QStringLiteral("#ffffff"));
            source.insert(QStringLiteral("opacity"), 1);
            source.insert(QStringLiteral("fillOpacity"), 35.0 / 255);
            source.insert(QStringLiteral("lineWidth"), 2);
            source.insert(QStringLiteral("labels"), false);
            QVERIFY(sources.put(QStringLiteral("stroke"), source).isEmpty());
            QVERIFY2(QTest::qWaitFor(
                         [&] {
                             return sources.shapeCount(QStringLiteral("stroke")) == 1;
                         },
                         5000),
                     qPrintable(sources.status(QStringLiteral("stroke"))));
            canvas.data = &sources;
        }
        canvas.selection.select(item);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QVERIFY(QTest::qWaitFor([&] {
            return canvas.isValid();
        }));
        const auto actual = canvas.grabFramebuffer();
        QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied);
        expected.setDevicePixelRatio(canvas.devicePixelRatioF());
        expected.fill(Qt::black);
        {
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::Antialiasing);
            QPen pen(Qt::white, kind == 0 && !feed ? 4 : 2);
            pen.setStyle(item.outlineStyle);
            painter.setPen(pen);
            painter.setBrush(kind == 0 ? Qt::NoBrush : QBrush(QColor(255, 255, 255, 35)));
            painter.drawPath(path);
        }
        // Sum coverage so different edge antialiasing does not change the width measurement.
        const auto inkCoverage = [](const QImage &image) {
            qint64 sum = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    sum += image.pixelColor(x, y).red();
                }
            }
            return sum;
        };
        const auto actualCount = inkCoverage(actual), expectedCount = inkCoverage(expected);
        QVERIFY2(std::abs(actualCount - expectedCount) < expectedCount / 8,
                 qPrintable(QStringLiteral("Rendered intensity is %1; expected about %2.")
                                .arg(actualCount)
                                .arg(expectedCount)));
        QVERIFY(
            actual.save(QStringLiteral("selection-%1-%2-%3.png").arg(zoom).arg(kind).arg(feed)));
    }

    void polygonInputLimits()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("polygon-limits.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("polygonEditor"));
        QVERIFY(editor);
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(editor);
        QVERIFY(settings);
        auto configuration = settings->savePreferences();
        configuration.insert(QStringLiteral("createKey"), QStringLiteral("Ctrl+P"));
        QVERIFY(!settings->validateSettings(configuration).isEmpty());
        configuration.insert(QStringLiteral("createKey"), QStringLiteral("O"));
        QVERIFY(settings->validateSettings(configuration).isEmpty());
        settings->restorePreferences(configuration);
        auto *tools = window.findChild<swak::MapTools *>();
        QVERIFY(tools);
        auto *tool = editor->findChild<swak::MapTool *>();
        QVERIFY(tool);
        const swak::Viewport view{QSizeF(800, 600), {0.5, 0.5}, 2};
        const auto click = [&](QPointF point) {
            QMouseEvent event(QEvent::MouseButtonRelease,
                              point,
                              point,
                              Qt::LeftButton,
                              Qt::NoButton,
                              Qt::NoModifier);
            tool->handle(event, view, *map);
        };
        QTest::keyClick(map, Qt::Key_O);
        QCOMPARE(tools->statusText(), QStringLiteral("A"));
        click({400, 200});
        click({400, 200});
        QVERIFY(!editor->draft());
        tools->cancel();
        for (int repeat = 0; repeat < 4; ++repeat) {
            tool->activate(*map);
            for (int i = 0; i < swak::MaxPolygonVertices; ++i) {
                const double angle = i * 6.283185307179586 / swak::MaxPolygonVertices;
                click({400 + 200 * std::cos(angle), 300 + 200 * std::sin(angle)});
            }
            click({400, 300});
            QKeyEvent complete(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QVERIFY(tool->handle(complete, view, *map));
            QVERIFY(editor->draft());
            QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(),
                     swak::MaxPolygonVertices);
            QImage overlay(800, 600, QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            {
                QPainter painter(&overlay);
                tool->paintOverlay(painter, view, {500, 500});
            }
            QCOMPARE(overlay.pixelColor(600, 300).alpha(), 0);
            editor->cancel();
        }
        tool->activate(*map);
        click({100, 100});
        click({300, 300});
        click({100, 300});
        click({300, 100});
        QKeyEvent complete(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QVERIFY(tool->handle(complete, view, *map));
        QVERIFY(!editor->draft());
        QCOMPARE(tools->statusText(), QStringLiteral("A"));
        tools->cancel();
        QVERIFY(!tools->guidesActive());
        QVERIFY(window.findChild<swak::UserObjects *>()->items().isEmpty());
    }

    void pluginManifestPanel()
    {
        const auto plugins = swak::builtInPlugins();
        QVERIFY(!plugins.isEmpty());
        QWidget owner;
        auto *panel = swak::createPluginPanel(plugins, &owner);
        auto *table = panel->findChild<QTableWidget *>(QStringLiteral("pluginTable"));
        auto *details = panel->findChild<QPlainTextEdit *>(QStringLiteral("pluginDetails"));
        QVERIFY(table);
        QVERIFY(details);
        QVERIFY(details->isReadOnly());
        QCOMPARE(table->rowCount(), plugins.size());
        QCOMPARE(table->editTriggers(), QAbstractItemView::NoEditTriggers);
        owner.resize(650, 700);
        auto *layout = new QVBoxLayout(&owner);
        layout->addWidget(panel);
        owner.show();
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        for (int direction = 0; direction < 2; ++direction) {
            table->sortItems(0, direction == 0 ? Qt::AscendingOrder : Qt::DescendingOrder);
            for (int row = 0; row < table->rowCount(); ++row) {
                table->selectRow(row);
                const auto manifest = table->item(row, 0)->data(Qt::UserRole).toJsonObject();
                QVERIFY(details->toPlainText().contains(
                    manifest.value(QStringLiteral("id")).toString()));
                QVERIFY(details->toPlainText().contains(
                    manifest.value(QStringLiteral("description")).toString()));
                for (auto it = manifest.begin(); it != manifest.end(); ++it) {
                    QVERIFY(details->toPlainText().contains(it.key() + QStringLiteral(": ")));
                }
            }
        }
        table->sortItems(0);
        table->selectRow(0);
        table->scrollToItem(table->item(0, 0));
        QVERIFY(owner.grab().save(QStringLiteral("plugins-manifest.png")));
        table->clearSelection();
        QVERIFY(details->toPlainText().isEmpty());
        QObject unknown;
        QVERIFY(swak::pluginManifest(&unknown).isEmpty());
        QVERIFY(swak::pluginManifest(nullptr).isEmpty());
        auto *empty = swak::createPluginPanel({&unknown}, &owner);
        QCOMPARE(empty->findChild<QTableWidget *>()->rowCount(), 0);
        QVERIFY(empty->findChild<QPlainTextEdit *>()->toPlainText().isEmpty());
        delete empty;
    }

    void menusProvidersAndWheel()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("menus.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QVERIFY(plugins);
        for (auto *action : plugins->actions()) {
            QVERIFY(action->text() != QStringLiteral("Circle and line tools"));
        }
        for (int i = 0; i < 3; ++i) {
            QVERIFY(plugins->actions().contains(
                window.findChild<QAction *>(QStringLiteral("shapeMode%1").arg(i))));
        }
        const auto sorted = [](const QList<QAction *> &actions) {
            QCollator collator;
            collator.setCaseSensitivity(Qt::CaseInsensitive);
            QString previous;
            for (const auto *action : actions) {
                if (action->isSeparator()) {
                    return false;
                }
                auto text = action->text();
                text.remove('&');
                if (!previous.isEmpty() && collator.compare(previous, text) > 0) {
                    return false;
                }
                previous = text;
            }
            return true;
        };
        QStringList menuTitles;
        for (auto *action : window.menuBar()->actions()) {
            menuTitles.append(QString(action->text()).remove('&'));
        }
        QCOMPARE(menuTitles,
                 (QStringList{QStringLiteral("File"),
                              QStringLiteral("Edit"),
                              QStringLiteral("View"),
                              QStringLiteral("Tools"),
                              QStringLiteral("Help")}));
        for (auto *menu : window.findChildren<QMenu *>()) {
            auto actions = menu->actions();
            if (menu->objectName() == QStringLiteral("viewMenu")) {
                QCOMPARE(actions.takeLast()->objectName(), QStringLiteral("resetLayoutAction"));
                QVERIFY(actions.takeLast()->isSeparator());
            }
            if (menu->objectName() == QStringLiteral("helpMenu")) {
                QCOMPARE(actions.takeFirst()->objectName(), QStringLiteral("creditsAction"));
            }
            if (menu->objectName() == QStringLiteral("fileMenu")) {
                QCOMPARE(actions.takeLast(),
                         window.findChild<QAction *>(QStringLiteral("quitAction")));
                QVERIFY(actions.takeLast()->isSeparator());
            }
            QVERIFY(sorted(actions));
        }
        QCOMPARE(window.findChild<QToolBar *>()->toggleViewAction()->text(),
                 QStringLiteral("Toolbar"));
        auto *mapMenu =
            window.findChild<QToolButton *>(QStringLiteral("mapProviderButton"))->menu();
        qsizetype actionCount = 0;
        for (int i = 0; i < 5; ++i) {
            mapMenu->popup(window.mapToGlobal(QPoint(60, 60)));
            QVERIFY(QTest::qWaitForWindowExposed(mapMenu));
            auto entries = mapMenu->actions();
            QCOMPARE(entries.takeLast()->text(), QStringLiteral("More ..."));
            QCOMPARE(entries.takeLast()->text(), QStringLiteral("Disable"));
            QVERIFY(entries.takeLast()->isSeparator());
            QVERIFY(sorted(entries));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const auto count =
                mapMenu->findChildren<QAction *>(QString{}, Qt::FindDirectChildrenOnly).size();
            if (i == 0) {
                actionCount = count;
            } else {
                QCOMPARE(count, actionCount);
            }
            mapMenu->close();
        }
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(servers);
        auto server = swak::serverDefaults();
        server.insert(QStringLiteral("name"), QStringLiteral("Zulu test"));
        server.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        server.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        server.insert(QStringLiteral("port"), 1);
        server.insert(QStringLiteral("enabled"), false);
        QVERIFY(servers->put(QStringLiteral("local-test"), server).isEmpty());
        auto *data = window.findChild<swak::DataSources *>();
        QVERIFY(data);
        auto feed = swak::dataDefaults();
        feed.insert(QStringLiteral("attribution"), QStringLiteral("Test"));
        feed.insert(QStringLiteral("name"), QStringLiteral("Zulu feed"));
        feed.insert(QStringLiteral("format"), QStringLiteral("org.swak.kml"));
        feed.insert(QStringLiteral("endpoint"), QStringLiteral("https://127.0.0.1:1/test.kml"));
        feed.insert(QStringLiteral("enabled"), false);
        QVERIFY(data->put(QStringLiteral("local-test"), feed).isEmpty());
        for (const auto &name :
             {QStringLiteral("serverProviderButton"), QStringLiteral("dataProviderButton")}) {
            auto *button = window.findChild<QToolButton *>(name);
            QVERIFY(button);
            auto *menu = button->menu();
            QVERIFY(menu);
            menu->popup(window.mapToGlobal(QPoint(60, 60)));
            QVERIFY(QTest::qWaitForWindowExposed(menu));
            auto entries = menu->actions();
            auto *more = entries.takeLast();
            QCOMPARE(more->text(), QStringLiteral("More ..."));
            QVERIFY(entries.takeLast()->isSeparator());
            QVERIFY(sorted(entries));
            auto *manage = window.findChild<QAction *>(name.startsWith("server")
                                                           ? QStringLiteral("serversAction")
                                                           : QStringLiteral("dataProvidersAction"));
            QVERIFY(manage);
            QVERIFY(manage->text() != more->text());
            QSignalSpy opened(manage, &QAction::triggered);
            if (name.startsWith(QStringLiteral("server"))) {
                QTimer::singleShot(0, &window, [] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    QVERIFY(dialog);
                    dialog->reject();
                });
            }
            more->trigger();
            QCOMPARE(opened.count(), 1);
            if (auto *dialog = window.findChild<swak::DataManager *>()) {
                dialog->close();
            }
            QAction *toggle = nullptr;
            for (auto *action : menu->actions()) {
                if (action->data().toString() == QStringLiteral("local-test")) {
                    toggle = action;
                }
            }
            QVERIFY(toggle);
            QVERIFY(toggle->isCheckable());
            QVERIFY(!toggle->isChecked());
            toggle->trigger();
            const auto values =
                name.startsWith(QStringLiteral("server")) ? servers->sources() : data->sources();
            QVERIFY(values.value(QStringLiteral("local-test"))
                        .toObject()
                        .value(QStringLiteral("enabled"))
                        .toBool());
            toggle->trigger();
            menu->close();
        }
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(selection);
        swak::MapItem item;
        item.label = QStringLiteral("Wheel test");
        item.description = QString(10000, 'x');
        selection->select(item);
        auto *description = window.findChild<QWidget *>(QStringLiteral("informationPane"))
                                ->findChild<QPlainTextEdit *>();
        QVERIFY(description);
        QVERIFY(QTest::qWaitFor([&] {
            return description->isVisible();
        }));
        auto *prefs = dynamic_cast<swak::PreferenceParticipant *>(map);
        const auto zoom = prefs->savePreferences().value(QStringLiteral("zoom"));
        description->verticalScrollBar()->setValue(description->verticalScrollBar()->maximum());
        const auto position = description->viewport()->rect().center();
        for (int delta : {-120, 120}) {
            description->verticalScrollBar()->setValue(
                delta < 0 ? description->verticalScrollBar()->maximum() : 0);
            QWheelEvent wheel(position,
                              description->viewport()->mapToGlobal(position),
                              {},
                              QPoint(0, delta),
                              Qt::NoButton,
                              Qt::NoModifier,
                              Qt::NoScrollPhase,
                              false);
            QApplication::sendEvent(description->viewport(), &wheel);
            QCOMPARE(prefs->savePreferences().value(QStringLiteral("zoom")), zoom);
        }
        QVERIFY(window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))
                    ->grab()
                    .save(QStringLiteral("provider-toolbar.png")));
    }
    void cachedLabelInversion()
    {
        SelectionCanvas canvas;
        canvas.resize(320, 240);
        canvas.view = {QSizeF(320, 240), QPointF(0.5, 0.5), 4};
        swak::MapItem item;
        item.kind = swak::MapItem::Kind::Line;
        const auto scale = canvas.view.scale();
        item.cachedLabelBounds =
            QRectF(canvas.view.center + QPointF(-100, -70) / scale, QSizeF(90 / scale, 30 / scale));
        item.kind = swak::MapItem::Kind::Polygon;
        item.path.addRect(
            item.cachedLabelBounds.adjusted(-10 / scale, -10 / scale, 10 / scale, 10 / scale));
        item.cachedLabelText = QStringLiteral("Selected");
        item.cachedLabelFontHeight = 12 / scale;
        canvas.selection.select(item);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const auto frame = canvas.grabFramebuffer();
        const auto ratio = canvas.devicePixelRatioF();
        QVERIFY(frame.pixelColor(QPoint(qRound(62 * ratio), qRound(52 * ratio))).lightness() < 10);
        int white = 0;
        for (int y = qRound(50 * ratio); y < qRound(80 * ratio); ++y) {
            for (int x = qRound(60 * ratio); x < qRound(150 * ratio); ++x) {
                if (frame.pixelColor(x, y).lightness() > 200) {
                    ++white;
                }
            }
        }
        QVERIFY(white > 10);
        QVERIFY(frame.pixelColor(QPoint(qRound(40 * ratio), qRound(40 * ratio))).lightness() < 10);
    }
    void generatedIdentityPersists()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("identity.json"));
        QString uid;
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            uid = window.findChild<QLineEdit *>(QStringLiteral("identity"))->text();
            QVERIFY(!QUuid(uid).isNull());
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), uid);
    }
    void selectableLabels()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("labels.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(objects && selection);
        for (auto *dock : window.findChildren<QDockWidget *>()) {
            dock->hide();
        }
        auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map);
        preferences->restorePreferences({{QStringLiteral("centerX"), 0.5},
                                         {QStringLiteral("centerY"), 0.5},
                                         {QStringLiteral("zoom"), 8}});
        const swak::Viewport view{map->size(), {0.5, 0.5}, 8};
        const auto world = [&](QPointF point) {
            return view.center +
                   (point - QPointF(map->width() / 2.0, map->height() / 2.0)) / view.scale();
        };
        const QPoint point(180, 180);
        const auto coordinate = world(point);
        swak::UserObject marker;
        marker.id = QStringLiteral("label-marker");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Select this marker label");
        marker.geometry = {{QStringLiteral("x"), coordinate.x()},
                           {QStringLiteral("y"), coordinate.y()}};
        QVERIFY(objects->put(marker).isEmpty());
        const auto actions = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))->actions();
        const auto index =
            actions.indexOf(window.findChild<QAction *>(QStringLiteral("markerModeAction")));
        QVERIFY(index >= 0);
        QCOMPARE(actions.at(index + 1)->objectName(), QStringLiteral("polygonModeAction"));
        QCOMPARE(actions.at(index + 2)->objectName(), QStringLiteral("shapeMode0"));
        QCOMPARE(actions.at(index + 3)->objectName(), QStringLiteral("shapeMode2"));
        QCOMPARE(actions.at(index + 4)->objectName(), QStringLiteral("routeModeAction"));
        QVERIFY(actions.at(index + 5)->isSeparator());
        QCOMPARE(actions.at(index + 6)->objectName(), QStringLiteral("shapeMode1"));
        QCOMPARE(actions.at(index + 7)->objectName(), QStringLiteral("routePlanningAction"));
        const auto bounds =
            swak::mapLabelBounds(map->fontMetrics(), point + QPointF(16, -8), marker.title);
        const auto before = map->grabFramebuffer();
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, bounds.center().toPoint());
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, marker.id);
        const auto after = map->grabFramebuffer();
        const auto sample =
            ((bounds.topLeft() + QPointF(1, 1)) * map->devicePixelRatioF()).toPoint();
        QVERIFY(after.pixelColor(sample).lightness() > before.pixelColor(sample).lightness() + 80);
        QVERIFY(after.save(QStringLiteral("selected-label.png")));
        QTest::mouseClick(
            map, Qt::LeftButton, Qt::NoModifier, bounds.topRight().toPoint() + QPoint(5, -5));
        QVERIFY(!selection->item());
        objects->remove(marker.id);
        swak::Event event;
        event.uid = QStringLiteral("label-cot");
        event.callsign = QStringLiteral("Selectable CoT label");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-g");
        const auto geo = swak::unproject(coordinate);
        event.latitude = geo.y();
        event.longitude = geo.x();
        event.time = QDateTime::currentDateTimeUtc();
        event.start = event.time;
        event.stale = event.time.addSecs(300);
        QVERIFY(window.store().accept(event).isEmpty());
        const auto cotBounds =
            swak::mapLabelBounds(map->fontMetrics(), point + QPointF(16, -8), event.callsign);
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, cotBounds.center().toPoint());
        QCOMPARE(window.store().selected(), event.uid);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, event.uid);
        QImage normal(320, 80, QImage::Format_ARGB32_Premultiplied), inverted = normal;
        normal.fill(Qt::transparent);
        inverted.fill(Qt::transparent);
        {
            QPainter painter(&normal);
            swak::paintMapLabel(painter, {10, 40}, QStringLiteral("Label"));
        }
        {
            QPainter painter(&inverted);
            swak::paintMapLabel(painter, {10, 40}, QStringLiteral("Label"), true);
        }
        for (int y = 0; y < normal.height(); ++y) {
            for (int x = 0; x < normal.width(); ++x) {
                const auto a = normal.pixelColor(x, y), b = inverted.pixelColor(x, y);
                QCOMPARE(a.alpha(), b.alpha());
                if (a.alpha() > 0) {
                    QVERIFY(std::abs(a.red() + b.red() - 255) <= 2);
                }
            }
        }
    }
    void overlappingObjects()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("overlap.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *objects = window.findChild<swak::UserObjects *>();
        QVERIFY(objects);
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(selection);
        const auto state = dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
        const swak::Viewport view{map->size(),
                                  {state.value(QStringLiteral("centerX")).toDouble(),
                                   state.value(QStringLiteral("centerY")).toDouble()},
                                  state.value(QStringLiteral("zoom")).toDouble()};
        const QPoint point(200, 200);
        const auto world = [&](QPointF screen) {
            return view.center +
                   (screen - QPointF(map->width() / 2.0, map->height() / 2.0)) / view.scale();
        };
        const auto coordinate = world(point);
        swak::UserObject marker;
        marker.id = QStringLiteral("marker-a");
        marker.title = QStringLiteral("First marker");
        marker.type = QStringLiteral("circle");
        marker.color = QStringLiteral("#fb8c00");
        marker.geometry = {{QStringLiteral("x"), coordinate.x()},
                           {QStringLiteral("y"), coordinate.y()}};
        QVERIFY(objects->put(marker).isEmpty());
        marker.id = QStringLiteral("marker-b");
        marker.title = QStringLiteral("Second marker");
        QVERIFY(objects->put(marker).isEmpty());
        swak::UserObject polygon;
        polygon.id = QStringLiteral("area-a");
        polygon.title = QStringLiteral("First area");
        polygon.type = QStringLiteral("polygon");
        polygon.color = QStringLiteral("#1e88e5");
        polygon.geometry = swak::polygonGeometry(
            {world({100, 100}), world({300, 100}), world({300, 300}), world({100, 300})});
        QVERIFY(objects->put(polygon).isEmpty());
        polygon.id = QStringLiteral("area-b");
        polygon.title = QStringLiteral("Second area");
        QVERIFY(objects->put(polygon).isEmpty());
        for (const auto &id : {QStringLiteral("marker-a"),
                               QStringLiteral("marker-b"),
                               QStringLiteral("area-a"),
                               QStringLiteral("area-b")}) {
            chooseMapObject(map, objects->find(id)->title);
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
            QVERIFY(selection->item());
            QCOMPARE(selection->item()->id, id);
        }
        QTimer::singleShot(0, map, [map] {
            auto *menu = map->findChild<QMenu *>(QStringLiteral("mapObjectChooser"));
            QVERIFY(menu);
            QCOMPARE(menu->actions().size(), 4);
            QVERIFY(menu->toolTipsVisible());
            for (auto *action : menu->actions()) {
                QVERIFY(!action->isSeparator());
                QVERIFY(!action->icon().isNull());
                QVERIFY(action->text().startsWith(QStringLiteral("First ")) ||
                        action->text().startsWith(QStringLiteral("Second ")));
                QVERIFY(!action->text().contains('('));
            }
            QVERIFY(menu->grab().save(QStringLiteral("overlap-chooser.png")));
            menu->close();
        });
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(selection->item()->id, QStringLiteral("area-b"));
        // A model update cancels the menu before its snapshot becomes stale.
        QTimer::singleShot(0, map, [objects] {
            objects->remove(QStringLiteral("area-b"));
        });
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(!selection->item());
        for (const auto &pair :
             {qMakePair(QStringLiteral("First marker"), QStringLiteral("deleteMarker")),
              qMakePair(QStringLiteral("First area"), QStringLiteral("deletePolygon"))}) {
            QTimer timer;
            timer.setInterval(0);
            int step = 0;
            connect(&timer, &QTimer::timeout, map, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu) {
                    return;
                }
                const auto close = qScopeGuard([menu] {
                    menu->close();
                });
                if (step++ == 0) {
                    QCOMPARE(menu->objectName(), QStringLiteral("mapObjectChooser"));
                    for (auto *action : menu->actions()) {
                        if (action->text().contains(pair.first)) {
                            action->trigger();
                            return;
                        }
                    }
                    QFAIL("Object is absent from the chooser");
                }
                timer.stop();
                auto *action = menu->findChild<QAction *>(pair.second);
                QVERIFY(action);
                answerDeletion();
                action->trigger();
            });
            timer.start();
            QContextMenuEvent context(QContextMenuEvent::Mouse, point, map->mapToGlobal(point));
            QApplication::sendEvent(map, &context);
            QCOMPARE(step, 2);
        }
        QCOMPARE(objects->items().size(), 1);
        // A single hit selects directly, and empty space clears the selection.
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(selection->item()->id, QStringLiteral("marker-b"));
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(400, 400));
        QVERIFY(!selection->item());
        for (int i = 0; i < 31; ++i) {
            marker.id = QStringLiteral("extra-%1").arg(i, 2, 10, QLatin1Char('0'));
            QVERIFY(objects->put(marker).isEmpty());
        }
        QTimer::singleShot(0, map, [&] {
            auto *menu = map->findChild<QMenu *>(QStringLiteral("mapObjectChooser"));
            QVERIFY(menu);
            const auto close = qScopeGuard([menu] {
                menu->close();
            });
            QMenu *more = nullptr;
            for (auto *action : menu->actions()) {
                if (action->menu()) {
                    more = action->menu();
                }
            }
            QVERIFY(more);
            QVERIFY(QMetaObject::invokeMethod(more, "aboutToShow", Qt::DirectConnection));
            for (auto *action : more->actions()) {
                if (action->data().toString() == QStringLiteral("marker-b")) {
                    action->trigger();
                    return;
                }
            }
            QFAIL("Last overlapping marker is absent from the next page");
        });
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, QStringLiteral("marker-b"));
        QTest::mousePress(map, Qt::LeftButton, Qt::NoModifier, point);
        const auto destination = point + QPoint(40, 0);
        QMouseEvent movement(QEvent::MouseMove,
                             destination,
                             map->mapToGlobal(destination),
                             Qt::NoButton,
                             Qt::LeftButton,
                             Qt::NoModifier);
        QApplication::sendEvent(map, &movement);
        QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, destination);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, QStringLiteral("marker-b"));
        QVERIFY(dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences().value(
                    QStringLiteral("centerX")) != state.value(QStringLiteral("centerX")));
    }

    void shapeWorkflow()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("shapes.json"));
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            QVERIFY(map);
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("shapesEditor"));
            QVERIFY(editor);
            auto *objects = window.findChild<swak::UserObjects *>();
            auto *selection = window.findChild<swak::MapSelection *>();
            auto *tools = window.findChild<swak::MapTools *>();
            QVERIFY(objects && selection && tools);
            for (auto *dock : window.findChildren<QDockWidget *>()) {
                dock->hide();
            }
            auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map);
            auto values = mapSettings->savePreferences();
            values.insert(QStringLiteral("centerX"), 0.5);
            values.insert(QStringLiteral("centerY"), 0.5);
            values.insert(QStringLiteral("zoom"), 8);
            mapSettings->restorePreferences(values);
            auto *areaSettings = dynamic_cast<swak::SettingsParticipant *>(
                window.findChild<QWidget *>(QStringLiteral("polygonEditor")));
            QVERIFY(areaSettings);
            auto areaValues = areaSettings->savePreferences();
            areaValues.insert(QStringLiteral("fillOpacity"), 0.65);
            areaSettings->restorePreferences(areaValues);
            QCOMPARE(tools->areaFillOpacity(), 0.65);
            const auto click = [&](QPoint p) {
                QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, p);
            };
            const auto motion = [&](QPoint p) {
                QMouseEvent event(QEvent::MouseMove,
                                  QPointF(p),
                                  QPointF(map->mapToGlobal(p)),
                                  Qt::NoButton,
                                  Qt::NoButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(map, &event);
            };
            const auto contextAction = [&](const QString &name, QPoint p) {
                bool invoked = false;
                QTimer::singleShot(0, &window, [&] {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                    if (!menu) {
                        return;
                    }
                    if (auto *action = menu->findChild<QAction *>(name)) {
                        action->trigger();
                        invoked = true;
                    }
                    menu->close();
                });
                QContextMenuEvent event(QContextMenuEvent::Mouse, p, map->mapToGlobal(p));
                QApplication::sendEvent(map, &event);
                return invoked;
            };
            for (int i = 0; i < 3; ++i) {
                auto *action = window.findChild<QAction *>(QStringLiteral("shapeMode%1").arg(i));
                QVERIFY(action);
                action->trigger();
                QVERIFY(action->isChecked());
                QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
                action->trigger();
                QVERIFY(!action->isChecked());
                QVERIFY(map->cursor().shape() != Qt::CrossCursor);
            }
            for (const auto key : {Qt::Key_C, Qt::Key_L}) {
                editor = window.findChild<swak::ObjectEditor *>(
                    key == Qt::Key_C ? QStringLiteral("shapesEditor")
                                     : QStringLiteral("linesEditor"));
                QVERIFY(editor);
                QTest::keyClick(map, key, Qt::ShiftModifier);
                QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
                click({180, 180});
                motion({260, 180});
                if (key == Qt::Key_C) {
                    const auto locale = QLocale();
                    const auto preview = [&](const QLocale &units) {
                        QLocale::setDefault(units);
                        QImage frame(map->size(), QImage::Format_ARGB32_Premultiplied);
                        frame.fill(Qt::transparent);
                        QPainter painter(&frame);
                        tools->paintOverlay(painter, {map->size(), {.5, .5}, 8}, {260, 180});
                        return frame;
                    };
                    const auto metric = preview(QLocale(QLocale::Swedish, QLocale::Sweden));
                    const auto imperial = preview(QLocale(QLocale::English, QLocale::UnitedStates));
                    QLocale::setDefault(locale);
                    QVERIFY(metric != imperial);
                    int white = 0;
                    for (int y = 140; y < 178; ++y) {
                        for (int x = 220; x < 400; ++x) {
                            const auto color = metric.pixelColor(x, y);
                            white += color.alpha() > 0 && color.red() > 240 &&
                                     color.green() > 240 && color.blue() > 240;
                        }
                    }
                    QVERIFY(white > 0);
                    QVERIFY(metric.save(QStringLiteral("circle-radius-preview.png")));
                }
                click({260, 180});
                QVERIFY(editor->draft());
                const bool circle = key == Qt::Key_C;
                QCOMPARE(editor->draft()->type,
                         circle ? QStringLiteral("range-circle") : QStringLiteral("line"));
                editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"))
                    ->setText(circle ? QStringLiteral("Circle test") : QStringLiteral("Line test"));
                editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
                QVERIFY(selection->item());
                const auto id = selection->item()->id;
                const auto original = objects->find(id)->geometry;
                QVERIFY(selection->item()->properties.contains(
                    circle ? QStringLiteral("Area") : QStringLiteral("Distance")));
                QVERIFY(contextAction(QStringLiteral("moveShape"), {220, 180}));
                motion({240, 210});
                QTest::keyClick(map, Qt::Key_Escape);
                QCOMPARE(objects->find(id)->geometry, original);
                QVERIFY(contextAction(QStringLiteral("moveShape"), {220, 180}));
                motion({240, 210});
                click({240, 210});
                QVERIFY(objects->find(id)->geometry != original);
                QVERIFY(contextAction(QStringLiteral("editShape"), {240, 210}));
                QVERIFY(editor->draft());
                const auto editing = editor->draft()->geometry;
                QVERIFY(contextAction(QStringLiteral("moveShapePoint"), {200, 210}));
                motion({210, 220});
                QTest::keyClick(map, Qt::Key_Escape);
                QCOMPARE(editor->draft()->geometry, editing);
                QVERIFY(contextAction(QStringLiteral("moveShapePoint"), {200, 210}));
                motion({210, 220});
                click({210, 220});
                QVERIFY(editor->draft()->geometry != editing);
                auto points = swak::polygonVertices(editor->draft()->geometry);
                points[1].setY(points[1].y() + 0.0001);
                QVERIFY(editor->setGeometry(swak::polygonGeometry(points)));
                editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
                QVERIFY(QTest::qWaitFor([&] {
                    return map->isValid();
                }));
                const auto frame = map->grabFramebuffer();
                QVERIFY(!frame.isNull());
                QVERIFY(frame.save(circle ? QStringLiteral("circle-tool.png")
                                          : QStringLiteral("line-tool.png")));
                if (circle) {
                    const auto color =
                        frame.pixelColor(QPoint(200, 250) * map->devicePixelRatioF());
                    QVERIFY(color.blue() > color.red());
                }
                answerDeletion();
                QTest::keyClick(map, Qt::Key_Delete);
                QVERIFY(!objects->find(id));
                objects->undoStack().undo();
                QVERIFY(objects->find(id));
                objects->remove(id);
            }
            objects->undoStack().undo();
            QCOMPARE(objects->items().size(), 1);
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(editor);
            auto temporary = settings->savePreferences();
            temporary.insert(QStringLiteral("holdSeconds"), 1);
            temporary.insert(QStringLiteral("fadeMs"), 100);
            settings->restorePreferences(temporary);
            auto *tool = dynamic_cast<swak::MapTool *>(
                window.findChild<QObject *>(QStringLiteral("linesTool")));
            QVERIFY(tool);
            const swak::Viewport view{QSizeF(map->size()), QPointF(0.5, 0.5), 8};
            const auto frame = [&] {
                QImage image(map->size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent);
                QPainter painter(&image);
                tool->paint(painter, view);
                return image;
            };
            selection->clear();
            const auto before = frame();
            const auto count = objects->items().size();
            QTest::keyClick(map, Qt::Key_D, Qt::ShiftModifier);
            click({400, 200});
            motion({500, 250});
            click({500, 250});
            QCOMPARE(objects->items().size(), count);
            QVERIFY(frame() != before);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return frame() == before;
                },
                2500));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *objects = restored.findChild<swak::UserObjects *>();
        QVERIFY(objects);
        QCOMPARE(objects->items().size(), 1);
        QCOMPARE(objects->items().first().type, QStringLiteral("line"));
    }
    void restrictedArea_data()
    {
        QTest::addColumn<bool>("circle");
        QTest::newRow("area") << false;
        QTest::newRow("circle") << true;
    }
    void restrictedArea()
    {
        QFETCH(bool, circle);
        const auto editorName =
            circle ? QStringLiteral("shapesEditor") : QStringLiteral("polygonEditor");
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("restricted-area.json"));
        QString id = QStringLiteral("restricted-area");
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *objects = window.findChild<swak::UserObjects *>();
            auto *editor = window.findChild<swak::ObjectEditor *>(editorName);
            auto *selection = window.findChild<swak::MapSelection *>();
            auto *tools = window.findChild<swak::MapTools *>();
            auto *map = window.findChild<QOpenGLWidget *>();
            swak::UserObject area;
            area.id = id;
            area.type = circle ? QStringLiteral("range-circle") : QStringLiteral("polygon");
            area.title = QStringLiteral("Restricted area");
            area.color = QStringLiteral("#1e88e5");
            area.geometry =
                swak::polygonGeometry(circle ? QPolygonF{{.5, .5}, {.6, .5}}
                                             : QPolygonF{{.4, .4}, {.6, .4}, {.6, .6}, {.4, .6}});
            QVERIFY(objects->put(area).isEmpty());
            const auto image = [&] {
                QImage result(400, 400, QImage::Format_ARGB32_Premultiplied);
                result.fill(Qt::transparent);
                QPainter painter(&result);
                tools->paint(painter, swak::Viewport{QSizeF(400, 400), QPointF(.5, .5), 2});
                return result;
            };
            const auto plain = image();
            editor->edit(area);
            auto *restricted = editor->findChild<QCheckBox *>(QStringLiteral("objectRestricted"));
            QVERIFY(restricted && restricted->isVisible());
            QVERIFY(!restricted->isChecked());
            restricted->click();
            QVERIFY(editor->draft()->restricted);
            QVERIFY(!objects->find(id)->restricted);
            const auto preview = image();
            int stripe = 0, fill = 0;
            for (int y = 120; y < 160; ++y) {
                for (int x = 120; x < 160; ++x) {
                    const auto alpha = preview.pixelColor(x, y).alpha();
                    stripe += alpha > 200;
                    fill += alpha == 77;
                }
            }
            QVERIFY(stripe > 100 && fill > 100);
            QCOMPARE(preview.pixelColor(30, 30).alpha(), 0);
            editor->cancel();
            QCOMPARE(image(), plain);
            editor->edit(area);
            restricted->click();
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
            QVERIFY(objects->find(id)->restricted);
            QVERIFY(selection->item());
            QCOMPARE(selection->item()->properties.value(QStringLiteral("Restricted")).toString(),
                     QStringLiteral("Yes"));
            selection->clear();
            const auto saved = image();
            QVERIFY(saved != plain);
            QVERIFY(saved.save(QStringLiteral("restricted-area.png")));
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
            auto values = settings->savePreferences();
            values.insert(QStringLiteral("centerX"), .5);
            values.insert(QStringLiteral("centerY"), .5);
            values.insert(QStringLiteral("zoom"), 3);
            settings->restorePreferences(values);
            QVERIFY(map->grabFramebuffer().save(QStringLiteral("restricted-area-map.png")));
            objects->remove(id);
            objects->undoStack().undo();
            QVERIFY(objects->find(id)->restricted);
            editor->edit(*objects->find(id));
            QVERIFY(restricted->isChecked());
            auto vertices = swak::polygonVertices(editor->draft()->geometry);
            vertices[0].rx() += .01;
            QVERIFY(editor->setGeometry(swak::polygonGeometry(vertices)));
            QVERIFY(editor->draft()->restricted);
            restricted->click();
            editor->cancel();
            QVERIFY(objects->find(id)->restricted);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        auto *objects = restored.findChild<swak::UserObjects *>();
        QVERIFY(objects->find(id)->restricted);
        auto *editor = restored.findChild<swak::ObjectEditor *>(editorName);
        editor->edit(*objects->find(id));
        auto *restricted = editor->findChild<QCheckBox *>(QStringLiteral("objectRestricted"));
        QVERIFY(restricted->isChecked());
        restricted->click();
        editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
        QVERIFY(!objects->find(id)->restricted);
        auto old = objects->find(id)->json();
        old.remove(QStringLiteral("restricted"));
        QVERIFY(!swak::UserObject::fromJson(old).restricted);
        old.insert(QStringLiteral("restricted"), QStringLiteral("true"));
        QVERIFY(!swak::UserObject::fromJson(old).restricted);
        auto *markerEditor =
            restored.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
        auto marker = *objects->find(id);
        marker.type = QStringLiteral("circle");
        marker.geometry = {{QStringLiteral("x"), .5}, {QStringLiteral("y"), .5}};
        markerEditor->edit(marker);
        QVERIFY(
            markerEditor->findChild<QCheckBox *>(QStringLiteral("objectRestricted"))->isHidden());
    }
    void routeMarkerWorkflow()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("routes.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("routeEditor"));
        auto *tools = window.findChild<swak::MapTools *>();
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *selection = window.findChild<swak::MapSelection *>();
        auto *action = window.findChild<QAction *>(QStringLiteral("routeModeAction"));
        QVERIFY(map && editor && tools && objects && selection && action);
        QVERIFY(!action->icon().isNull());
        for (auto *pane : window.findChildren<QDockWidget *>()) {
            pane->hide();
        }
        const auto click = [&](QPoint point) {
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
        };
        action->trigger();
        QVERIFY(action->isChecked());
        QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
        QTest::keyClick(map, Qt::Key_Escape);
        QVERIFY(!action->isChecked());
        QTest::keyClick(map, Qt::Key_R, Qt::ShiftModifier);
        QVERIFY(action->isChecked());
        const QPoint a(100, 200), b(250, 120), c(400, 220);
        click(a);
        // One point is not a route. Enter waits for a second point.
        QTest::keyClick(map, Qt::Key_Return);
        QVERIFY(!editor->draft());
        click(b);
        click(c);
        // While drawing, a route with three points shows its total length beside the last point.
        const auto savedView = dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
        const swak::Viewport view{map->size(),
                                  {savedView.value(QStringLiteral("centerX")).toDouble(),
                                   savedView.value(QStringLiteral("centerY")).toDouble()},
                                  savedView.value(QStringLiteral("zoom")).toDouble()};
        const auto labelPixels = [&] {
            QImage overlay(map->size(), QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            {
                QPainter painter(&overlay);
                tools->paintOverlay(painter, view, c);
            }
            int white = 0;
            const QRect region(c.x() + 8, c.y() + 4, 120, 16);
            for (int y = region.top(); y <= region.bottom(); ++y) {
                for (int x = region.left(); x <= region.right(); ++x) {
                    const auto color = overlay.pixelColor(x, y);
                    white += color.alpha() > 0 && color.red() > 240 && color.green() > 240 &&
                             color.blue() > 240;
                }
            }
            return white;
        };
        QVERIFY(labelPixels() > 0);
        QTest::keyClick(map, Qt::Key_Return);
        QVERIFY(editor->draft());
        QVERIFY(editor->isVisible());
        QCOMPARE(swak::routePoints(editor->draft()->geometry).size(), 3);
        QCOMPARE(editor->findChild<QComboBox *>(QStringLiteral("objectType"))->currentText(),
                 QStringLiteral("Route"));
        auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"));
        QCOMPARE(title->placeholderText(), QStringLiteral("New Route"));
        title->setText(QStringLiteral("Patrol route"));
        QTest::keyClick(title, Qt::Key_Return);
        QCOMPARE(objects->items().size(), 1);
        const auto id = objects->items().firstKey();
        QCOMPARE(objects->find(id)->type, QStringLiteral("route"));
        QVERIFY(QTest::qWaitFor([&] {
            return map->isValid();
        }));
        click((a + b) / 2);
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, id);
        QCOMPARE(selection->item()->type, QStringLiteral("Route"));
        const auto length =
            selection->item()->properties.value(QStringLiteral("Length")).toString();
        QVERIFY(!length.isEmpty());
        QCOMPARE(selection->item()->properties.value(QStringLiteral("Points")).toString(),
                 QStringLiteral("3"));
        QStringList names;
        QTimer::singleShot(0, map, [&names] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            for (auto *entry : menu->actions()) {
                names.append(entry->objectName());
            }
            menu->close();
        });
        const QPoint middle = (b + c) / 2;
        QContextMenuEvent context(QContextMenuEvent::Mouse, middle, map->mapToGlobal(middle));
        QApplication::sendEvent(map, &context);
        for (const auto &name : {QStringLiteral("editRoute"),
                                 QStringLiteral("moveRoute"),
                                 QStringLiteral("deleteRoute")}) {
            QVERIFY2(names.contains(name), qPrintable(names.join(',')));
        }
        // Move the route by 40 pixels to the right.
        const auto before = swak::routePoints(objects->find(id)->geometry);
        QTimer::singleShot(0, map, [] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(menu);
            for (auto *entry : menu->actions()) {
                if (entry->objectName() == QStringLiteral("moveRoute")) {
                    entry->trigger();
                }
            }
            menu->close();
        });
        QApplication::sendEvent(map, &context);
        QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
        click(middle + QPoint(40, 0));
        const auto after = swak::routePoints(objects->find(id)->geometry);
        QCOMPARE(after.size(), before.size());
        QVERIFY(after.first().x() > before.first().x());
        QVERIFY(std::abs(after.first().y() - before.first().y()) < 1e-9);
        QVERIFY(map->grabFramebuffer().save(QStringLiteral("route-marker.png")));
        auto *planning = window.findChild<QAction *>(QStringLiteral("routePlanningAction"));
        auto *routeDock =
            window.findChild<QDockWidget *>(QStringLiteral("org.swak.route-planner.pane"));
        QVERIFY(planning && routeDock);
        QVERIFY(!planning->isChecked());
        planning->trigger();
        QVERIFY(QTest::qWaitFor([&] {
            return routeDock->isVisible() && planning->isChecked();
        }));
        planning->trigger();
        QVERIFY(QTest::qWaitFor([&] {
            return !routeDock->isVisible() && !planning->isChecked();
        }));
    }
    void polygonWorkflow()
    {
        QTemporaryDir directory;
        const auto profile = directory.filePath(QStringLiteral("polygons.json"));
        QString id;
        {
            swak::Window window(profile, QPluginLoader::staticInstances());
            window.show();
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            QVERIFY(map);
            auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("polygonEditor"));
            QVERIFY(editor);
            auto *tools = window.findChild<swak::MapTools *>();
            QVERIFY(tools);
            auto *objects = window.findChild<swak::UserObjects *>();
            QVERIFY(objects);
            auto *selection = window.findChild<swak::MapSelection *>();
            QVERIFY(selection);
            auto *action = window.findChild<QAction *>(QStringLiteral("polygonModeAction"));
            QVERIFY(action);
            const QPoint a(100, 140), b(300, 140), c(300, 300), d(100, 300), middle(200, 220);
            const auto click = [&](QPoint point) {
                QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, point);
            };
            const auto motion = [&](QPoint point) {
                QMouseEvent event(QEvent::MouseMove,
                                  QPointF(point),
                                  QPointF(map->mapToGlobal(point)),
                                  Qt::NoButton,
                                  Qt::NoButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(map, &event);
            };
            QTest::keyClick(map, Qt::Key_A, Qt::ShiftModifier);
            QCOMPARE(tools->statusText(), QStringLiteral("A"));
            QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
            QCOMPARE(tools->statusText(), QStringLiteral("M"));
            QTest::keyClick(map, Qt::Key_A, Qt::ShiftModifier);
            QCOMPARE(tools->statusText(), QStringLiteral("A"));
            click(a);
            QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
            click(b);
            motion(c);
            QImage overlay(map->size(), QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            const auto savedView =
                dynamic_cast<swak::PreferenceParticipant *>(map)->savePreferences();
            swak::Viewport view{map->size(),
                                {savedView.value(QStringLiteral("centerX")).toDouble(),
                                 savedView.value(QStringLiteral("centerY")).toDouble()},
                                savedView.value(QStringLiteral("zoom")).toDouble()};
            {
                QPainter painter(&overlay);
                tools->paintOverlay(painter, view, c);
            }
            const auto shade = overlay.pixelColor(250, 180);
            QCOMPARE(shade.alpha(), 77);
            QVERIFY(shade.blue() > shade.red());
            QCOMPARE(overlay.pixelColor(20, c.y()).alpha(), 128);
            for (const QRect region : {QRect(200, 110, 90, 25), QRect(300, 190, 90, 25)}) {
                int white = 0;
                for (int y = region.top(); y <= region.bottom(); ++y) {
                    for (int x = region.left(); x <= region.right(); ++x) {
                        const auto color = overlay.pixelColor(x, y);
                        white += color.alpha() > 0 && color.red() > 240 && color.green() > 240 &&
                                 color.blue() > 240;
                    }
                }
                QVERIFY(white > 0);
            }
            QVERIFY(overlay.save(QStringLiteral("polygon-preview.png")));
            click(c);
            click(d);
            click(a);
            QVERIFY(editor->draft());
            QVERIFY(editor->isVisible());
            QVERIFY(tools->statusText().isEmpty());
            QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), 4);
            QVERIFY(
                !editor->findChild<QComboBox *>(QStringLiteral("objectCollection"))->isVisible());
            QCOMPARE(editor->findChild<QComboBox *>(QStringLiteral("objectType"))->currentText(),
                     QStringLiteral("Area"));
            auto *title = editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"));
            title->setText(QStringLiteral("Test area"));
            editor->findChild<QPlainTextEdit *>(QStringLiteral("objectDescription"))
                ->setPlainText(QStringLiteral("Area description"));
            editor->findChild<QToolButton *>(QStringLiteral("objectColor3"))->click();
            editor->findChild<QPushButton *>(QStringLiteral("addObjectProperty"))->click();
            auto *properties =
                editor->findChild<QTableWidget *>(QStringLiteral("objectProperties"));
            auto *propertyName = properties->findChild<QLineEdit *>();
            QVERIFY(propertyName);
            propertyName->setText(QStringLiteral("Owner"));
            QTest::keyClick(propertyName, Qt::Key_Return);
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return properties->item(0, 0)->text() == QStringLiteral("Owner");
                },
                1000));
            properties->item(0, 1)->setText(QStringLiteral("Local"));
            QVERIFY(editor->grab().save(QStringLiteral("polygon-editor.png")));
            QTest::keyClick(title, Qt::Key_Return);
            QCOMPARE(objects->items().size(), 1);
            QVERIFY(!editor->draft());
            id = objects->items().firstKey();
            QCOMPARE(objects->find(id)->color, QStringLiteral("#43a047"));
            QCOMPARE(objects->find(id)->properties.value(QStringLiteral("Owner")).toString(),
                     QStringLiteral("Local"));
            click(middle);
            QVERIFY(selection->item());
            QCOMPARE(selection->item()->id, id);
            QCOMPARE(selection->item()->outlineStyle, Qt::SolidLine);
            QVERIFY(
                !selection->item()->properties.value(QStringLiteral("Area")).toString().isEmpty());
            QVERIFY(QTest::qWaitFor([&] {
                return map->isValid();
            }));
            const auto frame = map->grabFramebuffer();
            const double ratio = map->devicePixelRatioF();
            QVERIFY(frame.pixelColor(qRound(150 * ratio), qRound(180 * ratio)) !=
                    frame.pixelColor(qRound(450 * ratio), qRound(180 * ratio)));
            QVERIFY(frame.save(QStringLiteral("polygon-frame.png")));
            QVERIFY(map->grab().save(QStringLiteral("polygon-selected.png")));
            const auto context = [&](QPoint point, const QString &name, bool enabled = true) {
                bool seen = false;
                QTimer::singleShot(0, map, [&] {
                    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                    QVERIFY(menu);
                    auto *entry = menu->findChild<QAction *>(name);
                    QVERIFY(entry);
                    seen = true;
                    QCOMPARE(entry->isEnabled(), enabled);
                    if (menu->objectName() == QStringLiteral("polygonVertexMenu")) {
                        QCOMPARE(menu->actions().first()->objectName(),
                                 QStringLiteral("movePolygonVertex"));
                    }
                    if (enabled) {
                        if (name.startsWith(QStringLiteral("delete"))) {
                            answerDeletion();
                        }
                        entry->trigger();
                    }
                    menu->close();
                });
                QContextMenuEvent event(QContextMenuEvent::Mouse, point, map->mapToGlobal(point));
                QApplication::sendEvent(map, &event);
                QVERIFY(seen);
            };
            context(middle, QStringLiteral("editPolygon"));
            QVERIFY(editor->draft());
            const auto original = editor->draft()->geometry;
            overlay.fill(Qt::transparent);
            {
                QPainter painter(&overlay);
                tools->paintOverlay(painter, view, middle);
            }
            QCOMPARE(overlay.pixelColor(a), QColor("#43a047"));
            editor->findChild<QToolButton *>(QStringLiteral("objectColor0"))->click();
            overlay.fill(Qt::transparent);
            {
                QPainter painter(&overlay);
                tools->paintOverlay(painter, view, middle);
            }
            QCOMPARE(overlay.pixelColor(a), QColor(editor->draft()->color));
            QVERIFY(overlay.save(QStringLiteral("polygon-corner-color.png")));
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, a);
            QTest::mouseDClick(map, Qt::LeftButton, Qt::NoModifier, a);
            QCOMPARE(tools->statusText(), QStringLiteral("A"));
            QMouseEvent release(QEvent::MouseButtonRelease,
                                a,
                                map->mapToGlobal(a),
                                Qt::LeftButton,
                                Qt::NoButton,
                                Qt::NoModifier);
            QApplication::sendEvent(map, &release);
            QCOMPARE(editor->draft()->geometry, original);
            motion(a + QPoint(15, 15));
            click(a + QPoint(15, 15));
            QVERIFY(editor->draft()->geometry != original);
            QVERIFY(editor->setGeometry(original));
            // Escape ends a corner move. The draft stays, and a later click moves nothing.
            context(a, QStringLiteral("movePolygonVertex"));
            motion(a + QPoint(30, 0));
            QTest::keyClick(map, Qt::Key_Escape);
            QVERIFY(editor->draft());
            QCOMPARE(editor->draft()->geometry, original);
            context(a, QStringLiteral("movePolygonVertex"));
            QCOMPARE(tools->statusText(), QStringLiteral("A"));
            motion(b);
            click(b);
            QCOMPARE(editor->draft()->geometry, original);
            QCOMPARE(tools->statusText(), QStringLiteral("A"));
            QTest::keyClick(map, Qt::Key_Escape);
            QVERIFY(editor->draft());
            QVERIFY(tools->statusText().isEmpty());
            context(a, QStringLiteral("movePolygonVertex"));
            click(QPoint(80, 120));
            QVERIFY(editor->draft()->geometry != original);
            QCOMPARE(objects->find(id)->geometry, original);
            QTest::keyClick(editor, Qt::Key_Escape);
            QVERIFY(!editor->draft());
            QCOMPARE(objects->find(id)->geometry, original);
            context(middle, QStringLiteral("editPolygon"));
            context(a, QStringLiteral("deletePolygonVertex"));
            QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), 3);
            context(b, QStringLiteral("deletePolygonVertex"), false);
            editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
            QCOMPARE(swak::polygonVertices(objects->find(id)->geometry).size(), 3);
            answerDeletion();
            QTest::keyClick(map, Qt::Key_Delete);
            QVERIFY(objects->items().isEmpty());
            objects->undoStack().undo();
            QCOMPARE(objects->items().size(), 1);
            QVERIFY(selection->item());
            context(QPoint(250, 250), QStringLiteral("deletePolygon"));
            QVERIFY(objects->items().isEmpty());
            objects->undoStack().undo();
            QCOMPARE(objects->items().size(), 1);
            action->trigger();
            click(a);
            click(b);
            motion(c);
            QTest::keyClick(map, Qt::Key_Return);
            QVERIFY(editor->draft());
            QCOMPARE(swak::polygonVertices(editor->draft()->geometry).size(), 3);
            QTest::keyClick(editor, Qt::Key_Escape);
            QCOMPARE(objects->items().size(), 1);
            action->trigger();
            click(a);
            click(b);
            motion(QPoint(200, 140));
            QTest::keyClick(map, Qt::Key_Return);
            QVERIFY(!editor->draft());
            QCOMPARE(tools->statusText(), QStringLiteral("A"));
            QTest::keyClick(map, Qt::Key_A, Qt::ShiftModifier);
            QVERIFY(tools->statusText().isEmpty());
            action->trigger();
            click(a);
            QTest::keyClick(map, Qt::Key_Return);
            QVERIFY(!editor->draft());
            QTest::keyClick(map, Qt::Key_Escape);
            QVERIFY(tools->statusText().isEmpty());
            click(QPoint(500, 100));
            QVERIFY(!selection->item());
            window.close();
        }
        swak::Window restored(profile, QPluginLoader::staticInstances());
        const auto *objects = restored.findChild<swak::UserObjects *>();
        QVERIFY(objects->find(id));
        QCOMPARE(objects->find(id)->title, QStringLiteral("Test area"));
        QCOMPARE(swak::polygonVertices(objects->find(id)->geometry).size(), 3);
    }

    void markerMenuActivation()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("marker-menu.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *menu = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QVERIFY(menu);
        auto *action = window.findChild<QAction *>(QStringLiteral("markerModeAction"));
        QVERIFY(action);
        QVERIFY(menu->actions().contains(action));
        QCOMPARE(action->text(), QStringLiteral("Marker"));
        for (auto *entry : menu->actions()) {
            QVERIFY(entry->text() != QStringLiteral("Edit marker"));
        }
        auto *tools = window.findChild<swak::MapTools *>();
        QVERIFY(tools);
        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor"));
        QVERIFY(editor);
        auto *objects = window.findChild<swak::UserObjects *>();
        QVERIFY(objects);
        QVERIFY(!editor->isVisible());
        menu->popup(window.mapToGlobal(QPoint(400, 300)));
        QVERIFY(QTest::qWaitForWindowExposed(menu));
        QSignalSpy triggered(action, &QAction::triggered);
        QTest::mouseClick(
            menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
        QCOMPARE(triggered.count(), 1);
        QVERIFY(!menu->isVisible());
        QCOMPARE(tools->statusText(), QStringLiteral("M"));
        QCOMPARE(map->cursor().shape(), Qt::CrossCursor);
        QVERIFY(!editor->isVisible());
        QTest::keyClick(map, Qt::Key_Escape);
        QVERIFY(tools->statusText().isEmpty());
        action->trigger();
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
        QVERIFY(editor->isVisible());
        QVERIFY(tools->statusText().isEmpty());
        editor->findChild<QLineEdit *>(QStringLiteral("objectTitle"))
            ->setText(QStringLiteral("Menu marker"));
        editor->findChild<QPushButton *>(QStringLiteral("applyObject"))->click();
        QCOMPARE(objects->items().size(), 1);
        action->trigger();
        QCOMPARE(tools->statusText(), QStringLiteral("M"));
        QTest::keyClick(map, Qt::Key_M, Qt::ShiftModifier);
        QVERIFY(tools->statusText().isEmpty());
    }
    void providerEndpointToolTips()
    {
        // Restored profiles keep entries that fail validation. Their endpoints stay plain text.
        const auto markup = QStringLiteral("<img src='file:///private' />");
        const QJsonObject values{
            {QStringLiteral("sources"),
             QJsonObject{{QStringLiteral("raw"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("Raw")},
                                      {QStringLiteral("format"), QStringLiteral("absent")},
                                      {QStringLiteral("endpoint"), markup}}}}}};
        const auto check = [&](QWidget &manager, const QString &table) {
            auto *list = manager.findChild<QTableWidget *>(table);
            QVERIFY(list);
            QCOMPARE(list->rowCount(), 1);
            const auto tip = list->item(0, 1)->toolTip();
            QVERIFY(Qt::mightBeRichText(tip));
            QVERIFY(tip.contains(markup.toHtmlEscaped()));
            QVERIFY(!tip.contains(markup));
        };
        swak::MapSources maps({});
        maps.restorePreferences(values);
        QCOMPARE(maps.sources().value(QStringLiteral("raw")).toObject().value("endpoint"), markup);
        swak::MapManager mapManager(maps);
        check(mapManager, QStringLiteral("mapProviderList"));
        if (QTest::currentTestFailed()) {
            return;
        }
        swak::DataSources feeds({});
        feeds.restorePreferences(values);
        QCOMPARE(feeds.sources().value(QStringLiteral("raw")).toObject().value("endpoint"), markup);
        swak::DataManager dataManager(feeds);
        check(dataManager, QStringLiteral("dataProviderList"));
    }
    void mapCredentialDialog()
    {
        swak::MapSources maps(QPluginLoader::staticInstances());
        const auto id = QStringLiteral("org.swak.gdal.raster");
        auto source = maps.sources().value(id).toObject();
        source.insert(QStringLiteral("authMode"), QStringLiteral("bearer"));
        QVERIFY(maps.put(id, source).isEmpty());
        swak::MapManager manager(maps, nullptr, [] {
            return QString{};
        });
        manager.show();
        QVERIFY(QTest::qWaitForWindowExposed(&manager));
        auto *table = manager.findChild<QTableWidget *>(QStringLiteral("mapProviderList"));
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 1)->data(Qt::UserRole).toString() == id) {
                table->setCurrentCell(row, 1);
            }
        }
        const auto enter = [&](const QString &text, bool accept) {
            bool seen = false;
            QTimer::singleShot(0, &manager, [&] {
                auto *dialog = manager.findChild<QDialog *>(QStringLiteral("mapCredentialsDialog"));
                QVERIFY(dialog);
                seen = true;
                const auto close = qScopeGuard([dialog] {
                    if (dialog->isVisible()) {
                        dialog->reject();
                    }
                });
                auto *secret = dialog->findChild<QLineEdit *>(QStringLiteral("mapSecret"));
                QVERIFY(secret);
                QVERIFY(secret->text().isEmpty());
                QCOMPARE(secret->echoMode(), QLineEdit::Password);
                secret->setText(text);
                QVERIFY(dialog->grab().save(QStringLiteral("map-credentials.png")));
                if (accept) {
                    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                } else {
                    dialog->reject();
                }
            });
            manager.findChild<QPushButton *>(QStringLiteral("mapCredentials"))->click();
            QVERIFY(seen);
        };
        enter(QStringLiteral("test-token"), true);
        QCOMPARE(maps.credential(id), QByteArray("test-token"));
        QVERIFY(!QJsonDocument(maps.savePreferences()).toJson().contains("test-token"));
        enter(QStringLiteral("cancelled-token"), false);
        QCOMPARE(maps.credential(id), QByteArray("test-token"));
        enter({}, true);
        QVERIFY(maps.credential(id).isEmpty());
        QTimer::singleShot(0, &manager, [&] {
            auto *dialog = manager.findChild<swak::SettingsDialog *>();
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] {
                dialog->reject();
            });
            auto *mode = dialog->findChild<QComboBox *>(id + QStringLiteral("/authMode"));
            QVERIFY(mode);
            QCOMPARE(mode->currentData().toString(), QStringLiteral("bearer"));
            QVERIFY(dialog->grab().save(QStringLiteral("map-auth-settings.png")));
        });
        manager.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
        source.remove(QStringLiteral("authMode"));
        QVERIFY(maps.put(id, source).isEmpty());
        QTimer::singleShot(0, &manager, [&] {
            auto *dialog = manager.findChild<swak::SettingsDialog *>();
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] {
                dialog->reject();
            });
            QCOMPARE(dialog->findChild<QComboBox *>(id + QStringLiteral("/authMode"))
                         ->currentData()
                         .toString(),
                     QStringLiteral("none"));
        });
        manager.findChild<QPushButton *>(QStringLiteral("editMapProvider"))->click();
        manager.findChild<QPushButton *>(QStringLiteral("mapCredentials"))->click();
        QVERIFY(maps.credential(id).isEmpty());
    }
    void providerActivationTables()
    {
        swak::MapSources maps(QPluginLoader::staticInstances());
        swak::MapManager manager(maps);
        auto *table = manager.findChild<QTableWidget *>(QStringLiteral("mapProviderList"));
        QVERIFY(table);
        const auto first = table->item(0, 1)->data(Qt::UserRole).toString();
        const auto second = table->item(1, 1)->data(Qt::UserRole).toString();
        manager.findChild<QRadioButton *>(QStringLiteral("activeProvider/") + first)->click();
        QVERIFY(QTest::qWaitFor([&] {
            return maps.activeId() == first;
        }));
        table->setCurrentCell(1, 2);
        QCOMPARE(maps.activeId(), first);
        manager.findChild<QRadioButton *>(QStringLiteral("activeProvider/") + second)->click();
        QVERIFY(QTest::qWaitFor([&] {
            return maps.activeId() == second;
        }));
        QVERIFY(!manager.findChild<QRadioButton *>(QStringLiteral("activeProvider/") + first)
                     ->isChecked());
        manager.findChild<QPushButton *>(QStringLiteral("clearMapProvider"))->click();
        QVERIFY(maps.activeSource().isEmpty());
        for (auto *radio : manager.findChildren<QRadioButton *>()) {
            QVERIFY(!radio->isChecked());
        }
        TileServer server;
        QVERIFY(server.load(QFINDTESTDATA("fixtures")));
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.body =
            "<kml><Placemark><Point><coordinates>0,0</coordinates></Point></Placemark></kml>";
        swak::DataSources feeds(QPluginLoader::staticInstances());
        feeds.restorePreferences({{QStringLiteral("sources"), QJsonObject{}}});
        for (const auto &id : {QStringLiteral("a"), QStringLiteral("b")}) {
            auto source = swak::dataDefaults();
            source.insert(QStringLiteral("format"), feeds.providers().first()->dataId());
            source.insert(QStringLiteral("name"), id);
            source.insert(QStringLiteral("attribution"), QStringLiteral("Test data"));
            source.insert(
                QStringLiteral("endpoint"),
                QStringLiteral("https://localhost:%1/%2.kml").arg(server.serverPort()).arg(id));
            source.insert(QStringLiteral("ignoreTls"), true);
            QVERIFY(feeds.put(id, source).isEmpty());
        }
        swak::DataManager dataManager(feeds, nullptr, [] {
            return QString{};
        });
        auto *dataTable = dataManager.findChild<QTableWidget *>(QStringLiteral("dataProviderList"));
        QVERIFY(dataTable);
        for (const auto &id : {QStringLiteral("a"), QStringLiteral("b")}) {
            dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/") + id)->click();
            QVERIFY(QTest::qWaitFor([&] {
                return feeds.sources()
                    .value(id)
                    .toObject()
                    .value(QStringLiteral("enabled"))
                    .toBool();
            }));
        }
        QVERIFY(
            dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/a"))->isChecked());
        QVERIFY(
            dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/b"))->isChecked());
        dataManager.findChild<QCheckBox *>(QStringLiteral("activeProvider/a"))->click();
        QVERIFY(QTest::qWaitFor([&] {
            return !feeds.sources()
                        .value(QStringLiteral("a"))
                        .toObject()
                        .value(QStringLiteral("enabled"))
                        .toBool();
        }));
        QVERIFY(feeds.sources()
                    .value(QStringLiteral("b"))
                    .toObject()
                    .value(QStringLiteral("enabled"))
                    .toBool());
        dataTable->setCurrentCell(0, 2);
        answerDeletion();
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
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            QVERIFY(map);
            auto *preferences = dynamic_cast<swak::PreferenceParticipant *>(map);
            QVERIFY(preferences);
            preferences->restorePreferences(
                {{QStringLiteral("zoom"), 4}, {QStringLiteral("diskCacheMiB"), 256}});
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
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                QVERIFY(dialog);
                const auto close = qScopeGuard([dialog] {
                    dialog->reject();
                });
                QVERIFY(dialog->findChild<QTableWidget *>(QStringLiteral("mapProviderList")));
                QVERIFY(dialog->findChild<QTableWidget *>(QStringLiteral("dataProviderList")));
                QVERIFY(!dialog->findChild<QComboBox *>(QStringLiteral("org.swak.maps/active")));
                auto *sections =
                    dialog->findChild<QListWidget *>(QStringLiteral("settingsSections"));
                for (int i = 0; i < sections->count(); ++i) {
                    if (sections->item(i)->data(Qt::UserRole).toString() ==
                        QStringLiteral("org.swak.maps")) {
                        sections->setCurrentRow(i);
                    }
                }
                QVERIFY(QTest::qWaitForWindowExposed(dialog));
                QVERIFY(dialog->grab().save(QStringLiteral("provider-settings-smoke.png")));
                tested = true;
            });
            window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
            QVERIFY(tested);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        const auto saved =
            dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>())
                ->savePreferences();
        QCOMPARE(saved.value(QStringLiteral("centerX")).toDouble(), expectedX);
        QCOMPARE(saved.value(QStringLiteral("diskCacheMiB")).toInt(), 256);
    }
    void compactToolbarAndKeys()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("compact.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(toolbar);
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *tools = window.findChild<swak::MapTools *>();
        QVERIFY(tools);
        auto *marker = window.findChild<QAction *>(QStringLiteral("markerModeAction"));
        auto *area = window.findChild<QAction *>(QStringLiteral("polygonModeAction"));
        QVERIFY(marker && area);
        QVERIFY(toolbar->actions().contains(marker));
        QVERIFY(toolbar->actions().contains(area));
        QVERIFY(!marker->icon().isNull());
        QVERIFY(!area->icon().isNull());
        QCOMPARE(area->text(), QStringLiteral("Area"));
        QVERIFY(!toolbar->findChild<QLabel *>(QStringLiteral("toolbarPluginLabel")));
        QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        auto *markerButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(marker));
        QVERIFY(markerButton);
        QTest::mouseClick(markerButton, Qt::LeftButton);
        QCOMPARE(tools->statusText(), QStringLiteral("M"));
        QVERIFY(marker->isChecked());
        auto *areaButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(area));
        QVERIFY(areaButton);
        QTest::mouseClick(areaButton, Qt::LeftButton);
        QCOMPARE(tools->statusText(), QStringLiteral("A"));
        QVERIFY(area->isChecked());
        QVERIFY(!marker->isChecked());
        QTest::mouseClick(areaButton, Qt::LeftButton);
        QVERIFY(tools->statusText().isEmpty());
        QVERIFY(!area->isChecked());
        QTest::keyClick(map, Qt::Key_A, Qt::ShiftModifier);
        QCOMPARE(tools->statusText(), QStringLiteral("A"));
        QTest::keyClick(map, Qt::Key_A, Qt::ShiftModifier);
        QVERIFY(tools->statusText().isEmpty());
        auto *navigation = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(navigation);
        navigation->restorePreferences({{QStringLiteral("panNorthKey"), QStringLiteral("W")},
                                        {QStringLiteral("panWestKey"), QStringLiteral("A")},
                                        {QStringLiteral("panSouthKey"), QStringLiteral("S")},
                                        {QStringLiteral("panEastKey"), QStringLiteral("D")},
                                        {QStringLiteral("stepWestKey"), QStringLiteral("Left")}});
        const auto keys = navigation->savePreferences();
        QCOMPARE(keys.value(QStringLiteral("panNorthKey")).toString(), QStringLiteral("W"));
        QCOMPARE(keys.value(QStringLiteral("panWestKey")).toString(), QStringLiteral("A"));
        QCOMPARE(keys.value(QStringLiteral("panSouthKey")).toString(), QStringLiteral("S"));
        QCOMPARE(keys.value(QStringLiteral("panEastKey")).toString(), QStringLiteral("D"));
        QVERIFY(keys.value(QStringLiteral("stepWestKey")).toString().isEmpty());
        navigation->restorePreferences({{"navigationKeysVersion", 2},
                                        {"panNorthKey", "Up"},
                                        {"panWestKey", "Left"},
                                        {"panSouthKey", "Down"},
                                        {"panEastKey", "Right"}});
        QCOMPARE(navigation->savePreferences().value("panWestKey").toString(), QStringLiteral("A"));
        navigation->restorePreferences({{"navigationKeysVersion", 3},
                                        {"panNorthKey", "Up"},
                                        {"panWestKey", "Left"},
                                        {"panSouthKey", "Down"},
                                        {"panEastKey", "Right"}});
        QCOMPARE(navigation->savePreferences().value("panWestKey").toString(),
                 QStringLiteral("Left"));
        navigation->restorePreferences({});

        auto *editor = window.findChild<swak::ObjectEditor *>(QStringLiteral("polygonEditor"));
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(editor);
        QVERIFY(settings);
        // Marker mode uses Shift+M. Earlier profiles with the old M default move to Shift+M.
        auto *markers = dynamic_cast<swak::SettingsParticipant *>(
            window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")));
        QVERIFY(markers);
        QCOMPARE(markers->savePreferences().value(QStringLiteral("createKey")).toString(),
                 QStringLiteral("Shift+M"));
        QVERIFY(
            window.findChild<QAction *>(QStringLiteral("markerModeAction"))
                ->toolTip()
                .contains(
                    QKeySequence(QStringLiteral("Shift+M")).toString(QKeySequence::NativeText)));
        markers->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("M")}});
        QCOMPARE(markers->savePreferences().value(QStringLiteral("createKey")).toString(),
                 QStringLiteral("Shift+M"));
        markers->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("M")},
                                     {QStringLiteral("markerShortcutVersion"), 2}});
        QCOMPARE(markers->savePreferences().value(QStringLiteral("createKey")).toString(),
                 QStringLiteral("M"));
        auto markerKey = markers->savePreferences();
        markerKey.insert(QStringLiteral("createKey"), QStringLiteral("Ctrl+M"));
        QVERIFY(!markers->validateSettings(markerKey).isEmpty());
        markerKey.insert(QStringLiteral("createKey"), QStringLiteral("Shift+K"));
        QVERIFY(markers->validateSettings(markerKey).isEmpty());
        markers->restorePreferences({});
        settings->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("P")}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("createKey")).toString(),
                 QStringLiteral("Shift+A"));
        settings->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("B")}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("createKey")).toString(),
                 QStringLiteral("B"));
        QVERIFY(area->toolTip().contains('B'));
        settings->restorePreferences({{QStringLiteral("createKey"), QStringLiteral("P")},
                                      {QStringLiteral("areaShortcutVersion"), 2}});
        QCOMPARE(settings->savePreferences().value(QStringLiteral("createKey")).toString(),
                 QStringLiteral("P"));
        settings->restorePreferences({});
        QVERIFY(toolbar->grab().save(QStringLiteral("compact-toolbar.png")));
        const auto previousPalette = QApplication::palette();
        const auto previousStyle = QApplication::style()->objectName();
        const auto restorePalette = qScopeGuard([previousPalette, previousStyle] {
            if (auto *style = QStyleFactory::create(previousStyle)) {
                QApplication::setStyle(style);
            }
            QApplication::setPalette(previousPalette);
        });
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QPalette dark(QColor("#202020"));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::Window, QColor("#202020"));
        dark.setColor(QPalette::Button, QColor("#202020"));
        QApplication::setPalette(dark);
        window.setPalette(dark);
        toolbar->setPalette(dark);
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto frame = toolbar->grab().toImage();
                return frame.pixelColor(frame.width() - 4, frame.height() / 2).lightness() < 100;
            },
            1000));
        QVERIFY(toolbar->grab().save(QStringLiteral("compact-toolbar-dark.png")));
        for (const auto icon : {swak::UiIcon::Clear,
                                swak::UiIcon::Delete,
                                swak::UiIcon::Marker,
                                swak::UiIcon::Area,
                                swak::UiIcon::Map,
                                swak::UiIcon::Edit,
                                swak::UiIcon::Move}) {
            const auto image = swak::uiIcon(icon);
            QVERIFY(!image.pixmap(QSize(24, 24), 2).isNull());
            QVERIFY(!image.pixmap(QSize(24, 24), QIcon::Disabled).isNull());
            QVERIFY(!image.pixmap(QSize(24, 24), QIcon::Selected).isNull());
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
        answerAction();
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
        QVERIFY(window.menuBar()->actions().contains(help->menuAction()));
        QVERIFY(help->actions().contains(about));
        for (int i = 0; i < 2; ++i) {
            about->trigger();
            QPointer<QMessageBox> dialog =
                window.findChild<QMessageBox *>(QStringLiteral("aboutDialog"));
            QVERIFY(dialog);
            QVERIFY(
                dialog->text().contains(QStringLiteral("Version ") + QStringLiteral(SWAK_VERSION)));
            QVERIFY(dialog->informativeText().contains(QStringLiteral(SWAK_REVISION)));
            QVERIFY(QStringLiteral(SWAK_VERSION).contains(QStringLiteral("+g")) ||
                    QStringLiteral(SWAK_VERSION).endsWith(QStringLiteral("+unknown")));
            QCOMPARE(dialog->textFormat(), Qt::PlainText);
            about->trigger();
            QCOMPARE(window.findChildren<QMessageBox *>().size(), 1);
            QVERIFY(QTest::qWaitForWindowExposed(dialog));
            QVERIFY(dialog->grab().save(QStringLiteral("about-smoke.png")));
            dialog->close();
            QVERIFY(QTest::qWaitFor([&dialog] {
                return dialog.isNull();
            }));
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
        swak::Window window(directory.filePath(QStringLiteral("activate.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *root = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QVERIFY(root);
        int activated = 0;
        for (auto *action : root->actions()) {
            auto docks = window.findChildren<QDockWidget *>();
            auto found = std::find_if(docks.begin(), docks.end(), [action](auto *dock) {
                return dock->windowTitle() == action->text();
            });
            if (found == docks.end()) {
                continue;
            }
            ++activated;
            auto *dock = *found;
            QVERIFY(action->isCheckable());
            if (action->menu()) {
                for (auto *child : action->menu()->actions()) {
                    QVERIFY(child->text() != QStringLiteral("Show pane"));
                }
            }
            dock->hide();
            root->popup(window.mapToGlobal(QPoint(40, 60)));
            QVERIFY(QTest::qWaitForWindowExposed(root));
            QTest::mouseClick(
                root, Qt::LeftButton, Qt::NoModifier, root->actionGeometry(action).center());
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
        QCOMPARE(activated, 16);
        QVERIFY(window.grab().save(QStringLiteral("plugin-labels-smoke.png")));
    }
    void clearEventsMenu()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("clear.json")),
                            QPluginLoader::staticInstances());
        auto *plugins = window.findChild<QMenu *>(QStringLiteral("pluginsMenu"));
        QVERIFY(plugins);
        QMenu *events = nullptr;
        for (auto *action : plugins->actions()) {
            if (action->menu() && action->menu()->title() == QStringLiteral("CoT Messages")) {
                events = action->menu();
            }
        }
        QVERIFY(events);
        auto *clear = window.findChild<QAction *>(QStringLiteral("clearEventsAction"));
        QVERIFY(clear && events->actions().contains(clear));
        QCOMPARE(clear->text(), QStringLiteral("Clear ..."));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(toolbar && !toolbar->actions().contains(clear));
        QVERIFY(!clear->icon().isNull());
        QCOMPARE(clear->toolTip(), QStringLiteral("Clear CoT Messages"));
        QVERIFY(!window.menuBar()->actions().contains(events->menuAction()));
        auto *generate = window.findChild<QPushButton *>(QStringLiteral("generateEvent"));
        QVERIFY(generate);
        generate->click();
        QCOMPARE(window.store().rowCount(), 1);
        QVERIFY(!window.store().selected().isEmpty());
        answerAction(Qt::Key_Escape);
        clear->trigger();
        QCOMPARE(window.store().rowCount(), 1);
        answerAction();
        clear->trigger();
        QCOMPARE(window.store().rowCount(), 0);
        QVERIFY(window.store().selected().isEmpty());
        answerAction();
        clear->trigger();
        QCOMPARE(window.store().rowCount(), 0);
    }
    void localizedSettingsLists()
    {
        const QLocale previous;
        const auto restoreLocale = qScopeGuard([previous] {
            QLocale::setDefault(previous);
        });
        QLocale::setDefault(QLocale(QStringLiteral("sv_SE")));
        const QStringList labels{QStringLiteral("Örebro"),
                                 QStringLiteral("Ångström"),
                                 QStringLiteral("Zulu"),
                                 QStringLiteral("Älg"),
                                 QStringLiteral("Alpha")};
        QList<swak::SettingsPage> pages;
        int chosen = -1;
        for (const auto &label : labels) {
            pages.append({label, label, {}, {}, {}, {}});
        }
        pages.append(
            {QStringLiteral("choice"),
             QStringLiteral("Choice"),
             {swak::choiceSetting(
                 QStringLiteral("value"), QStringLiteral("Value"), labels, {10, 20, 30, 40, 50})},
             {{QStringLiteral("value"), 20}},
             {},
             [&](const QJsonObject &values) {
                 chosen = values.value(QStringLiteral("value")).toInt();
             }});
        pages.append({QStringLiteral("pane-z"), QStringLiteral("Zulu"), {}, {}, {}, {}, true});
        pages.append({QStringLiteral("pane-a"), QStringLiteral("Alpha"), {}, {}, {}, {}, true});
        swak::SettingsDialog dialog(
            pages,
            [] {
                return QString{};
            },
            nullptr);
        const QCollator collator;
        for (auto *list : dialog.findChildren<QListWidget *>()) {
            for (int i = 1; i < list->count(); ++i) {
                QVERIFY(collator.compare(list->item(i - 1)->text(), list->item(i)->text()) <= 0);
            }
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
        dialog.restoreNavigation(
            {{QStringLiteral("tab"), QStringLiteral("panes")},
             {QStringLiteral("paneSettingsSections"), QStringLiteral("pane-z")}});
        QCOMPARE(dialog.findChild<QTabWidget *>()->currentIndex(), 1);
        QCOMPARE(dialog.findChild<QListWidget *>(QStringLiteral("paneSettingsSections"))
                     ->currentItem()
                     ->text(),
                 QStringLiteral("Zulu"));
        swak::SettingsDialog reopened(
            pages,
            [] {
                return QString{};
            },
            nullptr);
        reopened.restoreNavigation(dialog.navigation());
        QCOMPARE(reopened.navigation(), dialog.navigation());
    }
    void pauseAndInformationTrace()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("pause.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        swak::Event event;
        event.uid = QStringLiteral("paused");
        event.callsign = QStringLiteral("Paused unit");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-s");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(60);
        QVERIFY(window.store().accept(event).isEmpty());
        const auto before = window.store().motion().snapshot().first().world;
        map->setFocus();
        QTest::keyClick(map, Qt::Key_Space);
        QVERIFY(window.store().motion().paused());
        event.longitude = 1;
        event.time = event.time.addMSecs(100);
        QVERIFY(window.store().accept(event).isEmpty());
        QCOMPARE(window.store().events().first().longitude, 1.0);
        QCOMPARE(window.store().motion().snapshot().first().world, before);
        QTest::keyClick(map, Qt::Key_Space);
        QVERIFY(!window.store().motion().paused());
        QCOMPARE(window.store().motion().snapshot().first().world, swak::project(0, 1));
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(selection);
        swak::MapItem item;
        item.owner = QStringLiteral("org.swak.events");
        item.id = event.uid;
        item.label = event.callsign;
        item.type = event.type;
        item.properties.insert(QStringLiteral("How"), event.how);
        selection->select(item);
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("informationType"))->text(),
                 QStringLiteral("Object / Friend / Ground / Unit (a-f-G-U)"));
        QCOMPARE(window.findChild<QTableWidget *>(QStringLiteral("informationProperties"))
                     ->item(0, 1)
                     ->text(),
                 QStringLiteral("Machine generated / Simulation (m-s)"));
        QCOMPARE(swak::cotFieldDescription(swak::CotField::How, QStringLiteral("x-z")),
                 QStringLiteral("Unknown category (x-z)"));
        QCOMPARE(swak::cotFieldDescription(swak::CotField::Type, QStringLiteral("a-f-G-Z")),
                 QStringLiteral("Object / Friend / Ground / Unknown category (a-f-G-Z)"));
        auto *box = window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker"));
        QVERIFY(box);
        QVERIFY(!box->isChecked());
        box->click();
        auto *trace = dynamic_cast<swak::SettingsParticipant *>(
            window.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(trace);
        QCOMPARE(trace->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
        QVERIFY(QTest::qWaitFor([&] {
            return window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker"))
                ->isChecked();
        }));
        auto *traceTool =
            dynamic_cast<swak::MapTool *>(window.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(traceTool);
        const auto traceImage = [&] {
            QImage image(800, 200, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            traceTool->paint(painter, swak::Viewport{QSizeF(800, 200), swak::project(0, 1), 8});
            return image;
        };
        event.longitude = 1.2;
        event.time = event.time.addMSecs(100);
        QVERIFY(window.store().accept(event).isEmpty());
        const auto trail = traceImage();
        map->setFocus();
        QTest::keyClick(map, Qt::Key_Space);
        QVERIFY(window.store().motion().paused());
        event.longitude = 1.5;
        event.time = event.time.addMSecs(100);
        QVERIFY(window.store().accept(event).isEmpty());
        QCOMPARE(traceImage(), trail);
        QCOMPARE(window.store().events().first().longitude, 1.5);
        QTest::keyClick(map, Qt::Key_Space);
        QVERIFY(!window.store().motion().paused());
        QVERIFY(traceImage() != trail);
        QVERIFY(window.grab().save(QStringLiteral("information-trace.png")));
        window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker"))->click();
        QCOMPARE(trace->savePreferences().value(QStringLiteral("targets")).toArray().size(), 0);
        item.kind = swak::MapItem::Kind::Polygon;
        item.owner = QStringLiteral("org.swak.polygons");
        selection->select(item);
        QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("information/traceMarker")));
    }
    void scaledMarkerInteraction()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("scaled-points.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("centerX"), .5);
        values.insert(QStringLiteral("centerY"), .5);
        values.insert(QStringLiteral("zoom"), 12);
        auto *objects = window.findChild<swak::UserObjects *>();
        auto *selection = window.findChild<swak::MapSelection *>();
        swak::UserObject marker;
        marker.id = QStringLiteral("scaled-local");
        marker.type = QStringLiteral("circle");
        marker.title = QStringLiteral("Scaled local marker");
        marker.geometry = {{QStringLiteral("x"), .5}, {QStringLiteral("y"), .5}};
        QVERIFY(objects->put(marker).isEmpty());
        const QPoint center(map->width() / 2, map->height() / 2);
        for (const int percent : {50, 100, 200}) {
            values.insert(QStringLiteral("markerScale"), percent);
            settings->restorePreferences(values);
            selection->clear();
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, center + QPoint(0, 15));
            QCOMPARE(selection->item().has_value(), percent == 200);
            QVERIFY(map->grabFramebuffer().save(QStringLiteral("marker-size-%1.png").arg(percent)));
        }
        objects->remove(marker.id);
        selection->clear();
        swak::Event event;
        event.uid = QStringLiteral("scaled-cot");
        event.callsign = QStringLiteral("Scaled CoT marker");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-s");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(60);
        QVERIFY(window.store().accept(event).isEmpty());
        for (const int percent : {50, 100, 200}) {
            values.insert(QStringLiteral("markerScale"), percent);
            settings->restorePreferences(values);
            selection->clear();
            QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, center + QPoint(0, 20));
            QCOMPARE(selection->item().has_value(), percent == 200);
        }
    }
    void markerScaleSettings()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("marker-scale.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
            QVERIFY(settings);
            QCOMPARE(settings->savePreferences().value(QStringLiteral("markerScale")).toInt(), 100);
            auto *objects = window.findChild<swak::UserObjects *>();
            swak::UserObject existing;
            existing.id = QStringLiteral("existing-size-test");
            existing.type = QStringLiteral("circle");
            existing.title = QStringLiteral("Existing marker");
            existing.color = QStringLiteral("#ffc857");
            existing.geometry = {{"x", .5}, {"y", .5}};
            QVERIFY(objects->put(existing).isEmpty());
            const auto markerPixels = [&] {
                const auto image = map->grabFramebuffer();
                int count = 0;
                for (int y = image.height() / 2 - 50; y < image.height() / 2 + 50; ++y) {
                    for (int x = image.width() / 2 - 50; x < image.width() / 2 + 50; ++x) {
                        count += image.pixelColor(x, y) == QColor("#ffc857");
                    }
                }
                return count;
            };
            const int originalPixels = markerPixels();
            QVERIFY(originalPixels > 10);
            const auto toolbarSize = window.findChild<QToolBar *>()->iconSize();
            for (const bool apply : {false, true}) {
                QTimer::singleShot(0, &window, [&] {
                    auto *dialog = window.findChild<swak::SettingsDialog *>();
                    QVERIFY(dialog);
                    dialog->restoreNavigation(
                        {{QStringLiteral("settingsSections"), QStringLiteral("org.swak.map")}});
                    auto *combo =
                        dialog->findChild<QComboBox *>(QStringLiteral("org.swak.map/markerScale"));
                    QVERIFY(combo);
                    QCOMPARE(combo->count(), 6);
                    for (const int percent : {50, 75, 100, 125, 150, 200}) {
                        QVERIFY(combo->findData(percent) >= 0);
                    }
                    combo->setCurrentIndex(combo->findData(200));
                    QVERIFY(dialog->grab().save(QStringLiteral("marker-scale-settings.png")));
                    dialog->findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"))
                        ->button(apply ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)
                        ->click();
                });
                window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
                QCOMPARE(settings->savePreferences().value(QStringLiteral("markerScale")).toInt(),
                         apply ? 200 : 100);
            }
            QCOMPARE(objects->items().size(), 1);
            QVERIFY(markerPixels() > 2 * originalPixels);
            auto *markerSettings = dynamic_cast<swak::SettingsParticipant *>(
                window.findChild<swak::ObjectEditor *>(QStringLiteral("objectEditor")));
            QVERIFY(markerSettings);
            const auto oldRadius = markerPixels();
            auto markerValues = markerSettings->savePreferences();
            markerValues.insert("radius", 14);
            markerSettings->restorePreferences(markerValues);
            QVERIFY(markerPixels() > 2 * oldRadius);
            QCOMPARE(objects->items().size(), 1);
            QCOMPARE(window.findChild<QToolBar *>()->iconSize(), toolbarSize);
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *settings =
            dynamic_cast<swak::SettingsParticipant *>(restored.findChild<QOpenGLWidget *>());
        QVERIFY(settings);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("markerScale")).toInt(), 200);
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("markerScale"), 9999);
        settings->restorePreferences(values);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("markerScale")).toInt(), 100);
    }
    void informationPredict()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("predict.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *tool = dynamic_cast<swak::MapTool *>(
                window.findChild<QObject *>(QStringLiteral("predictTool")));
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(tool);
            QVERIFY(tool && settings);
            QCOMPARE(window.findChildren<swak::TrackMotion *>().size(), 1);
            auto *map = window.findChild<QOpenGLWidget *>();
            auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map);
            auto mapValues = mapSettings->savePreferences();
            mapValues.insert(QStringLiteral("centerX"), swak::project(0, .03).x());
            mapValues.insert(QStringLiteral("centerY"), .5);
            mapValues.insert(QStringLiteral("zoom"), 12);
            mapSettings->restorePreferences(mapValues);
            auto animation = window.store().motion().savePreferences();
            animation.insert(QStringLiteral("duration"), 0);
            window.store().motion().restorePreferences(animation);
            swak::Event event;
            event.uid = QStringLiteral("forecast-unit");
            event.callsign = QStringLiteral("Predicted unit");
            event.type = QStringLiteral("a-f-G-U");
            event.how = QStringLiteral("m-s");
            event.time = event.start = QDateTime::currentDateTimeUtc().addSecs(-60);
            event.stale = event.time.addSecs(600);
            QVERIFY(window.store().accept(event).isEmpty());
            swak::MapItem item;
            item.owner = QStringLiteral("org.swak.events");
            item.id = event.uid;
            item.label = event.callsign;
            window.findChild<swak::MapSelection *>()->select(item);
            auto *box = window.findChild<QCheckBox *>(QStringLiteral("information/predictMarker"));
            QVERIFY(box && !box->isChecked());
            box->click();
            const auto image = [&] {
                QImage result(1000, 400, QImage::Format_ARGB32_Premultiplied);
                result.fill(Qt::transparent);
                QPainter painter(&result);
                tool->paint(painter, swak::Viewport{QSizeF(1000, 400), swak::project(0, .03), 12});
                return result;
            };
            const auto empty = image();
            event.time = event.time.addSecs(1);
            event.longitude = .0001;
            QVERIFY(window.store().accept(event).isEmpty());
            const auto forecast = image();
            QVERIFY(forecast != empty);
            // This straight track travels 0.03 degrees in five minutes.
            const swak::Viewport forecastView{QSizeF(1000, 400), swak::project(0, .03), 12};
            const auto terminal = forecastView.screen(0, event.longitude + .03);
            for (const auto &[seconds, halfTick] :
                 {std::pair{30, 7}, std::pair{60, 5}, std::pair{300, 3}}) {
                const auto tick = forecastView.screen(0, event.longitude + seconds * .0001);
                bool visible = false;
                for (int x = qFloor(tick.x()) - 1; x <= qCeil(tick.x()) + 1; ++x) {
                    visible = visible || forecast.pixelColor(x, 201 - halfTick).alpha() > 0;
                }
                QVERIFY2(visible,
                         qPrintable(QStringLiteral("Missing tick at %1 seconds").arg(seconds)));
            }
            QVERIFY(forecast.pixelColor(qFloor(terminal.x()) - 2, 200).alpha() > 0);
            // A click on a prediction line reports the predicted CoT object.
            const auto predicted = tool->itemsAt(forecastView, QPointF(terminal.x() - 20, 202));
            QCOMPARE(predicted.size(), 1);
            QCOMPARE(predicted.first().owner, QStringLiteral("org.swak.events"));
            QCOMPARE(predicted.first().id, event.uid);
            QCOMPARE(predicted.first().label, event.callsign);
            QVERIFY(tool->itemsAt(forecastView, QPointF(terminal.x() - 20, 260)).isEmpty());
            // Show predictions hides the lines and their hits but keeps the history.
            auto *predictSettings = dynamic_cast<swak::SettingsParticipant *>(tool);
            auto hidden = predictSettings->savePreferences();
            hidden.insert(QStringLiteral("visible"), false);
            predictSettings->restorePreferences(hidden);
            QCOMPARE(image(), empty);
            QVERIFY(tool->itemsAt(forecastView, QPointF(terminal.x() - 20, 202)).isEmpty());
            hidden.insert(QStringLiteral("visible"), true);
            predictSettings->restorePreferences(hidden);
            QCOMPARE(image(), forecast);
            for (int x = qCeil(terminal.x()) + 2; x < forecast.width(); ++x) {
                QVERIFY(forecast.pixelColor(x, 200).alpha() == 0);
            }
            QVERIFY(forecast.save(QStringLiteral("prediction-lines.png")));
            QVERIFY(map->grabFramebuffer().save(QStringLiteral("prediction-map.png")));
            window.store().motion().setPaused(true);
            event.time = event.time.addSecs(1);
            event.latitude = .0001;
            QVERIFY(window.store().accept(event).isEmpty());
            QCOMPARE(image(), forecast);
            window.store().motion().setPaused(false);
            QVERIFY(image() != forecast);
            animation.insert(QStringLiteral("duration"), 200);
            animation.insert(QStringLiteral("fade"), 400);
            animation.insert(QStringLiteral("teleportKm"), .1);
            window.store().motion().restorePreferences(animation);
            const auto alpha = [](const QImage &frame) {
                qint64 sum = 0;
                for (int y = 0; y < frame.height(); ++y) {
                    for (int x = 0; x < frame.width(); ++x) {
                        sum += frame.pixelColor(x, y).alpha();
                    }
                }
                return sum;
            };
            const auto initialAlpha = alpha(image());
            event.time = event.time.addSecs(1);
            event.latitude += .0001;
            QVERIFY(window.store().accept(event).isEmpty());
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return alpha(image()) < initialAlpha / 3;
                },
                500));
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return alpha(image()) > initialAlpha / 2;
                },
                1000));
            window.store().expire(event.stale.addSecs(1));
            QVERIFY(!image().isNull());
            window.store().clear();
            QCOMPARE(image(), empty);
            QVERIFY(window.store().accept(event).isEmpty());
            QCOMPARE(image(), empty);
            event.time = event.time.addSecs(1);
            QVERIFY(window.store().accept(event).isEmpty());
            const auto stationary = image();
            int occupiedRows = 0;
            for (int y = 0; y < stationary.height(); ++y) {
                bool painted = false;
                for (int x = 0; x < stationary.width(); ++x) {
                    painted = painted || stationary.pixelColor(x, y).alpha() > 0;
                }
                occupiedRows += painted;
            }
            QVERIFY(occupiedRows > 0);
            QVERIFY(occupiedRows <= 18);
            QMenu menu;
            tool->populateContextMenu(item, menu);
            auto *action = menu.findChild<QAction *>(QStringLiteral("predictMarker"));
            QVERIFY(action && action->isChecked());
            action->trigger();
            QCOMPARE(image(), empty);
            QVERIFY(
                settings->savePreferences().value(QStringLiteral("targets")).toArray().isEmpty());
            QMenu enable;
            tool->populateContextMenu(item, enable);
            enable.findChild<QAction *>(QStringLiteral("predictMarker"))->trigger();
            QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(),
                     1);
            item.owner = QStringLiteral("org.swak.markers");
            window.findChild<swak::MapSelection *>()->select(item);
            QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("information/predictMarker")));
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(
            restored.findChild<QObject *>(QStringLiteral("predictTool")));
        QVERIFY(settings);
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
        answerAction();
        restored.findChild<QAction *>(QStringLiteral("clearPredictions"))->trigger();
        QVERIFY(settings->savePreferences().value(QStringLiteral("targets")).toArray().isEmpty());
    }
    void resetTraceSettings()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("trace-reset.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *settings = dynamic_cast<swak::SettingsParticipant *>(
                window.findChild<QObject *>(QStringLiteral("traceTool")));
            QVERIFY(settings);
            auto values = settings->savePreferences();
            values.insert(QStringLiteral("positions"), 10);
            values.insert(QStringLiteral("opacity"), 0.35);
            values.insert(
                QStringLiteral("targets"),
                QJsonArray{QJsonObject{{QStringLiteral("owner"), QStringLiteral("org.swak.events")},
                                       {QStringLiteral("id"), QStringLiteral("retained")}}});
            settings->restorePreferences(values);
            for (const bool apply : {false, true}) {
                QTimer::singleShot(0, &window, [&] {
                    auto *dialog = window.findChild<swak::SettingsDialog *>();
                    QVERIFY(dialog);
                    dialog->restoreNavigation(
                        {{QStringLiteral("settingsSections"), QStringLiteral("org.swak.trace")}});
                    auto *count = dialog->findChild<QDoubleSpinBox *>(
                        QStringLiteral("org.swak.trace/positions"));
                    QVERIFY(count);
                    QCOMPARE(count->value(), 10.0);
                    auto *reset = dialog->findChild<QPushButton *>(
                        QStringLiteral("org.swak.trace/restoreDefaults"));
                    QVERIFY(reset);
                    reset->click();
                    QCOMPARE(count->value(), 50.0);
                    QCOMPARE(
                        dialog
                            ->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.trace/opacity"))
                            ->value(),
                        0.7);
                    QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(),
                             10);
                    QVERIFY(dialog->grab().save(QStringLiteral("trace-reset.png")));
                    dialog->findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"))
                        ->button(apply ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)
                        ->click();
                });
                window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
                QCOMPARE(settings->savePreferences().value(QStringLiteral("positions")).toInt(),
                         apply ? 50 : 10);
                QCOMPARE(
                    settings->savePreferences().value(QStringLiteral("targets")).toArray().size(),
                    1);
            }
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(
            restored.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(settings);
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
                if (!dialog) {
                    QFAIL("Settings dialog did not open.");
                    return;
                }
                auto *field =
                    dialog->findChild<QCheckBox *>(QStringLiteral("org.swak.example/enabled"));
                if (!field) {
                    dialog->reject();
                    QFAIL("The extension section is missing.");
                    return;
                }
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
        swak::SettingsDialog dialog(
            {{QStringLiteral("test"),
              QStringLiteral("Precision"),
              {swak::numberSetting(QStringLiteral("number"), QStringLiteral("Number"), 0, 1, 2)},
              {{QStringLiteral("number"), 0.123456789}},
              {},
              [&](const QJsonObject &) {
                  ++applications;
              }}},
            [] {
                return QString{};
            },
            nullptr);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(applications, 0);
    }
    void tableColumnSettings()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("table.json")),
                            QPluginLoader::staticInstances());
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
            QVERIFY(
                window.findChild<QMenu *>(QStringLiteral("fileMenu"))->actions().contains(action));
            QCOMPARE(action->text(), QStringLiteral("&Settings"));
            QVERIFY(!window.findChild<QMenu *>(QStringLiteral("settingsMenu")));
            QCOMPARE(window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"))
                         ->windowTitle(),
                     QStringLiteral("CoT Event Generator"));
            QCOMPARE(window.findChild<QDockWidget *>(QStringLiteral("org.swak.events.pane"))
                         ->windowTitle(),
                     QStringLiteral("CoT Messages"));
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                if (!dialog) {
                    QFAIL("Settings dialog did not open.");
                    return;
                }
                const auto cancel = [&] {
                    dialog->reject();
                };
                auto *identity =
                    dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.editor/identity"));
                if (!identity) {
                    cancel();
                    QFAIL("The editor section is missing.");
                    return;
                }
                identity->setText(QStringLiteral("cancelled"));
                cancel();
            });
            const auto initial = window.findChild<QLineEdit *>(QStringLiteral("identity"))->text();
            action->trigger();
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(), initial);
            bool complete = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<swak::SettingsDialog *>();
                if (!dialog) {
                    QFAIL("Settings dialog did not open.");
                    return;
                }
                auto *tabs = dialog->findChild<QTabWidget *>();
                auto *panes =
                    dialog->findChild<QListWidget *>(QStringLiteral("paneSettingsSections"));
                if (!tabs || tabs->count() != 3 || !panes || panes->count() != 21) {
                    dialog->reject();
                    QFAIL("The pane tab is incomplete.");
                    return;
                }
                for (auto *label : dialog->findChildren<QLabel *>()) {
                    if (label->buddy() && label->wordWrap()) {
                        dialog->reject();
                        QFAIL("A Settings label wraps across lines.");
                        return;
                    }
                    if (label->buddy() && label->minimumWidth() < label->sizeHint().width()) {
                        dialog->reject();
                        QFAIL("A Settings field label can be clipped.");
                        return;
                    }
                }
                const QCollator collator;
                for (auto *list : dialog->findChildren<QListWidget *>()) {
                    for (int i = 0; i < list->count(); ++i) {
                        if (list->item(i)->text().startsWith(QStringLiteral("Pane:")) ||
                            (i > 0 && collator.compare(list->item(i - 1)->text(),
                                                       list->item(i)->text()) > 0)) {
                            dialog->reject();
                            QFAIL("Settings sections are not correctly named and sorted.");
                            return;
                        }
                    }
                }
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                auto *identity =
                    dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.editor/identity"));
                auto *color =
                    dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.markers/color"));
                auto *enabled = dialog->findChild<QCheckBox *>(QStringLiteral("logging/enabled"));
                auto *severity = dialog->findChild<QComboBox *>(QStringLiteral("logging/severity"));
                auto *interval =
                    dialog->findChild<QDoubleSpinBox *>(QStringLiteral("application/saveInterval"));
                auto *zoom =
                    dialog->findChild<QDoubleSpinBox *>(QStringLiteral("org.swak.map/zoom"));
                auto *width = dialog->findChild<QDoubleSpinBox *>(
                    QStringLiteral("org.swak.events/columns/0/width"));
                if (!buttons || !identity || !color || !enabled || !severity || !interval ||
                    !zoom || !width) {
                    dialog->reject();
                    QFAIL("A settings field is missing.");
                    return;
                }
                identity->setText(QStringLiteral("configured"));
                color->setText(QStringLiteral("!"));
                buttons->button(QDialogButtonBox::Apply)->click();
                if (window.findChild<QLineEdit *>(QStringLiteral("identity"))->text() != initial) {
                    dialog->reject();
                    QFAIL("An invalid page allowed partial application.");
                    return;
                }
                color->setText(QStringLiteral("#ffc857"));
                enabled->setChecked(true);
                severity->setCurrentIndex(severity->findData(3));
                interval->setValue(2000);
                zoom->setValue(4);
                width->setValue(230);
                buttons->button(QDialogButtonBox::Apply)->click();
                if (!QTest::qWaitFor(
                        [dialog] {
                            for (auto *scroll : dialog->findChildren<QScrollArea *>()) {
                                if (scroll->isVisible() &&
                                    scroll->widget()->width() > scroll->viewport()->width()) {
                                    return false;
                                }
                            }
                            return true;
                        },
                        1000)) {
                    dialog->reject();
                    QFAIL("The Settings form overflows horizontally.");
                    return;
                }
                if (!dialog->grab().save(QStringLiteral("settings-smoke.png"))) {
                    dialog->reject();
                    QFAIL("Settings screenshot failed.");
                    return;
                }
                tabs->setCurrentIndex(1);
                if (!QTest::qWaitFor(
                        [dialog] {
                            for (auto *scroll : dialog->findChildren<QScrollArea *>()) {
                                if (scroll->isVisible() &&
                                    scroll->widget()->width() > scroll->viewport()->width()) {
                                    return false;
                                }
                            }
                            return true;
                        },
                        1000)) {
                    dialog->reject();
                    QFAIL("The pane form overflows horizontally.");
                    return;
                }
                if (!dialog->grab().save(QStringLiteral("settings-panes.png"))) {
                    dialog->reject();
                    QFAIL("Pane screenshot failed.");
                    return;
                }
                identity->setText(QStringLiteral("discard-after-apply"));
                dialog->reject();
                complete = true;
            });
            action->trigger();
            QVERIFY(complete);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(),
                     QStringLiteral("configured"));
            QVERIFY(swak::loggingConfiguration().value(QStringLiteral("enabled")).toBool());
            window.close();
        }
        {
            swak::Window restored(path, QPluginLoader::staticInstances());
            QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("identity"))->text(),
                     QStringLiteral("configured"));
            QCOMPARE(swak::loggingConfiguration().value(QStringLiteral("severity")).toInt(), 3);
            auto *map =
                dynamic_cast<swak::PreferenceParticipant *>(restored.findChild<QOpenGLWidget *>());
            QVERIFY(map);
            QCOMPARE(map->savePreferences().value(QStringLiteral("zoom")).toDouble(), 4.0);
        }
        swak::configureLogging({});
    }
    void settingsSaveFailure()
    {
        bool applied = false;
        swak::SettingsDialog dialog(
            {{QStringLiteral("test"),
              QStringLiteral("Test"),
              {swak::textSetting(QStringLiteral("value"), QStringLiteral("Value"), 20)},
              {{QStringLiteral("value"), QStringLiteral("before")}},
              {},
              [&](const QJsonObject &) {
                  applied = true;
              }}},
            [] {
                return QStringLiteral("Read-only profile");
            },
            nullptr);
        dialog.findChild<QLineEdit *>()->setText(QStringLiteral("after"));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QVERIFY(applied);
        QCOMPARE(dialog.result(), 0);
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("settingsStatus"))
                    ->text()
                    .contains(QStringLiteral("Read-only")));
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
            window.findChild<QLineEdit *>(QStringLiteral("identity"))
                ->setText(QStringLiteral("saved-identity"));
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))
                ->setText(QStringLiteral("Saved callsign"));
            window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"))->setValue(12.345);
            window.findChild<QSpinBox *>(QStringLiteral("lifetime"))->setValue(1200);
            auto savedServer = swak::serverDefaults();
            savedServer.insert("name", "Saved UDP");
            savedServer.insert("protocol", "udp");
            savedServer.insert("host", "::1");
            savedServer.insert("port", 17001);
            QVERIFY(window.findChild<swak::Servers *>()->put("saved-udp", savedServer).isEmpty());
            auto *table = window.findChild<QTableView *>(QStringLiteral("eventsTable"));
            table->horizontalHeader()->resizeSection(0, 240);
            table->horizontalHeader()->moveSection(3, 0);
            table->sortByColumn(2, Qt::DescendingOrder);
            auto *map = window.findChild<QOpenGLWidget *>(QStringLiteral("map"));
            QTest::keyClick(map, Qt::Key_Plus);
            QTest::keyClick(map, Qt::Key_D);
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
            QCOMPARE(window.dockWidgetArea(
                         window.findChild<QDockWidget *>(QStringLiteral("org.swak.editor.pane"))),
                     Qt::LeftDockWidgetArea);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(),
                     QStringLiteral("saved-identity"));
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(),
                     QStringLiteral("Saved callsign"));
            QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"))->value(),
                     12.345);
            QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("lifetime"))->value(), 1200);
            QCOMPARE(window.findChild<swak::Servers *>()
                         ->sources()
                         .value(QStringLiteral("saved-udp"))
                         .toObject()
                         .value(QStringLiteral("host"))
                         .toString(),
                     QStringLiteral("::1"));
            QCOMPARE(window.findChild<swak::Servers *>()
                         ->sources()
                         .value(QStringLiteral("saved-udp"))
                         .toObject()
                         .value(QStringLiteral("port"))
                         .toInt(),
                     17001);
            auto *table = window.findChild<QTableView *>(QStringLiteral("eventsTable"));
            QCOMPARE(table->horizontalHeader()->sectionSize(0), 240);
            QCOMPARE(table->horizontalHeader()->logicalIndex(0), 3);
            QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 2);
            QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
            auto *map = dynamic_cast<swak::PreferenceParticipant *>(
                window.findChild<QOpenGLWidget *>(QStringLiteral("map")));
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
            if (qobject_cast<swak::SourcePlugin *>(plugin)) {
                available.removeAll(plugin);
            }
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *receiver = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
            receiver->setFloating(true);
            receiver->resize(480, 310);
            receiver->move(170, 140);
            auto savedServer = swak::serverDefaults();
            savedServer.insert("name", "Saved UDP");
            savedServer.insert("protocol", "udp");
            savedServer.insert("host", "127.0.0.1");
            savedServer.insert("port", 17654);
            QVERIFY(window.findChild<swak::Servers *>()->put("saved-udp", savedServer).isEmpty());
            auto lan = swak::serverDefaults();
            lan.insert("name", "Saved LAN");
            lan.insert("protocol", "lan");
            lan.insert("localAddress", "203.0.113.254");
            QVERIFY(window.findChild<swak::Servers *>()->put("saved-lan", lan).isEmpty());
            auto *lanPane = window.findChild<QDockWidget *>("org.swak.lan.pane");
            lanPane->setFloating(true);
            lanPane->resize(700, 300);
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
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))
                ->setText(QStringLiteral("Changed without receiver"));
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *receiver = window.findChild<QDockWidget *>(QStringLiteral("org.swak.udp.pane"));
            QVERIFY(receiver->isFloating());
            QCOMPARE(receiver->size(), QSize(480, 310));
            auto *lanPane = window.findChild<QDockWidget *>("org.swak.lan.pane");
            QVERIFY(lanPane && lanPane->isFloating());
            QCOMPARE(lanPane->size(), QSize(700, 300));
            QCOMPARE(window.findChild<swak::Servers *>()
                         ->sources()
                         .value("saved-lan")
                         .toObject()
                         .value("localAddress")
                         .toString(),
                     QStringLiteral("203.0.113.254"));
            QCOMPARE(window.findChild<swak::Servers *>()
                         ->sources()
                         .value(QStringLiteral("saved-udp"))
                         .toObject()
                         .value(QStringLiteral("port"))
                         .toInt(),
                     17654);
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(),
                     QStringLiteral("Changed without receiver"));
            window.close();
        }
        swak::Preferences preferences(path);
        QCOMPARE(preferences.document().value(QStringLiteral("unknownFutureKey")).toString(),
                 QStringLiteral("preserve"));
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
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))
                ->setText(QStringLiteral("Keep draft"));
            QVERIFY(window.savePreferences());
        }
        auto available = QPluginLoader::staticInstances();
        for (auto *plugin : QPluginLoader::staticInstances()) {
            if (withoutCodec ? qobject_cast<swak::Codec *>(plugin) != nullptr
                             : qobject_cast<swak::ViewPlugin *>(plugin) != nullptr) {
                available.removeAll(plugin);
            }
        }
        {
            swak::Window window(path, available);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            if (withoutCodec) {
                QVERIFY(!window.findChild<QWidget *>(QStringLiteral("callsign")));
            } else {
                QVERIFY(window.findChild<QWidget *>(QStringLiteral("missingMap")));
            }
            window.close();
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(),
                     QStringLiteral("Keep draft"));
        }
    }
    void cotFieldPickers()
    {
        QWidget host;
        QVBoxLayout layout(&host);
        auto *field = new QLineEdit(&host);
        field->setObjectName(QStringLiteral("value"));
        field->setAccessibleName(QStringLiteral("Type"));
        field->setMaxLength(128);
        layout.addWidget(swak::cotFieldEditor(field, swak::CotField::Type));
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        auto *button = host.findChild<QPushButton *>(QStringLiteral("value/select"));
        QVERIFY(button);
        field->setText(QStringLiteral("a-f-G-U-C-I-custom"));
        button->click();
        auto *dialog = field->findChild<QDialog *>();
        QVERIFY(dialog);
        auto *value = dialog->findChild<QLineEdit *>(QStringLiteral("cotPickerValue"));
        QCOMPARE(value->text(), field->text());
        value->setText(QStringLiteral("invalid code"));
        QVERIFY(
            !dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(field->text(), QStringLiteral("a-f-G-U-C-I-custom"));
        field->setText(QStringLiteral("a-f-G-U"));
        button->click();
        dialog = field->findChild<QDialog *>();
        QVERIFY(dialog);
        auto *tree = dialog->findChild<QTreeWidget *>();
        QVERIFY(tree->currentItem());
        QCOMPARE(tree->currentItem()->text(1), field->text());
        auto matches = tree->findItems(
            QStringLiteral("a-h-G-E-V-C"), Qt::MatchExactly | Qt::MatchRecursive, 1);
        QCOMPARE(matches.size(), 1);
        tree->setCurrentItem(matches.first());
        QVERIFY(dialog->grab().save(QStringLiteral("cot-type-picker.png")));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(field->text(), QStringLiteral("a-h-G-E-V-C"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (const auto kind : {swak::CotField::How, swak::CotField::Team, swak::CotField::Role}) {
            QWidget owner;
            auto *edit = new QLineEdit(&owner);
            edit->setMaxLength(kind == swak::CotField::How ? 3 : 64);
            auto *container = swak::cotFieldEditor(edit, kind);
            container->findChild<QPushButton *>()->click();
            auto *picker = edit->findChild<QDialog *>();
            QVERIFY(picker);
            auto *list = picker->findChild<QTreeWidget *>();
            if (kind == swak::CotField::How) {
                const auto human = list->findItems(QStringLiteral("h"), Qt::MatchExactly, 1);
                QCOMPARE(human.size(), 1);
                list->setCurrentItem(human.first());
                QVERIFY(!picker->findChild<QDialogButtonBox *>()
                             ->button(QDialogButtonBox::Ok)
                             ->isEnabled());
                const auto gps = list->findItems(
                    QStringLiteral("m-g"), Qt::MatchExactly | Qt::MatchRecursive, 1);
                QCOMPARE(gps.size(), 1);
                list->setCurrentItem(gps.first());
            } else {
                QCOMPARE(list->topLevelItemCount(), kind == swak::CotField::Team ? 14 : 8);
                const QCollator collator;
                for (int i = 1; i < list->topLevelItemCount(); ++i) {
                    QVERIFY(collator.compare(list->topLevelItem(i - 1)->text(0),
                                             list->topLevelItem(i)->text(0)) <= 0);
                }
                if (kind == swak::CotField::Team) {
                    for (int i = 0; i < list->topLevelItemCount(); ++i) {
                        auto *item = list->topLevelItem(i);
                        QVERIFY(!item->icon(0).isNull());
                        auto name = item->text(1);
                        name.remove(' ');
                        QCOMPARE(item->icon(0).pixmap(24, 24).toImage().pixelColor(12, 12).rgb(),
                                 QColor(name).rgb());
                    }
                }
                list->setCurrentItem(list->topLevelItem(0));
            }
            const auto selected = list->currentItem()->text(1);
            picker->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QCOMPARE(edit->text(), selected);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            container->findChild<QPushButton *>()->click();
            QPointer<QDialog> pending = edit->findChild<QDialog *>();
            QVERIFY(pending);
            delete container;
            QVERIFY(pending.isNull());
        }
    }
    void cotPickerIntegration()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("pickers.json"));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            for (const auto *name : {"eventType", "locationMethod", "team", "role"}) {
                QVERIFY(window.findChild<QPushButton *>(QString::fromLatin1(name) +
                                                        QStringLiteral("/select")));
            }
            window.findChild<QLineEdit *>(QStringLiteral("locationMethod"))
                ->setText(QStringLiteral("m-g"));
            window.findChild<QLineEdit *>(QStringLiteral("team"))
                ->setText(QStringLiteral("Dark Blue"));
            window.findChild<QLineEdit *>(QStringLiteral("role"))->setText(QStringLiteral("Medic"));
            window.findChild<QPushButton *>(QStringLiteral("generateEvent"))->click();
            const auto decoded =
                swak::decodeCot(window.findChild<QPlainTextEdit *>(QStringLiteral("xmlPreview"))
                                    ->toPlainText()
                                    .toUtf8());
            QVERIFY(decoded.event);
            QCOMPARE(decoded.event->how, QStringLiteral("m-g"));
            QCOMPARE(decoded.event->team, QStringLiteral("Dark Blue"));
            QCOMPARE(decoded.event->role, QStringLiteral("Medic"));
            QTimer::singleShot(0, &window, [&] {
                auto *settings = window.findChild<swak::SettingsDialog *>();
                QVERIFY(settings);
                const auto close = qScopeGuard([settings] {
                    settings->reject();
                });
                for (const auto *page : {"org.swak.editor"}) {
                    for (const auto *name : {"type", "how", "team", "role"}) {
                        QVERIFY(settings->findChild<QPushButton *>(QString::fromLatin1(page) + '/' +
                                                                   QString::fromLatin1(name) +
                                                                   QStringLiteral("/select")));
                    }
                }
                settings->reject();
            });
            window.findChild<QAction *>(QStringLiteral("settingsAction"))->trigger();
            QVERIFY(window.savePreferences());
        }
        swak::Window restored(path, QPluginLoader::staticInstances());
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("locationMethod"))->text(),
                 QStringLiteral("m-g"));
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("team"))->text(),
                 QStringLiteral("Dark Blue"));
        QCOMPARE(restored.findChild<QLineEdit *>(QStringLiteral("role"))->text(),
                 QStringLiteral("Medic"));
    }
    void cotAnimationRendering()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("motion.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (auto *dock : window.findChildren<QDockWidget *>()) {
            dock->hide();
        }
        auto *map = window.findChild<QOpenGLWidget *>();
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto values = settings->savePreferences();
        values.insert(QStringLiteral("zoom"), 8);
        values.insert(QStringLiteral("centerX"), 0.5);
        values.insert(QStringLiteral("centerY"), 0.5);
        settings->restorePreferences(values);
        auto *motion = window.findChild<swak::TrackMotion *>();
        QVERIFY(motion);
        motion->restorePreferences({{QStringLiteral("duration"), 1000},
                                    {QStringLiteral("fade"), 100},
                                    {QStringLiteral("teleportKm"), 100},
                                    {QStringLiteral("staleSeconds"), 1}});
        swak::Event event;
        event.uid = QStringLiteral("moving");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-g");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(120);
        QVERIFY(window.store().accept(event).isEmpty());
        event.longitude = 0.5;
        QVERIFY(window.store().accept(event).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto x = motion->snapshot().first().world.x();
                return x > 0.5003 && x < 0.501;
            },
            800));
        const auto visual = motion->snapshot().first();
        const auto logical = (visual.world - QPointF(0.5, 0.5)) * 65536 +
                             QPointF(map->width() / 2.0, map->height() / 2.0);
        const auto image = map->grabFramebuffer();
        const auto pixel = (logical * map->devicePixelRatioF()).toPoint();
        int blue = 0;
        for (int y = pixel.y() - 12; y <= pixel.y() + 12; ++y) {
            for (int x = pixel.x() - 12; x <= pixel.x() + 12; ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.blue() > 120 && color.blue() > color.red()) {
                    ++blue;
                }
            }
        }
        QVERIFY(blue >= 5);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return motion->snapshot().first().world ==
                       swak::project(event.latitude, event.longitude);
            },
            1500));
        window.store().expire(event.stale.addSecs(1));
        QCOMPARE(window.store().rowCount(), 0);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return motion->snapshot().first().gray == 1;
            },
            500));
        const auto gray = map->grabFramebuffer();
        QVERIFY(gray.save(QStringLiteral("cot-stale-gray.png")));
        const auto target =
            ((swak::project(event.latitude, event.longitude) - QPointF(0.5, 0.5)) * 65536 +
             QPointF(map->width() / 2.0, map->height() / 2.0)) *
            map->devicePixelRatioF();
        const auto color = gray.pixelColor(target.toPoint());
        QVERIFY(qAbs(color.red() - color.blue()) < 20);
        QVERIFY(color.lightness() > 65);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return motion->snapshot().isEmpty();
            },
            2000));
        QVERIFY(window.savePreferences());
    }
    void localLanPane()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("lan-pane.json");
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            auto *pane = window.findChild<QDockWidget *>("org.swak.lan.pane");
            QVERIFY(pane);
            auto *menu = window.findChild<QMenu *>("pluginsMenu");
            QVERIFY(menu);
            QAction *toggle = nullptr;
            for (auto *action : menu->actions()) {
                if (action->text() == QStringLiteral("Local LAN")) {
                    toggle = action;
                }
            }
            QVERIFY(toggle && toggle->isCheckable());
            pane->hide();
            toggle->trigger();
            QVERIFY(!pane->isHidden());
            auto *summary = pane->findChild<QPlainTextEdit *>("localLanSummary");
            QVERIFY(summary && summary->isReadOnly());
            QVERIFY(summary->toPlainText().contains("No Local LAN servers"));
            auto *servers = window.findChild<swak::Servers *>();
            const auto editDraft = [&](const QString &title, bool accept) {
                QTimer::singleShot(0, &window, [&, title, accept] {
                    auto *dialog = window.findChild<QDialog *>("serverEditor");
                    QVERIFY(dialog);
                    for (auto *line : dialog->findChildren<QLineEdit *>()) {
                        if (line->objectName().endsWith("/name")) {
                            line->setText(title);
                        }
                    }
                    for (auto *choice : dialog->findChildren<QComboBox *>()) {
                        if (choice->objectName().endsWith("/protocol")) {
                            choice->setCurrentIndex(choice->findData(QStringLiteral("lan")));
                        }
                        if (choice->objectName().endsWith("/localAddress") &&
                            servers->sources().isEmpty()) {
                            int selected = -1;
                            for (int i = 0; i < choice->count(); ++i) {
                                const QHostAddress address(choice->itemData(i).toString());
                                if (address.protocol() == QAbstractSocket::IPv4Protocol &&
                                    !address.isLoopback() && !address.isNull()) {
                                    selected = i;
                                    break;
                                }
                            }
                            if (selected < 0) {
                                dialog->reject();
                                QFAIL("This test requires a local IPv4 interface.");
                            }
                            choice->setCurrentIndex(selected);
                        }
                    }
                    if (accept) {
                        dialog->findChild<QDialogButtonBox *>()
                            ->button(QDialogButtonBox::Ok)
                            ->click();
                    } else {
                        dialog->reject();
                    }
                });
            };
            editDraft(QStringLiteral("Test LAN"), true);
            window.findChild<QPushButton *>("addServer")->click();
            QCOMPARE(servers->sources().size(), 1);
            const auto id = servers->sources().keys().first();
            QVERIFY(!servers->sources().value(id).toObject().value("enabled").toBool());
            QCOMPARE(servers->sources().value(id).toObject().value("protocol").toString(),
                     QStringLiteral("lan"));
            editDraft(QStringLiteral("Cancelled"), false);
            window.findChild<QPushButton *>("editServer")->click();
            QCOMPARE(servers->sources().value(id).toObject().value("name").toString(),
                     QStringLiteral("Test LAN"));
            editDraft(QStringLiteral("Renamed LAN"), true);
            window.findChild<QPushButton *>("editServer")->click();
            QCOMPARE(servers->sources().value(id).toObject().value("name").toString(),
                     QStringLiteral("Renamed LAN"));
            QVERIFY(summary->toPlainText().contains("Renamed LAN: Disconnected"));
            pane->setFloating(true);
            pane->resize(850, 320);
            QVERIFY(pane->grab().save(QStringLiteral("local-lan-pane.png")));
            QVERIFY(window.savePreferences());
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *pane = window.findChild<QDockWidget *>("org.swak.lan.pane");
            QVERIFY(pane && pane->isFloating());
            QCOMPARE(pane->size(), QSize(850, 320));
        }
    }
    void serverFormAndTrace()
    {
        auto values = swak::serverDefaults();
        values.insert(QStringLiteral("enabled"), true);
        values.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        values.insert(QStringLiteral("ca"), QStringLiteral("retained.pem"));
        swak::SettingsDialog dialog(
            {{QStringLiteral("server"),
              QStringLiteral("Server"),
              swak::serverFields(),
              values,
              {},
              {}}},
            [] {
                return QString{};
            },
            nullptr,
            true);
        QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("server/enabled")));
        auto *protocol = dialog.findChild<QComboBox *>(QStringLiteral("server/protocol"));
        QVERIFY(protocol);
        auto *ca = dialog.findChild<QLineEdit *>(QStringLiteral("server/ca"));
        QVERIFY(ca);
        QVERIFY(!ca->isEnabled());
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("tls")));
        QVERIFY(ca->isEnabled());
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("udp")));
        QVERIFY(!ca->isEnabled());
        QCOMPARE(ca->text(), QStringLiteral("retained.pem"));
        auto *host = dialog.findChild<QLineEdit *>(QStringLiteral("server/host"));
        auto *port = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("server/port"));
        auto *interface = dialog.findChild<QComboBox *>(QStringLiteral("server/localAddress"));
        QVERIFY(host && port && interface);
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("lan")));
        QVERIFY(!host->isEnabled());
        QVERIFY(!port->isEnabled());
        QVERIFY(!ca->isEnabled());
        QVERIFY(interface->isEnabled());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(dialog.grab().save(QStringLiteral("server-lan-fields.png")));
        protocol->setCurrentIndex(protocol->findData(QStringLiteral("stcp")));
        QVERIFY(host->isEnabled());
        QVERIFY(port->isEnabled());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(dialog.grab().save(QStringLiteral("server-tcp-fields.png")));
        dialog.hide();
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("trace.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *trace =
            dynamic_cast<swak::MapTool *>(window.findChild<QObject *>(QStringLiteral("traceTool")));
        QVERIFY(trace);
        auto *settings = dynamic_cast<swak::SettingsParticipant *>(trace);
        QVERIFY(settings);
        auto *selection = window.findChild<swak::MapSelection *>();
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("org.swak.information.pane"));
        swak::Event event;
        event.uid = QStringLiteral("traced");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-s");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(120);
        QVERIFY(window.store().accept(event).isEmpty());
        swak::MapItem item;
        item.owner = QStringLiteral("org.swak.events");
        item.id = event.uid;
        item.label = QStringLiteral("Trace test");
        item.point = swak::project(0, 0);
        selection->select(item);
        window.store().select(event.uid);
        QVERIFY(dock->isVisible());
        QDialog unrelated(&window);
        unrelated.show();
        unrelated.close();
        QVERIFY(selection->item());
        dock->close();
        QVERIFY(!selection->item());
        QVERIFY(window.store().selected().isEmpty());
        event.longitude = 0.001;
        event.time = event.time.addMSecs(100);
        QVERIFY(window.store().accept(event).isEmpty());
        QVERIFY(!dock->isVisible());
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto mapValues = mapSettings->savePreferences();
        mapValues.insert(QStringLiteral("centerX"), 0.5);
        mapValues.insert(QStringLiteral("centerY"), 0.5);
        mapValues.insert(QStringLiteral("zoom"), 12);
        mapSettings->restorePreferences(mapValues);
        bool offered = false;
        QTimer::singleShot(0, &window, [&] {
            auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            QVERIFY(popup);
            auto *entry = popup->findChild<QAction *>(QStringLiteral("traceMarker"));
            QVERIFY(entry);
            offered = true;
            entry->trigger();
            popup->close();
        });
        const auto center = map->rect().center();
        QContextMenuEvent context(QContextMenuEvent::Mouse, center, map->mapToGlobal(center));
        QApplication::sendEvent(map, &context);
        QVERIFY(offered);
        QMenu menu;
        trace->populateContextMenu(item, menu);
        auto *action = menu.findChild<QAction *>(QStringLiteral("traceMarker"));
        QVERIFY(action);
        QVERIFY(action->isChecked());
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 1);
        QList<swak::Event> batch;
        for (int i = 1; i <= 52; ++i) {
            event.longitude = i * 0.0025;
            event.time = event.time.addMSecs(100);
            batch.append(event);
        }
        window.store().acceptBatch(batch);
        const swak::Viewport view{QSizeF(600, 200), swak::project(0, 0.06), 12};
        QImage image(600, 200, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            trace->paint(painter, view);
        }
        const auto older = view.screen(0, 0.00875).toPoint();
        const auto newer = view.screen(0, 0.12875).toPoint();
        QVERIFY(image.pixelColor(newer).alpha() > image.pixelColor(older).alpha());
        QVERIFY(image.pixelColor(newer).alpha() > 0);
        QCOMPARE(image.pixelColor(view.screen(0, 0.00375).toPoint()).alpha(), 0);
        QVERIFY(image.save(QStringLiteral("marker-trace.png")));
        action->trigger();
        QCOMPARE(settings->savePreferences().value(QStringLiteral("targets")).toArray().size(), 0);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            trace->paint(painter, view);
        }
        QCOMPARE(image.pixelColor(newer).alpha(), 0);
    }
    void emulatorMap()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("emulator-ui.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        for (auto *dock : window.findChildren<QDockWidget *>()) {
            dock->hide();
        }
        auto *participant = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(participant);
        auto settings = participant->savePreferences();
        const auto center = swak::project(59.33, 18.0);
        settings.insert(QStringLiteral("zoom"), 11);
        settings.insert(QStringLiteral("centerX"), center.x());
        settings.insert(QStringLiteral("centerY"), center.y());
        participant->restorePreferences(settings);
        const auto before = map->grabFramebuffer();
        QTcpServer reserve;
        QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
        const auto port = reserve.serverPort();
        reserve.close();
        struct Child final : QProcess
        {
            ~Child() override
            {
                if (state() != NotRunning) {
                    terminate();
                    if (!waitForFinished(2000)) {
                        kill();
                        waitForFinished(2000);
                    }
                }
            }
        } process;
        process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/swak_cot_emulator"),
                      {QStringLiteral("--port"),
                       QString::number(port),
                       QStringLiteral("--frequency"),
                       QStringLiteral("5"),
                       QStringLiteral("--duration"),
                       QStringLiteral("10")});
        QVERIFY(process.waitForStarted());
        auto source = swak::serverDefaults();
        source.insert(QStringLiteral("name"), QStringLiteral("Emulator"));
        source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        source.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        source.insert(QStringLiteral("port"), port);
        source.insert(QStringLiteral("enabled"), true);
        source.insert(QStringLiteral("retrySeconds"), 1);
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(servers->put(QStringLiteral("emulator"), source).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return window.store().rowCount() == 22;
            },
            5000));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        const auto image = map->grabFramebuffer();
        QVERIFY(image != before);
        QVERIFY(image.save(QStringLiteral("emulator-map.png")));
        int red = 0, blue = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.red() > 180 && color.red() > color.blue() + 80) {
                    ++red;
                }
                if (color.blue() > 180 && color.blue() > color.red() + 80) {
                    ++blue;
                }
            }
        }
        QVERIFY(red > 20);
        QVERIFY(blue > 20);
        process.terminate();
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitCode(), 0);
    }
    void emergencyAlerts()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("emergency.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *button = window.findChild<QToolButton *>(QStringLiteral("emergencyButton"));
        auto *kind = window.findChild<QComboBox *>(QStringLiteral("emergencyKind"));
        auto *send = window.findChild<QPushButton *>(QStringLiteral("sendEmergency"));
        auto *cancel = window.findChild<QPushButton *>(QStringLiteral("cancelEmergency"));
        QVERIFY(button && kind && send && cancel);
        QCOMPARE(button->toolTip(), QStringLiteral("No active emergencies"));
        QVERIFY(!cancel->isEnabled());
        QCOMPARE(kind->count(), 4);
        auto &store = window.store();
        // Escape keeps the alert unsent.
        answerAction(Qt::Key_Escape);
        send->click();
        QCOMPARE(store.rowCount(), 0);
        kind->setCurrentText(QStringLiteral("Ring The Bell"));
        answerAction();
        send->click();
        QCOMPARE(store.rowCount(), 1);
        QVERIFY(swak::activeEmergency(store.events().first()));
        QCOMPARE(store.events().first().type, QStringLiteral("b-a-o-pan"));
        QVERIFY(cancel->isEnabled());
        QVERIFY(button->toolTip().contains(QStringLiteral("Ring The Bell")));
        QCOMPARE(button->property("activeEmergencies").toInt(), 1);
        QVERIFY(!button->styleSheet().isEmpty());
        // The Events log has one entry, even when TAK Server repeats the alert.
        auto repeat = store.events().first();
        repeat.time = repeat.time.addSecs(1);
        QVERIFY(store.accept(repeat).isEmpty());
        auto *activity = window.findChild<swak::ActivityModel *>();
        const auto alertEntries = [activity] {
            int entries = 0;
            for (int row = 0; row < activity->rowCount(); ++row) {
                entries += activity->index(row, swak::ActivityModel::MessageColumn)
                               .data()
                               .toString()
                               .startsWith(QStringLiteral("Emergency: Ring The Bell"));
            }
            return entries;
        };
        QVERIFY(QTest::qWaitFor(
            [&] {
                return alertEntries() > 0;
            },
            2000));
        QTest::qWait(300);
        QCOMPARE(alertEntries(), 1);
        // The menu lists the alert and centers the map on it.
        auto *menu = button->menu();
        QMetaObject::invokeMethod(menu, "aboutToShow");
        QCOMPARE(menu->actions().size(), 1);
        menu->actions().first()->trigger();
        QCOMPARE(store.selected(), store.events().first().uid);
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map->grabFramebuffer().save(QStringLiteral("emergency-map.png")));
        cancel->click();
        QCOMPARE(store.rowCount(), 0);
        QVERIFY(!cancel->isEnabled());
        QCOMPARE(button->toolTip(), QStringLiteral("No active emergencies"));
        QVERIFY(button->styleSheet().isEmpty());
    }
    void serverEnrollmentDialog()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("enroll-ui.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(servers && servers->findChild<swak::TakServerApi *>());
        auto *enroll = window.findChild<QAction *>(QStringLiteral("enrollServer"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("enrollStatus"));
        QVERIFY(enroll && status);
        enroll->trigger();
        QCOMPARE(status->text(), QStringLiteral("Select a TLS server entry first."));
        auto *channels = window.findChild<QAction *>(QStringLiteral("serverChannels"));
        QVERIFY(channels && window.findChild<QPushButton *>(QStringLiteral("takServerActions")));
        channels->trigger();
        QVERIFY(status->text().startsWith(QStringLiteral("Select a TLS or QUIC entry")));
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("authUser"), QStringLiteral("alice"));
        entry.insert(QStringLiteral("authPasswordEnv"), QStringLiteral("SWAK_COT_UI_TEST"));
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        auto *list = window.findChild<QTableWidget *>(QStringLiteral("serverList"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return list->rowCount() == 1;
            },
            2000));
        list->setCurrentCell(0, 1);
        bool checked = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QStringLiteral("enrollDialog"));
            QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("enrollUser"))->text(),
                     QStringLiteral("alice"));
            QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("enrollPassword"))->echoMode(),
                     QLineEdit::Password);
            QCOMPARE(dialog->findChild<QSpinBox *>(QStringLiteral("enrollPort"))->value(), 8446);
            auto *keyFile = dialog->findChild<QLineEdit *>(QStringLiteral("enrollKeyFile"));
            QVERIFY(!keyFile->isEnabled());
            dialog->findChild<QComboBox *>(QStringLiteral("enrollKeyChoice"))->setCurrentIndex(1);
            QVERIFY(keyFile->isEnabled());
            checked = true;
            dialog->reject();
        });
        const auto before = status->text();
        enroll->trigger();
        QVERIFY(checked);
        QCOMPARE(status->text(), before);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->accept();
        });
        enroll->trigger();
        QCOMPARE(status->text(), QStringLiteral("Enter the password of the TAK account."));
        QVERIFY(enroll->isEnabled());
    }
    void serverChannelsDialog()
    {
        // Channels ... reads the groups of the user, and applies the checked channels.
        HttpsHarness api;
        QVERIFY(api.load());
        QByteArray groups =
            "{\"data\":[{\"name\":\"Cyan\",\"direction\":\"IN\",\"bitpos\":2,\"active\":true},"
            "{\"name\":\"Cyan\",\"direction\":\"OUT\",\"bitpos\":2,\"active\":true},"
            "{\"name\":\"Ops\",\"direction\":\"IN\",\"bitpos\":3,\"active\":false,"
            "\"description\":\"Operations\"}]}";
        int status = 200;
        api.respond = [&](const QByteArray &request) {
            if (request.startsWith("GET /Marti/api/groups/all?useCache=true ")) {
                return httpReply(status, groups);
            }
            return httpReply(status, {});
        };
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("channels.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *servers = window.findChild<swak::Servers *>();
        auto *channels = window.findChild<QAction *>(QStringLiteral("serverChannels"));
        auto *label = window.findChild<QLabel *>(QStringLiteral("enrollStatus"));
        auto *list = window.findChild<QTableWidget *>(QStringLiteral("serverList"));
        QVERIFY(servers && channels && label && list);
        auto entry = swak::serverDefaults();
        entry.insert(QStringLiteral("name"), QStringLiteral("TAK"));
        entry.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        entry.insert(QStringLiteral("port"), 9);
        entry.insert(QStringLiteral("protocol"), QStringLiteral("tls"));
        entry.insert(QStringLiteral("enabled"), false);
        entry.insert(QStringLiteral("ca"), QFINDTESTDATA("fixtures/map-test-cert.pem"));
        entry.insert(QStringLiteral("apiPort"), api.serverPort());
        QVERIFY(servers->put(QStringLiteral("tak"), entry).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return list->rowCount() == 1;
            },
            2000));
        list->setCurrentCell(0, 1);
        // The dialog shows each channel once, with its state and description.
        const auto choose = [&](bool accept) {
            auto shown = std::make_shared<QStringList>();
            auto *timer = new QTimer(&window);
            QObject::connect(timer, &QTimer::timeout, timer, [&window, timer, shown, accept] {
                auto *dialog = visibleDialog(window, QStringLiteral("channelDialog"));
                if (!dialog) {
                    return;
                }
                timer->deleteLater();
                auto *channelList = dialog->findChild<QListWidget *>(QStringLiteral("channelList"));
                for (int row = 0; row < channelList->count(); ++row) {
                    auto *item = channelList->item(row);
                    shown->append(item->text() +
                                  (item->checkState() == Qt::Checked ? QStringLiteral("+")
                                                                     : QStringLiteral("-")) +
                                  item->toolTip());
                    item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked
                                                                          : Qt::Checked);
                }
                accept ? dialog->accept() : dialog->reject();
            });
            timer->start(20);
            channels->trigger();
            return shown;
        };
        const auto sent = api.requests.size();
        auto shown = choose(false);
        QCOMPARE(label->text(), QStringLiteral("Reading the channels of the server…"));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return !shown->isEmpty();
            },
            15000));
        QCOMPARE(*shown, (QStringList{QStringLiteral("Cyan+"), QStringLiteral("Ops-Operations")}));
        QTest::qWait(100);
        QCOMPARE(api.requests.size(), sent + 1);
        choose(true);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return label->text() == QStringLiteral("The active channels are updated.");
            },
            15000));
        const auto put = api.requests.last();
        QVERIFY(put.startsWith("PUT /Marti/api/groups/active?"));
        const auto body = QJsonDocument::fromJson(put.mid(put.indexOf("\r\n\r\n") + 4)).array();
        QCOMPARE(body.size(), 3);
        QVERIFY(!body.at(0).toObject().value(QStringLiteral("active")).toBool());
        QVERIFY(body.at(2).toObject().value(QStringLiteral("active")).toBool());
        // A user without channels and a refused read show the reason.
        groups = "{\"data\":[]}";
        channels->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return label->text() == QStringLiteral("The server has no channels for this user.");
            },
            15000));
        status = 403;
        channels->trigger();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return label->text().contains(QStringLiteral("does not allow"));
            },
            15000));
    }
    void serverBrowserTable()
    {
        // The table names each protocol. Its check box enables an entry, and a double click
        // opens the editor.
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("server-table.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *servers = window.findChild<swak::Servers *>();
        auto *list = window.findChild<QTableWidget *>(QStringLiteral("serverList"));
        QVERIFY(servers && list);
        auto output = swak::serverDefaults();
        output.insert(QStringLiteral("name"), QStringLiteral("A output"));
        output.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        output.insert(QStringLiteral("port"), 4242);
        output.insert(QStringLiteral("protocol"), QStringLiteral("udpout"));
        output.insert(QStringLiteral("enabled"), false);
        QVERIFY(servers->put(QStringLiteral("out"), output).isEmpty());
        auto mesh = output;
        mesh.insert(QStringLiteral("name"), QStringLiteral("B mesh"));
        mesh.insert(QStringLiteral("host"), QStringLiteral("239.2.3.1"));
        mesh.insert(QStringLiteral("port"), 6969);
        mesh.insert(QStringLiteral("protocol"), QStringLiteral("mcast"));
        mesh.insert(QStringLiteral("localAddress"), QStringLiteral("127.0.0.1"));
        const auto meshError = servers->put(QStringLiteral("mesh"), mesh);
        QVERIFY2(meshError.isEmpty(), qPrintable(meshError));
        auto input = output;
        input.insert(QStringLiteral("name"), QStringLiteral("C input"));
        input.insert(QStringLiteral("protocol"), QStringLiteral("udp"));
        QVERIFY(servers->put(QStringLiteral("in"), input).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return list->rowCount() == 3;
            },
            2000));
        QCOMPARE(list->item(0, 2)->text(), QStringLiteral("UDP output"));
        QCOMPARE(list->item(1, 2)->text(), QStringLiteral("Multicast mesh"));
        QCOMPARE(list->item(2, 2)->text(), QStringLiteral("UDP"));
        QCOMPARE(list->item(0, 1)->toolTip(), QStringLiteral("127.0.0.1:4242"));
        // The check box saves the entry with the new state.
        auto *active = window.findChild<QAbstractButton *>(QStringLiteral("activeProvider/out"));
        QVERIFY(active && !active->isChecked());
        active->click();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers->sources()
                    .value(QStringLiteral("out"))
                    .toObject()
                    .value(QStringLiteral("enabled"))
                    .toBool();
            },
            2000));
        // A double click on the name opens the editor. A double click on the check box does not.
        bool edited = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = visibleDialog(window, QStringLiteral("serverEditor"));
            edited = dialog != nullptr;
            if (dialog) {
                dialog->reject();
            }
        });
        emit list->cellDoubleClicked(1, 1);
        QVERIFY(edited);
        emit list->cellDoubleClicked(1, 0);
        QVERIFY(!visibleDialog(window, QStringLiteral("serverEditor")));
        // An entry removed before the check box saves it stays removed.
        auto *inputActive =
            window.findChild<QAbstractButton *>(QStringLiteral("activeProvider/in"));
        QVERIFY(inputActive);
        inputActive->click();
        servers->remove(QStringLiteral("in"));
        QTest::qWait(50);
        QVERIFY(!servers->sources().contains(QStringLiteral("in")));
        // Add stops at 16 entries.
        for (auto index = servers->sources().size(); index < 16; ++index) {
            auto extra = output;
            extra.insert(QStringLiteral("name"), QStringLiteral("Extra %1").arg(index));
            QVERIFY(servers->put(QStringLiteral("extra%1").arg(index), extra).isEmpty());
        }
        window.findChild<QPushButton *>(QStringLiteral("addServer"))->click();
        QCOMPARE(window.findChild<QPlainTextEdit *>(QStringLiteral("serverStatus"))->toPlainText(),
                 QStringLiteral("The server limit is 16."));
        QVERIFY(!visibleDialog(window, QStringLiteral("serverEditor")));
    }
    void serverBrowserAndIdentity()
    {
        QTemporaryDir directory;
        swak::Window window(directory.filePath(QStringLiteral("servers-ui.json")),
                            QPluginLoader::staticInstances());
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *servers = window.findChild<swak::Servers *>();
        QVERIFY(servers);
        auto *list = window.findChild<QTableWidget *>(QStringLiteral("serverList"));
        QVERIFY(list);
        QCOMPARE(list->rowCount(), 0);
        auto *details = window.findChild<QPlainTextEdit *>(QStringLiteral("serverStatus"));
        QVERIFY(details);
        QVERIFY(details->isReadOnly());
        bool emptyAdd = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("serverEditor"));
            QVERIFY(dialog);
            for (auto *field : dialog->findChildren<QLineEdit *>()) {
                if (field->objectName().endsWith(QStringLiteral("/name"))) {
                    emptyAdd = field->text().isEmpty();
                }
            }
            dialog->reject();
        });
        window.findChild<QPushButton *>(QStringLiteral("addServer"))->click();
        QVERIFY(emptyAdd);
        QCOMPARE(list->rowCount(), 0);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("identityDialog"));
            QVERIFY(dialog);
            auto *uid = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/uid"));
            QVERIFY(uid);
            QVERIFY(uid->isReadOnly());
            QVERIFY(!uid->text().isEmpty());
            const auto generated = uid->text();
            QTest::keyClicks(uid, "blocked");
            QCOMPARE(uid->text(), generated);
            auto *lock =
                dialog->findChild<QToolButton *>(QStringLiteral("org.swak.identity/uid/lock"));
            QVERIFY(lock);
            QVERIFY(!lock->icon().isNull());
            const auto lockedIcon = lock->icon().pixmap(24, 24).toImage();
            QCOMPARE(lockedIcon, swak::uiIcon(swak::UiIcon::Unlock).pixmap(24, 24).toImage());
            const auto muted = uid->palette().color(QPalette::Text);
            QVERIFY(lock->toolTip().contains(QStringLiteral("Unlock")));
            lock->click();
            QVERIFY(!uid->isReadOnly());
            QCOMPARE(lock->icon().pixmap(24, 24).toImage(),
                     swak::uiIcon(swak::UiIcon::Lock).pixmap(24, 24).toImage());
            QVERIFY(lock->toolTip().contains(QStringLiteral("Lock")));
            QVERIFY(uid->palette().color(QPalette::Text) != muted);
            uid->setText(QStringLiteral("sender-test"));
            lock->click();
            QVERIFY(uid->isReadOnly());
            for (const auto &path : {QStringLiteral("type"),
                                     QStringLiteral("how"),
                                     QStringLiteral("team"),
                                     QStringLiteral("role")}) {
                auto *field =
                    dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/") + path);
                QVERIFY(field);
                QVERIFY(field->text().contains('('));
                QVERIFY(!swak::cotFieldValue(field).contains('('));
            }
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(buttons->button(QDialogButtonBox::Ok)->icon().pixmap(24, 24).toImage() !=
                    buttons->button(QDialogButtonBox::Apply)->icon().pixmap(24, 24).toImage());
            QVERIFY(dialog->grab().save(QStringLiteral("identity-locked.png")));
            dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/callsign"))
                ->setText(QStringLiteral("Test Sender"));
            dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/team"))
                ->setText(QStringLiteral("Blue"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        window.findChild<QAction *>(QStringLiteral("identityAction"))->trigger();
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("identityDialog"));
            QVERIFY(dialog);
            auto *uid = dialog->findChild<QLineEdit *>(QStringLiteral("org.swak.identity/uid"));
            QVERIFY(uid->isReadOnly());
            QCOMPARE(uid->text(), QStringLiteral("sender-test"));
            dialog->reject();
        });
        window.findChild<QAction *>(QStringLiteral("identityAction"))->trigger();
        QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("identity"))->text(),
                 QStringLiteral("sender-test"));
        QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(),
                 QStringLiteral("Test Sender"));
        QTcpServer harness;
        QVERIFY(harness.listen(QHostAddress::LocalHost, 0));
        auto source = swak::serverDefaults();
        source.insert(QStringLiteral("name"), QStringLiteral("Harness"));
        source.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1"));
        source.insert(QStringLiteral("protocol"), QStringLiteral("stcp"));
        source.insert(QStringLiteral("port"), harness.serverPort());
        source.insert(QStringLiteral("enabled"), true);
        QVERIFY(servers->put(QStringLiteral("harness"), source).isEmpty());
        QCOMPARE(list->rowCount(), 1);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return harness.hasPendingConnections();
            },
            2000));
        auto *socket = harness.nextPendingConnection();
        auto *map = window.findChild<QOpenGLWidget *>();
        QVERIFY(map);
        for (auto *dock : window.findChildren<QDockWidget *>()) {
            dock->hide();
        }
        auto *mapSettings = dynamic_cast<swak::SettingsParticipant *>(map);
        auto settings = mapSettings->savePreferences();
        settings.insert(QStringLiteral("zoom"), 8);
        settings.insert(QStringLiteral("centerX"), 0.5);
        settings.insert(QStringLiteral("centerY"), 0.5);
        mapSettings->restorePreferences(settings);
        swak::Event event;
        event.uid = QStringLiteral("remote");
        event.type = QStringLiteral("a-f-G-U");
        event.how = QStringLiteral("m-g");
        event.callsign = QStringLiteral("Received contact");
        event.time = event.start = QDateTime::currentDateTimeUtc();
        event.stale = event.time.addSecs(120);
        const auto before = map->grabFramebuffer();
        socket->write(swak::encodeCot(event));
        QVERIFY(QTest::qWaitFor(
            [&] {
                return window.store().rowCount() == 1;
            },
            2000));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        const auto after = map->grabFramebuffer();
        QVERIFY(after != before);
        QVERIFY(after.save(QStringLiteral("server-cot-map.png")));
        int markerPixels = 0;
        const auto imageCenter = after.rect().center();
        for (int y = imageCenter.y() - 12; y <= imageCenter.y() + 12; ++y) {
            for (int x = imageCenter.x() - 12; x <= imageCenter.x() + 12; ++x) {
                const auto color = after.pixelColor(x, y);
                if (color.blue() > 120 && color.blue() > color.red()) {
                    ++markerPixels;
                }
            }
        }
        QVERIFY2(markerPixels >= 5, "The received CoT symbol is absent from the OpenGL frame.");
        const auto center = map->rect().center();
        QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, center);
        auto *selection = window.findChild<swak::MapSelection *>();
        QVERIFY(selection->item());
        QCOMPARE(selection->item()->id, event.uid);
        QVERIFY(QTest::qWaitFor(
            [&] {
                return servers->status(QStringLiteral("harness"))
                    .contains(QStringLiteral("Connected"));
            },
            2000));
        window.findChild<QCheckBox *>(QStringLiteral("sendGenerated"))->setChecked(true);
        window.findChild<QPushButton *>(QStringLiteral("generateEvent"))->click();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return socket->bytesAvailable() > 0;
            },
            2000));
        const auto sent = swak::decodeCot(socket->readAll());
        QVERIFY(sent.event);
        QCOMPARE(sent.event->uid, QStringLiteral("sender-test"));
        QCOMPARE(sent.event->team, QStringLiteral("Blue"));
        auto *browser = window.findChild<QWidget *>(QStringLiteral("serverBrowser"));
        QVERIFY(browser->grab().save(QStringLiteral("server-browser.png")));
        list->setCurrentCell(0, 1);
        answerDeletion(Qt::Key_Escape);
        window.findChild<QPushButton *>(QStringLiteral("removeServer"))->click();
        QCOMPARE(list->rowCount(), 1);
        answerDeletion();
        window.findChild<QPushButton *>(QStringLiteral("removeServer"))->click();
        QCOMPARE(list->rowCount(), 0);
        QTcpServer unused;
        QVERIFY(unused.listen(QHostAddress::LocalHost, 0));
        const auto unusedPort = unused.serverPort();
        unused.close();
        source.insert(QStringLiteral("port"), unusedPort);
        QVERIFY(servers->put(QStringLiteral("missing"), source).isEmpty());
        QVERIFY(QTest::qWaitFor(
            [&] {
                return details->toPlainText().contains(QStringLiteral("Connection refused"));
            },
            3000));
        QVERIFY(details->toPlainText().contains(QString::number(unusedPort)));
        details->selectAll();
        details->copy();
        QCOMPARE(QApplication::clipboard()->text(), details->toPlainText());
        QVERIFY(browser->grab().save(QStringLiteral("server-connection-error.png")));
        QVERIFY(window.savePreferences());
    }
    void receiverRestarts()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("servers.json"));
        QUdpSocket reservation;
        QVERIFY(reservation.bind(QHostAddress::LocalHost, 0));
        const auto port = reservation.localPort();
        reservation.close();
        for (int cycle = 0; cycle < 2; ++cycle) {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *servers = window.findChild<swak::Servers *>();
            QVERIFY(servers);
            if (!cycle) {
                auto savedServer = swak::serverDefaults();
                savedServer.insert("name", "Saved UDP");
                savedServer.insert("protocol", "udp");
                savedServer.insert("host", "127.0.0.1");
                savedServer.insert("port", port);
                savedServer.insert("enabled", true);
                QVERIFY(servers->put("saved-udp", savedServer).isEmpty());
            }
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return servers->status(QStringLiteral("saved-udp"))
                        .contains(QStringLiteral("Listening"));
                },
                5000));
            QVERIFY(!reservation.bind(QHostAddress::LocalHost, port));
            QVERIFY(window.savePreferences());
        }
        QVERIFY(reservation.bind(QHostAddress::LocalHost, port));
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *servers = window.findChild<swak::Servers *>();
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return servers->status(QStringLiteral("saved-udp"))
                        .contains(QStringLiteral("Cannot bind"));
                },
                5000));
            QVERIFY(window.savePreferences());
        }
        reservation.close();
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            auto *servers = window.findChild<swak::Servers *>();
            QVERIFY(QTest::qWaitFor(
                [&] {
                    return servers->status(QStringLiteral("saved-udp"))
                        .contains(QStringLiteral("Listening"));
                },
                5000));
            servers->remove(QStringLiteral("saved-udp"));
            QVERIFY(window.savePreferences());
        }
        {
            swak::Window window(path, QPluginLoader::staticInstances());
            QVERIFY(window.findChild<swak::Servers *>()->sources().isEmpty());
        }
    }
    void invalidPreferenceValues()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("preferences.json"));
        {
            swak::Preferences preferences(path);
            QVERIFY(preferences.save(
                {{QStringLiteral("version"), 1},
                 {QStringLiteral("window"),
                  QJsonObject{{QStringLiteral("geometry"), QStringLiteral("broken")},
                              {QStringLiteral("layout"), QStringLiteral("YnJva2Vu")}}},
                 {QStringLiteral("components"),
                  QJsonObject{{QStringLiteral("org.swak.map"),
                               QJsonObject{{QStringLiteral("zoom"), 10000},
                                           {QStringLiteral("centerX"), -3}}},
                              {QStringLiteral("org.swak.udp"),
                               QJsonObject{{QStringLiteral("port"), 99999}}},
                              {QStringLiteral("org.swak.editor"),
                               QJsonObject{{QStringLiteral("latitude"), 999},
                                           {QStringLiteral("lifetime"), -1}}}}}}));
        }
        swak::Window window(path, QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.findChild<QDoubleSpinBox *>(QStringLiteral("latitude"))->value(), 0.0);
        QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("lifetime"))->value(), 300);
        QVERIFY(window.findChild<swak::Servers *>()->sources().isEmpty());
        const auto values = dynamic_cast<swak::PreferenceParticipant *>(
                                window.findChild<QOpenGLWidget *>(QStringLiteral("map")))
                                ->savePreferences();
        QCOMPARE(values.value(QStringLiteral("zoom")).toDouble(),
                 std::ceil(std::log2(std::max(window.findChild<QOpenGLWidget *>()->width(),
                                              window.findChild<QOpenGLWidget *>()->height()) /
                                     256.0)));
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
            window.findChild<QLineEdit *>(QStringLiteral("callsign"))
                ->setText(QStringLiteral("Autosaved"));
            QVERIFY(QTest::qWaitFor(
                [&path] {
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly)) {
                        return false;
                    }
                    const auto json = QJsonDocument::fromJson(file.readAll()).object();
                    return json.value(QStringLiteral("components"))
                               .toObject()
                               .value(QStringLiteral("org.swak.editor"))
                               .toObject()
                               .value(QStringLiteral("callsign"))
                               .toString() == QStringLiteral("Autosaved");
                },
                2500));
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
            answerAction();
            window.findChild<QAction *>(QStringLiteral("resetLayoutAction"))->trigger();
            QVERIFY(!plugins->isFloating());
            QCOMPARE(window.findChild<QLineEdit *>(QStringLiteral("callsign"))->text(),
                     QStringLiteral("Autosaved"));
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
            QVERIFY(
                !window.findChild<QLabel *>(QStringLiteral("preferencesStatus"))->text().isEmpty());
            window.close();
        }
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), invalid);
    }
    void independentPanes()
    {
        QTemporaryDir preferences;
        swak::Window window(preferences.filePath(QStringLiteral("preferences.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto docks = window.findChildren<QDockWidget *>();
        QCOMPARE(docks.size(), 21);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("toolsDock")));
        auto *viewMenu = window.findChild<QMenu *>(QStringLiteral("viewMenu"));
        auto *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
        auto *quit = window.findChild<QAction *>(QStringLiteral("quitAction"));
        QVERIFY(viewMenu && fileMenu && quit);
        QCOMPARE(viewMenu->actions().size(), 6);
        QVERIFY(viewMenu->actions().contains(
            window.findChild<QToolBar *>(QStringLiteral("mainToolbar"))->toggleViewAction()));
        for (int repeat = 0; repeat < 5; ++repeat) {
            swak::sortMenuActions(*fileMenu);
            const auto actions = fileMenu->actions();
            QCOMPARE(actions.last(), quit);
            QVERIFY(actions.at(actions.size() - 2)->isSeparator());
            QCOMPARE(std::count_if(actions.begin(),
                                   actions.end(),
                                   [](const QAction *action) {
                                       return action->isSeparator();
                                   }),
                     1);
        }
        auto *lateAction = fileMenu->addAction(QStringLiteral("Zebra"));
        QCOMPARE(fileMenu->actions().last(), quit);
        QVERIFY(fileMenu->actions().at(fileMenu->actions().size() - 2)->isSeparator());
        lateAction->setText(QStringLiteral("Aardvark"));
        QCOMPARE(fileMenu->actions().first(), lateAction);
        QCOMPARE(fileMenu->actions().last(), quit);
        delete lateAction;
        QCOMPARE(window.menuBar()->actions().first()->text(), QStringLiteral("&File"));
        QCOMPARE(quit->shortcut(), QKeySequence(QKeySequence::Quit));
        for (const auto *menu : window.findChildren<QMenu *>()) {
            if (menu != fileMenu) {
                QVERIFY(!menu->actions().contains(quit));
            }
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
        swak::Window window(preferences.filePath(QStringLiteral("preferences.json")),
                            QPluginLoader::staticInstances());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *map = window.findChild<QOpenGLWidget *>(QStringLiteral("map"));
        QVERIFY(map);
        QVERIFY(map->isValid());
        map->makeCurrent();
        qInfo() << "OpenGL renderer:"
                << reinterpret_cast<const char *>(
                       map->context()->functions()->glGetString(GL_RENDERER));
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
        QTest::keyClick(map, Qt::Key_D);
        QTest::keyClick(map, Qt::Key_Home);
        auto *mapConfiguration = dynamic_cast<swak::SettingsParticipant *>(map);
        QVERIFY(mapConfiguration);
        auto *grids = window.findChild<swak::GridTypes *>();
        const auto previousGrid = grids->selected();
        grids->select({});
        const QImage frame = map->grabFramebuffer();
        QVERIFY(!frame.isNull());
        QCOMPARE(frame.pixelColor(20, 21), QColor(swak::MapBackgroundRgb));
        grids->select(previousGrid);
        QVERIFY(frame.save(QStringLiteral("map-smoke.png")));
        QSignalSpy frames(map, &QOpenGLWidget::frameSwapped);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(
            window.screen()
                ->grabWindow(window.winId())
                .save(light ? QStringLiteral("gui-light.png") : QStringLiteral("gui-smoke.png")));
        window.resize(950, 700);
        map->update();
        QVERIFY(frames.wait(2000));
        QVERIFY(map->width() >= 320);
        answerAction();
        window.findChild<QAction *>(QStringLiteral("clearEventsAction"))->trigger();
        QCOMPARE(window.store().rowCount(), 0);
        window.close();
    }
};
int main(int argc, char **argv)
{
    swak::configureGraphics();
    // The Qt FFmpeg backend probes video hardware through libdrm, which leaks 79 bytes once
    // per process. The test display has no video hardware, so turn off the decode and encode
    // probes and keep the leak check focused on SWAK. The application keeps the Qt default.
    qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "");
    qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", "");
    QApplication app(argc, argv);
    // Close cached GDAL resources after the tests and before the leak check.
    swak::GdalShutdownGuard gdal;
    QTemporaryDir mapCache;
    qputenv("XDG_CACHE_HOME", mapCache.path().toUtf8());
    GuiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_gui.moc"
