#ifndef SNOW_SHOT_PLATFORM_LINUX_AUTOSTARTREGISTRATION_H
#define SNOW_SHOT_PLATFORM_LINUX_AUTOSTARTREGISTRATION_H

#include <QString>

namespace snow_shot::platform::linux {

struct AutostartResult {
    bool success = false;
    QString error;
};

// Creates or removes $XDG_CONFIG_HOME/autostart/snow-shot.desktop.
[[nodiscard]] AutostartResult setAutostartEnabled(bool enabled);

// Reads whether the desktop entry currently exists and is enabled.
[[nodiscard]] bool autostartEnabled();

} // namespace snow_shot::platform::linux

#endif // SNOW_SHOT_PLATFORM_LINUX_AUTOSTARTREGISTRATION_H
