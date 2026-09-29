#ifndef SELECTOR_H
#define SELECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 on success. On failure, out is left unchanged.
int compute_selector(const char* func_signature, uint8_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* SELECTOR_H */
