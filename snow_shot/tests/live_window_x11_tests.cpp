// Opt-in X11 smoke test for live pinned window capture.
//
// Run against an isolated X server, for example:
//   SNOW_TEST_X11_DISPLAY=:8 SNOW_TEST_X11_WINDOW_TITLE=xclock \
//     snow-shot-live-window-x11-tests
//
// Without SNOW_TEST_X11_DISPLAY the test reports 77 (skipped) so automated
// suites without a display stay green.

#include "snow_shot/presentation/livewindowsource.h"

#include "../src/presentation/pinned/livewindownative_p.h"
#include "snow_shot/platform/linux/x11windowquery.h"

#include <QCoreApplication>
#include <QImage>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::optional<snow_shot::platform::x11::WindowStackEntry>
findWindow(const QString& titleSubstring) {
    for (const auto& entry : snow_shot::platform::x11::stackedTopLevelWindows()) {
        if (!entry.mapped || entry.skip || entry.frameRect.isEmpty())
            continue;
        if (!titleSubstring.isEmpty() && !entry.title.contains(titleSubstring, Qt::CaseInsensitive))
            continue;
        return entry;
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const QByteArray display = qgetenv("SNOW_TEST_X11_DISPLAY");
    if (display.isEmpty()) {
        std::cout << "SNOW_TEST_X11_DISPLAY is not set; skipping\n";
        return 77;
    }
    qputenv("DISPLAY", display);
    const QString title = QString::fromLocal8Bit(qgetenv("SNOW_TEST_X11_WINDOW_TITLE"));

    const auto window = findWindow(title);
    require(window.has_value(), "no matching top-level window was found");

    const QPoint center = window->frameRect.center();
    const auto target = snow_shot::presentation::liveWindowTargetAtPoint(center, {});
    require(target.has_value(), "live window targeting did not find the window");
    require(target->nativeId == window->windowId, "live window targeting returned another window");
    require(!target->title.isEmpty(), "live window targeting must keep the window title");

    auto monitor = snow_shot::presentation::live_window_detail::createNativeWindowMonitor(*target);
    require(monitor != nullptr, "live window monitor was not created");
    QString error;
    require(monitor->initialize(&error), error.toLatin1().constData());
    const auto state = monitor->inspect();
    require(state.state == snow_shot::presentation::LiveWindowState::Live,
            "an initialized window must report Live");
    require(state.desktopBounds == window->frameRect,
            "monitor bounds must match the reported frame");

    auto capture = snow_shot::presentation::live_window_detail::createNativeWindowCapture(*target);
    require(capture != nullptr, "live window capture was not created");
    snow_shot::presentation::live_window_detail::NativeCaptureFrame first;
    require(capture->receive(&first), "the first live capture failed");
    require(first.pixels != nullptr && !first.pixels->image().isNull(),
            "the first live capture produced no pixels");
    require(first.pixels->image().size() == window->frameRect.size(),
            "captured pixels must match the source window size");
    require(first.desktopBounds == QRectF(window->frameRect),
            "captured desktop bounds must match the source window");

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    snow_shot::presentation::live_window_detail::NativeCaptureFrame second;
    require(capture->receive(&second), "the second live capture failed");
    require(second.pixels != nullptr &&
                second.pixels->image().size() == first.pixels->image().size(),
            "consecutive live frames must share the source size");

    monitor->inspect();
    capture->cancel();
    capture.reset();
    monitor.reset();
    return 0;
}
