// Qt headers must be parsed before the X11 headers: Xlib's Bool/Status macros
// would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QtCore/qglobal.h>

#include "snow_shot/platform/linux/scrollinput.h"
#include "snow_shot/platform/linux/x11connection.h"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

// Xlib defines Status as a macro; drop it so the platform result enum stays usable.
#undef Status

#include <algorithm>
#include <cstdlib>

namespace snow_shot::platform::x11 {
using windows::ScrollInputResult;

namespace {

constexpr int kWheelStep = 120;
constexpr unsigned int kWheelUpButton = 4;
constexpr unsigned int kWheelDownButton = 5;
constexpr unsigned int kWheelLeftButton = 6;
constexpr unsigned int kWheelRightButton = 7;

bool warpPointer(Display* display, const QPoint& position) {
    return XWarpPointer(display, None, DefaultRootWindow(display), 0, 0, 0, 0, position.x(),
                        position.y()) != 0;
}

} // namespace

windows::ScrollInputResult sendScrollingWheelStep(const QRect& physicalSelection,
                                                  const QPoint& wheelDelta) {
    if (physicalSelection.isEmpty() || wheelDelta.isNull()) {
        return {ScrollInputResult::Status::InvalidRequest, 0};
    }
    const bool horizontal = wheelDelta.x() != 0;
    const int delta = horizontal ? wheelDelta.x() : wheelDelta.y();
    if (delta == 0) {
        return {ScrollInputResult::Status::InvalidRequest, 0};
    }

    X11Connection connection;
    Display* display = connection.display();
    if (display == nullptr) {
        return {ScrollInputResult::Status::Unsupported, 0};
    }
    int eventBase = 0;
    int errorBase = 0;
    int major = 0;
    if (!XQueryExtension(display, "XTEST", &major, &eventBase, &errorBase)) {
        return {ScrollInputResult::Status::Unsupported, 0};
    }

    const unsigned int button = horizontal ? (delta > 0 ? kWheelRightButton : kWheelLeftButton)
                                           : (delta > 0 ? kWheelUpButton : kWheelDownButton);
    const int steps = std::clamp(std::abs(delta) / kWheelStep, 1, 10);

    const Window root = DefaultRootWindow(display);
    Window rootReturn = None;
    Window childReturn = None;
    int pointerX = 0;
    int pointerY = 0;
    int windowX = 0;
    int windowY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(display, root, &rootReturn, &childReturn, &pointerX, &pointerY, &windowX,
                       &windowY, &mask)) {
        return {ScrollInputResult::Status::CoordinateFailure, 0};
    }

    const QPoint target = physicalSelection.center();
    if (!warpPointer(display, target)) {
        return {ScrollInputResult::Status::CoordinateFailure, 0};
    }
    XSync(display, False);

    bool posted = true;
    for (int step = 0; step < steps; ++step) {
        if (!XTestFakeButtonEvent(display, button, True, CurrentTime) ||
            !XTestFakeButtonEvent(display, button, False, CurrentTime)) {
            posted = false;
            break;
        }
    }
    XSync(display, False);

    static_cast<void>(warpPointer(display, QPoint(pointerX, pointerY)));
    XSync(display, False);
    if (!posted) {
        return {ScrollInputResult::Status::PostFailed, 0};
    }
    return {ScrollInputResult::Status::Posted, 0};
}

} // namespace snow_shot::platform::x11
