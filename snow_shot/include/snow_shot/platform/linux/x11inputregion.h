#ifndef SNOW_SHOT_PLATFORM_LINUX_X11INPUTREGION_H
#define SNOW_SHOT_PLATFORM_LINUX_X11INPUTREGION_H

#include <QRect>
#include <QRegion>

class QWidget;

namespace snow_shot::platform::x11 {

// Replaces the widget's X input shape with its full area minus `hole`
// (widget-local logical pixels). An empty hole restores full input.
// Returns false when the window or display is unavailable.
[[nodiscard]] bool setWindowInputHole(QWidget* widget, const QRect& hole);

// Replaces the widget's X input shape with `region` (widget-local logical
// pixels). An empty region restores full input. Only ShapeInput changes, so
// painted shadows outside the region stay visible while input passes through.
// Returns false when the window or display is unavailable.
[[nodiscard]] bool setWindowInputRegion(QWidget* widget, const QRegion& region);

// Enables or removes the window's input region entirely. A disabled window is
// click-through while its painted content (ShapeBounding) stays untouched.
// Returns false when the window or display is unavailable.
[[nodiscard]] bool setWindowInputEnabled(QWidget* widget, bool enabled);

} // namespace snow_shot::platform::x11

#endif // SNOW_SHOT_PLATFORM_LINUX_X11INPUTREGION_H
