#ifdef __3DS__

#include "c3d_utils.h"

#include <math.h>

u32 nextPOT(u32 n) {
    if (n < TEX_MIN_SIZE) return TEX_MIN_SIZE;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

static float sin_lut[3600];
static float cos_lut[3600];
static bool lut_init = false;

// ВЫЗВАТЬ ЭТУ ФУНКЦИЮ В C3DRenderer_init (в c3d_init.c)
void initMathLUTs() {
    if (lut_init) return;
    for (int i = 0; i < 3600; i++) {
        float rad = (float)i * (M_PI / 1800.0f);
        sin_lut[i] = sinf(rad);
        cos_lut[i] = cosf(rad);
    }
    lut_init = true;
}

// Замена rotatePoint
void fast_sincosf(float angleDeg, float *out_sin, float *out_cos) {
    int idx = (int)(angleDeg * 10.0f) % 3600;
    if (idx < 0) idx += 3600;
    *out_sin = sin_lut[idx];
    *out_cos = cos_lut[idx];
}

#endif // __3DS__
