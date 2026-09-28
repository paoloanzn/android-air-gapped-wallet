#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <stddef.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "eth_crypto.h"

static int bip39_encode(const char **wordlist, const uint8_t entropy[32],
                        char words[24][16], char phrase[24 * 16],
                        size_t *phrase_len)
{
    uint8_t checksum[32];
    unsigned int checksum_len = 0;
    int result = 0;

    if (!EVP_Digest(entropy, 32, checksum, &checksum_len, EVP_sha256(), NULL) ||
        checksum_len != sizeof(checksum))
        goto cleanup;

    size_t used = 0;
    for (size_t word = 0; word < 24; ++word) {
        unsigned int index = 0;
        for (size_t bit = word * 11; bit < word * 11 + 11; ++bit) {
            uint8_t byte = bit < 256 ? entropy[bit / 8] : checksum[0];
            unsigned int shift = 7 - (unsigned int)(bit % 8);
            index = (index << 1) | ((byte >> shift) & 1U);
        }
        const char *text = wordlist[index];
        if (!text)
            goto cleanup;
        const char *end = memchr(text, '\0', sizeof(words[0]));
        if (!end || end == text)
            goto cleanup;
        size_t length = (size_t)(end - text);
        if (used + length + (word != 0) >= 24 * 16)
            goto cleanup;
        memcpy(words[word], text, length + 1);
        if (word != 0)
            phrase[used++] = ' ';
        memcpy(phrase + used, text, length);
        used += length;
    }
    phrase[used] = '\0';
    *phrase_len = used;
    result = 1;
cleanup:
    OPENSSL_cleanse(checksum, sizeof(checksum));
    return result;
}

static int bip32_child(secp256k1_context *ctx, uint8_t key[32],
                       uint8_t chain_code[32], uint32_t index)
{
    uint8_t data[37] = {0}, digest[64], child[32];
    unsigned int digest_len = 0;
    const uint32_t last_index = (index & 0x80000000U) ? UINT32_MAX : 0x7fffffffU;
    int result = 0;

    for (;;) {
        if (index & 0x80000000U) {
            memcpy(data + 1, key, 32);
        } else {
            secp256k1_pubkey pubkey;
            size_t serialized_len = 33;
            if (!secp256k1_ec_pubkey_create(ctx, &pubkey, key) ||
                !secp256k1_ec_pubkey_serialize(ctx, data, &serialized_len,
                                               &pubkey, SECP256K1_EC_COMPRESSED) ||
                serialized_len != 33)
                goto cleanup;
        }
        data[33] = (uint8_t)(index >> 24);
        data[34] = (uint8_t)(index >> 16);
        data[35] = (uint8_t)(index >> 8);
        data[36] = (uint8_t)index;
        if (!HMAC(EVP_sha512(), chain_code, 32, data, sizeof(data),
                  digest, &digest_len) || digest_len != sizeof(digest))
            goto cleanup;

        memcpy(child, key, 32);
        if (secp256k1_ec_seckey_tweak_add(ctx, child, digest)) {
            memcpy(key, child, 32);
            memcpy(chain_code, digest + 32, 32);
            result = 1;
            goto cleanup;
        }
        // BIP-32 requires trying the next index for an invalid child.
        if (index == last_index)
            goto cleanup;
        ++index;
    }
cleanup:
    OPENSSL_cleanse(data, sizeof(data));
    OPENSSL_cleanse(digest, sizeof(digest));
    OPENSSL_cleanse(child, sizeof(child));
    return result;
}

int eth_keccak256(
    const void *data,
    size_t data_len,
    uint8_t out[32])
{
    size_t out_len = 0;

    if (!EVP_Q_digest(
            NULL,
            "KECCAK-256",
            NULL,
            data,
            data_len,
            out,
            &out_len))
    {
        return 0;
    }

    return out_len == 32;
}

