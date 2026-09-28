#include "eip_1559_tx.h"
#include "eth_crypto.h"
#include "rlp.h"

#include <stdlib.h>
#include <string.h>

enum {
    TRANSACTION_TYPE = 0x02,
    UNSIGNED_FIELD_COUNT = 9,
    SIGNED_FIELD_COUNT = 12
};

void eip_1559_uint256(uint8_t out[32], uint64_t value)
{
    memset(out, 0, 32);

    size_t position = 32;
    while (value != 0) {
        --position;
        out[position] = (uint8_t)value;
        value >>= 8;
    }
}

static eip_1559_status validate_transaction(const eip_1559_tx *tx)
{
    if (tx == NULL) {
        return EIP_1559_INVALID_ARGUMENT;
    }
    if (tx->data == NULL && tx->data_len != 0) {
        return EIP_1559_INVALID_ARGUMENT;
    }
    if (tx->access_list == NULL && tx->access_list_count != 0) {
        return EIP_1559_INVALID_ARGUMENT;
    }

    // Big-endian amounts can be compared byte by byte.
    if (memcmp(tx->max_priority_fee_per_gas, tx->max_fee_per_gas, 32) > 0) {
        return EIP_1559_INVALID_ARGUMENT;
    }

    return EIP_1559_OK;
}

// Build [[address, [key, ...]], ...] in one allocation; the caller frees it.
// Descriptions reference the original address/key bytes without copying them.
static eip_1559_status build_access_list(const eip_1559_tx *tx,
                                        rlp_value **descriptors_out)
{
    *descriptors_out = NULL;
    size_t entry_count = tx->access_list_count;
    if (entry_count == 0) {
        return EIP_1559_OK;
    }

    // Each entry needs three descriptions, plus one per key. Bound the allocation.
    size_t max_descriptors = SIZE_MAX / sizeof(rlp_value);
    if (entry_count > max_descriptors / 3) {
        return EIP_1559_OVERFLOW;
    }

    size_t descriptor_count = 3 * entry_count;
    for (size_t i = 0; i < entry_count; ++i) {
        const eip_1559_access_entry *entry = &tx->access_list[i];
        if (entry->storage_keys == NULL && entry->storage_key_count != 0) {
            return EIP_1559_INVALID_ARGUMENT;
        }
        if (entry->storage_key_count > max_descriptors - descriptor_count) {
            return EIP_1559_OVERFLOW;
        }
        descriptor_count += entry->storage_key_count;
    }

    rlp_value *descriptors = malloc(descriptor_count * sizeof(*descriptors));
    if (descriptors == NULL) {
        return EIP_1559_NO_MEMORY;
    }

    // Reserve outer entries first, then each entry's address, key list and keys.
    size_t next_descriptor = entry_count;
    for (size_t i = 0; i < entry_count; ++i) {
        const eip_1559_access_entry *entry = &tx->access_list[i];
        rlp_value *entry_fields = &descriptors[next_descriptor];
        next_descriptor += 2;
        rlp_value *storage_keys = &descriptors[next_descriptor];

        descriptors[i] = rlp_list(entry_fields, 2);
        entry_fields[0] = rlp_bytes(entry->address, 20);
        entry_fields[1] = rlp_list(storage_keys, entry->storage_key_count);

        for (size_t j = 0; j < entry->storage_key_count; ++j) {
            storage_keys[j] = rlp_bytes(entry->storage_keys[j], 32);
        }
        next_descriptor += entry->storage_key_count;
    }

    *descriptors_out = descriptors;
    return EIP_1559_OK;
}

