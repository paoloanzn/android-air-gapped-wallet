#pragma once

#include "wallet.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ANativeActivity;

// Disk format: AA, version 1, little-endian key count and byte size, then keys.
// Each key is name length, 60 encrypted bytes, 20 address bytes, and name bytes.
// The 60 bytes are a 12-byte GCM nonce, 32 ciphertext bytes, and a 16-byte tag.
// C++ struct padding is never saved.

struct StoredKey {
    std::string name;
    std::array<uint8_t, 60> encryptedKey{};
    Wallet::Address address{};
};

struct Envelope {
    std::vector<StoredKey> keys;
};

class WalletStore {
public:
    struct WalletInfo {
        std::string name;
        Wallet::Address address{};
    };

    // An absent file opens as an empty store; a malformed file fails closed.
    // The activity must remain alive while this store is used.
    static std::optional<WalletStore> open(ANativeActivity* activity);

    WalletStore(const WalletStore&) = delete;
    WalletStore& operator=(const WalletStore&) = delete;
    WalletStore(WalletStore&&) = default;
    WalletStore& operator=(WalletStore&&) = default;

    // Names and addresses are public metadata, authenticated on load().
    std::vector<WalletInfo> list() const;

    // save() and load() require Android 12+ and a TEE-backed Keystore key.
    bool save(std::string_view name, const Wallet& wallet);
    std::optional<Wallet> load(std::string_view name) const;
    bool erase(std::string_view name);

private:
    WalletStore(ANativeActivity* activity, std::string path);
    bool write(const Envelope& envelope) const;

    ANativeActivity* activity_;
    std::string path_;
    Envelope envelope_;
};
