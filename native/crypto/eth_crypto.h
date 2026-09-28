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
// words points to 2048 BIP-39 English words in index order, such as the array
// returned by Bip39WordList::words() in native/bip39_words.hpp. Generate 24
// words from 256 bits of entropy and derive the first Ethereum account at
// m/44'/60'/0'/0/0 (empty BIP-39 passphrase). Return its private key, 64-byte
// public key (no 0x04 prefix), 20-byte address, and 24 NUL-terminated words.
// Each word buffer must hold 16 bytes. Returns 0 on success, -1 on failure.
// Outputs must be non-NULL, non-overlapping; unchanged on failure.
int generate_eth_wallet(const char **words, uint8_t private_key_out[32],
                        uint8_t public_key_out[64],
                        uint8_t address_out[20], char words_out[24][16]);
// Sign a 32 bytes tx hash; produces r(32), s(32), v,
// (recovery id, 0 or 1). Returns 0 on success.
int sign_hash(const uint8_t private_key[32], const uint8_t hash[32],
            uint8_t result[32], uint8_t s[32], int *v);

#ifdef __cplusplus
}
#endif

#endif /* ETH_CRYPTO_H */
