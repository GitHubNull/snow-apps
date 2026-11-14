#ifndef SNOW_SHOT_PRESENTATION_LINUXSELECTEDTEXTBACKEND_H
#define SNOW_SHOT_PRESENTATION_LINUXSELECTEDTEXTBACKEND_H

#include "snow_shot/presentation/selectedtexttranslationcontroller.h"

#include <QString>

#include <atomic>
#include <memory>
#include <thread>

namespace snow_shot::presentation {

enum class SelectionTextReadStatus { Read, Empty, Unavailable };

struct SelectionTextReadResult {
    SelectionTextReadStatus status = SelectionTextReadStatus::Unavailable;
    QString text;
};

// Reads the desktop selection on capture workers. Implementations must honor
// the cancellation flag between bounded waits.
class SelectionTextReader {
  public:
    virtual ~SelectionTextReader() = default;
    virtual SelectionTextReadResult read(const std::atomic<bool>& cancelled) = 0;
};

// Reads the X11 PRIMARY selection. Other platforms report Unavailable.
[[nodiscard]] std::unique_ptr<SelectionTextReader> createPrimarySelectionTextReader();

// Bounded truncation that never splits a UTF-8 code point.
[[nodiscard]] QString truncateSelectionText(const QString& text, qsizetype maxUtf8Bytes);

class LinuxSelectedTextCaptureBackend final : public SelectedTextCaptureBackend {
  public:
    static constexpr qsizetype kMaxTextBytes = 1024 * 1024;
    static constexpr int kTimeoutMs = 800;

    explicit LinuxSelectedTextCaptureBackend(std::shared_ptr<SelectionTextReader> reader);
    ~LinuxSelectedTextCaptureBackend() override;

    SelectedTextCaptureResult start() override;
    SelectedTextCaptureResult poll() override;
    void cancel() override;

  private:
    struct State;

    std::shared_ptr<SelectionTextReader> m_reader;
    std::shared_ptr<State> m_state;
    std::thread m_worker;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_LINUXSELECTEDTEXTBACKEND_H
