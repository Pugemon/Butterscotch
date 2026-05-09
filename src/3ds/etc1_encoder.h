#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {

#endif
bool Etc1_encodeImageTiled(const uint8_t *rgbaLinear, int w, int h,
                           bool withAlpha, uint8_t *out);

void Etc1_encodeBlockRgb(const uint8_t rgb16[48], uint8_t out[8]);

#ifdef __cplusplus
}
#endif
