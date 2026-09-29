#pragma once

#include "qr_scanner.hpp"
#include "tx.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// Own one worker per camera session. Destruction stops and joins the thread.
class QrScanWorker {
public:
    struct Result {
        eth::Hash hash{};
        std::optional<Transaction> transaction;
    };

    struct Completion {
        std::optional<Result> result;
        std::string error;
    };

    QrScanWorker();
    ~QrScanWorker();
    QrScanWorker(const QrScanWorker&) = delete;
    QrScanWorker& operator=(const QrScanWorker&) = delete;

    // Takes ownership only when idle; no queue of frames can accumulate.
    bool submit(std::vector<uint8_t>&& rgba, int width, int height);
    std::optional<Completion> takeResult();

private:
    struct Frame {
        std::vector<uint8_t> rgba;
        int width;
        int height;
    };

    void run();
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_ = false;
    bool busy_ = false;

    std::optional<Frame> frame_;
    std::optional<Completion> completion_;
    std::thread thread_;
};
