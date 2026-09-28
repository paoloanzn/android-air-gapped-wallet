#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "hex.hpp"

static inline int has_prefix(const char* hex_str)
{
    if (!((hex_str[0] == '0') & ((hex_str[1] | 0x20) == 'x'))) {
        // "hex_str" does not start with "0x" or "0X"
        return 0;
    }
    return 1;
}

// Branchless fast hex string to integer converstion.
static inline int hexval(unsigned char c)
{
    unsigned x = c;

    unsigned d = x - '0';
    unsigned a = (x | 0x20u) - 'a';

    unsigned ok = (d <= 9u) | (a <= 5u);

    // ASCII trick:
    // '0'..'9': low nibble = 0..9
    // A..F/a..f: low nibble = 1..6, +9 => 10..15
    unsigned v = (x & 0x0fu) + ((x >> 6) * 9u);

    return (int)v | -(int)(ok == 0);
}

int hex_decode(const char *hex_str, uint8_t *out, size_t out_cap)
{
    // Skip "0x" prefix if present.
    if (has_prefix(hex_str)) hex_str += 2;

    size_t slen = strlen(hex_str);
    // Handle odd-len; treat as if leading zero.
    size_t start = 0; size_t nbytes = slen / 2;
    int is_odd = slen % 2;
    if (is_odd) nbytes++;

    if (nbytes > out_cap) return -1;

    size_t j = 0;
    if (is_odd) {
        int v = hexval(hex_str[0]);
        if (v < 0) return -1;
        out[j++] = (uint8_t)v;
        start = 1;
    }

    // 1 hex_str[i]     = 4 bits
    // hex_str[i]       = higher 4 bits
    // hex_str[i + 1]   = lower 4 bits
    // hi + low         = 8 bits = 1 byte = out[j]
    for (size_t i = start; i < slen; i += 2) {
        int hi = hexval(hex_str[i]);
        int low = hexval(hex_str[i + 1]);
        if (hi < 0 || low < 0) return -1;
        // this is NOT out[j + 1]
        // this is out[j] and THEN j++
        out[j++] = (uint8_t) (hi << 4 | low);
    }
    return (int)j;
}

void hex_encode(const uint8_t *bytes, size_t len, char *out)
{
    out[0] = '0'; out[1] = 'x';
    for (size_t i = 0; i < len; i++) {
        // Encode each byte as two hexadecimal characters.
        // The "0x" prefix occupies out[0] and out[1], so byte i
        // is written starting at out[2 + i * 2].
        sprintf(out + 2 + i * 2, "%02x", bytes[i]);
    }
    out[2 + len * 2] = '\0';
}

void hex_encode_quantity(const uint8_t *bytes, size_t len, char *out)
{
    // Skip leading zero bytes.
    size_t start = 0;
    while (start < len && bytes[start] == 0) start++;
    if (start == len) {
        strcpy(out, "0x0");
        return;
    }

    out[0] = '0'; out[1] = 'x';
    int pos = 2;
    pos += sprintf(out + pos, "%x", bytes[start]);
    for (size_t i = start + 1; i < len; i++) {
        pos += sprintf(out + pos, "%02x", bytes[i]);
    }
    out[pos] = '\0';
}