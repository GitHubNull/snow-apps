#include "pinnedwindowstackingplatform.h"

#include <QGuiApplication>
#include <QHash>
#include <array>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
#if defined(Q_OS_LINUX)
#include "snow_shot/platform/linux/x11connection.h"
#include "snow_shot/platform/linux/x11windowquery.h"
#include <X11/Xlib.h>
#endif

namespace snow_shot::presentation {
#if defined(Q_OS_MACOS)
PinnedWindowStackingPlatform
createCocoaPinnedWindowStackingPlatform(PinnedWindowStackingMetrics* metrics);
#endif
#if defined(Q_OS_WIN)
namespace {
HWND handle(QWidget* window) {
    return window ? reinterpret_cast<HWND>(window->internalWinId()) : nullptr;
}
qint64 level(QWidget* window) {
    const auto hwnd = handle(window);
    return hwnd && (GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
}
constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
std::optional<PinnedStackingWindows> capture(const PinnedStackingWindows& windows,
                                             PinnedWindowStackingMetrics* metrics) {
    if (metrics)
        ++metrics->snapshots;
    struct Context {
        QHash<HWND, QPointer<QWidget>> wanted;
        PinnedStackingWindows ordered;
    } context;
    for (const auto& window : windows) {
        const auto hwnd = handle(window);
        if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd))
            context.wanted.insert(hwnd, window);
    }
    if (context.wanted.isEmpty())
        return context.ordered;
    const auto enumerate = [](HWND hwnd, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(data);
        const auto found = state.wanted.constFind(hwnd);
        if (found != state.wanted.cend() && *found && handle(*found) == hwnd)
            state.ordered.append(*found);
        return TRUE;
    };
    if (!EnumWindows(enumerate, reinterpret_cast<LPARAM>(&context)))
        return std::nullopt;
    return context.ordered;
}
bool place(QWidget* window, QWidget* above, QWidget* below, PinnedWindowStackingMetrics* metrics) {
    const auto hwnd = handle(window);
    if (!hwnd)
        return false;
    HWND after = nullptr;
    if (above && handle(above) && level(above) == level(window)) {
        after = handle(above);
    } else if (below && handle(below) && level(below) == level(window)) {
        after = GetWindow(handle(below), GW_HWNDPREV);
        if (after == hwnd)
            return true;
        // Crossing the topmost boundary with a sibling handle can change bands.
        if (!after || (!level(window) && (GetWindowLongPtr(after, GWL_EXSTYLE) & WS_EX_TOPMOST)))
            after = level(window) ? HWND_TOPMOST : HWND_TOP;
    } else {
        return true;
    }
    if (metrics)
        ++metrics->placements;
    return SetWindowPos(hwnd, after, 0, 0, 0, 0, kFlags) != FALSE;
}
bool apply(const PinnedStackingWindows& windows, PinnedWindowStackingMetrics* metrics) {
    struct Position {
        QPointer<QWidget> window;
        QPointer<QWidget> above;
        HWND hwnd;
        HWND after;
    };
    QVector<Position> positions;
    std::array<QPointer<QWidget>, 2> previous;
    std::array<PinnedStackingWindows, 2> expected;
    for (const auto& window : windows) {
        const auto hwnd = handle(window);
        if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd))
            continue;
        const auto band = static_cast<size_t>(level(window));
        expected[band].append(window);
        if (previous[band])
            positions.append({window, previous[band], hwnd, handle(previous[band])});
        previous[band] = window;
    }
    if (positions.isEmpty())
        return true;
    const auto matches = [&expected](const PinnedStackingWindows& actual) {
        std::array<PinnedStackingWindows, 2> observed;
        for (const auto& window : actual)
            observed[static_cast<size_t>(level(window))].append(window);
        return observed == expected;
    };
    // Progressive insertion usually already established the final stack. Reconciliation
    // needs only one scan in that case, with no redundant native placement messages.
    if (const auto current = capture(windows, metrics); current && matches(*current))
        return true;
    if (metrics)
        ++metrics->deferredBatches;
    HDWP batch = BeginDeferWindowPos(static_cast<int>(positions.size()));
    for (const auto& position : positions) {
        if (!batch)
            break;
        if (metrics)
            ++metrics->placements;
        batch = DeferWindowPos(batch, position.hwnd, position.after, 0, 0, 0, 0, kFlags);
    }
    bool success = batch && EndDeferWindowPos(batch);
    if (!success) {
        success = true;
        // A failed batch may have applied a prefix. Apply the complete intended chain once.
        for (const auto& position : positions) {
            if (metrics)
                ++metrics->placements;
            if (!position.window || !position.above || handle(position.window) != position.hwnd ||
                handle(position.above) != position.after ||
                !SetWindowPos(position.hwnd, position.after, 0, 0, 0, 0, kFlags))
                success = false;
        }
    }
    const auto actual = capture(windows, metrics);
    if (!actual)
        return false;
    return success && matches(*actual);
}
} // namespace
#endif

