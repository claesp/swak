#include "app/window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QPluginLoader>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <exception>
#include <stdexcept>

int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("swak"));
    QCoreApplication::setOrganizationName(QStringLiteral("SWAK"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.swak.Swak"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate("main", "A native Cursor-on-Target workspace."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption preferencesOption(QStringLiteral("preferences"),
        QCoreApplication::translate("main", "Use this portable preferences file."), QStringLiteral("path"));
    parser.addOption(preferencesOption);
    parser.process(app);
    try {
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        QOpenGLContext probe;
        probe.setFormat(format);
        if (!probe.create() || !probe.makeCurrent(&surface))
            throw std::runtime_error("OpenGL 3.3 is unavailable. Install a compatible graphics driver and use a desktop session.");
        probe.doneCurrent();
        const QString preferencesPath = parser.isSet(preferencesOption) ? parser.value(preferencesOption)
                                                                         : swak::Preferences::defaultPath();
        swak::Window window(preferencesPath, QPluginLoader::staticInstances());
        window.show();
        return app.exec();
    } catch (const std::exception &error) {
        QMessageBox box(QMessageBox::Critical, QCoreApplication::translate("main", "Startup failed"),
                        QString::fromUtf8(error.what()), QMessageBox::Close);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return 1;
    }
}
