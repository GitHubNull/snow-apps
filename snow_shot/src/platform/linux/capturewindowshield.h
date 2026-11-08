#pragma once

#include <QPointer>
#include <QVector>

class QWidget;

namespace snow_shot::platform::x11 {

// Snapshot of the widgets a capture concealment hid, so only they are brought
// back after the grab. Dead widgets are tolerated through QPointer.
struct CaptureWindowShieldState {
    QVector<QPointer<QWidget>> concealed;
};

// Collects every visible top-level widget of this application: floating
// toolbar, main window, pinned windows, and any capture overlay the session
// already presented. GUI thread only.
[[nodiscard]] QVector<QWidget*> visibleTopLevelWidgets();

// Hides every visible widget in the list and waits until the X server stops
// rendering them (bounded). Windows that fail to unmap in time only produce a
// warning: a slightly stale toolbar in the frame beats skipping the capture.
[[nodiscard]] CaptureWindowShieldState concealWidgetsForCapture(const QVector<QWidget*>& widgets);

// Re-shows exactly the widgets the concealment step hid. GUI thread only.
void restoreWidgetsAfterCapture(CaptureWindowShieldState state);

} // namespace snow_shot::platform::x11
