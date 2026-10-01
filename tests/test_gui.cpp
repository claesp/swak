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
#include <QTabWidget>
#include <QTableView>
#include <QTest>

class GuiTest final : public QObject {
    Q_OBJECT
private slots:
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
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("toolsTabs"));
        QVERIFY(generate && latitude && longitude && callsign && tabs);
        tabs->setCurrentWidget(generate->parentWidget());
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
