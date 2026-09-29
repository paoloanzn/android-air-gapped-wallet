#pragma once

#include <array>
#include <cstdint>

namespace eth {

using Address = std::array<uint8_t, 20>;
using Hash = std::array<uint8_t, 32>;

// Unsigned 256-bit values are stored in big-endian byte order.
using Uint256 = std::array<uint8_t, 32>;

using PrivateKey = std::array<uint8_t, 32>;
// Uncompressed public key coordinates without the 0x04 prefix.
using PublicKey = std::array<uint8_t, 64>;

} // namespace eth
