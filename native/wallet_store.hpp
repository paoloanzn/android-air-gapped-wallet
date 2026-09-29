#pragma once

#include "types.hpp"
#include "wallet.hpp"

#include <jni.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ANativeActivity;

// Disk format: AA, version 2, little-endian key count and byte size, then keys.
// Each key is name length, key version, 60 encrypted bytes, 20 address bytes and
// name bytes. The 60 bytes are a 12-byte GCM nonce, 32 ciphertext bytes and a
// 16-byte tag. Version 1 files have no key version byte; all their keys are v1.
// C++ struct padding is never saved.

struct StoredKey {
    std::string name;
    // 1: legacy Keystore key without user authentication, re-sealed on first use.
    // 2: Keystore key that requires a fresh user authentication for every use.
    uint8_t keyVersion = 2;

    std::array<uint8_t, 60> encryptedKey{};
    eth::Address address{};
};

struct Envelope {
    std::vector<StoredKey> keys;
};

class WalletStore {
public:
    struct WalletInfo {
        std::string name;
        eth::Address address{};
    };

    // Text for the system authentication prompt.
    struct Prompt {
        std::string title;
        std::string subtitle;
    };

    // One wallet key use waiting for system authentication: a strong biometric
    // such as a fingerprint, or the device PIN, pattern or password as fallback.
    // Every request needs its own authentication; there is no grace period.
    // Poll from the thread that created it; destroying it cancels the prompt.
    class Request {
    public:
        enum class Status { Waiting, Done, Failed };

        ~Request();
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;

        // Once the user has authenticated, finishes the key operation here.
        Status poll();
        const std::string& error() const noexcept { return error_; }

        // The unlocked wallet of a finished load(), handed out once.
        std::optional<Wallet> takeWallet();

    private:
        enum class Kind { Save, Load, Reseal };

        friend class WalletStore;
        Request(WalletStore& store, Kind kind);

        Status fail(std::string error);
        bool finish();
        void release();

        WalletStore& store_;
        Kind kind_;
        StoredKey entry_;
        std::optional<Wallet> wallet_;

        Status status_ = Status::Waiting;
        std::string error_;
        jobject cipher_ = nullptr;
        jobject cancel_ = nullptr;
        jlong promptId_ = 0;
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

    // save() and load() require Android 12+ and a TEE-backed Keystore key, and
    // always return a request. The store must not move or be destroyed while
    // a request exists.
    std::unique_ptr<Request> save(std::string_view name, Wallet&& wallet,
                                  const Prompt& prompt);
    std::unique_ptr<Request> load(std::string_view name, const Prompt& prompt);
    bool erase(std::string_view name);

private:
    WalletStore(ANativeActivity* activity, std::string path);
    bool write(const Envelope& envelope) const;
    bool commit(const StoredKey& entry);
    const StoredKey* find(std::string_view name) const;

    std::unique_ptr<Request> start(std::unique_ptr<Request> request, int mode,
                                   const Prompt& prompt);

    ANativeActivity* activity_;
    std::string path_;
    Envelope envelope_;
};
