// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QHash>
#include <QSocketNotifier>
#include <QVector>
#include <QtGlobal>

#include "globalshortcutbackend_p.h"

#include "snow_shot/platform/linux/x11connection.h"
#include "snow_shot/platform/linux/x11grabkey.h"

#include <X11/X.h>
#include <X11/Xlib.h>

// Xlib's None/True/False macros clash with the shortcut enum and bool call
// sites below; the Xlib calls in this file pass bool literals instead.
#undef None
#undef True
#undef False

namespace snow_shot::presentation {
namespace {

// First X protocol error observed by the scoped error handler in
// grabOrUngrabOnAllRoots; reset before each grab batch. Registration runs on
// the GUI thread only, so the plain static is sufficient.
unsigned int firstGrabError = 0;

int grabErrorCatcher(Display*, XErrorEvent* error) {
    if (firstGrabError == 0) {
        firstGrabError = error->error_code;
    }
    return 0;
}

// XModifierKeymap lays out eight modifier groups (Shift, Lock, Control,
// Mod1..Mod5) of max_keypermod keycodes each; group N owns mask bit (1 << N).
QVector<unsigned int> modifierKeycodesForMask(Display* display, unsigned int mask) {
    QVector<unsigned int> keycodes;
    XModifierKeymap* modifiers = XGetModifierMapping(display);
    if (modifiers == nullptr) {
        return keycodes;
    }
    for (int group = 0; group < 8; ++group) {
        if ((mask & (1U << group)) == 0) {
            continue;
        }
        for (int entry = 0; entry < modifiers->max_keypermod; ++entry) {
            const unsigned int keycode = static_cast<unsigned int>(
                modifiers->modifiermap[group * modifiers->max_keypermod + entry]);
            if (keycode != 0) {
                keycodes.append(keycode);
            }
        }
    }
    XFreeModifiermap(modifiers);
    return keycodes;
}

void grabEveryCombinationOnAllRoots(Display* display, unsigned int keycode, unsigned int modifiers,
                                    bool grab) {
    const QVector<unsigned int> combinations = snow_shot::platform::x11::lockMaskCombinations();
    for (int screen = 0; screen < ScreenCount(display); ++screen) {
        const Window root = RootWindow(display, screen);
        for (const unsigned int combination : combinations) {
            if (grab) {
                XGrabKey(display, static_cast<int>(keycode), modifiers | combination, root, true,
                         GrabModeAsync, GrabModeAsync);
            } else {
                XUngrabKey(display, static_cast<int>(keycode), modifiers | combination, root);
            }
        }
    }
}

unsigned int grabOrUngrabOnAllRoots(Display* display, unsigned int keycode, unsigned int modifiers,
                                    bool grab) {
    firstGrabError = 0;
    // Grab errors arrive asynchronously; capture the first protocol error of
    // this batch before syncing so a competing owner surfaces deterministically.
    const auto previousHandler = XSetErrorHandler(grabErrorCatcher);
    grabEveryCombinationOnAllRoots(display, keycode, modifiers, grab);
    XSync(display, false);
    XSetErrorHandler(previousHandler);
    if (firstGrabError != 0 && grab) {
        // Roll back the combinations of this shortcut that did get grabbed so
        // a conflicting shortcut never ends up half-registered.
        grabEveryCombinationOnAllRoots(display, keycode, modifiers, false);
        XSync(display, false);
        return firstGrabError;
    }
    return 0;
}

} // namespace

class LinuxGlobalShortcutBackend final : public QObject, public GlobalShortcutBackend {
  public:
    explicit LinuxGlobalShortcutBackend(LinuxHotKeyInputApi api) : m_api(std::move(api)) {}
    ~LinuxGlobalShortcutBackend() override {
        releaseAll();
    }

    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
    }

    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        GlobalShortcutValidationResult result;
        result.binding = binding;
        result.shortcut = binding.portableText;
        const bool supported = snow_shot::platform::x11::isMappableShortcut(binding.portableText);
        result.supported = supported;
        result.failureReason = supported ? GlobalShortcutFailureReason::None
                                         : GlobalShortcutFailureReason::InvalidShortcut;
        return result;
    }

    GlobalShortcutBackendResult
    registerShortcut(int registrationId, const shortcuts::ShortcutBinding& binding) override {
        if (!ensureConnection()) {
            return {false, GlobalShortcutFailureReason::SystemError, 0};
        }
        const auto grab = snow_shot::platform::x11::resolveNativeKeyGrab(
            binding.portableText, static_cast<Display*>(m_display));
        if (!grab.has_value()) {
            return {false, GlobalShortcutFailureReason::InvalidShortcut, 0};
        }
        if (const unsigned int error = m_api.grabKey(m_display, grab->keycode, grab->modifiers);
            error != 0) {
            return {false,
                    error == kX11BadAccess ? GlobalShortcutFailureReason::AlreadyInUse
                                           : GlobalShortcutFailureReason::SystemError,
                    static_cast<qint64>(error)};
        }
        m_grabs.insert(registrationId, *grab);
        return {true, GlobalShortcutFailureReason::None, 0};
    }

    void unregisterShortcut(int registrationId) override {
        const auto grab = m_grabs.take(registrationId);
        if (grab.keycode != 0 && m_display != nullptr) {
            m_api.ungrabKey(m_display, grab.keycode, grab.modifiers);
        }
    }

    std::optional<GlobalShortcutInputState> inputState(int id) const override {
        if (m_display == nullptr) {
            return std::nullopt;
        }
        unsigned char keys[32] = {};
        if (!m_api.queryKeyMap(m_display, keys)) {
            return std::nullopt;
        }
        const auto down = [&keys](unsigned int keycode) {
            return keycode != 0 && keycode < 256 &&
                   (keys[keycode / 8] & (1U << (keycode % 8))) != 0;
        };
        GlobalShortcutInputState state;
        state.escapeDown = down(m_escapeKeycode);
        if (id == 0) {
            return state;
        }
        const snow_shot::platform::x11::NativeKeyGrab binding =
            m_grabs.value(id, snow_shot::platform::x11::NativeKeyGrab{});
        state.escapeIsShortcutKey = binding.keycode == m_escapeKeycode;
        state.anyShortcutKeyDown = down(binding.keycode);
        for (auto it = m_modifierKeycodes.cbegin(); it != m_modifierKeycodes.cend(); ++it) {
            if ((binding.modifiers & it.key()) == 0) {
                continue;
            }
            for (const unsigned int keycode : it.value()) {
                if (down(keycode)) {
                    state.anyShortcutKeyDown = true;
                    break;
                }
            }
        }
        return state;
    }

  private:
    bool ensureConnection() {
        if (m_display != nullptr) {
            return true;
        }
        if (m_api.installErrorHandlers) {
            m_api.installErrorHandlers();
        }
        m_display = m_api.openDisplay();
        if (m_display == nullptr) {
            return false;
        }
        m_escapeKeycode = m_api.keycodeForKeysymName(m_display, "Escape");
        for (const unsigned int mask :
             {snow_shot::platform::x11::kShiftMask, snow_shot::platform::x11::kControlMask,
              snow_shot::platform::x11::kMod1Mask, snow_shot::platform::x11::kMod4Mask}) {
            m_modifierKeycodes.insert(mask, m_api.modifierKeycodes(m_display, mask));
        }
        m_notifier =
            new QSocketNotifier(m_api.connectionNumber(m_display), QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { drainKeyEvents(); });
        return true;
    }

    void drainKeyEvents() {
        while (m_display != nullptr && m_api.pending(m_display) > 0) {
            unsigned int keycode = 0;
            unsigned int state = 0;
            if (!m_api.nextKeyPress(m_display, &keycode, &state)) {
                break;
            }
            if (!m_handler) {
                continue;
            }
            const auto registrationId =
                snow_shot::platform::x11::classifyKeyPress(keycode, state, m_grabs);
            if (registrationId.has_value()) {
                m_handler(*registrationId);
            }
        }
    }

    void releaseAll() {
        const QList<int> registrationIds = m_grabs.keys();
        for (const int registrationId : registrationIds) {
            unregisterShortcut(registrationId);
        }
        delete m_notifier;
        m_notifier = nullptr;
        if (m_display != nullptr) {
            m_api.closeDisplay(m_display);
            m_display = nullptr;
        }
    }

    LinuxHotKeyInputApi m_api;
    QHash<int, snow_shot::platform::x11::NativeKeyGrab> m_grabs;
    QHash<unsigned int, QVector<unsigned int>> m_modifierKeycodes;
    ActivationHandler m_handler;
    QSocketNotifier* m_notifier = nullptr;
    void* m_display = nullptr;
    unsigned int m_escapeKeycode = 0;
};

} // namespace snow_shot::presentation

