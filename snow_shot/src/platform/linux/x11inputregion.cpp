// Qt headers must be parsed before the X11 headers: Xlib's Bool/Status macros
// would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QtWidgets/qwidget.h>

#include "snow_shot/platform/linux/x11inputregion.h"
#include "snow_shot/platform/linux/x11connection.h"

#include <QList>

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

XRectangle scaledRectangle(const QRect& rect, qreal scale) {
    // Scale the edges instead of origin plus size so adjacent logical
    // rectangles cannot open fractional-pixel gaps in the input shape.
    const int left = qRound(rect.left() * scale);
    const int top = qRound(rect.top() * scale);
    const int right = qRound((rect.right() + 1) * scale);
    const int bottom = qRound((rect.bottom() + 1) * scale);
    return rectangle(left, top, right - left, bottom - top);
}

} // namespace

bool setWindowInputRegion(QWidget* widget, const QRegion& region) {
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
    const QRegion bounded = region.intersected(widget->rect());
    QList<XRectangle> rectangles;
    if (bounded.isEmpty()) {
        // An empty input shape would make every panel unreachable, so an
        // empty region restores the full window.
        rectangles.append(
            rectangle(0, 0, qRound(widget->width() * scale), qRound(widget->height() * scale)));
    } else {
        for (const QRect& rect : bounded.rects()) {
            rectangles.append(scaledRectangle(rect, scale));
        }
    }
    XShapeCombineRectangles(display, window, ShapeInput, 0, 0, rectangles.data(),
                            static_cast<int>(rectangles.size()), ShapeSet, Unsorted);
    XFlush(display);
    return true;
}

bool setWindowInputHole(QWidget* widget, const QRect& hole) {
    if (widget == nullptr) {
        return false;
    }
    QRegion region = widget->rect();
    if (!hole.isEmpty()) {
        region -= hole;
    }
    return setWindowInputRegion(widget, region);
}

} // namespace snow_shot::platform::x11
