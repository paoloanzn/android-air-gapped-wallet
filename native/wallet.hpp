#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

struct AAssetManager;

class Wallet {
public:
    using Hash = std::array<uint8_t, 32>;
    using PrivateKey = std::array<uint8_t, 32>;
    using PublicKey = std::array<uint8_t, 64>;
    using Address = std::array<uint8_t, 20>;
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
