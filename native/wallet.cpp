#include "wallet.hpp"

#include "bip39_words.hpp"
#include "crypto/eth_crypto.h"
#include "hex.hpp"

#include <openssl/crypto.h>
#include <secp256k1.h>

#include <array>
#include <cstring>
#include <memory>
#include <utility>

std::optional<Wallet> Wallet::create(AAssetManager* assets,
                                      RecoveryWords& recoveryWords) {
    Bip39WordList wordList;
    if (!wordList.load(assets))
        return std::nullopt;

    Wallet wallet;
    char words[24][16]{};
    const int result = generate_eth_wallet(wordList.words(),
                                           wallet.privateKey_.data(),
                                           wallet.publicKey_.data(),
                                           wallet.address_.data(), words);

    if (result == 0)
        std::memcpy(recoveryWords.data(), words, sizeof(words));

    OPENSSL_cleanse(words, sizeof(words));
    if (result != 0)
        return std::nullopt;

    return wallet;
}

std::optional<Wallet> Wallet::load(const PrivateKey& privateKey) {
    using Context = std::unique_ptr<secp256k1_context,
                                    decltype(&secp256k1_context_destroy)>;
    Context context(secp256k1_context_create(SECP256K1_CONTEXT_SIGN),
                    secp256k1_context_destroy);
    if (!context)
        return std::nullopt;

    secp256k1_pubkey publicKey;
    if (!secp256k1_ec_seckey_verify(context.get(), privateKey.data()) ||
        !secp256k1_ec_pubkey_create(context.get(), &publicKey,
                                    privateKey.data()))
        return std::nullopt;

    std::array<uint8_t, 65> serialized{};
    size_t length = serialized.size();
    if (!secp256k1_ec_pubkey_serialize(context.get(), serialized.data(),
                                       &length, &publicKey,
                                       SECP256K1_EC_UNCOMPRESSED) ||
        length != serialized.size())
        return std::nullopt;

    Hash hash{};
    if (!eth_keccak256(serialized.data() + 1, 64, hash.data()))
        return std::nullopt;

    Wallet wallet;
    wallet.privateKey_ = privateKey;
    std::memcpy(wallet.publicKey_.data(), serialized.data() + 1, 64);
    std::memcpy(wallet.address_.data(), hash.data() + 12, 20);

    return wallet;
}

std::optional<Wallet> Wallet::load(PrivateKey&& privateKey) {
    auto wallet = load(static_cast<const PrivateKey&>(privateKey));
    OPENSSL_cleanse(privateKey.data(), privateKey.size());
    return wallet;
}

Wallet::Wallet(Wallet&& other) noexcept : Wallet() {
    *this = std::move(other);
}

Wallet& Wallet::operator=(Wallet&& other) noexcept {
    if (this == &other)
        return *this;

    OPENSSL_cleanse(privateKey_.data(), privateKey_.size());
    privateKey_ = other.privateKey_;
    publicKey_ = other.publicKey_;
    address_ = other.address_;

    OPENSSL_cleanse(other.privateKey_.data(), other.privateKey_.size());
    other.publicKey_.fill(0);
    other.address_.fill(0);
    return *this;
}

Wallet::~Wallet() {
    OPENSSL_cleanse(privateKey_.data(), privateKey_.size());
}

bool Wallet::sign(const Hash& hash, Signature& signature) const {
    Signature result;
    if (sign_hash(privateKey_.data(), hash.data(), result.r.data(),
                  result.s.data(), &result.recoveryId) != 0)
        return false;

    signature = result;
    return true;
}

std::string Wallet::addressHexEncoded() const {
    std::array<char, 2 + 20 * 2 + 1> text{};
    hex_encode(address_.data(), address_.size(), text.data());
    return text.data();
}

std::string Wallet::publicKeyHexEncoded() const {
    std::array<char, 2 + 64 * 2 + 1> text{};
    hex_encode(publicKey_.data(), publicKey_.size(), text.data());
    return text.data();
}
