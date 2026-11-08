// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include <QKeySequence>
#include <Qt>

#include "snow_shot/platform/linux/x11grabkey.h"

#include <X11/X.h>
#include <X11/Xlib.h>

namespace snow_shot::platform::x11 {
namespace {

static_assert(kShiftMask == ShiftMask && kLockMask == LockMask && kControlMask == ControlMask &&
                  kMod1Mask == Mod1Mask && kMod2Mask == Mod2Mask && kMod3Mask == Mod3Mask &&
                  kMod4Mask == Mod4Mask && kMod5Mask == Mod5Mask,
              "local modifier masks must match the X protocol values");

bool isModifierOnlyKey(int qtKey) {
    switch (qtKey) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_NumLock:
    case Qt::Key_CapsLock:
    case Qt::Key_ScrollLock:
        return true;
    default:
        return false;
    }
}

unsigned int baseModifiersFromQt(Qt::KeyboardModifiers qtModifiers) {
    unsigned int modifiers = 0;
    if (qtModifiers.testFlag(Qt::ControlModifier)) {
        modifiers |= kControlMask;
    }
    if (qtModifiers.testFlag(Qt::AltModifier)) {
        modifiers |= kMod1Mask;
    }
    if (qtModifiers.testFlag(Qt::ShiftModifier)) {
        modifiers |= kShiftMask;
    }
    if (qtModifiers.testFlag(Qt::MetaModifier)) {
        modifiers |= kMod4Mask;
    }
    return modifiers;
}

} // namespace

unsigned int grabModifierLockMask() {
    return kLockMask | kMod2Mask | kMod5Mask;
}

unsigned int normalizeLockState(unsigned int state) {
    return state & ~grabModifierLockMask();
}

QVector<unsigned int> lockMaskCombinations() {
    QVector<unsigned int> combinations;
    combinations.reserve(8);
    for (unsigned int caps = 0; caps <= kLockMask; caps += kLockMask) {
        for (unsigned int num = 0; num <= kMod2Mask; num += kMod2Mask) {
            for (unsigned int scroll = 0; scroll <= kMod5Mask; scroll += kMod5Mask) {
                combinations.append(caps | num | scroll);
            }
        }
    }
    return combinations;
}

