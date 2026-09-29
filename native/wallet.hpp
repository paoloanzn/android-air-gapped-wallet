#pragma once

#include "types.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

struct AAssetManager;
class Transaction;

class Wallet {
public:
    using Hash = eth::Hash;
    using PrivateKey = eth::PrivateKey;
    using PublicKey = eth::PublicKey;
    using Address = eth::Address;
    using RecoveryWords = std::array<std::array<char, 16>, 24>;

    struct Signature {
        Hash r{};
        Hash s{};
        int recoveryId = 0;
    };

    // Creates the first Ethereum account and returns its 24 recovery words.
    // Keep the words private and clear them after making a secure backup.
    static std::optional<Wallet> create(AAssetManager* assets,
                                        RecoveryWords& recoveryWords);

    // Restores a wallet from a 32-byte secp256k1 private key.
    static std::optional<Wallet> load(const PrivateKey& privateKey);
    static std::optional<Wallet> load(PrivateKey&& privateKey);

    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;
    Wallet(Wallet&& other) noexcept;
    Wallet& operator=(Wallet&& other) noexcept;
    ~Wallet();

    bool sign(const Hash& hash, Signature& signature) const;

    // Adds signature to transaction and returns the raw 0x02-prefixed bytes,
    // ready to broadcast. Any signature the transaction already carries is
    // replaced. Fails unless this wallet signed transaction.signingHash().
    std::optional<std::vector<uint8_t>> encodeSigned(
        const Transaction& transaction, const Signature& signature) const;

    const Address& address() const noexcept { return address_; }
    const PublicKey& publicKey() const noexcept { return publicKey_; }

    std::string addressHexEncoded() const;
    std::string publicKeyHexEncoded() const;

private:
    Wallet() = default;
    friend class WalletStore;

    PrivateKey privateKey_{};
    PublicKey publicKey_{};
    Address address_{};
};
