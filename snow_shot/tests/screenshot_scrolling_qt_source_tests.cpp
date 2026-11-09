#include "presentation/capture/screenshotscrollingqtsource.h"

#include <QColorSpace>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>

#include <cstdlib>
#include <iostream>

namespace {

using snow_shot::capture_detail::qtscrolling::cropScrollingSelectionFrame;
using snow_shot::capture_detail::qtscrolling::isDuplicateScrollingFrame;
using snow_shot::capture_detail::qtscrolling::normalizeScrollingRootFrame;
using snow_shot::capture_detail::qtscrolling::toScrollingFramePayload;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QImage syntheticFrame(const QSize& size, QRgb seed) {
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            image.setPixel(x, y, seed + static_cast<QRgb>(y * 1000 + x));
        }
    }
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image;
}

CapturedDisplayModel displayModel(const QRect& physicalRect, QScreen* screen = nullptr) {
    CapturedDisplayModel display;
    display.physicalRect = physicalRect;
    display.canvasRect = physicalRect;
    display.screen = screen;
    display.geometryResolved = true;
    return display;
}

void normalizeResetsDevicePixelRatioAndFormat() {
    QImage frame = syntheticFrame(QSize(120, 60), 0xFF102030);
    frame.setDevicePixelRatio(2.0);
    const QImage normalized = normalizeScrollingRootFrame(frame);
    require(normalized.devicePixelRatio() == 1.0,
            "normalized root frames must not retain a device pixel ratio");
    require(normalized.format() == QImage::Format_RGB32,
            "normalized root frames must use RGB32 like the single-frame provider");
    require(normalized.size() == QSize(120, 60), "normalization must not rescale the frame");
    require(normalized.colorSpace() == QColorSpace(QColorSpace::SRgb),
            "normalized root frames must be tagged sRGB");
    require(normalizeScrollingRootFrame(QImage()).isNull(),
            "normalizing a null frame must stay null");
}

void cropsSelectionOutOfTheRootFrame() {
    const QVector<CapturedDisplayModel> displays{displayModel(QRect(0, 0, 300, 200))};
    const QImage root = syntheticFrame(QSize(300, 200), 0xFF000100);
    const QRect selection(10, 20, 80, 40);
    const QImage cropped =
        cropScrollingSelectionFrame(displays, root, selection, [](QScreen&) { return QImage(); });
    require(cropped.size() == selection.size(), "the crop must match the physical selection");
    require(cropped == root.copy(selection), "the crop must read the selection region exactly");
    require(cropScrollingSelectionFrame(displays, root, QRect(), [](QScreen&) { return QImage(); })
                .isNull(),
            "an empty selection must not produce a frame");
    require(cropScrollingSelectionFrame({}, root, selection, [](QScreen&) { return QImage(); })
                .isNull(),
            "a missing display layout must not produce a frame");
}

void fallsBackToPerDisplayGrabWhenTheRootCropMisses() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "the offscreen platform must expose a primary screen");
    const QVector<CapturedDisplayModel> displays{displayModel(QRect(0, 0, 300, 200), screen)};
    const QRect selection(40, 30, 64, 32);
    const QImage perDisplay = syntheticFrame(QSize(300, 200), 0xFF00FF00);
    int grabs = 0;
    const QImage cropped =
        cropScrollingSelectionFrame(displays, QImage(), selection, [&](QScreen&) {
            ++grabs;
            return perDisplay;
        });
    require(grabs == 1, "a missing root frame must trigger a per-display grab");
    require(cropped == perDisplay.copy(selection),
            "the per-display fallback must crop the selection region exactly");

    const QImage fullDisplay = syntheticFrame(QSize(300, 200), 0xFF0000FF);
    const QImage whole = cropScrollingSelectionFrame(displays, QImage(), QRect(0, 0, 300, 200),
                                                     [&](QScreen&) { return fullDisplay; });
    require(whole == fullDisplay,
            "a per-display grab sized like the display must pass through unchanged");
}

void payloadMatchesTheStitchContract() {
    QImage frame = syntheticFrame(QSize(75, 21), 0xFF204060);
    frame.setDevicePixelRatio(1.5);
    const QImage payload = toScrollingFramePayload(frame);
    require(payload.format() == QImage::Format_RGBA8888,
            "scrolling payloads must be RGBA8888 for the stitch pipeline");
    require(payload.bytesPerLine() == payload.width() * 4,
            "scrolling payload rows must be tightly packed");
    require(payload.devicePixelRatio() == 1.0, "scrolling payloads must use raw pixels");
    require(payload.colorSpace() == QColorSpace(QColorSpace::SRgb),
            "scrolling payloads must be tagged sRGB");
    require(toScrollingFramePayload(QImage()).isNull(),
            "a null frame must stay null in the payload conversion");
}

void duplicateDetectionComparesPixels() {
    const QImage frame = syntheticFrame(QSize(32, 24), 0xFF808080);
    require(isDuplicateScrollingFrame(frame, frame.copy()), "equal frames are duplicates");
    require(!isDuplicateScrollingFrame(QImage(), frame),
            "a missing previous frame is never a duplicate");
    QImage changed = frame.copy();
    changed.setPixel(3, 4, 0xFFFFFFFF);
    require(!isDuplicateScrollingFrame(frame, changed), "a single changed pixel is a new frame");
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    normalizeResetsDevicePixelRatioAndFormat();
    cropsSelectionOutOfTheRootFrame();
    fallsBackToPerDisplayGrabWhenTheRootCropMisses();
    payloadMatchesTheStitchContract();
    duplicateDetectionComparesPixels();
    return 0;
}
