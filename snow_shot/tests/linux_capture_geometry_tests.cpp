#include "presentation/capture/screenshotqtcapturegeometry.h"
#include "snow_shot/presentation/capture/screenshotqtcaptureprovider.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#include <QGuiApplication>
#include <QImage>
#include <QScreen>
#include <QSet>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace qtcapture = snow_shot::presentation::capture::qtcapture;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void stableIdsStayUnique() {
    QSet<QString> usedIds;
    require(qtcapture::displayStableId(QStringLiteral("DP-1"), 0, &usedIds) ==
                QStringLiteral("display:DP-1"),
            "a named screen must keep its name in the stable id");
    require(qtcapture::displayStableId(QStringLiteral("DP-1"), 1, &usedIds) ==
                QStringLiteral("display:qt-1"),
            "duplicate screen names must fall back to the index");
    require(qtcapture::displayStableId(QString(), 2, &usedIds) == QStringLiteral("display:qt-2"),
            "an unnamed screen must fall back to the index");
    require(qtcapture::displayStableId(QStringLiteral("  HDMI-1 "), 3, &usedIds) ==
                QStringLiteral("display:HDMI-1"),
            "surrounding whitespace must not leak into the stable id");
    require(usedIds.size() == 4, "stable ids must be unique per display");
}

void cropUsesVirtualDesktopOffsets() {
    QImage desktop(300, 200, QImage::Format_RGB32);
    desktop.fill(Qt::black);
    desktop.setPixel(15, 20, qRgb(1, 2, 3));
    desktop.setPixel(160, 10, qRgb(1, 2, 3));
    desktop.setPixel(260, 10, qRgb(10, 20, 30));
    const QRect bounds(0, 0, 300, 200);

    const QImage left = qtcapture::cropDisplayFrame(desktop, bounds, QRect(0, 0, 200, 200));
    require(left.size() == QSize(200, 200), "the left display must crop at the desktop origin");
    require(left.pixelColor(15, 20) == QColor(1, 2, 3), "the left display must keep its pixels");

    const QImage right = qtcapture::cropDisplayFrame(desktop, bounds, QRect(200, 0, 100, 200));
    require(right.size() == QSize(100, 200), "the right display must crop at its offset");
    require(right.pixelColor(60, 10) == QColor(10, 20, 30),
            "the right display must shift its desktop offset away");

    // X11 root frames start at the virtual desktop origin, which need not match
    // a synthetic non-zero bounds origin used by tests and other platforms.
    const QImage shifted =
        qtcapture::cropDisplayFrame(desktop, QRect(100, 100, 300, 200), QRect(250, 100, 150, 200));
    require(shifted.size() == QSize(150, 200), "bounds origin must be subtracted from the crop");
    require(shifted.pixelColor(10, 10) == QColor(1, 2, 3),
            "the shifted crop must read the translated desktop pixel");
}

void cropHandlesPerScreenGrabsAndMismatches() {
    QImage screenFrame(120, 80, QImage::Format_RGB32);
    screenFrame.fill(Qt::red);
    const QRect displayRect(40, 30, 120, 80);
    const QImage perScreen =
        qtcapture::cropDisplayFrame(screenFrame, QRect(0, 0, 300, 200), displayRect);
    require(perScreen.size() == screenFrame.size(),
            "a per-screen grab must be returned without cropping");

    QImage wrongSize(50, 50, QImage::Format_RGB32);
    require(qtcapture::cropDisplayFrame(wrongSize, QRect(0, 0, 300, 200), displayRect).isNull(),
            "a frame that is neither a display nor the desktop must be rejected");

    QImage desktop(300, 200, QImage::Format_RGB32);
    require(qtcapture::cropDisplayFrame(desktop, QRect(0, 0, 300, 200), QRect(280, 0, 100, 100))
                .isNull(),
            "a display that only partially covers the frame must be rejected");
    require(qtcapture::cropDisplayFrame(QImage(), QRect(0, 0, 300, 200), displayRect).isNull(),
            "an empty frame must be rejected");
}

