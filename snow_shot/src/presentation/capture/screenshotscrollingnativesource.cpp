#include "screenshotscrollingpipeline.h"
#include "screenshotscrollingdiagnostics.h"
#include "snow_capture.h"

#if defined(Q_OS_LINUX)
#include "screenshotscrollingqtsource.h"

#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QScreen>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#endif

#include <QColorSpace>
#include <limits>

namespace snow_shot::capture_detail {
namespace {
class NativeScrollingSource final : public ScrollingFrameSource {
  public:
    explicit NativeScrollingSource(SnowCaptureStream* stream, quint64 generation)
        : m_stream(stream), m_generation(generation) {}
    ~NativeScrollingSource() override {
        stop();
        snow_capture_stream_destroy(m_stream);
    }
    ScrollingSourceEvent receive(int timeoutMilliseconds) override {
        SnowCaptureStreamEvent event{};
        ScrollingSourceEvent result;
        if (snow_capture_stream_receive(m_stream, static_cast<std::uint32_t>(timeoutMilliseconds),
                                        &event) == 0) {
            result.kind = ScrollingSourceEvent::Kind::Error;
            result.error = QString::fromUtf8(snow_capture_last_error_message());
            return result;
        }
        switch (event.kind) {
        case SNOW_CAPTURE_STREAM_EVENT_FRAME: {
            SnowCaptureStreamFrameInfo info{};
            if (!event.frame || snow_capture_stream_frame_info(event.frame, &info) == 0 ||
                !info.rgba_bytes || info.width == 0 || info.height == 0 ||
                info.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4) ||
                info.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                info.stride_bytes != info.width * 4 ||
                info.rgba_len < static_cast<std::size_t>(info.stride_bytes) * info.height) {
                if (event.frame)
                    snow_capture_stream_frame_release(event.frame);
                result.kind = ScrollingSourceEvent::Kind::Error;
                result.error = QStringLiteral("capture stream returned an invalid frame");
                return result;
            }
            if (m_backend != info.backend_kind) {
                m_backend = info.backend_kind;
                logScrollingEvent("scrolling.backend_selected", m_generation,
                                  {{QStringLiteral("backend"), m_backend},
                                   {QStringLiteral("width"), static_cast<qint64>(info.width)},
                                   {QStringLiteral("height"), static_cast<qint64>(info.height)}});
            }
            result.kind = ScrollingSourceEvent::Kind::Frame;
            result.frame.duplicate = info.is_duplicate != 0;
            result.frame.image = QImage(
                info.rgba_bytes, static_cast<int>(info.width), static_cast<int>(info.height),
                static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888,
                [](void* frame) {
                    snow_capture_stream_frame_release(static_cast<SnowCaptureStreamFrame*>(frame));
                },
                event.frame);
            result.frame.image.setColorSpace(QColorSpace::SRgb);
            break;
        }
#ifdef Q_OS_MACOS
        case SNOW_CAPTURE_STREAM_EVENT_RESOLUTION_CHANGED:
            result.kind = ScrollingSourceEvent::Kind::Error;
            result.error =
                QStringLiteral("display reconfiguration invalidated the scrolling viewport");
            break;
#endif
        case SNOW_CAPTURE_STREAM_EVENT_FRAMES_DROPPED:
            result.kind = ScrollingSourceEvent::Kind::Dropped;
            break;
        case SNOW_CAPTURE_STREAM_EVENT_ERROR:
            result.kind = ScrollingSourceEvent::Kind::Error;
            result.error = QString::fromUtf8(snow_capture_last_error_message());
            break;
        case SNOW_CAPTURE_STREAM_EVENT_ENDED:
            result.kind = ScrollingSourceEvent::Kind::Ended;
            break;
        default:
            break;
        }
        return result;
    }
    void setTargetFps(int fps) override {
        if (fps != m_targetFps &&
            snow_capture_stream_set_target_fps(m_stream, static_cast<std::uint32_t>(fps)) != 0) {
            m_targetFps = fps;
        }
    }
    ScrollingSourceStats stats() const override {
        SnowCaptureStreamStats stats{};
        if (snow_capture_stream_stats(m_stream, &stats) == 0)
            return {};
        return {stats.capture_latency_ns, stats.buffer_fill, stats.frames_dropped};
    }
    void stop() override {
        static_cast<void>(snow_capture_stream_stop(m_stream));
    }