// Write 0x02 followed by RLP fields; check capacity before touching output.
static eip_1559_status encode_envelope(const rlp_value *fields, uint8_t *out,
                                      size_t capacity, size_t *written)
{
    size_t rlp_size;
    rlp_status status = rlp_encoded_size(fields, &rlp_size);

    // Only size overflow can fail here: the lists are valid and have fixed depth.
    if (status != RLP_OK || rlp_size == SIZE_MAX) {
        return EIP_1559_OVERFLOW;
    }

    *written = 1 + rlp_size;
    if (capacity < *written) {
        return EIP_1559_NO_SPACE;
    }

    // Sizing validated the data and capacity, so this write cannot fail.
    rlp_encode(fields, out + 1, capacity - 1, &rlp_size);
    out[0] = TRANSACTION_TYPE;
    return EIP_1559_OK;
}

// Hash the type byte and nine unsigned fields before adding the signature.
static eip_1559_status hash_signing_payload(const rlp_value *unsigned_fields,
                                           uint8_t hash[32])
{
    size_t payload_size = 0;
    eip_1559_status status = encode_envelope(unsigned_fields, NULL, 0,
                                            &payload_size);
    if (status != EIP_1559_NO_SPACE) {
        return status;
    }

    uint8_t *payload = malloc(payload_size);
    if (payload == NULL) {
        return EIP_1559_NO_MEMORY;
    }

    status = encode_envelope(unsigned_fields, payload, payload_size, &payload_size);
    if (status == EIP_1559_OK) {
        if (!eth_keccak256(payload, payload_size, hash)) {
            status = EIP_1559_CRYPTO_ERROR;
        }
    }

    free(payload);
    return status;
}

eip_1559_status eip_1559_tx_encode(const eip_1559_tx *tx,
                                  const uint8_t private_key[32],
                                  uint8_t *out, size_t capacity, size_t *written)
{
    if (written == NULL) {
        return EIP_1559_INVALID_ARGUMENT;
    }
    *written = 0;

    if (out == NULL && capacity != 0) {
        return EIP_1559_INVALID_ARGUMENT;
    }

    eip_1559_status status = validate_transaction(tx);
    if (status != EIP_1559_OK) {
        return status;
    }

    rlp_value *access_list = NULL;
    status = build_access_list(tx, &access_list);
    if (status != EIP_1559_OK) {
        return status;
    }

    // Keep the EIP-1559 field order. An empty recipient means contract deployment.
    size_t recipient_size = tx->to != NULL ? 20 : 0;
    rlp_value fields[SIGNED_FIELD_COUNT] = {
        rlp_uint64(tx->chain_id),
        rlp_uint64(tx->nonce),
        rlp_uint_be(tx->max_priority_fee_per_gas, 32),
        rlp_uint_be(tx->max_fee_per_gas, 32),
        rlp_uint64(tx->gas_limit),
        rlp_bytes(tx->to, recipient_size),
        rlp_uint_be(tx->value, 32),
        rlp_bytes(tx->data, tx->data_len),
        rlp_list(access_list, tx->access_list_count)
    };
    rlp_value transaction = rlp_list(fields, UNSIGNED_FIELD_COUNT);

    // Keep signature bytes alive until final encoding; RLP fields point to them.
    uint8_t signature_r[32];
    uint8_t signature_s[32];
    if (private_key != NULL) {
        uint8_t signing_hash[32];
        status = hash_signing_payload(&transaction, signing_hash);
        if (status != EIP_1559_OK) {
            goto cleanup;
        }

        int y_parity;
        if (sign_hash(private_key, signing_hash, signature_r, signature_s,
                      &y_parity) != 0) {
            status = EIP_1559_CRYPTO_ERROR;
            goto cleanup;
        }

        // Encode signature numbers without leading zeroes.
        fields[9] = rlp_uint64((uint64_t)y_parity);
        fields[10] = rlp_uint_be(signature_r, 32);
        fields[11] = rlp_uint_be(signature_s, 32);
        transaction = rlp_list(fields, SIGNED_FIELD_COUNT);
    }

    status = encode_envelope(&transaction, out, capacity, written);

cleanup:
    free(access_list);
    return status;
}
