// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include "snow_shot/platform/linux/windowalwaysontop.h"

#include "snow_shot/platform/linux/x11connection.h"
#include "snow_shot/platform/linux/x11windowquery.h"

#include <QDebug>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

namespace snow_shot::platform::linux {
namespace {

std::optional<QPoint> pointerPosition(Display* display, Window root) {
    Window rootReturn = None;
    Window child = None;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(display, root, &rootReturn, &child, &rootX, &rootY, &winX, &winY, &mask))
        return std::nullopt;
    return QPoint(rootX, rootY);
}

std::optional<bool> alwaysOnTopState(Display* display, Window window) {
    const Atom stateAtom = XInternAtom(display, "_NET_WM_STATE", True);
    const Atom aboveAtom = XInternAtom(display, "_NET_WM_STATE_ABOVE", True);
    if (stateAtom == None || aboveAtom == None)
        return std::nullopt;
    Atom actualType = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, stateAtom, 0, 64, False, XA_ATOM, &actualType, &format,
                           &count, &remaining, &data) != Success) {
        return std::nullopt;
    }
    bool above = false;
    if (data != nullptr) {
        for (unsigned long index = 0; index < count; ++index) {
            if (static_cast<Atom>(reinterpret_cast<unsigned long*>(data)[index]) == aboveAtom)
                above = true;
        }
        XFree(data);
    }
    return above;
}

bool requestAlwaysOnTop(Display* display, Window window, bool enabled) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window, &attributes))
        return false;
    const Atom stateAtom = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom aboveAtom = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = stateAtom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = enabled ? 1 : 0;
    event.xclient.data.l[1] = static_cast<long>(aboveAtom);
    event.xclient.data.l[2] = 0;
    // A pager source lets the window manager manage foreign top-level windows.
    event.xclient.data.l[3] = 2;
    event.xclient.data.l[4] = 0;
    XSendEvent(display, attributes.root, False, SubstructureRedirectMask | SubstructureNotifyMask,
               &event);
    XFlush(display);
    return true;
}

} // namespace

WindowAlwaysOnTopResult
toggleWindowUnderCursorAlwaysOnTop(const LocalWindowAlwaysOnTopHandler& localHandler) {
    x11::X11Connection connection;
    if (!connection.isValid())
        return WindowAlwaysOnTopResult::Failed;
    Display* display = connection.display();
    const Window root = DefaultRootWindow(display);
    const auto position = pointerPosition(display, root);
    if (!position)
        return WindowAlwaysOnTopResult::Failed;

    quintptr target = 0;
    for (const auto& entry : x11::stackedTopLevelWindows()) {
        if (!entry.mapped || entry.skip || entry.frameRect.isEmpty())
            continue;
        if (!entry.frameRect.contains(*position))
            continue;
        target = entry.windowId;
        break;
    }
    if (target == 0)
        return WindowAlwaysOnTopResult::Ignored;

    if (localHandler) {
        if (const auto handled = localHandler(static_cast<WId>(target)))
            return *handled ? WindowAlwaysOnTopResult::ChangeRequested
                            : WindowAlwaysOnTopResult::Failed;
    }

    const auto current = alwaysOnTopState(display, static_cast<Window>(target));
    if (!current) {
        qWarning("Could not read _NET_WM_STATE for the window under the cursor");
        return WindowAlwaysOnTopResult::Failed;
    }
    if (!requestAlwaysOnTop(display, static_cast<Window>(target), !*current)) {
        qWarning("Could not toggle Always on Top for the window under the cursor");
        return WindowAlwaysOnTopResult::Failed;
    }
    return WindowAlwaysOnTopResult::ChangeRequested;
}

} // namespace snow_shot::platform::linux
