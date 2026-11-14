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

#include <algorithm>
#include <array>

namespace snow_shot::platform::x11 {
namespace {

constexpr int kMaxFrameTreeDepth = 64;
constexpr int kPollIntervalMs = 10;
constexpr int kSettleDelayMs = 40;
constexpr long kMaxPropertyItems = 4096;

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

WindowId readFrameWindowId(Display* display, Window window) {
    Window current = window;
    for (int depth = 0; depth < kMaxFrameTreeDepth; ++depth) {
        Window rootReturn = None;
        Window parentReturn = None;
        Window* children = nullptr;
        unsigned int childCount = 0;
        if (!XQueryTree(display, current, &rootReturn, &parentReturn, &children, &childCount)) {
            return None;
        }
        if (children != nullptr) {
            XFree(children);
        }
        if (parentReturn == None) {
            return None;
        }
        if (parentReturn == rootReturn) {
            return current;
        }
        current = parentReturn;
    }
    return None;
}

std::optional<QRect> globalGeometryOnDisplay(Display* display, Window window) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window, &attributes)) {
        return std::nullopt;
    }
    int rootX = 0;
    int rootY = 0;
    Window child = None;
    if (!XTranslateCoordinates(display, window, attributes.root, 0, 0, &rootX, &rootY, &child)) {
        return std::nullopt;
    }
    return QRect(rootX, rootY, static_cast<int>(attributes.width),
                 static_cast<int>(attributes.height));
}

QString windowTitleOnDisplay(Display* display, Window window) {
    const Atom netWmName = XInternAtom(display, "_NET_WM_NAME", True);
    const Atom utf8String = XInternAtom(display, "UTF8_STRING", True);
    const auto read = [&](Atom property, Atom requestedType) -> QString {
        if (property == None)
            return {};
        Atom actualType = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, window, property, 0, kMaxPropertyItems, False,
                               requestedType, &actualType, &format, &count, &remaining,
                               &data) != Success ||
            data == nullptr) {
            return {};
        }
        QString title;
        if (format == 8)
            title = actualType == XA_STRING
                        ? QString::fromLatin1(reinterpret_cast<const char*>(data),
                                              static_cast<qsizetype>(count))
                        : QString::fromUtf8(reinterpret_cast<const char*>(data),
                                            static_cast<qsizetype>(count));
        XFree(data);
        return title;
    };
    QString title = read(netWmName, utf8String != None ? utf8String : AnyPropertyType);
    if (title.isEmpty())
        title = read(XA_WM_NAME, AnyPropertyType);
    return title;
}

quint32 readWindowProcessId(Display* display, Window window) {
    const Atom pidAtom = XInternAtom(display, "_NET_WM_PID", True);
    if (pidAtom == None)
        return 0;
    Atom actualType = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, pidAtom, 0, 1, False, XA_CARDINAL, &actualType, &format,
                           &count, &remaining, &data) != Success ||
        data == nullptr) {
        return 0;
    }
    const quint32 pid = format == 32 && count >= 1
                            ? static_cast<quint32>(*reinterpret_cast<unsigned long*>(data))
                            : 0;
    XFree(data);
    return pid;
}

bool windowTypeIsSkipped(Display* display, Window window) {
    static const char* const skippedTypes[] = {
        "_NET_WM_WINDOW_TYPE_DESKTOP",       "_NET_WM_WINDOW_TYPE_DOCK",
        "_NET_WM_WINDOW_TYPE_TOOLTIP",       "_NET_WM_WINDOW_TYPE_MENU",
        "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", "_NET_WM_WINDOW_TYPE_POPUP_MENU",
        "_NET_WM_WINDOW_TYPE_COMBO",         "_NET_WM_WINDOW_TYPE_DND",
        "_NET_WM_WINDOW_TYPE_NOTIFICATION",  "_NET_WM_WINDOW_TYPE_SPLASH"};
    const Atom typeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", True);
    if (typeAtom == None)
        return false;
    Atom actualType = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, window, typeAtom, 0, kMaxPropertyItems, False, XA_ATOM,
                           &actualType, &format, &count, &remaining, &data) != Success ||
        data == nullptr || format != 32 || count == 0) {
        if (data != nullptr)
            XFree(data);
        return false;
    }
    const Atom normalType = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", True);
    const Atom dialogType = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", True);
    std::array<Atom, std::size(skippedTypes)> skippedAtoms{};
    for (size_t index = 0; index < std::size(skippedTypes); ++index)
        skippedAtoms[index] = XInternAtom(display, skippedTypes[index], True);
    bool skipped = false;
    bool normal = false;
    for (unsigned long index = 0; index < count; ++index) {
        const Atom type = static_cast<Atom>(reinterpret_cast<unsigned long*>(data)[index]);
        if ((normalType != None && type == normalType) ||
            (dialogType != None && type == dialogType)) {
            normal = true;
        }
        if (std::find(skippedAtoms.begin(), skippedAtoms.end(), type) != skippedAtoms.end())
            skipped = true;
    }
    XFree(data);
    return skipped && !normal;
}

