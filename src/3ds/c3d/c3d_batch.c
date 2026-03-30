#ifdef __3DS__

#include "c3d_batch.h"
#include "c3d_constants.h"
#include "citro3d_renderer.h"

#include <3ds.h>
#include <citro3d.h>
#include <string.h>

void flushBatch(Citro3dRenderer *c3d) {
    int count = c3d->vertexCount - c3d->batchStart;
    if (count <= 0) return;

    // Сбрасываем D-cache только для диапазона новых вершин.
    // GPU работает через DMA и видит физическую память, а не CPU-кэш.
    GSPGPU_FlushDataCache(&c3d->vboData[c3d->batchStart],
                           (u32)(count * sizeof(C3D_Vertex)));

    // Выбираем текстуру: если у текущего слота нет данных (не загружена),
    // используем белую заглушку, чтобы не биндить NULL.
    C3D_Tex *texToBind = NULL;
    if (c3d->currentTexIndex >= 0) {
        C3D_Tex *candidate = &c3d->textures[c3d->currentTexIndex];
        texToBind = (candidate->data != NULL) ? candidate
                                              : &c3d->textures[c3d->whiteTexIndex];
    }

    C3D_TexBind(0, texToBind);
    C3D_DrawArrays(GPU_TRIANGLES, c3d->batchStart, count);

    c3d->batchStart = c3d->vertexCount;
}

void checkBatch(Citro3dRenderer *c3d, int verts, int texIdx) {
    bool texChanged  = (c3d->currentTexIndex != texIdx);
    bool bufferFull  = (c3d->vertexCount + verts > MAX_VERTICES);

    if (texChanged || bufferFull) {
        flushBatch(c3d);
        c3d->currentTexIndex = texIdx;

        // Если VBO полностью заполнен после сброса — перезаписываем с начала.
        // К этому моменту GPU уже завершил отрисовку сброшенного батча.
        if (c3d->vertexCount + verts > MAX_VERTICES) {
            c3d->vertexCount = 0;
            c3d->batchStart  = 0;
        }
    }
}

void calcUV(const C3D_Tex *tex, float srcX, float srcY, float srcW, float srcH, float *u0, float *v0, float *u1, float *v1) {
    if (tex->width == 0 || tex->height == 0) {
        *u0 = *v0 = *u1 = *v1 = 0.0f;
        return;
    }

    float invW = 1.0f / (float)tex->width;
    float invH = 1.0f / (float)tex->height;

    *u0 = srcX * invW;
    *u1 = (srcX + srcW) * invW;
    *v0 = 1.0f - (srcY * invH);
    *v1 = 1.0f - ((srcY + srcH) * invH);
}

#endif // __3DS__
