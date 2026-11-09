#ifndef SNOW_SHOT_PLATFORM_LINUX_SCROLLINPUT_H
#define SNOW_SHOT_PLATFORM_LINUX_SCROLLINPUT_H

#include "snow_shot/platform/windows/scrollinput.h"

#include <QPoint>
#include <QRect>

namespace snow_shot::platform::x11 {

// Posts one XTest wheel step at the center of the physical selection and
// restores the user's pointer position. The overlay must already exclude the
// selection from its input shape, otherwise the synthetic events land on our
// own window; the X server picks the receiver by pointer position, so no
// separate target probe is meaningful.
windows::ScrollInputResult sendScrollingWheelStep(const QRect& physicalSelection,
                                                  const QPoint& wheelDelta);

} // namespace snow_shot::platform::x11

#endif // SNOW_SHOT_PLATFORM_LINUX_SCROLLINPUT_H