  private:
    SnowCaptureStream* m_stream;
    int m_targetFps = 30;
    int m_backend = -1;
    quint64 m_generation;
};

#if defined(Q_OS_LINUX)
using ScrollingClock = std::chrono::steady_clock;
constexpr int kQtScrollingGrabTimeoutMs = 500;
constexpr int kQtScrollingMaxConsecutiveErrors = 30;

// QScreen::grabWindow must run on the GUI thread, so the producer posts one
// request per tick and waits for the result. The shared state keeps the late
// invocation safe if it runs after the source has been destroyed.
struct QtScrollingGrabRequest {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    QImage frame;
};

struct QtScrollingGrabChannel {
    // At most one request may sit on the GUI event loop; a stalled window
    // manager must not accumulate queued grabs.
    std::atomic<bool> queued{false};
    std::atomic<qint64> latencyNs{0};
};

QImage grabScrollingSelectionFrame(const QRect& selection) {
    QVector<CapturedDisplayModel> displays;
    QSet<QString> usedIds;
    const QList<QScreen*> screens = QGuiApplication::screens();
    displays.reserve(screens.size());
    for (qsizetype index = 0; index < screens.size(); ++index) {
        QScreen* screen = screens.at(index);
        if (screen == nullptr) {
            continue;
        }
        CapturedDisplayModel display = ScreenshotGeometryMapper::preCaptureDisplayModel(*screen);
        display.stableId =
            presentation::capture::qtcapture::displayStableId(screen->name(), index, &usedIds);
        display.geometryResolved = true;
        displays.push_back(display);
    }
    QImage rootFrame;
    if (QScreen* primary = QGuiApplication::primaryScreen(); primary != nullptr) {
        rootFrame = qtscrolling::normalizeScrollingRootFrame(primary->grabWindow(0).toImage());
    }
    const QImage cropped = qtscrolling::cropScrollingSelectionFrame(
        displays, rootFrame, selection, [](QScreen& screen) {
            return qtscrolling::normalizeScrollingRootFrame(screen.grabWindow(0).toImage());
        });
    return qtscrolling::toScrollingFramePayload(cropped);
}

class QtScrollingFrameSource final : public ScrollingFrameSource {
  public:
    QtScrollingFrameSource(const QRect& physicalSelection, quint64 generation)
        : m_selection(physicalSelection), m_generation(generation),
          m_channel(std::make_shared<QtScrollingGrabChannel>()) {
        logScrollingEvent("scrolling.qt_create", m_generation,
                          {{QStringLiteral("physical_selection"), scrollingRect(m_selection)}});
        m_thread = std::thread(snow_shot::runtime::trackRuntimeWork([this] {
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            produce();
        }));
    }
    ~QtScrollingFrameSource() override {
        stop();
    }

