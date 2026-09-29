#include "wallet.hpp"

#include "bip39_words.hpp"
#include "crypto/eth_crypto.h"
#include "crypto/rlp.h"
#include "hex.hpp"
#include "tx.hpp"

#include <openssl/crypto.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>

namespace {

constexpr uint8_t kTransactionType = 0x02;

// Returns true when publicKey signed hash with a low-s signature (EIP-2).
bool isSignedBy(const Wallet::PublicKey& publicKey, const Wallet::Hash& hash,
                const Wallet::Signature& signature) {
    using Context = std::unique_ptr<secp256k1_context,
                                    decltype(&secp256k1_context_destroy)>;
    Context context(secp256k1_context_create(SECP256K1_CONTEXT_NONE),
                    secp256k1_context_destroy);

    if (!context || signature.recoveryId < 0 || signature.recoveryId > 1)
        return false;

    std::array<uint8_t, 64> compact{};
    std::copy(signature.r.begin(), signature.r.end(), compact.begin());
    std::copy(signature.s.begin(), signature.s.end(), compact.begin() + 32);

    secp256k1_ecdsa_recoverable_signature recoverable;
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            context.get(), &recoverable, compact.data(), signature.recoveryId))
        return false;

    // normalize() returns 1 when s was in the upper half of the curve order.
    secp256k1_ecdsa_signature plain;
    secp256k1_ecdsa_recoverable_signature_convert(context.get(), &plain,
                                                  &recoverable);
    if (secp256k1_ecdsa_signature_normalize(context.get(), nullptr, &plain))
        return false;

    secp256k1_pubkey recovered;
    if (!secp256k1_ecdsa_recover(context.get(), &recovered, &recoverable,
                                 hash.data()))
        return false;

    std::array<uint8_t, 65> serialized{};
    size_t length = serialized.size();
    if (!secp256k1_ec_pubkey_serialize(context.get(), serialized.data(),
                                       &length, &recovered,
                                       SECP256K1_EC_UNCOMPRESSED))
        return false;

    // Skip the 0x04 prefix; PublicKey stores only the coordinates.
    return std::equal(publicKey.begin(), publicKey.end(),
                      serialized.begin() + 1);
}

// RLP descriptors for [[address, [storageKey, ...]], ...]. They point into
// the access list, which must stay alive and unchanged while encoding.
class AccessListRlp {
public:

    explicit AccessListRlp(const std::vector<Transaction::AccessEntry>& list)
        : entries_(list.size()), fields_(list.size()), keys_(list.size()) {
        for (size_t i = 0; i < list.size(); ++i) {
            const Transaction::AccessEntry& entry = list[i];
            for (const Transaction::Hash& key : entry.storageKeys)
                keys_[i].push_back(rlp_bytes(key.data(), key.size()));

            fields_[i][0] = rlp_bytes(entry.address.data(), entry.address.size());
            fields_[i][1] = rlp_list(keys_[i].data(), keys_[i].size());
            entries_[i] = rlp_list(fields_[i].data(), fields_[i].size());
        }
    }

    // Copies would keep pointing into the original's storage.
    AccessListRlp(const AccessListRlp&) = delete;
    AccessListRlp& operator=(const AccessListRlp&) = delete;

    rlp_value value() const { return rlp_list(entries_.data(), entries_.size()); }

private:
    std::vector<rlp_value> entries_;
    std::vector<std::array<rlp_value, 2>> fields_;
    std::vector<std::vector<rlp_value>> keys_;
};

} // namespace

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

std::optional<std::vector<uint8_t>> Wallet::encodeSigned(
    const Transaction& transaction, const Signature& signature) const {
    if (!isSignedBy(publicKey_, transaction.signingHash(), signature))
        return std::nullopt;

    const Transaction::Details& tx = transaction.details();
    const AccessListRlp accessList(tx.accessList);
    const uint8_t* recipient = tx.to ? tx.to->data() : nullptr;
    const size_t recipientSize = tx.to ? tx.to->size() : 0;

    // EIP-1559 field order. Integers are encoded without leading zeroes.
    const std::array<rlp_value, 12> fields = {
        rlp_uint64(tx.chainId),
        rlp_uint64(tx.nonce),
        rlp_uint_be(tx.maxPriorityFeePerGas.data(), tx.maxPriorityFeePerGas.size()),
        rlp_uint_be(tx.maxFeePerGas.data(), tx.maxFeePerGas.size()),
        rlp_uint64(tx.gasLimit),

        rlp_bytes(recipient, recipientSize),
        rlp_uint_be(tx.value.data(), tx.value.size()),
        rlp_bytes(tx.data.data(), tx.data.size()),
        accessList.value(),

        rlp_uint64(static_cast<uint64_t>(signature.recoveryId)),
        rlp_uint_be(signature.r.data(), signature.r.size()),
        rlp_uint_be(signature.s.data(), signature.s.size()),
    };

    const rlp_value list = rlp_list(fields.data(), fields.size());
    size_t size = 0;
    if (rlp_encoded_size(&list, &size) != RLP_OK)
        return std::nullopt;

    std::vector<uint8_t> encoded(1 + size);
    encoded[0] = kTransactionType;
    if (rlp_encode(&list, encoded.data() + 1, size, &size) != RLP_OK)
        return std::nullopt;

    // Decoding recomputes the signing hash, proving the bytes round-trip.
    const auto decoded = Transaction::decode(encoded);
    if (!decoded || decoded->signingHash() != transaction.signingHash())
        return std::nullopt;

    return encoded;
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
