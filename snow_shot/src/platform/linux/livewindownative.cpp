// Every Qt header must be parsed before the X11 headers: Xlib's Bool/Status
// macros would otherwise corrupt the Qt declarations and trip Qt's guards.
#include "../../presentation/pinned/livewindownative_p.h"

#include "snow_shot/platform/linux/x11connection.h"
#include "snow_shot/platform/linux/x11windowquery.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace snow_shot::presentation {
namespace {

namespace x11 = snow_shot::platform::x11;

QString nativeText(const char* text) {
    return QCoreApplication::translate("LiveWindowSource", text);
}

QString unavailableText() {
    return nativeText(QT_TRANSLATE_NOOP(
        "LiveWindowSource", "The window content is temporarily unavailable. Retrying…"));
}

QRect geometryOnDisplay(Display* display, Window window) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window, &attributes))
        return {};
    int rootX = 0;
    int rootY = 0;
    Window child = None;
    if (!XTranslateCoordinates(display, window, attributes.root, 0, 0, &rootX, &rootY, &child))
        return {};
    return QRect(rootX, rootY, static_cast<int>(attributes.width),
                 static_cast<int>(attributes.height));
}

class X11LiveWindow final : public live_window_detail::NativeWindowMonitor {
  public:
    explicit X11LiveWindow(LiveWindowTarget target) : m_target(std::move(target)) {}

    bool initialize(QString* error) override {
        if (!m_connection.isValid()) {
            if (error)
                *error = nativeText(QT_TRANSLATE_NOOP(
                    "LiveWindowSource", "The selected window is no longer available."));
            return false;
        }
        const auto state = inspect();
        if (state.state != LiveWindowState::Live) {
            if (error) {
                *error =
                    state.error.isEmpty()
                        ? nativeText(QT_TRANSLATE_NOOP(
                              "LiveWindowSource", "The selected window is no longer available."))
                        : state.error;
            }
            return false;
        }
        return true;
    }

    live_window_detail::NativeState inspect() override {
        Display* display = m_connection.display();
        if (display == nullptr)
            return {LiveWindowState::Closed, {}, {}};
        const Window window = static_cast<Window>(m_target.nativeId);
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, window, &attributes))
            return {LiveWindowState::Closed, {}, {}};
        const quint32 owner = x11::windowProcessIdOnDisplay(display, m_target.nativeId);
        if (owner != 0 && m_target.processId != 0 &&
            owner != static_cast<quint32>(m_target.processId)) {
            // The X server recycled the window id for another application.
            return {LiveWindowState::Closed, {}, {}};
        }
        if (attributes.map_state == IsUnmapped)
            return {LiveWindowState::Minimized, {}, {}};
        if (attributes.map_state != IsViewable)
            return {LiveWindowState::Unavailable, {}, unavailableText()};
        const Window frame = x11::frameWindowIdOnDisplay(display, m_target.nativeId);
        const QRect bounds = geometryOnDisplay(display, frame != None ? frame : window);
        if (bounds.isEmpty())
            return {LiveWindowState::Unavailable, {}, unavailableText()};
        return {LiveWindowState::Live, bounds, {}};
    }

  private:
    LiveWindowTarget m_target;
    x11::X11Connection m_connection;
};

class X11WindowFrame final : public live_window_detail::NativeWindowFrame {
  public:
    explicit X11WindowFrame(QImage image) : m_image(std::move(image)) {}
    QSize size() const override {
        return m_image.size();
    }
    QImage image() const override {
        return m_image;
    }
    const void* pixelBuffer() const override {
        return m_image.constBits();
    }

  private:
    QImage m_image;
};

QImage imageFromXImage(XImage* image) {
    if (image == nullptr || image->width <= 0 || image->height <= 0)
        return {};
    if (image->bits_per_pixel == 32) {
        QImage pixels(image->width, image->height, QImage::Format_RGB32);
        if (pixels.isNull())
            return {};
        const auto* source = reinterpret_cast<const uchar*>(image->data);
        const size_t rowBytes = static_cast<size_t>(image->width) * 4;
        for (int y = 0; y < image->height; ++y) {
            std::memcpy(pixels.scanLine(y), source + static_cast<size_t>(y) * image->bytes_per_line,
                        rowBytes);
        }
        return pixels;
    }
    if (image->bits_per_pixel == 24) {
        QImage pixels(image->width, image->height, QImage::Format_BGR888);
        if (pixels.isNull())
            return {};
        const auto* source = reinterpret_cast<const uchar*>(image->data);
        for (int y = 0; y < image->height; ++y) {
            const uchar* row = source + static_cast<size_t>(y) * image->bytes_per_line;
            std::memcpy(pixels.scanLine(y), row, static_cast<size_t>(image->width) * 3);
        }
        return pixels;
    }
    QImage pixels(image->width, image->height, QImage::Format_RGB32);
    if (pixels.isNull())
        return {};
    for (int y = 0; y < image->height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(pixels.scanLine(y));
        for (int x = 0; x < image->width; ++x) {
            const unsigned long pixel = XGetPixel(image, x, y);
            row[x] = qRgb(static_cast<int>((pixel >> 16) & 0xff),
                          static_cast<int>((pixel >> 8) & 0xff), static_cast<int>(pixel & 0xff));
        }
    }
    return pixels;
}

