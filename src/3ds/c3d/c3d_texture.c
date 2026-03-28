#ifdef __3DS__

#include "c3d_texture.h"
#include "c3d_utils.h"
#include "c3d_constants.h"
#include "citro3d_renderer.h"

#include "stb_image.h"
#include "binary_reader.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ─────────────────────────────────────────────────────────────────────────────
// Внутренние хелперы
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Записывает один пиксель в Morton-упорядоченный буфер текстуры (формат RGBA8).
 * Вычисляет глобальное смещение из номера тайла и Morton-смещения внутри тайла.
 */
static inline void writePixelMorton_RGBA8(u32 *dst, u32 potW,
                                           u32 x, u32 y, u32 pixel) {
    u32 tileX   = x >> 3;
    u32 tileY   = y >> 3;
    u32 tilesX  = potW >> 3;
    u32 tileOff = (tileY * tilesX + tileX) << 6;  // * 64 пикселей на тайл
    u32 locOff  = calcMortonOffset(x & 7, y & 7);
    dst[tileOff + locOff] = pixel;
}

/**
 * Записывает один пиксель в Morton-упорядоченный буфер текстуры (формат RGBA4).
 * Конвертирует RGBA8 → RGBA4, усекая каждый канал до 4 бит.
 */
static inline void writePixelMorton_RGBA4(u16 *dst, u32 potW,
                                           u32 x, u32 y,
                                           u8 r, u8 g, u8 b, u8 a) {
    u32 tileX   = x >> 3;
    u32 tileY   = y >> 3;
    u32 tilesX  = potW >> 3;
    u32 tileOff = (tileY * tilesX + tileX) << 6;
    u32 locOff  = calcMortonOffset(x & 7, y & 7);

    // Формат GPU_RGBA4: 4 бита на канал, упакованы в u16
    // Биты [15:12]=R, [11:8]=G, [7:4]=B, [3:0]=A
    u16 pixel = ((u16)(r >> 4) << 12) |
                ((u16)(g >> 4) <<  8) |
                ((u16)(b >> 4) <<  4) |
                 (u16)(a >> 4);
    dst[tileOff + locOff] = pixel;
}

// ─────────────────────────────────────────────────────────────────────────────
// Публичные функции
// ─────────────────────────────────────────────────────────────────────────────

void swizzleToTex(C3D_Tex *tex, const u32 *src, u32 srcW, u32 srcH) {
    u32 *dst  = (u32 *)tex->data;
    u32  potW = (u32)tex->width;
    u32  potH = (u32)tex->height;

    // Очищаем весь POT-буфер — пиксели за пределами srcW×srcH будут прозрачными
    memset(dst, 0, potW * potH * sizeof(u32));

    for (u32 y = 0; y < srcH; y++) {
        for (u32 x = 0; x < srcW; x++) {
            writePixelMorton_RGBA8(dst, potW, x, y, src[y * srcW + x]);
        }
    }

    // КРИТИЧЕСКИ ВАЖНО: сбросить D-cache CPU, чтобы GPU увидел свежие данные.
    // ARM11 и PICA200 используют разные кэши; без этого GPU прочитает старые данные.
    GSPGPU_FlushDataCache(tex->data, tex->size);
}

bool uploadTexture(C3D_Tex *tex, int *realW, int *realH, const u8 *rgba, int w, int h) {
    u32 potW = nextPOT((u32)w);
    u32 potH = nextPOT((u32)h);

    if (!C3D_TexInit(tex, (u16)potW, (u16)potH, GPU_RGBA4)) {
        fprintf(stderr, "[TEX] C3D_TexInit failed for %d×%d (POT %u×%u)\n", w, h, potW, potH);
        return false;
    }

    // LINEAR при увеличении — сглаживание; NEAREST при уменьшении — чёткость пикселей
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);
    // CLAMP_TO_EDGE: не повторять текстуру за краями (предотвращает "кровотечение" атласа)
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    u16 *dst = (u16 *)tex->data;
    memset(dst, 0, potW * potH * sizeof(u16));

    for (u32 y = 0; y < (u32)h; y++) {
        for (u32 x = 0; x < (u32)w; x++) {
            u32 srcOff = (y * (u32)w + x) * 4;
            writePixelMorton_RGBA4(dst, potW, x, y,
                                   rgba[srcOff + 0], rgba[srcOff + 1],
                                   rgba[srcOff + 2], rgba[srcOff + 3]);
        }
    }

    GSPGPU_FlushDataCache(tex->data, tex->size);

    *realW = w;
    *realH = h;
    return true;
}