int derive_address(const uint8_t private_key[32], uint8_t address_out[20])
{
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_pubkey public_key;
    if (!secp256k1_ec_pubkey_create(ctx, &public_key, private_key)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    uint8_t pub_serialized[65];
    size_t pub_len = 65;
    if (!secp256k1_ec_pubkey_serialize(ctx, pub_serialized, &pub_len, &public_key,
            SECP256K1_EC_UNCOMPRESSED)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    uint8_t hash[32];
    if (!eth_keccak256(pub_serialized + 1, 64, hash)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    // The address is the last 20 bytes of the hashed public key.
    memcpy(address_out, hash + 12, 20);

    secp256k1_context_destroy(ctx);
    return 0;
}

int generate_eth_wallet(const char **words, uint8_t private_key_out[32],
                        uint8_t public_key_out[64],
                        uint8_t address_out[20], char words_out[24][16])
{
    if (!words || !private_key_out || !public_key_out || !address_out || !words_out)
        return -1;
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx)
        return -1;
    uint8_t entropy[32], seed[64], master[64], private_key[32];
    uint8_t chain_code[32], serialized[65], hash[32];
    char mnemonic_words[24][16] = {{0}}, phrase[24 * 16] = {0};
    size_t phrase_len = 0;
    unsigned int master_len = 0;
    secp256k1_pubkey public_key;
    size_t public_key_len = sizeof(serialized);
    int result = -1;
    static const uint32_t path[5] = {
        0x8000002cU, 0x8000003cU, 0x80000000U, 0U, 0U
    };

    for (;;) {
        if (RAND_priv_bytes(entropy, sizeof(entropy)) != 1 ||
            !bip39_encode(words, entropy, mnemonic_words, phrase, &phrase_len) ||
            !PKCS5_PBKDF2_HMAC(phrase, (int)phrase_len,
                               (const unsigned char *)"mnemonic", 8, 2048,
                               EVP_sha512(), sizeof(seed), seed) ||
            !HMAC(EVP_sha512(), "Bitcoin seed", 12, seed, sizeof(seed),
                  master, &master_len) || master_len != sizeof(master))
            goto cleanup;
        memcpy(private_key, master, 32);
        if (secp256k1_ec_seckey_verify(ctx, private_key))
            break;
        // An invalid BIP-32 master key requires a new seed.
        OPENSSL_cleanse(entropy, sizeof(entropy));
        OPENSSL_cleanse(seed, sizeof(seed));
        OPENSSL_cleanse(master, sizeof(master));
        OPENSSL_cleanse(private_key, sizeof(private_key));
        OPENSSL_cleanse(mnemonic_words, sizeof(mnemonic_words));
        OPENSSL_cleanse(phrase, sizeof(phrase));
    }

    memcpy(chain_code, master + 32, 32);
    for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); ++i) {
        if (!bip32_child(ctx, private_key, chain_code, path[i]))
            goto cleanup;
    }
    if (!secp256k1_ec_pubkey_create(ctx, &public_key, private_key) ||
        !secp256k1_ec_pubkey_serialize(ctx, serialized, &public_key_len,
                                     &public_key, SECP256K1_EC_UNCOMPRESSED) ||
        public_key_len != sizeof(serialized) ||
        !eth_keccak256(serialized + 1, 64, hash))
        goto cleanup;
    memcpy(private_key_out, private_key, 32);
    memcpy(public_key_out, serialized + 1, 64);
    memcpy(address_out, hash + 12, 20);
    memcpy(words_out, mnemonic_words, sizeof(mnemonic_words));
    result = 0;
cleanup:
    OPENSSL_cleanse(entropy, sizeof(entropy));
    OPENSSL_cleanse(seed, sizeof(seed));
    OPENSSL_cleanse(master, sizeof(master));
    OPENSSL_cleanse(private_key, sizeof(private_key));
    OPENSSL_cleanse(chain_code, sizeof(chain_code));
    OPENSSL_cleanse(mnemonic_words, sizeof(mnemonic_words));
    OPENSSL_cleanse(phrase, sizeof(phrase));
    secp256k1_context_destroy(ctx);
    return result;
}

int sign_hash(const uint8_t private_key[32], const uint8_t hash[32],
              uint8_t result[32], uint8_t s[32], int *v)
{
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(ctx, &sig, hash, private_key, NULL, NULL)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    uint8_t sig_out[64];
    int recid;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, sig_out, &recid, &sig);
    if (recid > 1) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    memcpy(result, sig_out, 32);
    memcpy(s, sig_out + 32, 32);
    *v = recid;

    secp256k1_context_destroy(ctx);
    return 0;
}
