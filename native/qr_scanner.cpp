#include "qr_scanner.hpp"

#include <algorithm>
#include <limits>
#include <quirc.h>

namespace {

// Release the decoder on every return path, including exceptions.
struct BusyGuard {
    std::atomic<bool>& busy;
    ~BusyGuard() { busy.store(false); }
};

bool validFrameSize(size_t bytes, int width, int height) {
    if (width <= 0 || height <= 0)
        return false;

    // quirc uses signed int pixel offsets internally; RGBA must also fit size_t.
    const auto maxPixels = std::min(size_t(std::numeric_limits<int>::max()),
                                    std::numeric_limits<size_t>::max() / 4);
    if (size_t(width) > maxPixels / size_t(height))
        return false;

    return bytes == size_t(width) * size_t(height) * 4;
}

std::vector<QrScanner::Payload> decodePayloads(quirc* decoder) {
    std::vector<QrScanner::Payload> payloads;
    for (int index = 0; index < quirc_count(decoder); ++index) {
        quirc_code code{};
        quirc_data data{};
        quirc_extract(decoder, index, &code);

        auto status = quirc_decode(&code, &data);
        if (status == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&code);
            status = quirc_decode(&code, &data);
        }

        if (status == QUIRC_SUCCESS)
            payloads.emplace_back(data.payload, data.payload + data.payload_len);
    }

    return payloads;
}

} // namespace

QrScanner::QrScanner() : decoder_(quirc_new(), quirc_destroy) {}

QrScanner::~QrScanner() = default;

bool QrScanner::isBusy() const noexcept {
    return busy_.load();
}

std::vector<QrScanner::Payload> QrScanner::parseFrame(std::vector<uint8_t> rgba,
                                                    int width, int height) {
    if (busy_.exchange(true))
        return {};

    const BusyGuard guard{busy_};
    if (!validFrameSize(rgba.size(), width, height))
        return {};

    if (!decoder_)
        decoder_.reset(quirc_new());

    if (!decoder_)
        return {};

    if (width != width_ || height != height_) {
        if (quirc_resize(decoder_.get(), width, height) < 0)
            return {};

        width_ = width;
        height_ = height;
    }

    auto* grayscale = quirc_begin(decoder_.get(), nullptr, nullptr);
    const size_t pixels = rgba.size() / 4;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        const auto* color = rgba.data() + pixel * 4;
        grayscale[pixel] = uint8_t((77 * color[0] + 150 * color[1] + 29 * color[2]) >> 8);
    }

    quirc_end(decoder_.get());
    return decodePayloads(decoder_.get());
}
