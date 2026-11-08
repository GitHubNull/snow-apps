#include "snow_shot/runtime/runtimeactivitytracker.h"
#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/presentation/screenshotcapturecoordinator.h"

#include "screenshotcaptureworker.h"

#if defined(Q_OS_LINUX)
#include "capturewindowshield.h"
#include "snow_shot/presentation/capture/screenshotqtcaptureprovider.h"
#include "snow_shot/storage/settingsadapters.h"

#include <algorithm>
#endif

#include "snow_capture.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <utility>

#if !defined(Q_OS_LINUX)
struct ScreenshotCaptureCoordinator::CancellationState final {
    CancellationState() : token(snow_capture_cancellation_token_create()) {}

    ~CancellationState() {
        if (token != nullptr) {
            snow_capture_cancellation_token_destroy(token);
        }
    }

    void cancel() const {
        if (token != nullptr) {
            snow_capture_cancellation_token_cancel(token);
        }
    }

    SnowCaptureCancellationToken* token = nullptr;
};
#endif

ScreenshotCaptureCoordinator::ScreenshotCaptureCoordinator(QObject* parent) : QObject(parent) {}

ScreenshotCaptureCoordinator::~ScreenshotCaptureCoordinator() {
    shutdown();
}

bool ScreenshotCaptureCoordinator::hasWorker() const {
#if defined(Q_OS_LINUX)
    // The Qt capture provider lives on the GUI thread; "worker" reports whether
    // the capture runtime is available, which it always is once constructed.
    return true;
#else
    return m_worker != nullptr;
#endif
}

void ScreenshotCaptureCoordinator::prepareAsync(quint64 requestId) {
#if defined(Q_OS_LINUX)
    static_cast<void>(QMetaObject::invokeMethod(
        this, [this, requestId]() { emit prepared(requestId, true); }, Qt::QueuedConnection));
#else
    const QPointer<ScreenshotCaptureCoordinator> coordinator(this);
    static_cast<void>(postWorkerTask([coordinator, requestId](ScreenshotCaptureWorker& worker) {
        worker.prepare(requestId, coordinator);
    }));
#endif
}

void ScreenshotCaptureCoordinator::refreshLayoutAsync(quint64 requestId) {
#if defined(Q_OS_LINUX)
    static_cast<void>(QMetaObject::invokeMethod(
        this, [this, requestId]() { emit layoutRefreshed(requestId, true); },
        Qt::QueuedConnection));
#else
    const QPointer<ScreenshotCaptureCoordinator> coordinator(this);
    static_cast<void>(postWorkerTask([requestId, coordinator](ScreenshotCaptureWorker& worker) {
        worker.refreshLayout(requestId, coordinator);
    }));
#endif
}

void ScreenshotCaptureCoordinator::captureAsync(const ScreenshotCaptureRequest& request) {
#if defined(Q_OS_LINUX)
    // QScreen::grabWindow must run on the GUI thread. A queued grab keeps the same
    // asynchronous contract as the FFI worker, and the generation check preserves
    // its cancellation semantics.
    const quint64 generation = ++m_linuxGeneration;
    static_cast<void>(QMetaObject::invokeMethod(
        this, [this, generation, request]() { finishLinuxCapture(generation, request); },
        Qt::QueuedConnection));
#else
    // Cold worker initialization calls shutdown(); do it before publishing the new token.
    ensureWorker();
    cancelActiveCapture();
    auto cancellation = std::make_shared<CancellationState>();
    if (cancellation->token == nullptr) {
        ScreenshotCaptureResult result;
        result.requestId = request.requestId;
        result.purpose = request.purpose;
        result.errorMessage = QStringLiteral("Failed to create capture cancellation token");
        emit captureFinished(std::move(result));
        return;
    }
    m_activeCancellation = cancellation;
    ScreenshotCaptureRequest preparedRequest = request;
    if (request.captureCursor && !preparedRequest.cursorSnapshot) {
        preparedRequest.cursorSnapshot = std::shared_ptr<SnowCaptureCursorSnapshot>(
            snow_capture_cursor_snapshot_create(), snow_capture_cursor_snapshot_destroy);
        // Cursor sampling is optional to screenshot success. Never substitute stale
        // backend metadata when the global cursor cannot be sampled.
    }
    const QPointer<ScreenshotCaptureCoordinator> coordinator(this);
    if (!postWorkerTask(
            [coordinator, preparedRequest, cancellation](ScreenshotCaptureWorker& worker) {
                worker.capture(preparedRequest, coordinator, cancellation->token);
            })) {
        cancellation->cancel();
        ScreenshotCaptureResult result;
        result.requestId = request.requestId;
        result.purpose = request.purpose;
        result.errorMessage = QStringLiteral("Capture worker is unavailable");
        emit captureFinished(std::move(result));
    }
#endif
}