    ScrollingSourceEvent receive(int timeoutMilliseconds) override {
        std::unique_lock lock(m_mutex);
        const auto deadline =
            ScrollingClock::now() + std::chrono::milliseconds(std::max(0, timeoutMilliseconds));
        m_cv.wait_until(lock, deadline, [this] {
            return m_stop.load(std::memory_order_acquire) || m_pending.has_value() ||
                   m_fatalError.has_value() || m_replaced.load(std::memory_order_acquire) > 0;
        });
        ScrollingSourceEvent event;
        if (m_fatalError.has_value()) {
            event.kind = ScrollingSourceEvent::Kind::Error;
            event.error = *m_fatalError;
            return event;
        }
        if (m_stop.load(std::memory_order_acquire)) {
            event.kind = ScrollingSourceEvent::Kind::Ended;
            return event;
        }
        if (m_pending.has_value()) {
            QImage frame = std::move(*m_pending);
            m_pending.reset();
            m_buffered.store(0, std::memory_order_release);
            event.kind = ScrollingSourceEvent::Kind::Frame;
            event.frame.duplicate = qtscrolling::isDuplicateScrollingFrame(m_previous, frame);
            m_previous = frame;
            event.frame.image = std::move(frame);
            return event;
        }
        if (m_replaced.exchange(0, std::memory_order_acq_rel) > 0) {
            event.kind = ScrollingSourceEvent::Kind::Dropped;
            return event;
        }
        event.kind = ScrollingSourceEvent::Kind::Timeout;
        return event;
    }
    void setTargetFps(int fps) override {
        m_targetFps.store(std::clamp(fps, 1, 120), std::memory_order_relaxed);
        m_fpsWake.store(true, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
        }
        m_cv.notify_all();
    }
    ScrollingSourceStats stats() const override {
        ScrollingSourceStats result;
        result.captureLatencyNs =
            static_cast<std::uint64_t>(std::max<qint64>(0, m_channel->latencyNs.load()));
        result.bufferedFrames = m_buffered.load(std::memory_order_acquire);
        result.droppedFrames = m_droppedTotal.load(std::memory_order_acquire);
        return result;
    }
    void stop() override {
        const bool wasStopped = m_stop.exchange(true, std::memory_order_acq_rel);
        if (!wasStopped) {
            m_cv.notify_all();
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (!wasStopped) {
            // Wake a receive() that parked before stop became visible.
            m_cv.notify_all();
        }
    }

  private:
    void produce() {
        int consecutiveErrors = 0;
        while (!m_stop.load(std::memory_order_acquire)) {
            const auto cycleStart = ScrollingClock::now();
            const int fps = std::clamp(m_targetFps.load(std::memory_order_relaxed), 1, 120);
            const std::optional<QImage> frame = requestGrab();
            if (m_stop.load(std::memory_order_acquire)) {
                break;
            }
            if (!frame.has_value()) {
                // The GUI thread is busy; skip this tick without counting a
                // capture failure.
            } else if (frame->isNull()) {
                if (++consecutiveErrors >= kQtScrollingMaxConsecutiveErrors) {
                    logScrollingEvent("scrolling.qt_grab_failed", m_generation,
                                      {{QStringLiteral("consecutive_errors"), consecutiveErrors}},
                                      QtWarningMsg);
                    publishFatal(QStringLiteral("scrolling capture could not read the screen"));
                    break;
                }
            } else {
                consecutiveErrors = 0;
                publishFrame(std::move(*frame));
            }
            const auto period = std::chrono::microseconds(1'000'000 / fps);
            std::unique_lock lock(m_mutex);
            m_cv.wait_until(lock, cycleStart + period, [this] {
                return m_stop.load(std::memory_order_acquire) ||
                       m_fpsWake.exchange(false, std::memory_order_acq_rel);
            });
        }
        m_cv.notify_all();
    }

    std::optional<QImage> requestGrab() {
        const auto channel = m_channel;
        if (channel->queued.exchange(true, std::memory_order_acq_rel)) {
            return std::nullopt;
        }
        auto request = std::make_shared<QtScrollingGrabRequest>();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [channel, request, selection = m_selection] {
                const auto started = ScrollingClock::now();
                QImage frame = grabScrollingSelectionFrame(selection);
                channel->latencyNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             ScrollingClock::now() - started)
                                             .count());
                channel->queued.store(false, std::memory_order_release);
                {
                    std::lock_guard lock(request->mutex);
                    request->frame = std::move(frame);
                    request->done = true;
                }
                request->changed.notify_all();
            },
            Qt::QueuedConnection);
        std::unique_lock lock(request->mutex);
        const bool finished = request->changed.wait_for(
            lock, std::chrono::milliseconds(kQtScrollingGrabTimeoutMs),
            [&] { return request->done || m_stop.load(std::memory_order_acquire); });
        if (!finished || !request->done) {
            return std::nullopt;
        }
        return request->frame;
    }

    void publishFrame(QImage frame) {
        {
            std::lock_guard lock(m_mutex);
            if (m_pending.has_value()) {
                m_replaced.fetch_add(1, std::memory_order_acq_rel);
                m_droppedTotal.fetch_add(1, std::memory_order_acq_rel);
            }
            m_pending = std::move(frame);
            m_buffered.store(1, std::memory_order_release);
        }
        m_cv.notify_all();
    }
    void publishFatal(QString message) {
        {
            std::lock_guard lock(m_mutex);
            m_fatalError = std::move(message);
        }
        m_cv.notify_all();
    }

    QRect m_selection;
    quint64 m_generation;
    std::shared_ptr<QtScrollingGrabChannel> m_channel;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<QImage> m_pending;
    std::optional<QString> m_fatalError;
    QImage m_previous;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_fpsWake{false};
    std::atomic<int> m_targetFps{30};
    std::atomic<quint32> m_replaced{0};
    std::atomic<quint32> m_buffered{0};
    std::atomic<std::uint64_t> m_droppedTotal{0};
};
#endif
} // namespace

