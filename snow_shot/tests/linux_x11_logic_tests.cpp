#include "snow_shot/platform/linux/x11grabkey.h"

#include <QGuiApplication>
#include <iostream>
#include <Qt>

#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void lockMaskCombinationsCoverAllLockStates() {
    const QVector<unsigned int> combinations = snow_shot::platform::x11::lockMaskCombinations();
    require(combinations.size() == 8, "eight lock mask combinations");
    const unsigned int lockMask = snow_shot::platform::x11::grabModifierLockMask();
    for (const unsigned int combination : combinations) {
        require((combination & ~lockMask) == 0, "every combination only uses lock modifier bits");
    }
    require(combinations.contains(0), "combinations include the unlocked state");
    require(combinations.contains(lockMask), "combinations include all lock bits");
    for (int i = 1; i < combinations.size(); ++i) {
        require(combinations[i - 1] != combinations[i], "combinations are unique");
    }
}

void normalizeLockStateStripsLockBits() {
    using namespace snow_shot::platform::x11;
    constexpr unsigned int ctrlAlt = kControlMask | kMod1Mask;
    require(normalizeLockState(ctrlAlt | kLockMask | kMod2Mask | kMod5Mask) == ctrlAlt,
            "all lock bits are stripped");
    require(normalizeLockState(kShiftMask | kMod4Mask) == (kShiftMask | kMod4Mask),
            "regular modifiers survive normalization");
}

void classifyKeyPressMatchesNormalizedState() {
    using namespace snow_shot::platform::x11;
    QHash<int, NativeKeyGrab> registry;
    registry.insert(5, NativeKeyGrab{38, kControlMask | kMod1Mask});
    const auto pressed = classifyKeyPress(38, kControlMask | kMod1Mask | kMod2Mask, registry);
    require(pressed.has_value() && pressed == 5, "lock bits do not block classification");
    const auto wrongModifiers = classifyKeyPress(38, kControlMask, registry);
    require(!wrongModifiers.has_value(), "missing modifiers do not match");
    const auto wrongKeycode = classifyKeyPress(39, kControlMask | kMod1Mask, registry);
    require(!wrongKeycode.has_value(), "other keycodes do not match");
}

void keysymNamesCoverCoreKeys() {
    using namespace snow_shot::platform::x11;
    const auto letter = keysymNameForQtKey(Qt::Key_A, false);
    require(letter.has_value() && letter == QStringLiteral("a"), "Key_A maps to a");
    const auto digit = keysymNameForQtKey(Qt::Key_5, false);
    require(digit.has_value() && digit == QStringLiteral("5"), "Key_5 maps to 5");
    const auto function = keysymNameForQtKey(Qt::Key_F12, false);
    require(function.has_value() && function == QStringLiteral("F12"), "Key_F12 maps to F12");
    const auto space = keysymNameForQtKey(Qt::Key_Space, false);
    require(space.has_value() && space == QStringLiteral("space"), "Key_Space maps to space");
    const auto pageUp = keysymNameForQtKey(Qt::Key_PageUp, false);
    require(pageUp.has_value() && pageUp == QStringLiteral("Prior"), "Key_PageUp maps to Prior");
    const auto print = keysymNameForQtKey(Qt::Key_Print, false);
    require(print.has_value() && print == QStringLiteral("Print"), "Key_Print maps to Print");
    const auto punctuation = keysymNameForQtKey(Qt::Key_Minus, false);
    require(punctuation.has_value() && punctuation == QStringLiteral("minus"),
            "Key_Minus maps to minus");
    const auto media = keysymNameForQtKey(Qt::Key_VolumeMute, false);
    require(media.has_value() && media == QStringLiteral("XF86AudioMute"),
            "Key_VolumeMute maps to XF86AudioMute");
    const auto keypadDigit = keysymNameForQtKey(Qt::Key_5, true);
    require(keypadDigit.has_value() && keypadDigit == QStringLiteral("KP_5"),
            "keypad Key_5 maps to KP_5");
    const auto keypadPlus = keysymNameForQtKey(Qt::Key_Plus, true);
    require(keypadPlus.has_value() && keypadPlus == QStringLiteral("KP_Add"),
            "keypad Key_Plus maps to KP_Add");
    const auto unmapped = keysymNameForQtKey(Qt::Key_MediaLast, false);
    require(!unmapped.has_value(), "keys without a stable keysym return nullopt");
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    lockMaskCombinationsCoverAllLockStates();
    normalizeLockStateStripsLockBits();
    classifyKeyPressMatchesNormalizedState();
    keysymNamesCoverCoreKeys();
    return 0;
}
