#ifndef SELECTOR_H
#define SELECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void compute_selector(const char* func_signature, uint8_t out[4]);

#ifdef __cplusplus
}
#endif

#endif /* SELECTOR_H */
