#include "../../presentation/pinned/livewindownative_p.h"

namespace snow_shot::presentation {

// X11 exposes no window enumeration through Qt that maps a desktop point to a
// capturable native window, so live (continuously updating) pinned windows are
// unavailable. Returning nullopt makes the pin-window action report a regular
// "no capturable window" error instead of constructing a dead source.
std::optional<LiveWindowTarget> liveWindowTargetAtPoint(const QPoint&, const QVector<quintptr>&) {
    return std::nullopt;
}

namespace live_window_detail {

std::unique_ptr<NativeWindowCapture> createNativeWindowCapture(const LiveWindowTarget&) {
    return nullptr;
}

std::unique_ptr<NativeWindowMonitor> createNativeWindowMonitor(const LiveWindowTarget&) {
    return nullptr;
}

std::unique_ptr<NativeWindowPresentation>
createNativeWindowPresentation(const LiveWindowTarget&) {
    return nullptr;
}

} // namespace live_window_detail
} // namespace snow_shot::presentation
