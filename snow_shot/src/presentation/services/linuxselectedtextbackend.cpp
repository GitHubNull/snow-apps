#include "snow_shot/presentation/linuxselectedtextbackend.h"

#include <QChar>

#include <mutex>
#include <utility>

namespace snow_shot::presentation {

struct LinuxSelectedTextCaptureBackend::State {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    bool done = false;
    SelectionTextReadResult result;
};

namespace {

qsizetype utf8BytesForCodeUnit(const QString& text, qsizetype index, qsizetype* units) {
    const char16_t code = text.at(index).unicode();
    if (QChar::isHighSurrogate(code) && index + 1 < text.size() &&
        QChar::isLowSurrogate(text.at(index + 1).unicode())) {
        *units = 2;
        return 4;
    }
    *units = 1;
    if (code <= 0x7F)
        return 1;
    if (code <= 0x7FF)
        return 2;
    return 3;
}

} // namespace

QString truncateSelectionText(const QString& text, qsizetype maxUtf8Bytes) {
    if (maxUtf8Bytes <= 0 || text.isEmpty())
        return {};
    qsizetype end = 0;
    qsizetype bytes = 0;
    while (end < text.size()) {
        qsizetype units = 1;
        const qsizetype encoded = utf8BytesForCodeUnit(text, end, &units);
        if (bytes + encoded > maxUtf8Bytes)
            break;
        bytes += encoded;
        end += units;
    }
    return end >= text.size() ? text : text.left(end);
}

LinuxSelectedTextCaptureBackend::LinuxSelectedTextCaptureBackend(
    std::shared_ptr<SelectionTextReader> reader)
    : m_reader(std::move(reader)) {}

LinuxSelectedTextCaptureBackend::~LinuxSelectedTextCaptureBackend() {
    cancel();
    if (m_worker.joinable())
        m_worker.join();
}

SelectedTextCaptureResult LinuxSelectedTextCaptureBackend::start() {
    if (m_worker.joinable())
        m_worker.join();
    if (!m_reader)
        return {SelectedTextStatus::Unsupported, {}};

    m_state = std::make_shared<State>();
    const auto state = m_state;
    const auto reader = m_reader;
    m_worker = std::thread([state, reader] {
        const SelectionTextReadResult result = reader->read(state->cancelled);
        std::lock_guard lock(state->mutex);
        if (!state->cancelled.load(std::memory_order_acquire))
            state->result = result;
        state->done = true;
    });
    return {};
}

SelectedTextCaptureResult LinuxSelectedTextCaptureBackend::poll() {
    if (!m_state)
        return {};
    SelectionTextReadResult result;
    {
        std::lock_guard lock(m_state->mutex);
        if (!m_state->done)
            return {};
        result = m_state->result;
    }
    if (m_worker.joinable())
        m_worker.join();
    switch (result.status) {
    case SelectionTextReadStatus::Unavailable:
        return {SelectedTextStatus::Unsupported, {}};
    case SelectionTextReadStatus::Empty:
        return {SelectedTextStatus::NoSelection, {}};
    case SelectionTextReadStatus::Read:
        break;
    }
    const QString text = truncateSelectionText(result.text, kMaxTextBytes);
    if (text.isEmpty())
        return {SelectedTextStatus::NoSelection, {}};
    return {SelectedTextStatus::Selected, text};
}

void LinuxSelectedTextCaptureBackend::cancel() {
    if (!m_state)
        return;
    m_state->cancelled.store(true, std::memory_order_release);
}

} // namespace snow_shot::presentation
