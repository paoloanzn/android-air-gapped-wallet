#ifndef ABI_H
#define ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bounds recursive validation/writing when descriptors come from untrusted input.
#define ABI_MAX_DEPTH 64u

typedef enum {
    ABI_OK = 0,
    ABI_INVALID_ARGUMENT,
    ABI_OVERFLOW,
    ABI_NO_SPACE,
    ABI_NO_MEMORY,
    ABI_DEPTH_EXCEEDED
} abi_status;

typedef enum {
    ABI_VALUE_INVALID = 0,
    ABI_VALUE_SELECTOR,
    ABI_VALUE_WORD,
    ABI_VALUE_BYTES,
    ABI_VALUE_ARRAY,
    ABI_VALUE_FIXED_ARRAY,
    ABI_VALUE_TUPLE
} abi_value_kind;

typedef struct abi_value abi_value;

// A small, non-owning description of one ABI value. Constructors should be used
// instead of filling this structure manually. Referenced bytes and child values
// must remain alive until encoding finishes.
struct abi_value {
    uint8_t kind;
    uint8_t flags; // Private to the implementation.
    uint16_t reserved;
    union {
        uint8_t selector[4];
        uint8_t word[32];
        struct { const void *data; size_t len; } bytes;
        struct { const abi_value *values; size_t count; } list;
    } as;
};

typedef struct { uint8_t *data; size_t len; } abi_buffer;

// Scalar constructors. *_be inputs are big-endian; int_be is two's complement.
// uint<M>/ufixed use uint, int<M>/fixed use int (fixed-point inputs are scaled),
// bytes<M> uses fixed_bytes, and contract/enum use address/uint respectively.
// Invalid widths or NULL inputs produce a descriptor rejected during encoding.
abi_value abi_word(const uint8_t word[32]);
abi_value abi_selector(const uint8_t selector[4]);
abi_value abi_uint64(uint64_t value);
abi_value abi_int64(int64_t value);
abi_value abi_uint_be(const void *value, size_t len);
abi_value abi_int_be(const void *value, size_t len);
abi_value abi_ufixed_be(const void *scaled_value, size_t len);
abi_value abi_fixed_be(const void *scaled_value, size_t len);
abi_value abi_bool(int value);
abi_value abi_address(const uint8_t address[20]);
abi_value abi_fixed_bytes(const void *data, size_t len);
abi_value abi_function(const uint8_t address_and_selector[24]);

// bytes and strings have identical ABI encoding; string_n does not validate UTF-8.
abi_value abi_bytes(const void *data, size_t len);
abi_value abi_string_n(const char *utf8, size_t len);
abi_value abi_string(const char *utf8);
// Array elements must describe one homogeneous ABI type.
abi_value abi_array(const abi_value *values, size_t count);
abi_value abi_fixed_array(const abi_value *values, size_t count);
// Required only for a zero-length T[0] whose element type T is dynamic.
abi_value abi_fixed_array_dynamic(const abi_value *values, size_t count);
abi_value abi_tuple(const abi_value *values, size_t count);

// Root values are encoded as an ABI tuple. An optional abi_selector() may appear
// once at values[0]; it is written before the tuple without shifting ABI offsets.
// encode writes no heap; encode_alloc performs one exact-size allocation. Input
// and caller output storage must not overlap. On ABI_NO_SPACE, written receives
// the required size.
abi_status abi_encoded_size(const abi_value *values, size_t count, size_t *size);
abi_status abi_encode(const abi_value *values, size_t count,
                      uint8_t *out, size_t capacity, size_t *written);
abi_status abi_encode_alloc(const abi_value *values, size_t count, abi_buffer *out);
void abi_buffer_free(abi_buffer *buffer);
const char *abi_status_string(abi_status status);

#ifdef __cplusplus
}
#endif
#endif