void layoutMatchesScreens() {
    const QList<QScreen*> screens = QGuiApplication::screens();
    require(!screens.isEmpty(), "the offscreen QPA must expose at least one screen");
    const QVector<CapturedDisplayModel> displays = ScreenshotQtCaptureProvider::layoutDisplays();
    require(displays.size() == screens.size(),
            "the layout snapshot must be index-aligned with QGuiApplication::screens()");

    QSet<QString> usedIds;
    for (qsizetype index = 0; index < displays.size(); ++index) {
        const CapturedDisplayModel& display = displays.at(index);
        const QScreen& screen = *screens.at(index);
        require(display.active, "every layout display must be active");
        require(!display.stableId.isEmpty(), "every layout display must carry a stable id");
        require(!usedIds.contains(display.stableId), "layout stable ids must be unique");
        usedIds.insert(display.stableId);
        require(display.screen == &screen, "the display must bind to its QScreen");
        require(display.name == screen.name(), "the display must keep the QScreen name");
        require(display.physicalRect == ScreenshotGeometryMapper::physicalRectForScreen(screen),
                "the physical rect must match the shared screen geometry helper");
        require(display.canvasRect == display.physicalRect,
                "the capture canvas must use physical pixels");
        require(!display.canvasUsesPoints, "Linux capture stays in physical pixels");
        require(display.nativeDisplayId == 0, "Linux has no native display id");
        require(display.geometryResolved, "the capture layout must be fully resolved");
    }
}

void captureMatchesLayoutIdentity() {
    QString error;
    const QVector<CapturedDisplayModel> frames =
        ScreenshotQtCaptureProvider::captureDisplays(&error);
    if (frames.isEmpty()) {
        // The offscreen QPA does not implement QScreen::grabWindow; the real X11
        // path is covered by the interactive desktop acceptance run.
        require(!error.isEmpty(), "a failed capture must report an error");
        return;
    }

    const QVector<CapturedDisplayModel> layout = ScreenshotQtCaptureProvider::layoutDisplays();
    require(frames.size() == layout.size(), "capture must return one frame per display");
    for (qsizetype index = 0; index < frames.size(); ++index) {
        const CapturedDisplayModel& frame = frames.at(index);
        require(frame.stableId == layout.at(index).stableId,
                "capture frames must keep the layout stable ids");
        require(frame.physicalRect == layout.at(index).physicalRect,
                "capture frames must keep the layout physical rects");
        require(!frame.image.isNull(), "every display must carry a frame image");
        require(frame.image.size() == frame.physicalRect.size(),
                "frame images must be physical-pixel sized");
        require(frame.image.devicePixelRatio() == 1.0,
                "frame images must not retain a device pixel ratio");
        require(frame.image.format() == QImage::Format_RGB32,
                "desktop frames must be opaque RGB32 like the native backends");
    }
}

void cursorPlacementClipsAgainstDisplayBounds() {
    const QRect displayRect(0, 0, 1920, 1080);
    const QSize cursorSize(24, 24);

    // Fully inside: the image maps one hotspot up-left of the pointer.
    auto inside = qtcapture::cursorPlacement(displayRect, {600, 300}, {4, 4}, cursorSize);
    require(inside.visible && inside.pixelRect == QRect(596, 296, 24, 24) &&
                inside.sourceRect == QRect(0, 0, 24, 24),
            "interior cursor keeps the full image");

    // Crossing the display's left edge clips the source, not just the target.
    auto leftEdge = qtcapture::cursorPlacement(displayRect, {2, 300}, {4, 4}, cursorSize);
    require(leftEdge.visible && leftEdge.pixelRect == QRect(0, 296, 22, 24) &&
                leftEdge.sourceRect == QRect(2, 0, 22, 24),
            "left-edge cursor clips the source image columns");

    // Wholly outside the display never produces a placement.
    auto outside = qtcapture::cursorPlacement(displayRect, {-40, 300}, {4, 4}, cursorSize);
    require(!outside.visible, "off-display cursor is invisible");

    // Negative-origin displays keep display-local coordinates relative to
    // their own top-left.
    const QRect negativeDisplay(-1920, 0, 1920, 1080);
    auto negative = qtcapture::cursorPlacement(negativeDisplay, {-960, 540}, {4, 4}, cursorSize);
    require(negative.visible && negative.pixelRect == QRect(956, 536, 24, 24) &&
                negative.sourceRect == QRect(0, 0, 24, 24),
            "negative-origin display offsets cursor locally");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    stableIdsStayUnique();
    cropUsesVirtualDesktopOffsets();
    cropHandlesPerScreenGrabsAndMismatches();
    layoutMatchesScreens();
    captureMatchesLayoutIdentity();
    cursorPlacementClipsAgainstDisplayBounds();
    std::cout << "snow shot linux capture geometry tests passed\n";
    return 0;
}
