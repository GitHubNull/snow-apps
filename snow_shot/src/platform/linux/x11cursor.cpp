// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QtCore/qdatastream.h>
#include <QtCore/qmetatype.h>
#include <QtCore/qtextstream.h>
#include <QImage>

#include "snow_shot/platform/linux/x11cursor.h"
#include "snow_shot/platform/linux/x11connection.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

namespace snow_shot::platform::x11 {
namespace {

constexpr int kMaxCursorDimension = 512;

QImage premultipliedImageFromXFixes(const XFixesCursorImage* cursor) {
    QImage image(cursor->width, cursor->height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < cursor->height; ++y) {
        for (int x = 0; x < cursor->width; ++x) {
            // XFixes hands out straight (non-premultiplied) ARGB in the low
            // 32 bits of each unsigned long; QPainter compositing needs the
            // premultiplied form.
            const auto argb = static_cast<quint32>(cursor->pixels[y * cursor->width + x]);
            const auto alpha = static_cast<quint32>((argb >> 24) & 0xFFU);
            const auto red = static_cast<quint32>((argb >> 16) & 0xFFU);
            const auto green = static_cast<quint32>((argb >> 8) & 0xFFU);
            const auto blue = static_cast<quint32>(argb & 0xFFU);
            const auto premultiplied = (alpha << 24) | ((red * alpha / 255U) << 16) |
                                       ((green * alpha / 255U) << 8) | (blue * alpha / 255U);
            image.setPixel(x, y, premultiplied);
        }
    }
    return image;
}

} // namespace

X11CursorSample fetchCursorImage() {
    X11CursorSample sample;
    X11Connection connection;
    if (!connection.isValid()) {
        return sample;
    }
    Display* display = connection.display();
    int eventBase = 0;
    int errorBase = 0;
    if (!XFixesQueryExtension(display, &eventBase, &errorBase)) {
        return sample;
    }
    XFixesCursorImage* cursor = XFixesGetCursorImage(display);
    if (cursor == nullptr) {
        return sample;
    }
    if (cursor->width <= 0 || cursor->height <= 0 || cursor->width > kMaxCursorDimension ||
        cursor->height > kMaxCursorDimension) {
        XFree(cursor);
        return sample;
    }
    sample.image = premultipliedImageFromXFixes(cursor);
    sample.position = QPoint(cursor->x, cursor->y);
    sample.hotspot = QPoint(cursor->xhot, cursor->yhot);
    sample.valid = true;
    XFree(cursor);
    return sample;
}

} // namespace snow_shot::platform::x11
