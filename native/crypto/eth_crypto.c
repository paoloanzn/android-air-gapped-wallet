#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <stddef.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "eth_crypto.h"

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

int generate_eth_wallet(uint8_t private_key_out[32], uint8_t public_key_out[64],
                        uint8_t address_out[20])
{
    if (!private_key_out || !public_key_out || !address_out)
        return -1;
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx)
        return -1;
    uint8_t private_key[32], serialized[65], hash[32];
    secp256k1_pubkey public_key;
    size_t public_key_len = sizeof(serialized);
    int result = -1;
    do {
        if (RAND_priv_bytes(private_key, sizeof(private_key)) != 1)
            goto cleanup;
    } while (!secp256k1_ec_seckey_verify(ctx, private_key));
    if (!secp256k1_ec_pubkey_create(ctx, &public_key, private_key) ||
        !secp256k1_ec_pubkey_serialize(ctx, serialized, &public_key_len,
                                     &public_key, SECP256K1_EC_UNCOMPRESSED) ||
        !eth_keccak256(serialized + 1, 64, hash))
        goto cleanup;
    memcpy(private_key_out, private_key, 32);
    memcpy(public_key_out, serialized + 1, 64);
    memcpy(address_out, hash + 12, 20);
    result = 0;
cleanup:
    OPENSSL_cleanse(private_key, sizeof(private_key));
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