ScrollingSourceFactory nativeScrollingSource(QRect selection, bool restoreOriginalColors,
                                             const QVector<std::uint32_t>& excludedWindowIds,
                                             quint64 generation) {
#if defined(Q_OS_LINUX)
    // The Qt grab path has no per-window exclusion; scrolling capture relies on
    // the overlay exposing live content instead of native window filtering.
    Q_UNUSED(restoreOriginalColors);
    Q_UNUSED(excludedWindowIds);
    return [selection, generation]() -> std::unique_ptr<ScrollingFrameSource> {
        if (selection.isEmpty())
            return {};
        return std::make_unique<QtScrollingFrameSource>(selection, generation);
    };
#else
    return [selection, restoreOriginalColors, excludedWindowIds,
            generation]() -> std::unique_ptr<ScrollingFrameSource> {
        if (selection.isEmpty())
            return {};
        SnowCaptureStreamConfig config{};
        config.version = SNOW_CAPTURE_STREAM_CONFIG_VERSION;
        config.struct_size = sizeof(config);
        config.x = selection.x();
        config.y = selection.y();
        config.width = static_cast<std::uint32_t>(selection.width());
        config.height = static_cast<std::uint32_t>(selection.height());
        config.target_fps = 30;
        config.min_fps = 1;
        config.buffer_depth = 3;
        config.max_consecutive_errors = 30;
        config.capture_retry_count = 1;
        config.wgc_update_mode = SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY;
        // Auto tries WGC first, then DXGI and GDI on eligible capture failures.
        config.capture_backend = SNOW_CAPTURE_BACKEND_AUTO;
        config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
        config.adaptive_fps = 1;
        config.include_cursor = 0;
        config.restore_original_colors = restoreOriginalColors ? 1 : 0;
        config.exclusions.windows = excludedWindowIds.constData();
        config.exclusions.window_count = static_cast<size_t>(excludedWindowIds.size());
        logScrollingEvent("scrolling.native_create", generation,
                          {{QStringLiteral("physical_selection"), scrollingRect(selection)},
                           {QStringLiteral("backend"), static_cast<int>(config.capture_backend)},
                           {QStringLiteral("mode"), static_cast<int>(config.wgc_update_mode)}});
        auto* stream = snow_capture_stream_create_region(&config);
        if (!stream) {
            qWarning("Failed to create continuous scrolling stream: %s",
                     snow_capture_last_error_message());
            return {};
        }
        return std::make_unique<NativeScrollingSource>(stream, generation);
    };
#endif
}
} // namespace snow_shot::capture_detail
