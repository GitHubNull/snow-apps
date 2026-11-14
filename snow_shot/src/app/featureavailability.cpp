#include "snow_shot/app/featureavailability.h"

#include <QCoreApplication>
#include <QGuiApplication>

#include <utility>

namespace snow_shot::app {

#if defined(Q_OS_LINUX)
// Qt's Wayland platform cannot grab screens; only the literal Wayland QPA
// blocks capture, so offscreen test integrations keep working.
bool captureBlockedByWaylandSession() {
    const QCoreApplication* instance = QCoreApplication::instance();
    return instance != nullptr && instance->inherits("QGuiApplication") &&
           QGuiApplication::platformName() == QStringLiteral("wayland");
}
#endif

std::optional<FeatureFamily> featureFamilyFor(presentation::GlobalShortcutAction action) {
    using Action = presentation::GlobalShortcutAction;
    switch (action) {
    case Action::Screenshot:
    case Action::ScreenshotDelay:
    case Action::ScreenshotOcr:
    case Action::ScreenshotOcrCopy:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotCopy:
    case Action::ScreenshotSave:
    case Action::ScreenshotQuickSave:
    case Action::ScreenshotFullScreen:
    case Action::ScreenshotFocusedWindow:
        return FeatureFamily::Screenshot;
    case Action::ScreenshotFixed:
    case Action::PinClipboardContent:
    case Action::RestoreLastClosedWindows:
    case Action::HideShowAllWindows:
    case Action::PinSelectedFiles:
        return FeatureFamily::PinToScreen;
    case Action::ScreenRecord:
    case Action::ScreenRecordCopy:
        return FeatureFamily::ScreenRecording;
    case Action::OpenScreenRecordingFolder:
    case Action::OpenCaptureHistory:
    case Action::GlobalCanvas:
    case Action::StitchSelectedImages:
    case Action::SplitSelectedImages:
    case Action::SwitchWindowGroup:
    case Action::OpenPinToScreenManagement:
    case Action::OpenSettings:
    case Action::TranslateSelectedText:
    case Action::ToggleWindowUnderCursorAlwaysOnTop:
    case Action::ToggleGlobalHotkeys:
    case Action::ToggleDisableOnFocusedFullscreenWindow:
        return std::nullopt;
    }
    return std::nullopt;
}

FeatureFamily featureFamilyFor(presentation::settings::SettingsGlobalMouseAction action) {
    using Action = presentation::settings::SettingsGlobalMouseAction;
    switch (action) {
    case Action::ScreenshotFixed:
        return FeatureFamily::PinToScreen;
    case Action::ScreenRecording:
        return FeatureFamily::ScreenRecording;
    case Action::ScreenshotCopy:
    case Action::ScreenshotOcr:
    case Action::ScreenshotOcrCopy:
    case Action::ScreenshotTranslation:
    case Action::ScreenshotQuickSave:
    case Action::ScreenshotSave:
        return FeatureFamily::Screenshot;
    }
    return FeatureFamily::Screenshot;
}

bool isFeatureAvailable(FeatureFamily feature) {
#if defined(Q_OS_LINUX)
    // X11 provides both the screenshot and recording capture paths. Wayland
    // sessions cannot be captured without a portal backend.
    if ((feature == FeatureFamily::Screenshot || feature == FeatureFamily::ScreenRecording) &&
        captureBlockedByWaylandSession()) {
        return false;
    }
    return true;
#else
    (void)feature;
    return true;
#endif
}

FeatureGate::FeatureGate(UnavailableHandler unavailableHandler)
    : m_unavailableHandler(std::move(unavailableHandler)) {}

bool FeatureGate::allow(FeatureFamily feature, bool notify) const {
    if (isFeatureAvailable(feature)) {
        return true;
    }
    if (notify && m_unavailableHandler) {
        m_unavailableHandler(feature);
    }
    return false;
}

FeatureActionRouter::FeatureActionRouter(FeatureGate::UnavailableHandler unavailableHandler)
    : m_gate(std::move(unavailableHandler)) {}

void FeatureActionRouter::setSuspended(bool suspended) {
    m_suspended = suspended;
}

bool FeatureActionRouter::dispatch(FeatureFamily feature, Action action, bool notify) const {
    if (m_suspended || !m_gate.allow(feature, notify)) {
        return false;
    }
    if (action) {
        action();
    }
    return true;
}

bool FeatureActionRouter::beginGesture(FeatureFamily feature, Action cancel, Action action) const {
    if (m_suspended || !m_gate.allow(feature)) {
        if (cancel) {
            cancel();
        }
        return false;
    }
    if (action) {
        action();
    }
    return true;
}
} // namespace snow_shot::app
