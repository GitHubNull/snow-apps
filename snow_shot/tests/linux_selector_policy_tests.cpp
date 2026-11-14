#include "../src/presentation/selector/linuxselectorpolicy.h"

#include <cstdlib>
#include <iostream>

using snow_shot::platform::x11::WindowStackEntry;
using snow_shot::platform::x11::WindowTarget;
using snow_shot::presentation::selector::imageRectForPhysicalRect;
using snow_shot::presentation::selector::physicalRectForImageRect;
using snow_shot::presentation::selector::pickElementAtPoint;
using snow_shot::presentation::selector::pickWindowAtPoint;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

WindowStackEntry entry(quintptr id, const QRect& rect, quint32 pid = 100, int screen = 0,
                       bool mapped = true, bool skip = false) {
    WindowStackEntry value;
    value.windowId = id;
    value.frameRect = rect;
    value.processId = pid;
    value.screenIndex = screen;
    value.mapped = mapped;
    value.skip = skip;
    return value;
}

void testPickWindow() {
    const QVector<WindowStackEntry> stack{
        entry(1, QRect(0, 0, 800, 600)),                     // topmost
        entry(2, QRect(0, 0, 1200, 900)),                    // below
        entry(3, QRect(2000, 0, 400, 400)),                  // other monitor
        entry(4, QRect(0, 0, 800, 600), 100, 0, false),      // unmapped
        entry(5, QRect(0, 0, 800, 600), 100, 0, true, true), // panel
    };
    const auto top = pickWindowAtPoint(QPoint(100, 100), stack, {}, 0, -1, {});
    require(top.has_value() && top->windowId == 1, "topmost mapped window must win");
    require(top->frameRect == QRect(0, 0, 800, 600), "frame rect must be reported");

    const auto excluded = pickWindowAtPoint(QPoint(100, 100), stack, {1}, 0, -1, {});
    require(excluded.has_value() && excluded->windowId == 2, "excluded windows must be skipped");

    const auto ownProcess = pickWindowAtPoint(QPoint(100, 100), stack, {1, 2}, 100, -1, {});
    require(!ownProcess.has_value(), "own process windows must be skipped");

    const auto monitorTwo = pickWindowAtPoint(QPoint(2100, 100), stack, {}, 0, -1, {});
    require(monitorTwo.has_value() && monitorTwo->windowId == 3,
            "windows are selected by point containment");

    const auto outside = pickWindowAtPoint(QPoint(1500, 100), stack, {}, 0, -1, {});
    require(!outside.has_value(), "empty desktop must not select a window");
}

void testScreenFilterAndOffset() {
    QVector<WindowStackEntry> stack{
        entry(7, QRect(0, 0, 800, 600), 100, 1),
        entry(8, QRect(0, 0, 800, 600), 100, 0),
    };
    const auto screenZero = pickWindowAtPoint(QPoint(100, 100), stack, {}, 0, 0, QPoint(0, 0));
    require(screenZero.has_value() && screenZero->windowId == 8,
            "screen filter must select the matching root");

    const auto screenOne = pickWindowAtPoint(QPoint(2100, 100), stack, {}, 0, 1, QPoint(2000, 0));
    require(screenOne.has_value() && screenOne->windowId == 7,
            "root offsets must translate separate-screen windows");
    require(screenOne->frameRect == QRect(2000, 0, 800, 600),
            "translated frame rect must use virtual desktop coordinates");
}

void testImageMapping() {
    const QRect display(0, 0, 1920, 1080);
    const QRect window(100, 200, 400, 300);
    require(imageRectForPhysicalRect(display, QSize(1920, 1080), window) == window,
            "identity scale must preserve rectangles");

    const QRect scaled = imageRectForPhysicalRect(display, QSize(3840, 2160), window);
    require(scaled == QRect(200, 400, 800, 600), "2x scale must map into image pixels");

    const QRect partial =
        imageRectForPhysicalRect(display, QSize(1920, 1080), QRect(-100, -50, 300, 200));
    require(partial == QRect(0, 0, 200, 150), "partially visible windows must be clipped");

    require(imageRectForPhysicalRect(display, QSize(1920, 1080), QRect(3000, 0, 10, 10)).isEmpty(),
            "off-display rectangles must not map");

    const QRect physical =
        physicalRectForImageRect(display, QSize(3840, 2160), QRect(200, 400, 800, 600));
    require(physical == window, "inverse mapping must restore physical rectangles");
}

void testElementPick() {
    const QRect window(0, 0, 800, 600);
    const QVector<QRect> candidates{
        QRect(0, 0, 800, 600),
        QRect(10, 10, 100, 40),
        QRect(50, 5, 30, 30),
    };
    const auto element = pickElementAtPoint(candidates, QPoint(60, 20), window);
    require(element.has_value() && *element == QRect(50, 5, 30, 30),
            "the smallest containing element must win");

    const auto fallback = pickElementAtPoint(candidates, QPoint(700, 500), window);
    require(fallback.has_value() && *fallback == window,
            "window-sized candidates must match empty areas");

    require(!pickElementAtPoint({}, QPoint(1, 1), window).has_value(),
            "no candidates must report no element");

    const auto outside = pickElementAtPoint(candidates, QPoint(900, 700), window);
    require(!outside.has_value(), "points outside the window must not select an element");
}

} // namespace

int main() {
    testPickWindow();
    testScreenFilterAndOffset();
    testImageMapping();
    testElementPick();
    return 0;
}
