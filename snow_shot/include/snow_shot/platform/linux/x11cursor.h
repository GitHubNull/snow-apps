#pragma once

#include <QImage>
#include <QPoint>

namespace snow_shot::platform::x11 {

struct X11CursorSample {
    // Premultiplied ARGB32 image of the cursor shape.
    QImage image;
    // Global (root window) position of the cursor's hotspot.
    QPoint position;
    // Offset from the image top-left to the hotspot.
    QPoint hotspot;
    bool valid = false;
};

// Samples the current X11 cursor shape through XFixes over a short-lived
// display connection. Returns an invalid sample when XFixes is unavailable or
// the cursor cannot be read; callers must treat cursor capture as optional.
[[nodiscard]] X11CursorSample fetchCursorImage();

} // namespace snow_shot::platform::x11
