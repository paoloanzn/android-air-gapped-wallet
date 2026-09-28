#ifndef EIP_1559_TX_H
#define EIP_1559_TX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EIP_1559_OK = 0,
    EIP_1559_INVALID_ARGUMENT,
    EIP_1559_OVERFLOW,
    EIP_1559_NO_SPACE,
    EIP_1559_NO_MEMORY,
    EIP_1559_CRYPTO_ERROR
} eip_1559_status;

// Access-list address and 32-byte storage keys. Order and duplicates are kept.
typedef struct {
    uint8_t address[20];
    const uint8_t (*storage_keys)[32]; // NULL only when count is 0.
    size_t storage_key_count;
} eip_1559_access_entry;

// Initialize with {0}. Amounts are 32-byte unsigned big-endian integers.
// Keep referenced data alive and unchanged until encoding finishes.
// Builds bytes only; the caller supplies valid nonce, gas and fee settings.
typedef struct {
    uint64_t chain_id;
    uint64_t nonce;
    uint8_t max_priority_fee_per_gas[32]; // Wei/gas; must not exceed max_fee_per_gas.
    uint8_t max_fee_per_gas[32];          // Wei/gas, including tip.
    uint64_t gas_limit;
    const uint8_t *to;  // 20 address bytes, or NULL to deploy a contract.
    uint8_t value[32];  // Native currency in wei.
    const uint8_t *data; // Raw calldata; NULL only when data_len is 0.
    size_t data_len;
    const eip_1559_access_entry *access_list; // May be NULL if count is 0.
    size_t access_list_count;
} eip_1559_tx;

// Write value as 32 big-endian bytes, padding with leading zeroes.
void eip_1559_uint256(uint8_t out[32], uint64_t value);

// Encode 0x02 followed by RLP transaction fields. A 32-byte private_key signs
// the transaction; NULL returns the unsigned bytes to hash with Keccak-256.
// out=NULL, capacity=0 queries size (returns NO_SPACE); signing still runs.
// written is required: receives the encoded size on OK/NO_SPACE, otherwise 0.
// Errors leave out unchanged. out may be NULL only with capacity 0.
// out and written must not overlap each other or inputs. Temporary memory is
// freed internally; input storage remains the caller's responsibility.
eip_1559_status eip_1559_tx_encode(const eip_1559_tx *tx,
                                  const uint8_t private_key[32],
                                  uint8_t *out, size_t capacity, size_t *written);

#ifdef __cplusplus
}
#endif
#endif
