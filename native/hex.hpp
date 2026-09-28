#pragma once

#include <stdint.h>
#include <stdio.h>

// Decode hex string (with our without "0x" prefix)
// into bytes. Return the number of written bytes, or
// -1 on error. 'out' must hold at least strlen(hex_string)/2
// bytes.
[[nodiscard]] int hex_decode(const char *hex_str, uint8_t *out, size_t out_cap);

// Encode bytes[] into hex string representation with "0x"
// prefix. 'out' must hold at least 2*len + 3 bytes.
void hex_encode(const uint8_t *bytes, size_t len, char *out);

// Encode bytes[] to hex string without leading 
// zeros (quanity encoding).
void hex_encode_quantity(const uint8_t *bytes, size_t len, char *out);
