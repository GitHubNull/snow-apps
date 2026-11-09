#pragma once

#include "screenshotqtcapturegeometry.h"

#include "snow_shot/presentation/screenshotgeometry.h"

#include <QColorSpace>
#include <QImage>
#include <QRect>
#include <QVector>

#include <functional>

namespace snow_shot::capture_detail::qtscrolling {

// Mirrors the single-frame provider normalization: capture geometry is physical
// pixels, so a retained device pixel ratio would rescale the frame later.
[[nodiscard]] inline QImage normalizeScrollingRootFrame(QImage image) {
    if (image.isNull()) {
        return {};
    }
    image.setDevicePixelRatio(1.0);
    image.setColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGB32);
}

// Crops the scrolling viewport out of a root grab, falling back to per-display
// grabs the same way the single-frame provider does for independent X screens.
[[nodiscard]] inline QImage
cropScrollingSelectionFrame(const QVector<CapturedDisplayModel>& displays, const QImage& rootFrame,
                            const QRect& selection,
                            const std::function<QImage(QScreen&)>& perDisplayGrab) {
    if (selection.isEmpty()) {
        return {};
    }
    QRect bounds;
    for (const CapturedDisplayModel& display : displays) {
        bounds = bounds.united(display.physicalRect);
    }
    QImage frame = presentation::capture::qtcapture::cropDisplayFrame(rootFrame, bounds, selection);
    if (!frame.isNull()) {
        return frame;
    }
    for (const CapturedDisplayModel& display : displays) {
        if (display.screen == nullptr) {
            continue;
        }
        frame = presentation::capture::qtcapture::cropDisplayFrame(
            normalizeScrollingRootFrame(perDisplayGrab(*display.screen)), bounds, selection);
        if (!frame.isNull()) {
            return frame;
        }
    }
    return {};
}

// The stitch pipeline contract: RGBA8888 with tightly packed rows.
[[nodiscard]] inline QImage toScrollingFramePayload(QImage frame) {
    if (frame.isNull()) {
        return {};
    }
    frame.setDevicePixelRatio(1.0);
    frame.setColorSpace(QColorSpace::SRgb);
    return frame.convertToFormat(QImage::Format_RGBA8888);
}

[[nodiscard]] inline bool isDuplicateScrollingFrame(const QImage& previous, const QImage& current) {
    return !previous.isNull() && previous == current;
}

} // namespace snow_shot::capture_detail::qtscrolling
