#ifndef RLP_H
#define RLP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Root is at depth zero; at most 64 list ancestors are supported.
#define RLP_MAX_DEPTH 64u

typedef enum {
    RLP_OK = 0,
    RLP_INVALID_ARGUMENT,
    RLP_OVERFLOW,
    RLP_NO_SPACE,
    RLP_DEPTH_EXCEEDED
} rlp_status;

typedef enum {
    RLP_VALUE_INVALID = 0,
    RLP_VALUE_BYTES,
    RLP_VALUE_UINT64,
    RLP_VALUE_LIST
} rlp_value_kind;

typedef struct rlp_value rlp_value;

// Non-owning descriptor: use the constructors below. Referenced bytes and child
// arrays must remain alive and unchanged until sizing/encoding has finished.
struct rlp_value {
    rlp_value_kind kind;
    union {
        struct { const void *data; size_t len; } bytes;
        uint64_t integer;
        struct { const rlp_value *values; size_t count; } list;
    } as;
};

// Raw bytes preserve leading zeroes. NULL is valid only when len is zero.
rlp_value rlp_bytes(const void *data, size_t len);
// NUL-terminated string, excluding its terminator; NULL is invalid.
// Use rlp_bytes() for strings containing NULs or with an explicit length.
rlp_value rlp_string(const char *text);
// Integers use minimal big-endian bytes; zero encodes as the empty byte string.
rlp_value rlp_uint64(uint64_t value);
// Arbitrary-width unsigned big-endian integer (e.g. uint256); strips leading
// zeroes. NULL is valid only when len is zero. Borrows the supplied storage.
rlp_value rlp_uint_be(const void *data, size_t len);
// Nested list; NULL is valid only when count is zero.
rlp_value rlp_list(const rlp_value *values, size_t count);

// Encode one root item (use rlp_list() to wrap multiple items). No allocation.
// size/written are required and must not overlap input or output storage.
// On success, size/written receives the exact encoded length. On RLP_NO_SPACE,
// written receives the required capacity; on other errors it receives zero.
// Errors leave out untouched. Input descriptors/data must not overlap out.
// out may be NULL only with capacity zero, yielding RLP_NO_SPACE for valid input.
// Overflow means a size_t calculation or RLP's 8-byte length limit was exceeded.
rlp_status rlp_encoded_size(const rlp_value *value, size_t *size);
rlp_status rlp_encode(const rlp_value *value, uint8_t *out, size_t capacity,
                      size_t *written);

// Example: ["cat", "dog"] -> c8 83 63 61 74 83 64 6f 67
// rlp_value items[] = { rlp_string("cat"), rlp_string("dog") };
// rlp_value root = rlp_list(items, 2);
// uint8_t out[9];
// size_t written;
// rlp_status status = rlp_encode(&root, out, sizeof(out), &written);

#ifdef __cplusplus
}
#endif
#endif
