#include "selector.h"
#include "eth_crypto.h"
#include <stdint.h>
#include <string.h>

int compute_selector(const char *func_signature, uint8_t out[4])
{
    if (func_signature == NULL || out == NULL)
        return 0;

    uint8_t hash_buf[32];
    if (!eth_keccak256(func_signature, strlen(func_signature), hash_buf))
        return 0;

    memcpy(out, hash_buf, 4);
    return 1;
}
