#pragma once

#include <array>

struct AAssetManager;

// Owns the strings passed to generate_eth_wallet(). Keep this object alive
// until wallet generation has finished.
class Bip39WordList {
public:
    bool load(AAssetManager* assets);
    const char** words() noexcept { return loaded_ ? pointers_.data() : nullptr; }

    Bip39WordList() = default;
    Bip39WordList(const Bip39WordList&) = delete;
    Bip39WordList& operator=(const Bip39WordList&) = delete;

private:
    std::array<std::array<char, 16>, 2048> storage_{};
    std::array<const char*, 2048> pointers_{};
    bool loaded_ = false;
};
