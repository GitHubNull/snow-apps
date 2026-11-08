#include "snow_shot/platform/linux/x11connection.h"

#include <X11/Xlib.h>

namespace snow_shot::platform::x11 {
namespace {

int ignoreX11Errors(Display*, XErrorEvent*) {
    return 0;
}

} // namespace

void installX11ErrorHandlers() {
    static const bool installed = [] {
        XSetErrorHandler(ignoreX11Errors);
        return true;
    }();
    Q_UNUSED(installed);
}

X11Connection::X11Connection() {
    installX11ErrorHandlers();
    m_display = XOpenDisplay(nullptr);
}

X11Connection::~X11Connection() {
    if (m_display != nullptr) {
        XCloseDisplay(m_display);
    }
}

} // namespace snow_shot::platform::x11
