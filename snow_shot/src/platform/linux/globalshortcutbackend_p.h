#pragma once
#include "../../presentation/services/globalshortcutbackend_p.h"

#include <functional>

namespace snow_shot::presentation {

// Xlib call seams so the backend class can run against a fake in offscreen
// tests. The header stays free of X11 headers (Xlib's Bool/Status macros
// poison Qt declarations); display handles are opaque void* at this seam and
// the default implementation owns the typed Xlib calls.
//
// grabKey/ungrabKey grab every lock-mask combination (NumLock, CapsLock,
// ScrollLock) of the shortcut on every screen's root window and return the
// first X protocol error code observed (0 on success; BadAccess is 10, which
// maps to an already-occupied shortcut).
struct LinuxHotKeyInputApi {
    std::function<void*()> openDisplay;
    std::function<void(void*)> closeDisplay;
    std::function<unsigned int(void*, unsigned int, unsigned int)> grabKey;
    std::function<unsigned int(void*, unsigned int, unsigned int)> ungrabKey;
    std::function<int(void*)> connectionNumber;
    std::function<int(void*)> pending;
    // Pops the next queued KeyPress into {keycode, state}; false when the
    // queue holds no key event.
    std::function<bool(void*, unsigned int*, unsigned int*)> nextKeyPress;
    // XQueryKeymap: 32-byte bitmap, one bit per keycode.
    std::function<bool(void*, unsigned char*)> queryKeyMap;
    std::function<unsigned int(void*, const char*)> keycodeForKeysymName;
    // Keycodes currently bound to the given modifier mask.
    std::function<QVector<unsigned int>(void*, unsigned int)> modifierKeycodes;
    std::function<void()> installErrorHandlers;
};

inline constexpr unsigned int kX11BadAccess = 10;

[[nodiscard]] std::unique_ptr<GlobalShortcutBackend>
createLinuxGlobalShortcutBackend(LinuxHotKeyInputApi api);

[[nodiscard]] LinuxHotKeyInputApi defaultLinuxHotKeyInputApi();

} // namespace snow_shot::presentation
