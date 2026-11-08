#include "directcaptureqtx11.h"

#include "snow_shot/platform/linux/x11windowquery.h"
#include "snow_shot/presentation/capture/screenshotqtcaptureprovider.h"

#include <snow_capture.h>

#include <QColorSpace>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QPixmap>
#include <QRect>
#include <QScreen>
#include <QSemaphore>

#include <memory>
#include <optional>
#include <utility>

namespace snow_shot::presentation {
namespace {

namespace x11 = snow_shot::platform::x11;

constexpr int kGuiGrabTimeoutMs = 3000;

QImage normalizedFrame(QPixmap grabbed) {
    if (grabbed.isNull()) {
        return {};
    }
    QImage image = grabbed.toImage();
    // Capture geometry is physical pixels; a retained device pixel ratio would
    // scale the frame a second time when it is painted onto other surfaces.
    image.setDevicePixelRatio(1.0);
    image.setColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGB32);
}

qsizetype displayIndexForMonitorName(const QVector<CapturedDisplayModel>& displays,
                                     const QString& monitorName) {
    if (monitorName.isEmpty()) {
        return -1;
    }
    for (qsizetype index = 0; index < displays.size(); ++index) {
        // Mirrors the native backend: the monitor identity may arrive as the
        // stable display id or the raw display name.
        if (displays.at(index).stableId == monitorName || displays.at(index).name == monitorName) {
            return index;
        }
    }
    return -1;
}

qsizetype displayIndexUnderCursor(const QVector<CapturedDisplayModel>& displays) {
    const QPoint cursor = QCursor::pos();
    for (qsizetype index = 0; index < displays.size(); ++index) {
        if (displays.at(index).logicalRect.contains(cursor)) {
            return index;
        }
    }
    return -1;
}

DirectCaptureDisplay directDisplayFromModel(const CapturedDisplayModel& model) {
    DirectCaptureDisplay display;
    display.image = model.image;
    display.physicalBounds = model.physicalRect;
    display.stableId = model.stableId;
    display.name = model.name;
    display.logicalBounds = model.logicalRect;
    display.nativeDisplayId = model.nativeDisplayId;
    return display;
}

// Runs on the GUI thread: the only place Qt screen APIs are touched.
DirectCaptureFrame grabOnGuiThread(const DirectCaptureRequest& request) {
    DirectCaptureFrame frame;
    QString error;
    const QVector<CapturedDisplayModel> displays =
        ScreenshotQtCaptureProvider::captureDisplays(&error);
    if (displays.isEmpty()) {
        frame.error = error.isEmpty()
                          ? QCoreApplication::translate("DirectCaptureController",
                                                        "No screens are available for capture")
                          : error;
        return frame;
    }

    if (request.target == DirectCaptureTarget::CurrentMonitor) {
        qsizetype selected = displayIndexForMonitorName(displays, request.monitorName);
        if (selected < 0) {
            selected = displayIndexUnderCursor(displays);
        }
        if (selected < 0) {
            selected = 0;
        }
        const CapturedDisplayModel& chosen = displays.at(selected);
        if (request.admitRaster && !request.admitRaster(chosen.image.size())) {
            frame.error = QStringLiteral("resource_limit");
            return frame;
        }
        frame.image = chosen.image;
        frame.physicalBounds = chosen.physicalRect;
        frame.identity = chosen.stableId;
        frame.logicalBounds = chosen.logicalRect;
        frame.backend = SNOW_CAPTURE_BACKEND_X11_QT;
        for (const CapturedDisplayModel& model : displays) {
            frame.displays.push_back(directDisplayFromModel(model));
        }
        return frame;
    }

    const auto frameWindow = x11::frameWindowIdFor(request.window);
    if (!frameWindow.has_value()) {
        frame.error = QCoreApplication::translate("DirectCaptureController",
                                                  "The focused window is no longer visible");
        return frame;
    }
    const auto geometry = x11::windowGlobalGeometry(*frameWindow);
    if (!geometry.has_value() || geometry->isEmpty()) {
        frame.error = QCoreApplication::translate("DirectCaptureController",
                                                  "The focused window is no longer visible");
        return frame;
    }
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary == nullptr) {
        frame.error = QCoreApplication::translate("DirectCaptureController",
                                                  "No screens are available for capture");
        return frame;
    }
    // Crop the focused window (including its manager frame) out of the root
    // frame; occluded windows cannot be grabbed directly on X11.
    const QImage rootFrame = normalizedFrame(primary->grabWindow(0));
    const QRect visible = geometry->intersected(rootFrame.rect());
    if (rootFrame.isNull() || visible.isEmpty()) {
        frame.error = QCoreApplication::translate("DirectCaptureController",
                                                  "The focused window is no longer visible");
        return frame;
    }
    if (request.admitRaster && !request.admitRaster(visible.size())) {
        frame.error = QStringLiteral("resource_limit");
        return frame;
    }
    frame.image = rootFrame.copy(visible);
    frame.physicalBounds = visible;
    frame.identity = QStringLiteral("window:%1").arg(request.window);
    frame.backend = SNOW_CAPTURE_BACKEND_X11_QT;
    return frame;
}

struct GuiGrabState {
    QSemaphore done;
    DirectCaptureFrame frame;
};

DirectCaptureFrame grabViaGuiThread(const DirectCaptureRequest& request) {
    auto state = std::make_shared<GuiGrabState>();
    // The shared state keeps the GUI-side lambda valid even when the worker
    // gives up before the grab runs.
    const bool queued = QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [state, request]() {
            state->frame = grabOnGuiThread(request);
            state->done.release();
        },
        Qt::QueuedConnection);
    DirectCaptureFrame frame;
    if (!queued || !state->done.tryAcquire(1, kGuiGrabTimeoutMs)) {
        frame.error =
            QCoreApplication::translate("DirectCaptureController", "The capture request timed out");
        return frame;
    }
    frame = std::move(state->frame);
    return frame;
}

} // namespace

DirectCaptureFrame captureDirectTargetQt(const DirectCaptureRequest& request) {
    DirectCaptureRequest local = request;
    if (local.target == DirectCaptureTarget::FocusedWindow && local.window == 0) {
        // The controller fills the window for tray and shortcut entries, but
        // MCP callers may not; resolve the active window here instead.
        if (const auto active = x11::activeWindowId()) {
            local.window = static_cast<quintptr>(*active);
        } else {
            DirectCaptureFrame frame;
            frame.error = QCoreApplication::translate("DirectCaptureController",
                                                      "No active window is available to capture");
            return frame;
        }
    }
    return grabViaGuiThread(local);
}

} // namespace snow_shot::presentation