QVector<Window> stackingOrderOnDisplay(Display* display, Window root) {
    QVector<Window> order;
    const Atom stackingAtom = XInternAtom(display, "_NET_CLIENT_LIST_STACKING", True);
    if (stackingAtom != None) {
        Atom actualType = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, root, stackingAtom, 0, kMaxPropertyItems, False, XA_WINDOW,
                               &actualType, &format, &count, &remaining, &data) == Success &&
            data != nullptr && format == 32) {
            order.reserve(static_cast<qsizetype>(count));
            for (unsigned long index = 0; index < count; ++index)
                order.push_back(static_cast<Window>(reinterpret_cast<unsigned long*>(data)[index]));
            XFree(data);
            return order;
        }
        if (data != nullptr)
            XFree(data);
    }
    Window rootReturn = None;
    Window parentReturn = None;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(display, root, &rootReturn, &parentReturn, &children, &childCount) &&
        children != nullptr) {
        order.reserve(static_cast<qsizetype>(childCount));
        for (unsigned int index = 0; index < childCount; ++index)
            order.push_back(children[index]);
    }
    if (children != nullptr)
        XFree(children);
    return order;
}

QVector<WindowStackEntry> stackForRoot(Display* display, int screenIndex, Window root) {
    QVector<WindowStackEntry> entries;
    const QVector<Window> stacking = stackingOrderOnDisplay(display, root);
    entries.reserve(stacking.size());
    // _NET_CLIENT_LIST_STACKING and XQueryTree both report bottom-to-top.
    for (qsizetype index = stacking.size() - 1; index >= 0; --index) {
        const Window client = stacking.at(index);
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, client, &attributes))
            continue;
        Window frame = readFrameWindowId(display, client);
        if (frame == None)
            frame = client;
        const auto geometry = globalGeometryOnDisplay(display, frame);
        if (!geometry)
            continue;
        WindowStackEntry entry;
        entry.windowId = static_cast<quintptr>(client);
        entry.frameRect = *geometry;
        entry.title = windowTitleOnDisplay(display, client);
        entry.processId = readWindowProcessId(display, client);
        entry.screenIndex = screenIndex;
        entry.mapped = attributes.map_state == IsViewable;
        entry.skip = attributes.override_redirect || windowTypeIsSkipped(display, client);
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace

int x11ScreenCount() {
    X11Connection connection;
    return connection.isValid() ? ScreenCount(connection.display()) : 0;
}

QVector<WindowStackEntry> stackedTopLevelWindows() {
    X11Connection connection;
    if (!connection.isValid())
        return {};
    Display* display = connection.display();
    QVector<WindowStackEntry> entries;
    for (int screen = 0; screen < ScreenCount(display); ++screen) {
        const QVector<WindowStackEntry> screenEntries =
            stackForRoot(display, screen, RootWindow(display, screen));
        entries += screenEntries;
    }
    return entries;
}

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
    const WindowId frame = readFrameWindowId(connection.display(), static_cast<Window>(windowId));
    return frame == None ? std::nullopt : std::optional<WindowId>(frame);
}

std::optional<QRect> windowGlobalGeometry(WindowId windowId) {
    X11Connection connection;
    if (!connection.isValid() || windowId == 0) {
        return std::nullopt;
    }
    return globalGeometryOnDisplay(connection.display(), static_cast<Window>(windowId));
}

quint32 windowProcessId(WindowId windowId) {
    X11Connection connection;
    if (!connection.isValid() || windowId == 0)
        return 0;
    return windowProcessIdOnDisplay(connection.display(), windowId);
}

quint32 windowProcessIdOnDisplay(::_XDisplay* display, WindowId windowId) {
    if (display == nullptr || windowId == 0)
        return 0;
    return readWindowProcessId(display, static_cast<Window>(windowId));
}

WindowId frameWindowIdOnDisplay(::_XDisplay* display, WindowId windowId) {
    if (display == nullptr || windowId == 0)
        return 0;
    return static_cast<WindowId>(readFrameWindowId(display, static_cast<Window>(windowId)));
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
