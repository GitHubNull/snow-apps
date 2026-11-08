#pragma once

#include <QRect>
#include <QVector>
#include <optional>

namespace snow_shot::platform::x11 {

using WindowId = unsigned long;

// Reads _NET_ACTIVE_WINDOW from every screen's root window and returns the
// first active window; nullopt when no screen reports one.
[[nodiscard]] std::optional<WindowId> activeWindowId();

// Walks up the window tree to the nearest ancestor whose parent is a root
// window (the manager's frame around a client window). Returns nullopt for
// roots and dead window ids.
[[nodiscard]] std::optional<WindowId> frameWindowIdFor(WindowId windowId);

// Global (root-relative) geometry of the window including any manager frame.
[[nodiscard]] std::optional<QRect> windowGlobalGeometry(WindowId windowId);

// True when none of the given windows is viewable on the X server. Windows
// that disappeared from the server count as unviewable.
[[nodiscard]] bool windowsAreUnviewable(const QVector<WindowId>& windowIds);

// Polls until every window is unviewable or the timeout elapses (10 ms steps).
// After the last window disappears it waits one extra compositor frame before
// confirming, so buffers still holding the window are not captured. Returns
// the final unviewable state.
bool waitForWindowsUnviewable(const QVector<WindowId>& windowIds, int timeoutMs);

} // namespace snow_shot::platform::x11
