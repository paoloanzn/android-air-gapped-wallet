#include "qr_scan_worker.hpp"
#include "hex.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace {

std::optional<QrScanWorker::Result> parseBytes(std::span<const uint8_t> bytes) {
    QrScanWorker::Result result;
    if (bytes.size() == result.hash.size()) {
        std::copy(bytes.begin(), bytes.end(), result.hash.begin());
        return result;
    }

    result.transaction = Transaction::decode(bytes);
    if (!result.transaction)
        return std::nullopt;

    result.hash = result.transaction->signingHash();
    return result;
}

std::optional<QrScanWorker::Result> parsePayload(const QrScanner::Payload& payload) {
    if (payload.empty())
        return std::nullopt;

    const std::span<const uint8_t> bytes(payload);
    if (bytes.size() == 32 || (!bytes.empty() && bytes.front() == 0x02))
        return parseBytes(bytes);

    const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (text.find('\0') != std::string::npos)
        return std::nullopt;

    const size_t prefix = text.starts_with("0x") || text.starts_with("0X") ? 2 : 0;
    const size_t digits = text.size() - prefix;
    if (digits == 0 || digits % 2 != 0)
        return std::nullopt;

    std::vector<uint8_t> decoded(digits / 2);
    if (hex_decode(text.c_str(), decoded.data(), decoded.size()) != int(decoded.size()))
        return std::nullopt;

    return parseBytes(decoded);
}

} // namespace

QrScanWorker::QrScanWorker() : thread_([this] { run(); }) {}

QrScanWorker::~QrScanWorker() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        frame_.reset();
    }

    ready_.notify_one();
    thread_.join();
}

bool QrScanWorker::submit(std::vector<uint8_t>&& rgba, int width, int height) {
    const std::lock_guard lock(mutex_);
    if (stopping_ || busy_ || completion_)
        return false;

    frame_ = Frame{std::move(rgba), width, height};
    busy_ = true; // Reserve before waking the thread, including scheduling time.
    ready_.notify_one();
    return true;
}

std::optional<QrScanWorker::Completion> QrScanWorker::takeResult() {
    const std::lock_guard lock(mutex_);
    return std::exchange(completion_, std::nullopt);
}

void QrScanWorker::run() {
    QrScanner scanner;
    std::unique_lock lock(mutex_);
    while (true) {
        ready_.wait(lock, [this] { return stopping_ || frame_.has_value(); });
        if (stopping_)
            return;

        auto frame = std::move(*frame_);
        frame_.reset();
        lock.unlock();
        Completion completion;

        try {
            const auto payloads = scanner.parseFrame(std::move(frame.rgba),
                                                     frame.width, frame.height);

            for (const auto& payload : payloads) {
                completion.result = parsePayload(payload);
                if (completion.result)
                    break;
            }

        } catch (const std::exception&) {
            completion.error = "QR decoding failed. Please reopen the camera.";
        }

        lock.lock();
        if (stopping_)
            return;

        completion_ = std::move(completion);
        busy_ = false;
    }
}
