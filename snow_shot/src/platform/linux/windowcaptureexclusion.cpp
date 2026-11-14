#include "snow_shot/platform/windowcaptureexclusion.h"

#include <QGuiApplication>
#include <QWidget>

namespace snow_shot::platform {

bool setWindowExcludedFromCapture(QWidget* window, bool excluded) {
    Q_UNUSED(window);
    Q_UNUSED(excluded);
    // X11 has no per-window capture affinity. The recording capture backend
    // composites the desktop without the windows listed in the session
    // exclusion set, so the app only needs to report that the request is
    // accepted on xcb.
    return QGuiApplication::platformName() == QStringLiteral("xcb");
}

std::optional<std::uint32_t> captureWindowId(QWidget* window) {
    if (window == nullptr || QGuiApplication::platformName() != QStringLiteral("xcb"))
        return std::nullopt;
    const WId id = window->internalWinId();
    if (id == 0)
        return std::nullopt;
    return static_cast<std::uint32_t>(id);
}

} // namespace snow_shot::platform
