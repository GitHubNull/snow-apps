#pragma once

#include <QtGui/qwindowdefs.h>

#include <functional>
#include <optional>

namespace snow_shot::platform::linux {

enum class WindowAlwaysOnTopResult { ChangeRequested, Ignored, Failed };

// Runs before native state is read so a stateful owner can resolve auxiliary
// surfaces and toggle its own state. nullopt delegates to EWMH.
using LocalWindowAlwaysOnTopHandler = std::function<std::optional<bool>(WId)>;

// Toggles _NET_WM_STATE_ABOVE for the topmost window under the cursor.
[[nodiscard]] WindowAlwaysOnTopResult
toggleWindowUnderCursorAlwaysOnTop(const LocalWindowAlwaysOnTopHandler& localHandler = {});

} // namespace snow_shot::platform::linux
