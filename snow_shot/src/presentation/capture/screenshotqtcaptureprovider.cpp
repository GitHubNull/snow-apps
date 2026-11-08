#include "snow_shot/presentation/capture/screenshotqtcaptureprovider.h"

#include "screenshotqtcapturegeometry.h"

#include "snow_shot/platform/linux/x11cursor.h"
#include "snow_shot/presentation/screenshotgeometry.h"

#include <QColorSpace>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSet>

#include <utility>

namespace qtcapture = snow_shot::presentation::capture::qtcapture;

namespace {

QVector<QScreen*> captureScreens() {
    const QList<QScreen*> current = QGuiApplication::screens();
    QVector<QScreen*> screens;
    screens.reserve(current.size());
    for (QScreen* screen : current) {
        if (screen != nullptr) {
            screens.push_back(screen);
        }
    }
    return screens;
}

CapturedDisplayModel displayModelFor(QScreen& screen, qsizetype index, QSet<QString>* usedIds) {
    CapturedDisplayModel display = ScreenshotGeometryMapper::preCaptureDisplayModel(screen);
    display.stableId = qtcapture::displayStableId(screen.name(), index, usedIds);
    display.canvasRect = display.physicalRect;
    display.geometryResolved = true;
    return display;
}

QRect physicalBounds(const QVector<CapturedDisplayModel>& displays) {
    QRect bounds;
    for (const CapturedDisplayModel& display : displays) {
        bounds = bounds.united(display.physicalRect);
    }
    return bounds;
}

QImage normalizedFrame(QPixmap grabbed) {
    if (grabbed.isNull()) {
        return {};
    }
    QImage image = grabbed.toImage();
    // Capture geometry is physical pixels; a retained device pixel ratio would
    // scale the frame a second time when it is painted onto the overlay canvas.
    image.setDevicePixelRatio(1.0);
    image.setColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGB32);
}

} // namespace

QVector<CapturedDisplayModel> ScreenshotQtCaptureProvider::layoutDisplays() {
    const QVector<QScreen*> screens = captureScreens();
    QVector<CapturedDisplayModel> displays;
    QSet<QString> usedIds;
    displays.reserve(screens.size());
    for (qsizetype index = 0; index < screens.size(); ++index) {
        displays.push_back(displayModelFor(*screens.at(index), index, &usedIds));
    }
    return displays;
}

QString ScreenshotQtCaptureProvider::displayStableIdAt(const QPoint& position) {
    const QVector<QScreen*> screens = captureScreens();
    QSet<QString> usedIds;
    QString matched;
    // Stable ids are index-order dependent, so the dedupe set must observe
    // every screen even after the match is found.
    for (qsizetype index = 0; index < screens.size(); ++index) {
        QScreen* screen = screens.at(index);
        const QString stableId = qtcapture::displayStableId(screen->name(), index, &usedIds);
        if (screen->geometry().contains(position)) {
            matched = stableId;
        }
    }
    return matched;
}

QVector<CapturedDisplayModel> ScreenshotQtCaptureProvider::captureDisplays(QString* errorMessage,
                                                                           bool captureCursor) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    const QVector<QScreen*> screens = captureScreens();
    if (screens.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("No screens are available for capture");
        }
        return {};
    }

    // Sampling the cursor costs an X connection; skip it entirely when the
    // caller did not ask for the cursor.
    const snow_shot::platform::x11::X11CursorSample cursor =
        captureCursor ? snow_shot::platform::x11::fetchCursorImage()
                      : snow_shot::platform::x11::X11CursorSample{};

    QVector<CapturedDisplayModel> displays;
    QSet<QString> usedIds;
    displays.reserve(screens.size());
    for (qsizetype index = 0; index < screens.size(); ++index) {
        displays.push_back(displayModelFor(*screens.at(index), index, &usedIds));
    }
    const QRect bounds = physicalBounds(displays);

    // Qt can return the whole virtual desktop for QScreen::grabWindow(0) on X11, so
    // grab the root once and crop each display out of it. Fall back to a per-screen
    // grab when a crop does not cover the display, for example on separate X screens.
    QImage rootFrame;
    if (QScreen* primary = QGuiApplication::primaryScreen(); primary != nullptr) {
        rootFrame = normalizedFrame(primary->grabWindow(0));
    }

    bool complete = true;
    for (qsizetype index = 0; index < displays.size(); ++index) {
        const QRect displayRect = displays.at(index).physicalRect;
        QImage frame = qtcapture::cropDisplayFrame(rootFrame, bounds, displayRect);
        if (frame.isNull()) {
            frame = qtcapture::cropDisplayFrame(normalizedFrame(screens.at(index)->grabWindow(0)),
                                                bounds, displayRect);
        }
        if (frame.isNull()) {
            complete = false;
            break;
        }
        if (cursor.valid) {
            const qtcapture::CursorPlacement placement = qtcapture::cursorPlacement(
                displayRect, cursor.position, cursor.hotspot, cursor.image.size());
            if (placement.visible) {
                QPainter painter(&frame);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                painter.drawImage(placement.pixelRect.topLeft(), cursor.image,
                                  placement.sourceRect);
                painter.end();
                // The patch lets the color picker compensate for cursor pixels
                // covering the sampled point, mirroring the native backends.
                displays[index].cursorPatch = cursor.image.copy(placement.sourceRect);
                displays[index].cursorPixelRect = placement.pixelRect;
            }
        }
        displays[index].image = std::move(frame);
    }

    if (!complete) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("Failed to capture the X11 screen contents");
        }
        return {};
    }
    return displays;
}
