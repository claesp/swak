#include "app/window.h"
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

class GuiTest final : public QObject {
    Q_OBJECT
private slots:
    void independentPanes()
    {
        swak::Window window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto docks = window.findChildren<QDockWidget *>(QString{}, Qt::FindDirectChildrenOnly);
        QCOMPARE(docks.size(), 4);
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("toolsDock")));
        auto *viewMenu = window.findChild<QMenu *>(QStringLiteral("viewMenu"));
        auto *fileMenu = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
        auto *quit = window.findChild<QAction *>(QStringLiteral("quitAction"));
        QVERIFY(viewMenu && fileMenu && quit);
        QCOMPARE(viewMenu->actions().size(), 4);
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
                if (dock->objectName() == QStringLiteral("eventsDock")) {
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
        swak::Window window;
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