void ScreenshotCaptureCoordinator::cancelActiveCapture() {
#if defined(Q_OS_LINUX)
    ++m_linuxGeneration;
#else
    if (m_activeCancellation != nullptr) {
        m_activeCancellation->cancel();
        m_activeCancellation.reset();
    }
#endif
}

void ScreenshotCaptureCoordinator::shutdown() {
#if defined(Q_OS_LINUX)
    ++m_linuxGeneration;
#else
    cancelActiveCapture();
    if (m_thread == nullptr) {
        m_worker = nullptr;
        return;
    }

    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
#endif
}

void ScreenshotCaptureCoordinator::ensureWorker() {
#if defined(Q_OS_LINUX)
    return;
#else
    if (hasWorker()) {
        return;
    }

    shutdown();
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("snow-shot-capture"));
    m_worker = new ScreenshotCaptureWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    snow_shot::platform::configureApplicationQoSThread(m_thread);
    m_thread->start();
#endif
}

#if defined(Q_OS_LINUX)
void ScreenshotCaptureCoordinator::finishLinuxCapture(quint64 generation,
                                                      const ScreenshotCaptureRequest& request) {
    if (generation != m_linuxGeneration) {
        return;
    }

    if (request.purpose == ScreenshotCapturePurpose::Initial) {
        ScreenshotCaptureLayout layout;
        layout.requestId = request.requestId;
        layout.generation = request.requestId;
        layout.displays = ScreenshotQtCaptureProvider::layoutDisplays();
        if (layout.displays.isEmpty()) {
            ScreenshotCaptureResult result;
            result.requestId = request.requestId;
            result.purpose = request.purpose;
            result.errorMessage =
                ScreenshotQtCaptureProvider::tr("No screens are available for capture");
            emit captureFinished(std::move(result));
            return;
        }
        emit layoutReady(std::move(layout));
        // Handling layoutReady can cancel the session, which bumps the generation.
        if (generation != m_linuxGeneration) {
            return;
        }
    }

    ScreenshotCaptureResult result;
    result.requestId = request.requestId;
    result.purpose = request.purpose;
    // Own windows (floating toolbar, main window, pinned windows, and any
    // already-presented overlay) must not appear in the frames.
    const auto shieldState = snow_shot::platform::x11::concealWidgetsForCapture(
        snow_shot::platform::x11::visibleTopLevelWidgets());
    result.displays = ScreenshotQtCaptureProvider::captureDisplays(
        &result.errorMessage, snow_shot::storage::ScreenshotSettings().showCursor());
    snow_shot::platform::x11::restoreWidgetsAfterCapture(shieldState);
    result.succeeded = !result.displays.isEmpty();
    result.cursorAvailable = std::any_of(
        result.displays.cbegin(), result.displays.cend(),
        [](const CapturedDisplayModel& display) { return !display.cursorPatch.isNull(); });
    emit captureFinished(std::move(result));
}
#else
template <typename Task> bool ScreenshotCaptureCoordinator::postWorkerTask(Task&& task) {
    ensureWorker();
    if (!hasWorker()) {
        return false;
    }

    const QPointer<ScreenshotCaptureWorker> worker(m_worker);
    return QMetaObject::invokeMethod(
        m_worker,
        snow_shot::runtime::trackRuntimeWork([worker, task = std::forward<Task>(task)]() mutable {
            if (!worker.isNull()) {
                task(*worker);
            }
        }),
        Qt::QueuedConnection);
}
#endif
