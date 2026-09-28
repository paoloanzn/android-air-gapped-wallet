#include "selector.h"
#include "eth_crypto.h"
#include <stdint.h>
#include <string.h>

void compute_selector(const char *func_signature, uint8_t out[4])
{
    int len = strlen(func_signature);
    uint8_t hash_buf[32];
    eth_keccak256(func_signature, len, hash_buf);
    memcpy(out, hash_buf, 4);
}