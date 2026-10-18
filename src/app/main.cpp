#include "app/window.h"
#include "core/gdal_support.h"
#include "core/graphics.h"
#include "core/logging.h"
#include "core/plugin_catalog.h"
#include "core/quit_signals.h"
#include "swak/version.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

#include <exception>
#include <stdexcept>

int main(int argc, char **argv)
{
    swak::installLogHandler();
    swak::configureGraphics();
    const auto format = QSurfaceFormat::defaultFormat();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("swak"));
    QCoreApplication::setOrganizationName(QStringLiteral("SWAK"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SWAK_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.swak.Swak"));
    // SIGTERM and SIGINT close the window through the event loop, so it saves its preferences.
    // Closing all windows also ends an open dialog, which quit alone does not end.
    const swak::QuitSignals quitSignals;
    QObject::connect(&quitSignals, &swak::QuitSignals::requested, &app, [] {
        QApplication::closeAllWindows();
        QCoreApplication::quit();
    });
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main", "Receive, create, and present CoT data."));
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
    if (!quitSignals.active()) {
        swak::logMessage(
            swak::Severity::Warning,
            swak::Urgency::Attention,
            QStringLiteral("swak.startup"),
            QStringLiteral("SIGTERM and SIGINT stop SWAK without saving preferences."));
    }
    // The window and its GDAL workers close first. Then GDAL releases its cached resources.
    const swak::GdalShutdownGuard gdal;
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
