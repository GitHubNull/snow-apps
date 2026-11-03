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

} // namespace snow_shot::presentation::capture::qtcapture

#endif // SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREGEOMETRY_H