std::optional<QString> keysymNameForQtKey(int qtKey, bool keypad) {
    // Letters and digits share the Qt key codes' ASCII range; X11 keysym names
    // for them are the lowercase character and the digit itself.
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        return QString(QChar::fromLatin1(static_cast<char>('a' + (qtKey - Qt::Key_A))));
    }
    if (keypad) {
        // Keypad chords map only to their KP_* keysyms; X11 has no keypad
        // variants of function or navigation keys.
        if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
            return QStringLiteral("KP_%1").arg(qtKey - Qt::Key_0);
        }
        switch (qtKey) {
        case Qt::Key_Plus:
            return QStringLiteral("KP_Add");
        case Qt::Key_Minus:
            return QStringLiteral("KP_Subtract");
        case Qt::Key_Asterisk:
            return QStringLiteral("KP_Multiply");
        case Qt::Key_Slash:
            return QStringLiteral("KP_Divide");
        case Qt::Key_Period:
            return QStringLiteral("KP_Decimal");
        case Qt::Key_Enter:
            return QStringLiteral("KP_Enter");
        default:
            return std::nullopt;
        }
    }
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) {
        return QString(QChar::fromLatin1(static_cast<char>('0' + (qtKey - Qt::Key_0))));
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return QStringLiteral("F%1").arg(qtKey - Qt::Key_F1 + 1);
    }
    switch (qtKey) {
    case Qt::Key_Space:
        return QStringLiteral("space");
    case Qt::Key_Tab:
        return QStringLiteral("Tab");
    case Qt::Key_Backtab:
        return QStringLiteral("ISO_Left_Tab");
    case Qt::Key_Backspace:
        return QStringLiteral("BackSpace");
    case Qt::Key_Return:
        return QStringLiteral("Return");
    case Qt::Key_Escape:
        return QStringLiteral("Escape");
    case Qt::Key_Delete:
        return QStringLiteral("Delete");
    case Qt::Key_Insert:
        return QStringLiteral("Insert");
    case Qt::Key_Home:
        return QStringLiteral("Home");
    case Qt::Key_End:
        return QStringLiteral("End");
    case Qt::Key_PageUp:
        return QStringLiteral("Prior");
    case Qt::Key_PageDown:
        return QStringLiteral("Next");
    case Qt::Key_Left:
        return QStringLiteral("Left");
    case Qt::Key_Right:
        return QStringLiteral("Right");
    case Qt::Key_Up:
        return QStringLiteral("Up");
    case Qt::Key_Down:
        return QStringLiteral("Down");
    case Qt::Key_Print:
        return QStringLiteral("Print");
    case Qt::Key_SysReq:
        return QStringLiteral("Sys_Req");
    case Qt::Key_Pause:
        return QStringLiteral("Pause");
    case Qt::Key_NumLock:
        return QStringLiteral("Num_Lock");
    case Qt::Key_CapsLock:
        return QStringLiteral("Caps_Lock");
    case Qt::Key_ScrollLock:
        return QStringLiteral("Scroll_Lock");
    case Qt::Key_Menu:
        return QStringLiteral("Menu");
    case Qt::Key_QuoteLeft:
        return QStringLiteral("grave");
    case Qt::Key_QuoteDbl:
        return QStringLiteral("quotedbl");
    case Qt::Key_Exclam:
        return QStringLiteral("exclam");
    case Qt::Key_NumberSign:
        return QStringLiteral("numbersign");
    case Qt::Key_Dollar:
        return QStringLiteral("dollar");
    case Qt::Key_Percent:
        return QStringLiteral("percent");
    case Qt::Key_Ampersand:
        return QStringLiteral("ampersand");
    case Qt::Key_Apostrophe:
        return QStringLiteral("apostrophe");
    case Qt::Key_ParenLeft:
        return QStringLiteral("parenleft");
    case Qt::Key_ParenRight:
        return QStringLiteral("parenright");
    case Qt::Key_Asterisk:
        return QStringLiteral("asterisk");
    case Qt::Key_Plus:
        return QStringLiteral("plus");
    case Qt::Key_Comma:
        return QStringLiteral("comma");
    case Qt::Key_Minus:
        return QStringLiteral("minus");
    case Qt::Key_Period:
        return QStringLiteral("period");
    case Qt::Key_Slash:
        return QStringLiteral("slash");
    case Qt::Key_Colon:
        return QStringLiteral("colon");
    case Qt::Key_Semicolon:
        return QStringLiteral("semicolon");
    case Qt::Key_Less:
        return QStringLiteral("less");
    case Qt::Key_Equal:
        return QStringLiteral("equal");
    case Qt::Key_Greater:
        return QStringLiteral("greater");
    case Qt::Key_Question:
        return QStringLiteral("question");
    case Qt::Key_At:
        return QStringLiteral("at");
    case Qt::Key_BracketLeft:
        return QStringLiteral("bracketleft");
    case Qt::Key_Backslash:
        return QStringLiteral("backslash");
    case Qt::Key_BracketRight:
        return QStringLiteral("bracketright");
    case Qt::Key_AsciiCircum:
        return QStringLiteral("asciicircum");
    case Qt::Key_Underscore:
        return QStringLiteral("underscore");
    case Qt::Key_Bar:
        return QStringLiteral("bar");
    case Qt::Key_BraceLeft:
        return QStringLiteral("braceleft");
    case Qt::Key_BraceRight:
        return QStringLiteral("braceright");
    case Qt::Key_AsciiTilde:
        return QStringLiteral("asciitilde");
    case Qt::Key_VolumeDown:
        return QStringLiteral("XF86AudioLowerVolume");
    case Qt::Key_VolumeUp:
        return QStringLiteral("XF86AudioRaiseVolume");
    case Qt::Key_VolumeMute:
        return QStringLiteral("XF86AudioMute");
    case Qt::Key_MediaNext:
        return QStringLiteral("XF86AudioNext");
    case Qt::Key_MediaPrevious:
        return QStringLiteral("XF86AudioPrev");
    case Qt::Key_MediaPlay:
        return QStringLiteral("XF86AudioPlay");
    case Qt::Key_MediaPause:
        return QStringLiteral("XF86AudioPause");
    case Qt::Key_MediaStop:
        return QStringLiteral("XF86AudioStop");
    case Qt::Key_MailForward:
        return QStringLiteral("XF86MailForward");
    case Qt::Key_HomePage:
        return QStringLiteral("XF86HomePage");
    case Qt::Key_Search:
        return QStringLiteral("XF86Search");
    case Qt::Key_Back:
        return QStringLiteral("XF86Back");
    case Qt::Key_Forward:
        return QStringLiteral("XF86Forward");
    case Qt::Key_Refresh:
        return QStringLiteral("XF86Refresh");
    case Qt::Key_Stop:
        return QStringLiteral("XF86Stop");
    case Qt::Key_Favorites:
        return QStringLiteral("XF86Favorites");
    case Qt::Key_LaunchMedia:
        return QStringLiteral("XF86AudioMedia");
    case Qt::Key_Calculator:
        return QStringLiteral("XF86Calculator");
    default:
        return std::nullopt;
    }
}