#if defined(Q_OS_LINUX)
namespace {
qint64 level(QWidget* window) {
    return window && (window->windowFlags() & Qt::WindowStaysOnTopHint) ? 1 : 0;
}

Window frameOf(QWidget* widget) {
    if (widget == nullptr || widget->internalWinId() == 0)
        return None;
    const auto windowId = static_cast<snow_shot::platform::x11::WindowId>(widget->internalWinId());
    const auto frame = snow_shot::platform::x11::frameWindowIdFor(windowId);
    return frame ? static_cast<Window>(*frame) : static_cast<Window>(windowId);
}

std::optional<PinnedStackingWindows> capture(const PinnedStackingWindows& windows,
                                             PinnedWindowStackingMetrics* metrics) {
    snow_shot::platform::x11::X11Connection connection;
    if (!connection.isValid())
        return std::nullopt;
    Display* display = connection.display();
    QHash<Window, QPointer<QWidget>> wanted;
    for (const auto& window : windows) {
        if (!window || !window->isVisible())
            continue;
        const Window frame = frameOf(window);
        if (frame != None)
            wanted.insert(frame, window);
    }
    if (metrics)
        ++metrics->snapshots;
    if (wanted.isEmpty())
        return PinnedStackingWindows{};
    Window rootReturn = None;
    Window parentReturn = None;
    Window* children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, DefaultRootWindow(display), &rootReturn, &parentReturn, &children,
                    &count)) {
        return std::nullopt;
    }
    PinnedStackingWindows ordered;
    // XQueryTree reports bottom-to-top; the platform contract is front-to-back.
    for (int index = static_cast<int>(count) - 1; index >= 0; --index) {
        const auto found = wanted.constFind(children[index]);
        if (found != wanted.cend() && *found)
            ordered.append(*found);
    }
    if (children != nullptr)
        XFree(children);
    return ordered;
}

bool place(QWidget* window, QWidget* above, QWidget* below, PinnedWindowStackingMetrics* metrics) {
    if (window == nullptr)
        return false;
    if (above && level(above) != level(window))
        above = nullptr;
    if (below && level(below) != level(window))
        below = nullptr;
    if (!above && !below)
        return true;
    snow_shot::platform::x11::X11Connection connection;
    if (!connection.isValid())
        return false;
    const Window frame = frameOf(window);
    const Window sibling = frameOf(above ? above : below);
    if (frame == None || sibling == None)
        return false;
    XWindowChanges changes{};
    changes.sibling = sibling;
    changes.stack_mode = above ? Above : Below;
    if (metrics)
        ++metrics->placements;
    XConfigureWindow(connection.display(), frame, CWSibling | CWStackMode, &changes);
    XFlush(connection.display());
    return true;
}

bool apply(const PinnedStackingWindows& windows, PinnedWindowStackingMetrics* metrics) {
    // Restack each band back-to-front so the listed front-most surface ends up
    // on top of its band while topmost and normal bands stay intact.
    QPointer<QWidget> previous[2];
    for (qsizetype index = windows.size() - 1; index >= 0; --index) {
        const QPointer<QWidget>& window = windows.at(index);
        if (!window || !window->isVisible())
            continue;
        const auto band = static_cast<size_t>(level(window));
        if (previous[band] && !place(window, previous[band], nullptr, metrics))
            return false;
        previous[band] = window;
    }
    return true;
}
} // namespace
#endif

PinnedWindowStackingPlatform
createPinnedWindowStackingPlatform(PinnedWindowStackingMetrics* metrics) {
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == QStringLiteral("windows"))
        return {[metrics](const auto& windows) { return capture(windows, metrics); }, level,
                [metrics](auto* window, auto* above, auto* below) {
                    return place(window, above, below, metrics);
                },
                [metrics](const auto& windows) { return apply(windows, metrics); }};
#elif defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        return createCocoaPinnedWindowStackingPlatform(metrics);
#elif defined(Q_OS_LINUX)
    if (QGuiApplication::platformName() == QStringLiteral("xcb"))
        return {[metrics](const auto& windows) { return capture(windows, metrics); }, level,
                [metrics](auto* window, auto* above, auto* below) {
                    return place(window, above, below, metrics);
                },
                [metrics](const auto& windows) { return apply(windows, metrics); }};
#endif
    Q_UNUSED(metrics);
    return {};
}
} // namespace snow_shot::presentation
