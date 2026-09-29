#pragma once

#include "types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class CallDefinitions;

// A decoded EIP-1559 (type 0x02) transaction. This class never signs or
// changes transactions; pass signingHash() to Wallet::sign() if needed.
class Transaction {
public:
    using Hash = eth::Hash;
    using Address = eth::Address;
    using Amount = eth::Uint256;

    struct AccessEntry {
        Address address{};
        std::vector<Hash> storageKeys;
    };

    struct Signature {
        uint8_t yParity = 0;
        Amount r{};
        Amount s{};
    };

    struct Details {
        uint64_t chainId = 0;
        uint64_t nonce = 0;
        Amount maxPriorityFeePerGas{};
        Amount maxFeePerGas{};
        uint64_t gasLimit = 0;

        std::optional<Address> to;
        Amount value{};
        std::vector<uint8_t> data;
        std::vector<AccessEntry> accessList;
        std::optional<Signature> signature;
    };

    // Rejects malformed or noncanonical RLP and unsupported transaction types.
    // A present signature is decoded but its signer is not verified.
    static std::optional<Transaction> decode(std::span<const uint8_t> raw);

    const Details& details() const noexcept { return details_; }
    const Hash& signingHash() const noexcept { return signingHash_; }

    // Keeps raw calldata; supplied definitions label matching ABI calls.
    // Token metadata and contract behavior cannot be inferred from calldata.
    std::string describe(const CallDefinitions* definitions = nullptr) const;

private:
    Details details_;
    Hash signingHash_{};
};
