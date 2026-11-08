#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <optional>

struct _XDisplay;

namespace snow_shot::platform::x11 {

// X protocol modifier masks. Kept as local constants so the headers never
// include X11 headers: Xlib's Bool/Status/None macros poison every Qt header
// parsed after them. The implementation static_asserts the values match.
inline constexpr unsigned int kShiftMask = 1 << 0;
inline constexpr unsigned int kLockMask = 1 << 1;
inline constexpr unsigned int kControlMask = 1 << 2;
inline constexpr unsigned int kMod1Mask = 1 << 3;
inline constexpr unsigned int kMod2Mask = 1 << 4;
inline constexpr unsigned int kMod3Mask = 1 << 5;
inline constexpr unsigned int kMod4Mask = 1 << 6;
inline constexpr unsigned int kMod5Mask = 1 << 7;

struct NativeKeyGrab {
    unsigned int keycode = 0;
    unsigned int modifiers = 0;
};

// XGrabKey matches the raw event state, so NumLock (Mod2), CapsLock (Lock),
// and ScrollLock (Mod5) each require their own grab combination. Classification
// strips these bits before comparing against registered grabs.
[[nodiscard]] unsigned int grabModifierLockMask();
[[nodiscard]] unsigned int normalizeLockState(unsigned int state);
[[nodiscard]] QVector<unsigned int> lockMaskCombinations();

// Maps a Qt key to the registered X11 keysym name. Keypad keys resolve to
// their KP_* keysyms when the keypad flag is set; keys without a stable X11
// keysym name return nullopt. Function keys cap at F24 to match the Windows
// and macOS backends.
[[nodiscard]] std::optional<QString> keysymNameForQtKey(int qtKey, bool keypad);

// Reports whether a portable shortcut text parses into a single non-modifier
// chord whose key maps to an X11 keysym. Pure text check: it does not touch
// the display or the keyboard layout, so validation works without X.
[[nodiscard]] bool isMappableShortcut(const QString& portableText);

// Resolves a portable shortcut text such as "Ctrl+Alt+A" into the keycode and
// base modifier mask to grab on the given display. Character keysyms that are
// only reachable through a shifted column contribute the shift bit. Returns
// nullopt when the text is not a single non-modifier chord or the keysym has
// no keycode on the current keyboard layout.
[[nodiscard]] std::optional<NativeKeyGrab> resolveNativeKeyGrab(const QString& portableText,
                                                                _XDisplay* display);

// Finds the registered binding a KeyPress belongs to, ignoring lock-state
// bits. The registry maps registration ids to their grabs.
[[nodiscard]] std::optional<int> classifyKeyPress(unsigned int keycode, unsigned int rawState,
                                                  const QHash<int, NativeKeyGrab>& registry);

} // namespace snow_shot::platform::x11
