#pragma once

#include <atomic>
#include <memory>
#include <cstdint>
#include <vector>

struct quirc;

class QrScanner {
public:
    using Payload = std::vector<uint8_t>;

    QrScanner();
    ~QrScanner();

    QrScanner(const QrScanner&) = delete;
    QrScanner& operator=(const QrScanner&) = delete;
    QrScanner(QrScanner&&) = delete;
    QrScanner& operator=(QrScanner&&) = delete;

    // Synchronous: decodes packed RGBA pixels on the worker thread.
    // Returns raw QR payloads; the worker interprets hashes and transactions.
    // Invalid frames, decode failures and busy calls return an empty list.
    std::vector<Payload> parseFrame(std::vector<uint8_t> rgba, int width, int height);

    // Thread-safe snapshot of active decoding, not a reservation for queued work.
    // Keep at most one worker task outstanding, including before it starts running.
    // Start the worker when the camera opens; join it when the camera closes.
    // The worker must be joined before destroying this object.
    bool isBusy() const noexcept;

private:
    std::unique_ptr<quirc, void (*)(quirc*)> decoder_;
    std::atomic<bool> busy_{false};
    int width_ = 0;
    int height_ = 0;
};
