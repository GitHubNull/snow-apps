// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include "snow_shot/presentation/globalmousemanager.h"

#include <QGuiApplication>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>

// Xlib defines Status/Bool/None as macros; this file uses the gesture API's
// nested Status enum and needs the C++ identifiers back.
#undef Status
#undef Bool
#undef None
#undef True
#undef False

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace snow_shot::presentation {
namespace {

GlobalMouseModifiers modifiersFromState(unsigned int state) {
    GlobalMouseModifiers result;
    if ((state & ControlMask) != 0)
        result |= GlobalMouseModifier::Control;
    if ((state & ShiftMask) != 0)
        result |= GlobalMouseModifier::Shift;
    if ((state & Mod1Mask) != 0)
        result |= GlobalMouseModifier::Alt;
    if ((state & Mod4Mask) != 0)
        result |= GlobalMouseModifier::Super;
    return result;
}

Qt::MouseButton buttonFromDetail(int detail) {
    switch (detail) {
    case 1:
        return Qt::LeftButton;
    case 2:
        return Qt::MiddleButton;
    case 3:
        return Qt::RightButton;
    case 8:
        return Qt::BackButton;
    case 9:
        return Qt::ForwardButton;
    default:
        return Qt::NoButton;
    }
}

class LinuxGlobalMouseBackend final : public GlobalMouseBackend {
  public:
    ~LinuxGlobalMouseBackend() override {
        stop();
    }

    void start(Handler handler, FailureHandler failure) override {
        stop();
        if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
            if (failure)
                failure(1);
            return;
        }
        m_handler = std::move(handler);
        m_failure = std::move(failure);
        m_stopping.store(false, std::memory_order_release);
        if (pipe(m_wakePipe.data()) != 0) {
            m_wakePipe = {-1, -1};
            if (m_failure)
                m_failure(2);
            return;
        }
        for (int& descriptor : m_wakePipe) {
            static_cast<void>(fcntl(descriptor, F_SETFD, FD_CLOEXEC));
            static_cast<void>(fcntl(descriptor, F_SETFL, O_NONBLOCK));
        }
        m_thread = std::thread([this] { run(); });
    }

    void stop() override {
        if (!m_thread.joinable()) {
            m_handler = {};
            m_failure = {};
            return;
        }
        m_stopping.store(true, std::memory_order_release);
        if (m_wakePipe[1] >= 0) {
            const char wake = 1;
            static_cast<void>(write(m_wakePipe[1], &wake, 1));
        }
        m_thread.join();
        for (int& descriptor : m_wakePipe) {
            if (descriptor >= 0)
                close(descriptor);
            descriptor = -1;
        }
        m_handler = {};
        m_failure = {};
    }

    void configure(const GlobalMouseConfiguration& configuration) override {
        std::lock_guard lock(m_configurationMutex);
        m_configuration = configuration;
    }

    void cancel(quint64 id) override {
        if (m_thread.joinable()) {
            // The gesture engine is only touched on the worker thread.
            m_pendingCancel.store(id, std::memory_order_release);
        }
    }

    void beginButtonDrag(settings::SettingsGlobalMouseAction action) override {
        if (m_thread.joinable()) {
            m_pendingButtonDrag.store(static_cast<int>(action), std::memory_order_release);
        }
    }

    GlobalMousePermissionState permissionState() const override {
        GlobalMousePermissionState state;
        if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
            state.status = GlobalMousePermissionState::Status::Ready;
            state.listenGranted = true;
            state.accessibilityGranted = true;
            state.tapAvailable = true;
        } else {
            state.status = GlobalMousePermissionState::Status::Unavailable;
        }
        return state;
    }

  private:
    struct PointerState {
        QPointF position;
        GlobalMouseModifiers modifiers;
        Qt::MouseButtons heldButtons;
    };

    PointerState samplePointer(Display* display, Window root) const {
        PointerState state;
        Window rootReturn = 0;
        Window child = 0;
        int rootX = 0;
        int rootY = 0;
        int winX = 0;
        int winY = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(display, root, &rootReturn, &child, &rootX, &rootY, &winX, &winY,
                           &mask)) {
            return state;
        }
        state.position = QPointF(rootX, rootY);
        state.modifiers = modifiersFromState(mask);
        if ((mask & Button1Mask) != 0)
            state.heldButtons |= Qt::LeftButton;
        if ((mask & Button2Mask) != 0)
            state.heldButtons |= Qt::MiddleButton;
        if ((mask & Button3Mask) != 0)
            state.heldButtons |= Qt::RightButton;
        return state;
    }

    GlobalMouseConfiguration configurationSnapshot() const {
        std::lock_guard lock(m_configurationMutex);
        return m_configuration;
    }

    bool activationEligible(const GlobalMouseConfiguration& configuration,
                            GlobalMouseModifiers modifiers) const {
        return configuration.captureAvailable &&
               std::any_of(
                   configuration.bindings.cbegin(), configuration.bindings.cend(),
                   [modifiers](const auto& binding) { return binding.modifiers == modifiers; });
    }

    void run() {
        Display* display = XOpenDisplay(nullptr);
        if (display == nullptr) {
            if (m_failure)
                m_failure(3);
            return;
        }
        int xiOpcode = 0;
        int xiEventBase = 0;
        int xiErrorBase = 0;
        int xiMajor = 2;
        int xiMinor = 2;
        if (!XQueryExtension(display, "XInputExtension", &xiOpcode, &xiEventBase, &xiErrorBase) ||
            XIQueryVersion(display, &xiMajor, &xiMinor) != Success) {
            XCloseDisplay(display);
            if (m_failure)
                m_failure(4);
            return;
        }
        const Window root = DefaultRootWindow(display);
        unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {};
        XISetMask(mask, XI_RawMotion);
        XISetMask(mask, XI_RawButtonPress);
        XISetMask(mask, XI_RawButtonRelease);
        XISetMask(mask, XI_RawKeyPress);
        XISetMask(mask, XI_RawKeyRelease);
        XIEventMask eventMask{};
        eventMask.deviceid = XIAllMasterDevices;
        eventMask.mask_len = sizeof(mask);
        eventMask.mask = mask;
        if (XISelectEvents(display, root, &eventMask, 1) != Success) {
            XCloseDisplay(display);
            if (m_failure)
                m_failure(5);
            return;
        }
        XFlush(display);
        const KeyCode escapeKey = XKeysymToKeycode(display, XK_Escape);

        GlobalMouseGesture gesture;
        const auto emitEvent = [this](const GlobalMouseInputResult& result) {
            if (result.event && m_handler)
                m_handler(*result.event);
        };
        while (!m_stopping.load(std::memory_order_acquire)) {
            if (XPending(display) == 0) {
                fd_set readable;
                FD_ZERO(&readable);
                const int displayDescriptor = ConnectionNumber(display);
                FD_SET(displayDescriptor, &readable);
                if (m_wakePipe[0] >= 0)
                    FD_SET(m_wakePipe[0], &readable);
                const int highest = std::max(displayDescriptor, m_wakePipe[0]);
                timeval slice{};
                slice.tv_usec = 20 * 1000;
                static_cast<void>(select(highest + 1, &readable, nullptr, nullptr, &slice));
                if (m_wakePipe[0] >= 0 && FD_ISSET(m_wakePipe[0], &readable)) {
                    char drain[16];
                    static_cast<void>(read(m_wakePipe[0], drain, sizeof(drain)));
                }
                continue;
            }
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type != GenericEvent || event.xcookie.extension != xiEventBase)
                continue;
            const int kind = event.xcookie.evtype;
            if (!XGetEventData(display, &event.xcookie))
                continue;
            const auto* raw = static_cast<const XIRawEvent*>(event.xcookie.data);
            const GlobalMouseConfiguration configuration = configurationSnapshot();
            const PointerState pointer = samplePointer(display, root);

            if (kind == XI_RawButtonPress || kind == XI_RawButtonRelease) {
                const Qt::MouseButton button = buttonFromDetail(raw->detail);
                if (button != Qt::NoButton && (configuration.captureAvailable ||
                                               gesture.pending() || gesture.needsMouseInput())) {
                    GlobalMouseInput input;
                    input.kind = kind == XI_RawButtonPress ? GlobalMouseInput::Kind::Press
                                                           : GlobalMouseInput::Kind::Release;
                    input.position = pointer.position;
                    input.button = button;
                    input.modifiers = pointer.modifiers;
                    input.heldButtons = pointer.heldButtons;
                    if (kind == XI_RawButtonPress)
                        input.heldButtons |= button;
                    emitEvent(gesture.handle(input, configuration));
                }
            } else if (kind == XI_RawMotion) {
                if (gesture.needsMouseInput()) {
                    GlobalMouseInput input;
                    input.kind = GlobalMouseInput::Kind::Move;
                    input.position = pointer.position;
                    input.modifiers = pointer.modifiers;
                    input.heldButtons = pointer.heldButtons;
                    emitEvent(gesture.handle(input, configuration));
                }
            } else if (kind == XI_RawKeyPress && raw->detail == escapeKey) {
                if (gesture.pending() || gesture.active()) {
                    GlobalMouseInput input;
                    input.kind = GlobalMouseInput::Kind::Cancel;
                    input.position = pointer.position;
                    input.modifiers = pointer.modifiers;
                    emitEvent(gesture.handle(input, configuration));
                }
            }
            XFreeEventData(display, &event.xcookie);

            const quint64 cancelId = m_pendingCancel.exchange(0, std::memory_order_acq_rel);
            if (cancelId != 0)
                gesture.cancel(cancelId);
            const int buttonDrag = m_pendingButtonDrag.exchange(-1, std::memory_order_acq_rel);
            if (buttonDrag >= 0 && (pointer.heldButtons & Qt::LeftButton) != 0 &&
                configuration.captureAvailable && !gesture.pending() &&
                !gesture.needsMouseInput()) {
                emitEvent(gesture.beginButtonDrag(
                    static_cast<settings::SettingsGlobalMouseAction>(buttonDrag),
                    pointer.position));
            }
        }
        XCloseDisplay(display);
    }

    Handler m_handler;
    FailureHandler m_failure;
    mutable std::mutex m_configurationMutex;
    GlobalMouseConfiguration m_configuration;
    std::atomic<bool> m_stopping{false};
    std::atomic<quint64> m_pendingCancel{0};
    std::atomic<int> m_pendingButtonDrag{-1};
    std::array<int, 2> m_wakePipe{-1, -1};
    std::thread m_thread;
};

} // namespace

std::unique_ptr<GlobalMouseBackend> createLinuxGlobalMouseBackend() {
    return std::make_unique<LinuxGlobalMouseBackend>();
}

} // namespace snow_shot::presentation
