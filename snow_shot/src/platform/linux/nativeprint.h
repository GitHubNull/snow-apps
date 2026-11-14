#ifndef SNOW_SHOT_PLATFORM_LINUX_NATIVEPRINT_H
#define SNOW_SHOT_PLATFORM_LINUX_NATIVEPRINT_H

#include <QImage>
#include <QRectF>

class QPainter;

namespace snow_shot::platform::linux {

// Paints the image fitted and centered inside the page rectangle, with an
// opaque white background. Exposed for deterministic offscreen tests.
void paintPrintImage(QPainter& painter, const QImage& image, const QRectF& pageRect);

} // namespace snow_shot::platform::linux

#endif // SNOW_SHOT_PLATFORM_LINUX_NATIVEPRINT_H
