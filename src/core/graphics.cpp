#include "core/graphics.h"

#include <QByteArray>
#include <QSurfaceFormat>

namespace swak {
void configureGraphics()
{
    // Prefer EGL to avoid the Mesa GLX startup leak. Qt retains GLX as a fallback.
    if (!qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION")) {
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");
    }
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
}
} // namespace swak
