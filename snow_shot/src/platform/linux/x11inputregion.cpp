// Qt headers must be parsed before the X11 headers: Xlib's Bool/Status macros
// would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QtWidgets/qwidget.h>

#include "snow_shot/platform/linux/x11inputregion.h"
#include "snow_shot/platform/linux/x11connection.h"

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cmath>

namespace snow_shot::platform::x11 {
namespace {

unsigned short extent(int value) {
    return static_cast<unsigned short>(std::clamp(value, 0, 0xFFFF));
}

XRectangle rectangle(int x, int y, int width, int height) {
    return XRectangle{static_cast<short>(x), static_cast<short>(y), extent(width), extent(height)};
}

} // namespace

bool setWindowInputHole(QWidget* widget, const QRect& hole) {
    if (widget == nullptr) {
        return false;
    }
    X11Connection connection;
    Display* display = connection.display();
    if (display == nullptr) {
        return false;
    }
    const Window window = static_cast<Window>(widget->winId());
    if (window == None) {
        return false;
    }
    int eventBase = 0;
    int errorBase = 0;
    if (!XShapeQueryExtension(display, &eventBase, &errorBase)) {
        return false;
    }

    const qreal scale = widget->devicePixelRatioF();
    const QSize size = widget->size();
    const QRect bounds(0, 0, qRound(size.width() * scale), qRound(size.height() * scale));
    QRect holePixels;
    if (!hole.isEmpty()) {
        holePixels = QRect(qRound(hole.x() * scale), qRound(hole.y() * scale),
                           qRound(hole.width() * scale), qRound(hole.height() * scale))
                         .intersected(bounds);
    }

    XRectangle strips[4];
    int count = 0;
    if (holePixels.isEmpty()) {
        strips[count++] = rectangle(0, 0, bounds.width(), bounds.height());
    } else {
        const int holeBottom = holePixels.bottom() + 1;
        const int holeRight = holePixels.right() + 1;
        if (holePixels.top() > 0) {
            strips[count++] = rectangle(0, 0, bounds.width(), holePixels.top());
        }
        if (holeBottom < bounds.height()) {
            strips[count++] =
                rectangle(0, holeBottom, bounds.width(), bounds.height() - holeBottom);
        }
        if (holePixels.left() > 0) {
            strips[count++] =
                rectangle(0, holePixels.top(), holePixels.left(), holePixels.height());
        }
        if (holeRight < bounds.width()) {
            strips[count++] = rectangle(holeRight, holePixels.top(), bounds.width() - holeRight,
                                        holePixels.height());
        }
    }
    XShapeCombineRectangles(display, window, ShapeInput, 0, 0, strips, count, ShapeSet, Unsorted);
    XFlush(display);
    return true;
}

} // namespace snow_shot::platform::x11