bool createWhiteTexture(C3D_Tex *tex) {
    if (!C3D_TexInit(tex, WHITE_TEX_SIZE, WHITE_TEX_SIZE, GPU_RGBA8)) return false;

    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    // linearAlloc — обязательно для буферов, которые будут переданы в GPU/DMA
    u32 *tmp = (u32 *)linearAlloc(WHITE_TEX_PIXELS * sizeof(u32));
    if (!tmp) {
        C3D_TexDelete(tex);
        return false;
    }

    for (int i = 0; i < WHITE_TEX_PIXELS; i++) {
        tmp[i] = WHITE_COLOR_ABGR;
    }

    swizzleToTex(tex, tmp, WHITE_TEX_SIZE, WHITE_TEX_SIZE);
    linearFree(tmp);
    return true;
}

void ensureTextureLoaded(Citro3dRenderer *c3d, int pageId) {
    if (pageId < 0 || pageId >= (int)c3d->texCount) return;

    // Всегда обновляем время использования — даже если текстура уже загружена
    c3d->texLastUsed[pageId] = c3d->currentFrame;

    if (c3d->textures[pageId].data != NULL) return;  // уже в VRAM

    fprintf(stderr, "==> Frame %u: Loading texture %d...\n", c3d->currentFrame, pageId);

    // ── Шаг 1: проверяем лимит кэша ──────────────────────────────────────────
    int loadedCount = 0;
    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (c3d->textures[i].data != NULL) loadedCount++;
    }

    // ── Шаг 2: вытесняем LRU-текстуру при переполнении ───────────────────────
    if (loadedCount >= MAX_CACHED_TEXTURES) {
        int      oldestId   = -1;
        uint32_t oldestTime = UINT32_MAX;

        for (uint32_t i = 0; i < c3d->texCount; i++) {
            if (c3d->textures[i].data != NULL && c3d->texLastUsed[i] < oldestTime) {
                oldestTime = c3d->texLastUsed[i];
                oldestId   = (int)i;
            }
        }

        if (oldestId >= 0) {
            fprintf(stderr, "[C3D] Cache full (%d/%d). Evicting texture %d (frame %u).\n",
                    loadedCount, MAX_CACHED_TEXTURES, oldestId, oldestTime);

            // ВАЖНО: сбросить батч ДО удаления текстуры.
            // Батч может содержать вершины, ссылающиеся на эту текстуру.
            // Если удалить текстуру без сброса, GPU нарисует мусор.
            // flushBatch объявлен в c3d_batch.h, включаем через citro3d_renderer.h
            extern void flushBatch(Citro3dRenderer *c3d);
            flushBatch(c3d);

            C3D_TexDelete(&c3d->textures[oldestId]);
            c3d->textures[oldestId].data = NULL;
        }
    }

    // ── Шаг 3: загружаем текстуру из DataWin ─────────────────────────────────
    DataWin *dw   = c3d->base.dataWin;
    Texture *txtr = &dw->txtr.textures[pageId];

    if (txtr->blobSize == 0) {
        fprintf(stderr, "[C3D] Texture %d: blobSize == 0, skipping.\n", pageId);
        return;
    }

    BinaryReader reader = BinaryReader_create(dw->file, dw->fileSize);
    uint8_t *raw = BinaryReader_readBytesAt(&reader, txtr->blobOffset, txtr->blobSize);
    if (!raw) {
        fprintf(stderr, "[C3D] Texture %d: failed to read blob.\n", pageId);
        return;
    }

    // stbi_set_flip_vertically_on_load(1) нужен потому, что swizzleToTex
    // ожидает строки снизу вверх (OpenGL-конвенция).
    // TODO: при переходе на .tex3ds формат этот флип убрать.
    int w, h, ch;
    stbi_set_flip_vertically_on_load(1);
    uint8_t *px = stbi_load_from_memory(raw, (int)txtr->blobSize, &w, &h, &ch, 4);
    free(raw);

    if (!px) {
        fprintf(stderr, "[C3D] Texture %d: stbi_load_from_memory failed.\n", pageId);
        return;
    }

    bool ok = uploadTexture(&c3d->textures[pageId],
                             &c3d->texRealW[pageId],
                             &c3d->texRealH[pageId],
                             px, w, h);
    stbi_image_free(px);

    if (ok) {
        fprintf(stderr, "[C3D] Texture %d: uploaded (%d×%d).\n", pageId, w, h);
    } else {
        fprintf(stderr, "[C3D] Texture %d: uploadTexture failed.\n", pageId);
    }
}

#endif // __3DS__
