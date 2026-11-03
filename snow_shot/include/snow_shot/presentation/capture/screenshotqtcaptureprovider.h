#ifndef SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREPROVIDER_H
#define SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREPROVIDER_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QCoreApplication>
#include <QString>
#include <QVector>

// Linux has no capture backend behind the snow_capture FFI. This provider keeps the
// backend-neutral display contract with QScreen::grabWindow so the existing capture
// workflow, overlays, and editor run unchanged on X11.
class ScreenshotQtCaptureProvider {
    Q_DECLARE_TR_FUNCTIONS(ScreenshotQtCaptureProvider)

  public:
    // Layout snapshot identities, index-aligned with QGuiApplication::screens().
    [[nodiscard]] static QVector<CapturedDisplayModel> layoutDisplays();

    // Grabs one frame per display. Returns an empty vector and sets errorMessage
    // when no complete frame set can be acquired.
    [[nodiscard]] static QVector<CapturedDisplayModel> captureDisplays(QString* errorMessage);
};

#endif // SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTQTCAPTUREPROVIDER_H
