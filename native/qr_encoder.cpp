#include "qr_encoder.hpp"

#include <qrencode.h>

#include <climits>
#include <memory>

std::optional<QREncoder::Code> QREncoder::encode(
    const std::vector<uint8_t>& transaction) {
    if (transaction.empty() || transaction.size() > INT_MAX)
        return std::nullopt;

    // Prefer stronger error correction; fall back when the data does not fit.
    if (auto code = encodeAt(transaction, Level::Medium))
        return code;

    return encodeAt(transaction, Level::Low);
}

std::optional<QREncoder::Code> QREncoder::encodeAt(
    const std::vector<uint8_t>& transaction, Level level) {
    static_assert(static_cast<int>(Level::Low) == QR_ECLEVEL_L);
    static_assert(static_cast<int>(Level::Medium) == QR_ECLEVEL_M);

    // Version 0 picks the smallest symbol that holds the data.
    using Symbol = std::unique_ptr<QRcode, decltype(&QRcode_free)>;
    const Symbol symbol(QRcode_encodeData(static_cast<int>(transaction.size()),
                                          transaction.data(), 0,
                                          static_cast<QRecLevel>(level)),
                        QRcode_free);

    if (!symbol)
        return std::nullopt;

    Code code;
    code.size = symbol->width;
    code.modules.resize(static_cast<size_t>(code.size) * code.size);

    // libqrencode sets bit 0 of each module byte for dark modules.
    for (size_t i = 0; i < code.modules.size(); ++i)
        code.modules[i] = symbol->data[i] & 1;

    return code;
}
