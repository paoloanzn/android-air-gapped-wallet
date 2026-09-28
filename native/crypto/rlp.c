#include "rlp.h"

#include <string.h>

rlp_value rlp_bytes(const void *data, size_t len)
{
    rlp_value value = { .kind = RLP_VALUE_BYTES };
    value.as.bytes.data = data;
    value.as.bytes.len = len;
    return value;
}

rlp_value rlp_string(const char *text)
{
    if (!text) return (rlp_value){ .kind = RLP_VALUE_INVALID };
    return rlp_bytes(text, strlen(text));
}

rlp_value rlp_uint64(uint64_t integer)
{
    rlp_value value = { .kind = RLP_VALUE_UINT64 };
    value.as.integer = integer;
    return value;
}

rlp_value rlp_uint_be(const void *data, size_t len)
{
    const uint8_t *bytes = data;
    while (bytes && len && *bytes == 0) {
        ++bytes;
        --len;
    }
    return rlp_bytes(bytes, len);
}

rlp_value rlp_list(const rlp_value *values, size_t count)
{
    rlp_value value = { .kind = RLP_VALUE_LIST };
    value.as.list.values = values;
    value.as.list.count = count;
    return value;
}

// Return the minimal number of bytes needed to store an unsigned length.
static size_t length_bytes(size_t length)
{
    size_t count = 0;
    while (length) {
        ++count;
        length >>= 8;
    }
    return count;
}

static void write_prefix(uint8_t *out, size_t length, int is_list)
{
    unsigned base = is_list ? 0xc0u : 0x80u;
    if (length <= 55) {
        out[0] = (uint8_t)(base + length);
        return;
    }
    size_t count = length_bytes(length);
    out[0] = (uint8_t)(base + 55 + count);
    for (size_t i = count; i > 0; --i) {
        out[i] = (uint8_t)length;
        length >>= 8;
    }
}

// With out == NULL, validate and measure. Otherwise the caller has already
// validated the whole tree and provided enough space. List payloads are measured
// before writing their prefixes; depth bounds both recursion and repeated work.
static rlp_status encode_item(const rlp_value *value, unsigned depth,
                              uint8_t *out, size_t *size)
{
    if (!value) return RLP_INVALID_ARGUMENT;
    if (depth > RLP_MAX_DEPTH) return RLP_DEPTH_EXCEEDED;

    uint8_t integer[8];
    const uint8_t *bytes = NULL;
    size_t length = 0;
    int is_list = value->kind == RLP_VALUE_LIST;
    switch (value->kind) {
    case RLP_VALUE_BYTES:
        bytes = value->as.bytes.data;
        length = value->as.bytes.len;
        if (!bytes && length) return RLP_INVALID_ARGUMENT;
        break;
    case RLP_VALUE_UINT64: {
        uint64_t number = value->as.integer;
        bytes = integer + sizeof(integer);
        while (number) {
            integer[sizeof(integer) - ++length] = (uint8_t)number;
            number >>= 8;
        }
        bytes -= length;
        break;
    }
    case RLP_VALUE_LIST:
        if (!value->as.list.values && value->as.list.count)
            return RLP_INVALID_ARGUMENT;
        if (value->as.list.count > SIZE_MAX / sizeof(rlp_value))
            return RLP_OVERFLOW;
        for (size_t i = 0; i < value->as.list.count; ++i) {
            size_t child_size;
            rlp_status status = encode_item(&value->as.list.values[i],
                                            depth + 1, NULL, &child_size);
            if (status != RLP_OK) return status;
            if (child_size > SIZE_MAX - length) return RLP_OVERFLOW;
            length += child_size;
        }
        break;
    default:
        return RLP_INVALID_ARGUMENT;
    }

    // Single bytes below 0x80 are their own encoding (including raw byte zero).
    if (!is_list && length == 1 && bytes[0] < 0x80) {
        if (out) out[0] = bytes[0];
        *size = 1;
        return RLP_OK;
    }
    size_t count = length_bytes(length);
    if (count > 8) return RLP_OVERFLOW;
    size_t prefix = length <= 55 ? 1 : 1 + count;
    if (length > SIZE_MAX - prefix) return RLP_OVERFLOW;
    *size = prefix + length;
    if (!out) return RLP_OK;

    write_prefix(out, length, is_list);
    out += prefix;
    if (!is_list) {
        if (length) memcpy(out, bytes, length);
        return RLP_OK;
    }
    for (size_t i = 0; i < value->as.list.count; ++i) {
        size_t child_size;
        rlp_status status = encode_item(&value->as.list.values[i],
                                        depth + 1, out, &child_size);
        if (status != RLP_OK) return status;
        out += child_size;
    }
    return RLP_OK;
}

rlp_status rlp_encoded_size(const rlp_value *value, size_t *size)
{
    if (!size) return RLP_INVALID_ARGUMENT;
    *size = 0;
    return encode_item(value, 0, NULL, size);
}

rlp_status rlp_encode(const rlp_value *value, uint8_t *out, size_t capacity,
                      size_t *written)
{
    if (!written) return RLP_INVALID_ARGUMENT;
    *written = 0;
    if (!out && capacity) return RLP_INVALID_ARGUMENT;

    size_t required;
    rlp_status status = rlp_encoded_size(value, &required);
    if (status != RLP_OK) return status;
    *written = required;
    if (capacity < required) return RLP_NO_SPACE;
    return encode_item(value, 0, out, written);
}
