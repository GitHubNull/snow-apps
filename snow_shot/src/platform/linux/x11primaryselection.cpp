// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include "snow_shot/presentation/linuxselectedtextbackend.h"

#include "snow_shot/platform/linux/x11connection.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <array>
#include <chrono>
#include <sys/select.h>

namespace snow_shot::presentation {
namespace {

using Clock = std::chrono::steady_clock;

constexpr qsizetype kMaximumSelectionBytes = LinuxSelectedTextCaptureBackend::kMaxTextBytes + 4;
constexpr auto kPollSlice = std::chrono::milliseconds(20);

struct SelectionAtoms {
    Atom utf8String = None;
    Atom incr = None;
    Atom property = None;
    std::array<Atom, 3> targets{None, None, None};
};

bool beforeDeadline(const Clock::time_point& deadline) {
    return Clock::now() < deadline;
}

// Waits for one X event that matches the requestor. Returns false when the
// deadline or cancellation fires first.
bool nextRequestorEvent(Display* display, Window requestor, const Clock::time_point& deadline,
                        const std::atomic<bool>& cancelled, XEvent* event) {
    while (beforeDeadline(deadline)) {
        if (cancelled.load(std::memory_order_acquire))
            return false;
        while (XEventsQueued(display, QueuedAlready) > 0) {
            XNextEvent(display, event);
            if ((event->type == SelectionNotify && event->xselection.requestor == requestor) ||
                (event->type == PropertyNotify && event->xproperty.window == requestor))
                return true;
        }
        fd_set readable;
        FD_ZERO(&readable);
        const int descriptor = ConnectionNumber(display);
        FD_SET(descriptor, &readable);
        timeval slice{};
        slice.tv_usec = static_cast<suseconds_t>(kPollSlice.count() * 1000);
        static_cast<void>(select(descriptor + 1, &readable, nullptr, nullptr, &slice));
    }
    return false;
}

bool waitForSelectionNotify(Display* display, Window requestor, const Clock::time_point& deadline,
                            const std::atomic<bool>& cancelled, XSelectionEvent* selection) {
    XEvent event{};
    while (nextRequestorEvent(display, requestor, deadline, cancelled, &event)) {
        if (event.type == SelectionNotify) {
            *selection = event.xselection;
            return true;
        }
        // Stale property notifications from a previous incremental transfer.
    }
    return false;
}

bool waitForPropertyValue(Display* display, Window requestor, Atom property,
                          const Clock::time_point& deadline, const std::atomic<bool>& cancelled) {
    XEvent event{};
    while (nextRequestorEvent(display, requestor, deadline, cancelled, &event)) {
        if (event.type == PropertyNotify && event.xproperty.atom == property &&
            event.xproperty.state == PropertyNewValue)
            return true;
    }
    return false;
}

struct PropertyValue {
    Atom type = None;
    QByteArray bytes;
};

PropertyValue readProperty(Display* display, Window requestor, Atom property, bool remove) {
    Atom actualType = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, requestor, property, 0, kMaximumSelectionBytes / 4 + 1,
                           remove ? True : False, AnyPropertyType, &actualType, &format, &count,
                           &remaining, &data) != Success) {
        return {};
    }
    PropertyValue value;
    value.type = actualType;
    if (data != nullptr) {
        const unsigned long byteCount = format == 8 ? count : count * (format / 8);
        value.bytes =
            QByteArray(reinterpret_cast<const char*>(data), static_cast<qsizetype>(byteCount));
        XFree(data);
    }
    return value;
}

QByteArray readIncrementalTransfer(Display* display, Window requestor, Atom property,
                                   const Clock::time_point& deadline,
                                   const std::atomic<bool>& cancelled, bool* completed) {
    *completed = false;
    QByteArray collected;
    // Deleting the INCR property acknowledges the first chunk write.
    XDeleteProperty(display, requestor, property);
    XFlush(display);
    while (beforeDeadline(deadline) && !cancelled.load(std::memory_order_acquire)) {
        if (!waitForPropertyValue(display, requestor, property, deadline, cancelled))
            return {};
        const PropertyValue value = readProperty(display, requestor, property, true);
        if (value.type == None || value.bytes.isEmpty()) {
            *completed = true;
            return collected;
        }
        if (collected.size() + value.bytes.size() > kMaximumSelectionBytes) {
            collected.append(value.bytes.left(kMaximumSelectionBytes - collected.size()));
            *completed = true;
            return collected;
        }
        collected.append(value.bytes);
    }
    return {};
}

QString decodeSelection(const QByteArray& bytes, Atom type, Atom utf8String) {
    if (type == XA_STRING)
        return QString::fromLatin1(bytes);
    Q_UNUSED(utf8String);
    return QString::fromUtf8(bytes);
}

class X11PrimarySelectionTextReader final : public SelectionTextReader {
  public:
    SelectionTextReadResult read(const std::atomic<bool>& cancelled) override {
        const Clock::time_point deadline =
            Clock::now() + std::chrono::milliseconds(LinuxSelectedTextCaptureBackend::kTimeoutMs);
        snow_shot::platform::x11::X11Connection connection;
        if (!connection.isValid())
            return {SelectionTextReadStatus::Unavailable, {}};
        Display* display = connection.display();
        if (XGetSelectionOwner(display, XA_PRIMARY) == None)
            return {SelectionTextReadStatus::Empty, {}};

        SelectionAtoms atoms;
        atoms.utf8String = XInternAtom(display, "UTF8_STRING", True);
        atoms.incr = XInternAtom(display, "INCR", True);
        atoms.property = XInternAtom(display, "_SNOW_SHOT_SELECTION", False);
        atoms.targets = {atoms.utf8String, XInternAtom(display, "text/plain;charset=utf-8", True),
                         XA_STRING};

        const Window requestor =
            XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
        if (requestor == None)
            return {SelectionTextReadStatus::Unavailable, {}};
        XSelectInput(display, requestor, PropertyChangeMask);

        SelectionTextReadResult result{SelectionTextReadStatus::Unavailable, {}};
        for (const Atom target : atoms.targets) {
            if (target == None || !beforeDeadline(deadline) ||
                cancelled.load(std::memory_order_acquire))
                continue;
            XConvertSelection(display, XA_PRIMARY, target, atoms.property, requestor, CurrentTime);
            XFlush(display);
            XSelectionEvent selection{};
            if (!waitForSelectionNotify(display, requestor, deadline, cancelled, &selection))
                break;
            if (selection.property == None)
                continue;

            const PropertyValue initial = readProperty(display, requestor, atoms.property, false);
            if (atoms.incr != None && initial.type == atoms.incr) {
                bool completed = false;
                const QByteArray bytes = readIncrementalTransfer(display, requestor, atoms.property,
                                                                 deadline, cancelled, &completed);
                if (!completed)
                    continue;
                result = {bytes.isEmpty() ? SelectionTextReadStatus::Empty
                                          : SelectionTextReadStatus::Read,
                          decodeSelection(bytes, atoms.utf8String, atoms.utf8String)};
                break;
            }
            XDeleteProperty(display, requestor, atoms.property);
            result = {initial.bytes.isEmpty() ? SelectionTextReadStatus::Empty
                                              : SelectionTextReadStatus::Read,
                      decodeSelection(initial.bytes, initial.type, atoms.utf8String)};
            break;
        }
        XDestroyWindow(display, requestor);
        XFlush(display);
        return result;
    }
};

} // namespace

std::unique_ptr<SelectionTextReader> createPrimarySelectionTextReader() {
    return std::make_unique<X11PrimarySelectionTextReader>();
}

} // namespace snow_shot::presentation
