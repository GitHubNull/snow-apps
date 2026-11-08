#pragma once

#include <QtGlobal>

struct _XDisplay;

namespace snow_shot::platform::x11 {

using X11Display = ::_XDisplay;

// Xlib's default protocol error handler terminates the process. The helpers in
// this module probe possibly-stale window ids, so install a no-op handler once
// before the first Xlib call and keep it for the process lifetime.
void installX11ErrorHandlers();

// Short-lived XOpenDisplay(nullptr) wrapper for one-shot queries (cursor
// sampling, window state polling). Backends that need a persistent connection
// manage their own Display separately.
class X11Connection {
  public:
    X11Connection();
    ~X11Connection();
    X11Connection(const X11Connection&) = delete;
    X11Connection& operator=(const X11Connection&) = delete;

    [[nodiscard]] bool isValid() const {
        return m_display != nullptr;
    }
    [[nodiscard]] X11Display* display() const {
        return m_display;
    }

  private:
    X11Display* m_display = nullptr;
};

} // namespace snow_shot::platform::x11
