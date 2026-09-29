#pragma once

#include <cstdint>
#include <optional>
#include <vector>

// Turns an encoded signed transaction into raw QR modules. The UI draws them
// as an image and adds a 4-module light quiet zone around the symbol.
class QREncoder {
public:

    // A square grid of modules stored row by row: 1 is dark, 0 is light.
    struct Code {
        int size = 0;
        std::vector<uint8_t> modules;

        bool isDark(int x, int y) const {
            return modules[static_cast<size_t>(y) * size + x] != 0;
        }
    };

    // Stores the bytes as-is in 8-bit mode, the form QrScanner reads back.
    // Returns nothing when the bytes are empty or do not fit in one symbol.
    static std::optional<Code> encode(const std::vector<uint8_t>& transaction);

private:
    // Values match libqrencode's QRecLevel.
    enum class Level { Low = 0, Medium = 1 };

    static std::optional<Code> encodeAt(const std::vector<uint8_t>& transaction,
                                        Level level);
};
