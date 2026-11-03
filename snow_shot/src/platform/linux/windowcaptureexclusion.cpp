#include "snow_shot/platform/windowcaptureexclusion.h"

namespace snow_shot::platform {

bool setWindowExcludedFromCapture(QWidget* window, bool excluded) {
    Q_UNUSED(window);
    Q_UNUSED(excluded);
    // X11 has no display-affinity equivalent; overlay windows stay visible.
    return false;
}

std::optional<std::uint32_t> captureWindowId(QWidget* window) {
    Q_UNUSED(window);
    return std::nullopt;
}

} // namespace snow_shot::platform