bool isMappableShortcut(const QString& portableText) {
    const QKeySequence sequence =
        QKeySequence::fromString(portableText, QKeySequence::PortableText);
    if (sequence.count() != 1) {
        return false;
    }
    const QKeyCombination combination = sequence[0];
    const int qtKey = combination.key();
    if (qtKey == Qt::Key_unknown || isModifierOnlyKey(qtKey)) {
        return false;
    }
    return keysymNameForQtKey(qtKey, combination.keyboardModifiers().testFlag(Qt::KeypadModifier))
        .has_value();
}

std::optional<NativeKeyGrab> resolveNativeKeyGrab(const QString& portableText, _XDisplay* display) {
    const QKeySequence sequence =
        QKeySequence::fromString(portableText, QKeySequence::PortableText);
    if (display == nullptr || sequence.count() != 1) {
        return std::nullopt;
    }
    const QKeyCombination combination = sequence[0];
    const int qtKey = combination.key();
    if (qtKey == Qt::Key_unknown || isModifierOnlyKey(qtKey)) {
        return std::nullopt;
    }
    const Qt::KeyboardModifiers qtModifiers = combination.keyboardModifiers();
    unsigned int modifiers = baseModifiersFromQt(qtModifiers);
    const auto keysymName = keysymNameForQtKey(qtKey, qtModifiers.testFlag(Qt::KeypadModifier));
    if (!keysymName.has_value()) {
        return std::nullopt;
    }
    const KeySym keysym = XStringToKeysym(keysymName->toLatin1().constData());
    if (keysym == NoSymbol) {
        return std::nullopt;
    }
    const unsigned int keycode = XKeysymToKeycode(display, keysym);
    if (keycode == 0) {
        return std::nullopt;
    }
    // A keysym that lives in the shifted column of its keycode requires the
    // shift bit in every grab so the raw event state matches.
    int keysymsPerKeycode = 0;
    KeySym* mapping =
        XGetKeyboardMapping(display, static_cast<KeyCode>(keycode), 1, &keysymsPerKeycode);
    if (mapping != nullptr) {
        if (keysymsPerKeycode > 1 && mapping[0] != keysym && mapping[1] == keysym) {
            modifiers |= kShiftMask;
        }
        XFree(mapping);
    }
    return NativeKeyGrab{keycode, modifiers};
}

std::optional<int> classifyKeyPress(unsigned int keycode, unsigned int rawState,
                                    const QHash<int, NativeKeyGrab>& registry) {
    const unsigned int state = normalizeLockState(rawState);
    for (auto it = registry.cbegin(); it != registry.cend(); ++it) {
        if (it.value().keycode == keycode && it.value().modifiers == state) {
            return it.key();
        }
    }
    return std::nullopt;
}

} // namespace snow_shot::platform::x11
