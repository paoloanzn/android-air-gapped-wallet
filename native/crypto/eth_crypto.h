#ifndef ETH_CRYPTO_H
#define ETH_CRYPTO_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int eth_keccak256(const void *data, size_t data_len, uint8_t out[32]);
// Ethereum private key is a 32 bytes scalar, the public key is
// derived from the private key and it's 64 bytes uncompressed.
// The corresponding address is the last 20 bytes of
// keccak256(public_key_64_bytes).
int derive_address(const uint8_t private_key[32], uint8_t address_out[20]);
// Generate a random private key, a 64-byte public key (no 0x04 prefix),
// and a 20-byte address. Returns 0 on success, -1 on failure.
// Outputs must be non-NULL, non-overlapping; unchanged on failure.
int generate_eth_wallet(uint8_t private_key_out[32], uint8_t public_key_out[64],
                        uint8_t address_out[20]);
// Sign a 32 bytes tx hash; produces r(32), s(32), v,
// (recovery id, 0 or 1). Returns 0 on success.
int sign_hash(const uint8_t private_key[32], const uint8_t hash[32],
            uint8_t result[32], uint8_t s[32], int *v);

#ifdef __cplusplus
}
#endif

#endif /* ETH_CRYPTO_H */