namespace snow_shot::presentation {

LinuxHotKeyInputApi defaultLinuxHotKeyInputApi() {
    namespace x11 = snow_shot::platform::x11;
    LinuxHotKeyInputApi api;
    api.installErrorHandlers = [] { x11::installX11ErrorHandlers(); };
    api.openDisplay = [] { return static_cast<void*>(XOpenDisplay(nullptr)); };
    api.closeDisplay = [](void* display) { XCloseDisplay(static_cast<Display*>(display)); };
    api.grabKey = [](void* display, unsigned int keycode, unsigned int modifiers) {
        return grabOrUngrabOnAllRoots(static_cast<Display*>(display), keycode, modifiers, true);
    };
    api.ungrabKey = [](void* display, unsigned int keycode, unsigned int modifiers) {
        return grabOrUngrabOnAllRoots(static_cast<Display*>(display), keycode, modifiers, false);
    };
    api.connectionNumber = [](void* display) {
        return ConnectionNumber(static_cast<Display*>(display));
    };
    api.pending = [](void* display) { return XPending(static_cast<Display*>(display)); };
    api.nextKeyPress = [](void* display, unsigned int* keycode, unsigned int* state) {
        Display* connection = static_cast<Display*>(display);
        while (XPending(connection) > 0) {
            XEvent event{};
            XNextEvent(connection, &event);
            if (event.type == KeyPress) {
                *keycode = static_cast<unsigned int>(event.xkey.keycode);
                *state = static_cast<unsigned int>(event.xkey.state);
                return true;
            }
        }
        return false;
    };
    api.queryKeyMap = [](void* display, unsigned char* keys) {
        XQueryKeymap(static_cast<Display*>(display), reinterpret_cast<char*>(keys));
        return true;
    };
    api.keycodeForKeysymName = [](void* display, const char* name) {
        const KeySym keysym = XStringToKeysym(name);
        if (keysym == NoSymbol) {
            return 0U;
        }
        return static_cast<unsigned int>(XKeysymToKeycode(static_cast<Display*>(display), keysym));
    };
    api.modifierKeycodes = [](void* display, unsigned int mask) {
        return modifierKeycodesForMask(static_cast<Display*>(display), mask);
    };
    return api;
}

std::unique_ptr<GlobalShortcutBackend> createLinuxGlobalShortcutBackend(LinuxHotKeyInputApi api) {
    return std::make_unique<LinuxGlobalShortcutBackend>(std::move(api));
}

std::unique_ptr<GlobalShortcutBackend> createLinuxGlobalShortcutBackend() {
    return createLinuxGlobalShortcutBackend(defaultLinuxHotKeyInputApi());
}

} // namespace snow_shot::presentation
