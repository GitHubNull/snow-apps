// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QtCore/qdatastream.h>
#include <QtCore/qmetatype.h>
#include <QtCore/qtextstream.h>
#include <QThread>

#include "snow_shot/platform/linux/x11windowquery.h"
#include "snow_shot/platform/linux/x11connection.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace snow_shot::platform::x11 {
namespace {

constexpr int kMaxFrameTreeDepth = 64;
constexpr int kPollIntervalMs = 10;
constexpr int kSettleDelayMs = 40;

std::optional<WindowId> rootActiveWindow(Display* display, Window root) {
    const Atom activeWindowAtom = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    if (activeWindowAtom == None) {
        return std::nullopt;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, root, activeWindowAtom, 0, 1, False, XA_WINDOW, &actualType,
                           &actualFormat, &itemCount, &bytesAfter, &data) != Success) {
        return std::nullopt;
    }
    std::optional<WindowId> active;
    if (actualType == XA_WINDOW && actualFormat == 32 && itemCount >= 1 && data != nullptr) {
        const Window window = *reinterpret_cast<const Window*>(data);
        if (window != None) {
            active = static_cast<WindowId>(window);
        }
    }
    if (data != nullptr) {
        XFree(data);
    }
    return active;
}

} // namespace

std::optional<WindowId> activeWindowId() {
    X11Connection connection;
    if (!connection.isValid()) {
        return std::nullopt;
    }
    Display* display = connection.display();
    for (int screen = 0; screen < ScreenCount(display); ++screen) {
        if (const auto active = rootActiveWindow(display, RootWindow(display, screen))) {
            return active;
        }
    }
    return std::nullopt;
}

std::optional<WindowId> frameWindowIdFor(WindowId windowId) {
    X11Connection connection;
    if (!connection.isValid() || windowId == 0) {
        return std::nullopt;
    }
    Display* display = connection.display();
    Window current = static_cast<Window>(windowId);
    for (int depth = 0; depth < kMaxFrameTreeDepth; ++depth) {
        Window rootReturn = None;
        Window parentReturn = None;
        Window* children = nullptr;
        unsigned int childCount = 0;
        if (!XQueryTree(display, current, &rootReturn, &parentReturn, &children, &childCount)) {
            return std::nullopt;
        }
        if (children != nullptr) {
            XFree(children);
        }
        if (parentReturn == None) {
            // The window itself is a root; there is no frame around it.
            return std::nullopt;
        }
        if (parentReturn == rootReturn) {
            return static_cast<WindowId>(current);
        }
        current = parentReturn;
    }
    return std::nullopt;
}

bool windowsAreUnviewable(const QVector<WindowId>& windowIds) {
    X11Connection connection;
    if (!connection.isValid()) {
        return true;
    }
    Display* display = connection.display();
    for (const WindowId windowId : windowIds) {
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, static_cast<Window>(windowId), &attributes)) {
            continue;
        }
        if (attributes.map_state == IsViewable) {
            return false;
        }
    }
    return true;
}

bool waitForWindowsUnviewable(const QVector<WindowId>& windowIds, int timeoutMs) {
    int waited = 0;
    while (true) {
        if (windowsAreUnviewable(windowIds)) {
            // Give the server and compositor one frame to stop rendering the
            // windows before reporting success.
            QThread::msleep(kSettleDelayMs);
            if (windowsAreUnviewable(windowIds)) {
                return true;
            }
        }
        if (waited >= timeoutMs) {
            return windowsAreUnviewable(windowIds);
        }
        QThread::msleep(kPollIntervalMs);
        waited += kPollIntervalMs;
    }
}

} // namespace snow_shot::platform::x11
