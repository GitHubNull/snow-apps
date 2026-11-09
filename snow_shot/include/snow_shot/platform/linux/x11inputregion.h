#ifndef SNOW_SHOT_PLATFORM_LINUX_X11INPUTREGION_H
#define SNOW_SHOT_PLATFORM_LINUX_X11INPUTREGION_H

#include <QRect>

class QWidget;

namespace snow_shot::platform::x11 {

// Replaces the widget's X input shape with its full area minus `hole`
// (widget-local logical pixels). An empty hole restores full input.
// Returns false when the window or display is unavailable.
[[nodiscard]] bool setWindowInputHole(QWidget* widget, const QRect& hole);

} // namespace snow_shot::platform::x11

#endif // SNOW_SHOT_PLATFORM_LINUX_X11INPUTREGION_H
