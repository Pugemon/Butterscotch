#ifdef __3DS__

#include "c3d_utils.h"

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

u32 colorToABGR(uint32_t bgr, float alpha) {
    u8 r = (bgr)       & 0xFF;
    u8 g = (bgr >> 8)  & 0xFF;
    u8 b = (bgr >> 16) & 0xFF;
    u8 a = (u8)(alpha * 255.0f);
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | r;
}

#endif // __3DS__