class X11WindowCapture final : public live_window_detail::NativeWindowCapture {
  public:
    explicit X11WindowCapture(const LiveWindowTarget& target) : m_target(target) {
        if (m_connection.isValid()) {
            int eventBase = 0;
            int errorBase = 0;
            m_compositeAvailable =
                XCompositeQueryExtension(m_connection.display(), &eventBase, &errorBase) != 0;
        }
    }

    ~X11WindowCapture() override {
        cancel();
    }

    void cancel() override {
        m_cancelled.store(true, std::memory_order_release);
    }

    bool receive(live_window_detail::NativeCaptureFrame* frame) override {
        if (frame == nullptr || !m_connection.isValid())
            return false;
        Display* display = m_connection.display();
        const Window window = static_cast<Window>(m_target.nativeId);
        while (!m_cancelled.load(std::memory_order_acquire)) {
            XWindowAttributes attributes{};
            if (!XGetWindowAttributes(display, window, &attributes))
                return false;
            if (attributes.map_state != IsViewable || attributes.width <= 0 ||
                attributes.height <= 0) {
                return false;
            }
            // Capture the manager frame so title bars and borders match the
            // bounds window selection reported; fall back to the client window.
            const Window managerFrame = x11::frameWindowIdOnDisplay(display, m_target.nativeId);
            const Window drawable = managerFrame != None ? managerFrame : window;
            XWindowAttributes drawableAttributes{};
            if (!XGetWindowAttributes(display, drawable, &drawableAttributes))
                return false;
            if (drawableAttributes.width <= 0 || drawableAttributes.height <= 0)
                return false;
            int rootX = 0;
            int rootY = 0;
            Window child = None;
            if (!XTranslateCoordinates(display, drawable, drawableAttributes.root, 0, 0, &rootX,
                                       &rootY, &child)) {
                return false;
            }
            if (m_compositeAvailable) {
                // Names the window's offscreen pixmap so occluded content is readable.
                XCompositeNameWindowPixmap(display, drawable);
            }
            XImage* image = XGetImage(display, drawable, 0, 0, drawableAttributes.width,
                                      drawableAttributes.height, AllPlanes, ZPixmap);
            if (image == nullptr)
                return false;
            QImage pixels = imageFromXImage(image);
            XDestroyImage(image);
            if (pixels.isNull())
                return false;
            pixels.setDevicePixelRatio(1.0);
            pixels.setColorSpace(QColorSpace::SRgb);
            frame->pixels = std::make_shared<X11WindowFrame>(std::move(pixels));
            frame->desktopBounds =
                QRectF(rootX, rootY, drawableAttributes.width, drawableAttributes.height);
            frame->duplicate = false;
            pace();
            return true;
        }
        return false;
    }

  private:
    void pace() {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_nextFrame) {
            while (!m_cancelled.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < m_nextFrame) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
        m_nextFrame =
            std::max(std::chrono::steady_clock::now(), m_nextFrame) + std::chrono::milliseconds(33);
    }

    LiveWindowTarget m_target;
    x11::X11Connection m_connection;
    std::atomic<bool> m_cancelled{false};
    std::chrono::steady_clock::time_point m_nextFrame;
    bool m_compositeAvailable = false;
};

} // namespace

std::optional<LiveWindowTarget> liveWindowTargetAtPoint(const QPoint& point,
                                                        const QVector<quintptr>& excluded) {
    const quint32 ownProcess = static_cast<quint32>(QCoreApplication::applicationPid());
    for (const auto& entry : x11::stackedTopLevelWindows()) {
        if (!entry.mapped || entry.skip || entry.frameRect.isEmpty())
            continue;
        if (excluded.contains(entry.windowId))
            continue;
        if (entry.processId == ownProcess)
            continue;
        if (!entry.frameRect.contains(point))
            continue;
        return LiveWindowTarget{entry.windowId, entry.processId, entry.frameRect, point,
                                entry.title};
    }
    return std::nullopt;
}

namespace live_window_detail {

std::unique_ptr<NativeWindowCapture> createNativeWindowCapture(const LiveWindowTarget& target) {
    return std::make_unique<X11WindowCapture>(target);
}

std::unique_ptr<NativeWindowMonitor> createNativeWindowMonitor(const LiveWindowTarget& target) {
    return std::make_unique<X11LiveWindow>(target);
}

std::unique_ptr<NativeWindowPresentation> createNativeWindowPresentation(const LiveWindowTarget&) {
    return nullptr;
}

} // namespace live_window_detail
} // namespace snow_shot::presentation
