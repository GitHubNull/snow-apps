#include "capturewindowshield.h"

#include "snow_shot/platform/linux/x11windowquery.h"

#include <QApplication>
#include <QWidget>
#include <QtGlobal>

namespace snow_shot::platform::x11 {
namespace {

constexpr int kUnviewableTimeoutMs = 600;

} // namespace

QVector<QWidget*> visibleTopLevelWidgets() {
    QVector<QWidget*> visible;
    const QWidgetList topLevels = QApplication::topLevelWidgets();
    visible.reserve(topLevels.size());
    for (QWidget* widget : topLevels) {
        if (widget != nullptr && widget->isWindow() && widget->isVisible()) {
            visible.push_back(widget);
        }
    }
    return visible;
}

CaptureWindowShieldState concealWidgetsForCapture(const QVector<QWidget*>& widgets) {
    CaptureWindowShieldState state;
    QVector<WindowId> windowIds;
    windowIds.reserve(widgets.size());
    for (QWidget* widget : widgets) {
        if (widget == nullptr || !widget->isVisible()) {
            continue;
        }
        state.concealed.append(widget);
        widget->hide();
        windowIds.append(widget->winId());
    }
    if (!windowIds.isEmpty() && !waitForWindowsUnviewable(windowIds, kUnviewableTimeoutMs)) {
        qWarning("capture window shield: windows still viewable after %d ms", kUnviewableTimeoutMs);
    }
    return state;
}

void restoreWidgetsAfterCapture(CaptureWindowShieldState state) {
    for (const QPointer<QWidget>& widget : std::as_const(state.concealed)) {
        if (widget != nullptr && !widget->isVisible()) {
            widget->show();
        }
    }
}

} // namespace snow_shot::platform::x11
