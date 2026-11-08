#ifndef SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREGEOMETRY_H

#include <QImage>
#include <QRect>
#include <QSet>
#include <QString>

// Pure helpers shared by the Linux Qt capture provider and its geometry tests.
namespace snow_shot::presentation::capture::qtcapture {

// Stable display ids must match between the layout snapshot and the captured
// frames, and must stay unique even when QScreen::name() is empty or repeated.
[[nodiscard]] inline QString displayStableId(const QString& screenName, qsizetype index,
                                             QSet<QString>* usedIds) {
    QString stableId = screenName.trimmed();
    if (!stableId.isEmpty()) {
        stableId.prepend(QStringLiteral("display:"));
    }
    if (stableId.isEmpty() || usedIds->contains(stableId)) {
        stableId = QStringLiteral("display:qt-%1").arg(index);
    }
    usedIds->insert(stableId);
    return stableId;
}

// Maps one display out of a virtual-desktop frame. A frame that already has the
// display's size (a per-screen grab) is returned unchanged.
[[nodiscard]] inline QImage cropDisplayFrame(const QImage& frame, const QRect& virtualBounds,
                                             const QRect& displayRect) {
    if (frame.isNull() || virtualBounds.isEmpty() || displayRect.isEmpty()) {
        return {};
    }
    if (frame.size() == displayRect.size()) {
        return frame;
    }
    if (frame.size() != virtualBounds.size()) {
        return {};
    }
    const QRect cropRect =
        displayRect.translated(-virtualBounds.topLeft()).intersected(frame.rect());
    if (cropRect.size() != displayRect.size()) {
        return {};
    }
    return frame.copy(cropRect);
}

// Placement of a global-positioned cursor image inside one display's frame.
struct CursorPlacement {
    // Destination rect in display-local (physical) pixels.
    QRect pixelRect;
    // Source rect inside the cursor image after clipping to the display.
    QRect sourceRect;
    bool visible = false;
};

[[nodiscard]] inline CursorPlacement cursorPlacement(const QRect& displayRect,
                                                     const QPoint& cursorGlobalPos,
                                                     const QPoint& hotspot,
                                                     const QSize& cursorSize) {
    CursorPlacement placement;
    if (displayRect.isEmpty() || cursorSize.isEmpty()) {
        return placement;
    }
    // The hotspot anchors the cursor image's active pixel to the pointer
    // position, so the image's top-left sits one hotspot up-left of it.
    const QRect globalRect(
        QPoint(cursorGlobalPos.x() - hotspot.x(), cursorGlobalPos.y() - hotspot.y()), cursorSize);
    const QRect clipped = globalRect.intersected(displayRect);
    if (clipped.isEmpty()) {
        return placement;
    }
    placement.pixelRect = clipped.translated(-displayRect.topLeft());
    placement.sourceRect = clipped.translated(-globalRect.topLeft());
    placement.visible = true;
    return placement;
}

} // namespace snow_shot::presentation::capture::qtcapture

#endif // SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREGEOMETRY_H
