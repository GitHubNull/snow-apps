#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <optional>

struct _XDisplay;

namespace snow_shot::platform::x11 {

using WindowId = unsigned long;

// _NET_WM_PID of the window on an existing connection, or 0 when absent.
[[nodiscard]] quint32 windowProcessIdOnDisplay(::_XDisplay* display, WindowId windowId);

// Nearest ancestor whose parent is the root window, on an existing connection.
// Returns 0 for roots and dead window ids.
[[nodiscard]] WindowId frameWindowIdOnDisplay(::_XDisplay* display, WindowId windowId);

// One top-level window as reported by the X server, in top-to-bottom stacking
// order. Frame geometry is in the root window's (physical) coordinates.
struct WindowStackEntry {
    quintptr windowId = 0;
    QRect frameRect;
    QString title;
    quint32 processId = 0;
    int screenIndex = 0;
    bool mapped = true;
    // Panels, desktops, menus, tooltips, and override-redirect windows are
    // never valid window-selection targets.
    bool skip = false;
};

struct WindowTarget {
    quintptr windowId = 0;
    QRect frameRect;
    QString title;
    quint32 processId = 0;
};

// Number of X screens. A single screen spans every monitor (Xinerama/XRandR);
// separate screens place each root at its own origin.
[[nodiscard]] int x11ScreenCount();

// Top-to-bottom stack of top-level windows across every X screen. `screenIndex`
// identifies the root that owns each entry's coordinates.
[[nodiscard]] QVector<WindowStackEntry> stackedTopLevelWindows();

// Reads _NET_ACTIVE_WINDOW from every screen's root window and returns the
// first active window; nullopt when no screen reports one.
[[nodiscard]] std::optional<WindowId> activeWindowId();

// Walks up the window tree to the nearest ancestor whose parent is a root
// window (the manager's frame around a client window). Returns nullopt for
// roots and dead window ids.
[[nodiscard]] std::optional<WindowId> frameWindowIdFor(WindowId windowId);

// Global (root-relative) geometry of the window including any manager frame.
[[nodiscard]] std::optional<QRect> windowGlobalGeometry(WindowId windowId);

// _NET_WM_PID of the window, or 0 when the property is absent.
[[nodiscard]] quint32 windowProcessId(WindowId windowId);

// True when none of the given windows is viewable on the X server. Windows
// that disappeared from the server count as unviewable.
[[nodiscard]] bool windowsAreUnviewable(const QVector<WindowId>& windowIds);

// Polls until every window is unviewable or the timeout elapses (10 ms steps).
// After the last window disappears it waits one extra compositor frame before
// confirming, so buffers still holding the window are not captured. Returns
// the final unviewable state.
bool waitForWindowsUnviewable(const QVector<WindowId>& windowIds, int timeoutMs);

} // namespace snow_shot::platform::x11
