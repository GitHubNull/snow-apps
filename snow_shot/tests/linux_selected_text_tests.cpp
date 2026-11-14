#include "snow_shot/presentation/linuxselectedtextbackend.h"
#include "snow_shot/presentation/selectedtexttranslationcontroller.h"

#include <QCoreApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using snow_shot::presentation::LinuxSelectedTextCaptureBackend;
using snow_shot::presentation::SelectedTextCaptureResult;
using snow_shot::presentation::SelectedTextStatus;
using snow_shot::presentation::SelectedTextTranslationController;
using snow_shot::presentation::SelectionTextReader;
using snow_shot::presentation::SelectionTextReadResult;
using snow_shot::presentation::SelectionTextReadStatus;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class FixedReader final : public SelectionTextReader {
  public:
    explicit FixedReader(SelectionTextReadResult result) : m_result(std::move(result)) {}
    SelectionTextReadResult read(const std::atomic<bool>&) override {
        return m_result;
    }

  private:
    SelectionTextReadResult m_result;
};

class BlockingReader final : public SelectionTextReader {
  public:
    SelectionTextReadResult read(const std::atomic<bool>& cancelled) override {
        while (!cancelled.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return {SelectionTextReadStatus::Unavailable, {}};
    }
};

SelectedTextCaptureResult waitForResult(LinuxSelectedTextCaptureBackend& backend) {
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 2000) {
        SelectedTextCaptureResult result = backend.poll();
        if (result.status != SelectedTextStatus::Pending)
            return result;
        QThread::msleep(2);
    }
    require(false, "selected-text backend did not finish before the deadline");
    return {};
}

void testTruncation() {
    using snow_shot::presentation::truncateSelectionText;
    require(truncateSelectionText(QStringLiteral("abc"), 3) == QStringLiteral("abc"),
            "exact-fit ASCII must be preserved");
    require(truncateSelectionText(QStringLiteral("abcd"), 3) == QStringLiteral("abc"),
            "ASCII truncation must stop at the byte budget");
    require(truncateSelectionText(QString::fromUtf8("ab中"), 4) == QStringLiteral("ab"),
            "truncation must not split a three-byte code point");
    require(truncateSelectionText(QString::fromUtf8("😀"), 3).isEmpty(),
            "truncation must not split a surrogate pair");
    require(truncateSelectionText(QStringLiteral("abc"), 0).isEmpty(),
            "a zero budget must yield empty text");
}

void testSelectedText() {
    const QString selection = QString::fromUtf8("选中中文");
    auto backend = std::make_unique<LinuxSelectedTextCaptureBackend>(std::make_shared<FixedReader>(
        SelectionTextReadResult{SelectionTextReadStatus::Read, selection}));
    require(backend->start().status == SelectedTextStatus::Pending,
            "capture must start asynchronously");
    const SelectedTextCaptureResult result = waitForResult(*backend);
    require(result.status == SelectedTextStatus::Selected,
            "selection must be reported as selected");
    require(result.text == selection, "selection text must be delivered unchanged");
}

void testEmptySelection() {
    LinuxSelectedTextCaptureBackend backend(
        std::make_shared<FixedReader>(SelectionTextReadResult{SelectionTextReadStatus::Empty, {}}));
    static_cast<void>(backend.start());
    require(waitForResult(backend).status == SelectedTextStatus::NoSelection,
            "an empty selection must be reported as no selection");
}

void testUnavailableSelection() {
    LinuxSelectedTextCaptureBackend backend(std::make_shared<FixedReader>(
        SelectionTextReadResult{SelectionTextReadStatus::Unavailable, {}}));
    static_cast<void>(backend.start());
    require(waitForResult(backend).status == SelectedTextStatus::Unsupported,
            "a missing selection owner must be reported as unsupported");
}

void testOversizedSelection() {
    const QString oversized(LinuxSelectedTextCaptureBackend::kMaxTextBytes + 16, QLatin1Char('a'));
    LinuxSelectedTextCaptureBackend backend(std::make_shared<FixedReader>(
        SelectionTextReadResult{SelectionTextReadStatus::Read, oversized}));
    static_cast<void>(backend.start());
    const SelectedTextCaptureResult result = waitForResult(backend);
    require(result.status == SelectedTextStatus::Selected, "oversized text must still be selected");
    require(result.text.toUtf8().size() <= LinuxSelectedTextCaptureBackend::kMaxTextBytes,
            "oversized text must be truncated to the byte budget");
}

void testCancellation() {
    QElapsedTimer elapsed;
    elapsed.start();
    {
        LinuxSelectedTextCaptureBackend backend(std::make_shared<BlockingReader>());
        static_cast<void>(backend.start());
        backend.cancel();
    }
    require(elapsed.elapsed() < 1000, "cancellation must stop the reader promptly");
}

void testControllerIntegration() {
    auto reader = std::make_shared<FixedReader>(
        SelectionTextReadResult{SelectionTextReadStatus::Read, QStringLiteral("hello")});
    SelectedTextTranslationController controller(
        std::make_unique<LinuxSelectedTextCaptureBackend>(reader));
    QString delivered;
    int completions = 0;
    QObject::connect(&controller, &SelectedTextTranslationController::textReady, &controller,
                     [&](const QString& text) {
                         delivered = text;
                         ++completions;
                     });
    controller.capture();
    QElapsedTimer deadline;
    deadline.start();
    while (completions == 0 && deadline.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(2);
    }
    require(completions == 1 && delivered == QStringLiteral("hello"),
            "the controller must deliver the selected text");
}

} // namespace

// The main application supplies this factory from the X11 helper archive. The
// injected backends above never construct it, so a null reader is sufficient.
namespace snow_shot::presentation {
std::unique_ptr<SelectionTextReader> createPrimarySelectionTextReader() {
    return nullptr;
}
} // namespace snow_shot::presentation

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testTruncation();
    testSelectedText();
    testEmptySelection();
    testUnavailableSelection();
    testOversizedSelection();
    testCancellation();
    testControllerIntegration();
    return 0;
}
