#include "app/window.h"
#include "core/logging.h"
#include "core/plugin_catalog.h"
#include "swak/version.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPluginLoader>
#include <QSurfaceFormat>

#include <exception>
#include <stdexcept>

int main(int argc, char **argv)
{
    swak::installLogHandler();
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("swak"));
    QCoreApplication::setOrganizationName(QStringLiteral("SWAK"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SWAK_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.swak.Swak"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "A native Cursor-on-Target workspace."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption preferencesOption(
        QStringLiteral("preferences"),
        QCoreApplication::translate("main", "Use this portable preferences file."),
        QStringLiteral("path"));
    parser.addOption(preferencesOption);
    parser.process(app);
    const QString preferencesPath = parser.isSet(preferencesOption)
                                        ? parser.value(preferencesOption)
                                        : swak::Preferences::defaultPath();
    swak::configureLoggingFromFile(preferencesPath);
    try {
        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        QOpenGLContext probe;
        probe.setFormat(format);
        if (!probe.create() || !probe.makeCurrent(&surface)) {
            throw std::runtime_error("OpenGL 3.3 is unavailable. Install a compatible graphics "
                                     "driver and use a desktop session.");
        }
        probe.doneCurrent();
        swak::Window window(preferencesPath, swak::builtInPlugins());
        window.show();
        return app.exec();
    } catch (const std::exception &error) {
        swak::logMessage(swak::Severity::Error,
                         swak::Urgency::Immediate,
                         QStringLiteral("swak.startup"),
                         QStringLiteral("Application startup failed."));
        QMessageBox box(QMessageBox::Critical,
                        QCoreApplication::translate("main", "Startup failed"),
                        QString::fromUtf8(error.what()),
                        QMessageBox::Close);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return 1;
    }
}
