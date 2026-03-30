#ifdef __3DS__

// ─────────────────────────────────────────────────────────────────────────────
// c3d_texture.c — управление текстурами с асинхронным декодом на Core 1
// ─────────────────────────────────────────────────────────────────────────────

#include "c3d_texture.h"
#include "c3d_utils.h"
#include "c3d_constants.h"
#include "citro3d_renderer.h"
#include "decode_thread.h"

#include "stb_image.h"
#include "binary_reader.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ─────────────────────────────────────────────────────────────────────────────
// Внутренние хелперы (не изменились)
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
    u32 locOff  = kMortonTable[y & 7][x & 7];
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
    u32 locOff  = kMortonTable[y & 7][x & 7];

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

    u32 tilesX = potW >> 3;

    // Очищаем весь POT-буфер — пиксели за пределами srcW×srcH будут прозрачными
    //memset(dst, 0, potW * potH * sizeof(u32));

    for (u32 ty = 0; ty < srcH; ty += 8) {
        for (u32 tx = 0; tx < srcW; tx += 8) {

            u32 tileX   = tx >> 3;
            u32 tileY   = ty >> 3;
            u32 tileOff = (tileY * tilesX + tileX) << 6;

            for (u32 y = 0; y < 8; y++) {
                u32 srcY = ty + y;
                if (srcY >= srcH) break;

                const u32 *row = src + srcY * srcW;

                for (u32 x = 0; x < 8; x++) {
                    u32 srcX = tx + x;
                    if (srcX >= srcW) break;

                    u32 locOff = kMortonTable[y][x];
                    dst[tileOff + locOff] = row[srcX];
                }
            }
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
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    // CLAMP_TO_EDGE: не повторять текстуру за краями (предотвращает "кровотечение" атласа)
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    u16 *dst = (u16 *)tex->data;
    u32 tilesX = potW >> 3;

    memset(dst, 0, potW * potH * sizeof(u16));

    for (u32 ty = 0; ty < (u32)h; ty += 8) {
        for (u32 tx = 0; tx < (u32)w; tx += 8) {

            u32 tileX   = tx >> 3;
            u32 tileY   = ty >> 3;
            u32 tileOff = (tileY * tilesX + tileX) << 6;

            for (u32 y = 0; y < 8; y++) {
                u32 srcY = ty + y;
                if (srcY >= (u32)h) break;

                const u8 *row = rgba + (srcY * (u32)w * 4);

                for (u32 x = 0; x < 8; x++) {
                    u32 srcX = tx + x;
                    if (srcX >= (u32)w) break;



                    const u8 *px = row + srcX * 4;

                    u16 pixel = ((u16)(px[0] >> 4) << 12) |
                                ((u16)(px[1] >> 4) <<  8) |
                                ((u16)(px[2] >> 4) <<  4) |
                                 (u16)(px[3] >> 4);
                    u32 locOff = kMortonTable[y][x];
                    dst[tileOff + locOff] = pixel;
                }
            }
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

// ─────────────────────────────────────────────────────────────────────────────
// Вспомогательный: синхронная загрузка из уже-прочитанных байт
// Используется как fallback когда очередь декода полна.
// ─────────────────────────────────────────────────────────────────────────────

static bool loadTextureSync(Citro3dRenderer *c3d, int pageId,
                             uint8_t *raw, uint32_t rawSize) {
    int w, h, ch;
    uint8_t *px = stbi_load_from_memory(raw, (int)rawSize, &w, &h, &ch, 4);
    free(raw);

    if (!px) {
        fprintf(stderr, "[C3D] Texture %d: stbi_load_from_memory failed.\n", pageId);
        return false;
    }

    bool ok = uploadTexture(&c3d->textures[pageId],
                             &c3d->texRealW[pageId],
                             &c3d->texRealH[pageId],
                             px, w, h);
    stbi_image_free(px);

    if (ok) {
        fprintf(stderr, "[C3D] Texture %d: sync upload done (%d×%d).\n", pageId, w, h);
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Вспомогательный: вытеснение LRU-текстуры
// Вынесен отдельно, чтобы ensureTextureLoaded читался линейно.
// ─────────────────────────────────────────────────────────────────────────────

static void evictLRU(Citro3dRenderer *c3d) {
    int      oldestId   = -1;
    uint32_t oldestTime = UINT32_MAX;

    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (c3d->textures[i].data != NULL && c3d->texLastUsed[i] < oldestTime) {
            oldestTime = c3d->texLastUsed[i];
            oldestId   = (int)i;
        }
    }

    if (oldestId < 0) return;

    fprintf(stderr, "[C3D] Evicting texture %d (last used frame %u).\n",
            oldestId, oldestTime);

    // Сбросить батч ДО удаления текстуры — в нём могут быть ссылки на неё
    extern void flushBatch(Citro3dRenderer *c3d);
    flushBatch(c3d);

    C3D_TexDelete(&c3d->textures[oldestId]);
    c3d->textures[oldestId].data = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureTextureLoaded — основная функция, вызывается из prepareQuad (Core 0)
//
// Конвейер:
//
//   Кадр N:   обнаружили нужную текстуру → читаем PNG bytes → dispatch Core 1
//             рисуем белой заглушкой
//
//   Кадр N+1: Core 1 всё ещё декодирует → рисуем заглушкой
//
//   Кадр N+2: tryCollect → C3D_TexInit + memcpy → текстура готова
//
// ─────────────────────────────────────────────────────────────────────────────

void ensureTextureLoaded(Citro3dRenderer *c3d, int pageId) {
    if (pageId < 0 || pageId >= (int)c3d->texCount) return;

    // Всегда обновляем LRU-метку, даже если текстура ещё не загружена.
    // Иначе только-что-запрошенная текстура может быть сразу вытеснена.
    c3d->texLastUsed[pageId] = c3d->currentFrame;

    // ── Шаг 0: забираем результат, если Core 1 уже всё сделал ───────────────
    //
    // Проверяем ДО c3d->textures[pageId].data, потому что в момент между
    // dispatch и collect data == NULL, и без этой проверки мы отправим
    // второй dispatch на ту же текстуру.
    if (c3d->decodeThread != NULL) {
        u16     *swizzled;
        uint32_t potW, potH;
        int      realW, realH;

        if (DecodeThread_tryCollect(c3d->decodeThread, pageId,
                                    &swizzled, &potW, &potH, &realW, &realH))
        {
            // Core 1 вернул Morton-swizzled буфер. Финализируем здесь:
            //   C3D_TexInit — вызывает linearAlloc → только Core 0
            //   memcpy      → пишем в linear heap → только Core 0
            //   FlushDataCache → GPU/GSP → только Core 0
            C3D_Tex *tex = &c3d->textures[pageId];
            if (C3D_TexInit(tex, (u16)potW, (u16)potH, GPU_RGBA4)) {
                C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

                // Swizzled данные уже в правильном формате — просто копируем.
                // memcpy здесь быстрый (~0.5ms для 512KB), не stbi.
                memcpy(tex->data, swizzled, potW * potH * sizeof(u16));
                GSPGPU_FlushDataCache(tex->data, tex->size);

                c3d->texRealW[pageId] = realW;
                c3d->texRealH[pageId] = realH;

                fprintf(stderr, "[C3D] Texture %d: async collect done (%d×%d).\n",
                        pageId, realW, realH);
            } else {
                fprintf(stderr, "[C3D] Texture %d: C3D_TexInit failed after async decode.\n",
                        pageId);
            }
            free(swizzled);
            return;
        }
    }

    // ── Шаг 1: текстура уже в VRAM — ничего делать не нужно ─────────────────
    if (c3d->textures[pageId].data != NULL) return;

    // ── Шаг 2: уже в очереди декода — ждём, рисуем белой заглушкой ──────────
    if (c3d->decodeThread != NULL &&
        DecodeThread_isInFlight(c3d->decodeThread, pageId))
    {
        return; // Core 1 работает, придём снова в следующем кадре
    }

    // ── Шаг 3: новая загрузка — сначала вытесняем LRU если нужно ─────────────
    int loadedCount = 0;
    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (c3d->textures[i].data != NULL) loadedCount++;
    }
    if (loadedCount >= MAX_CACHED_TEXTURES) {
        evictLRU(c3d);
    }

    // ── Шаг 4: читаем PNG-blob на Core 0 (FILE* не thread-safe) ─────────────
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

    fprintf(stderr, "[C3D] Texture %d: dispatching async decode (%u bytes PNG)...\n",
            pageId, txtr->blobSize);

    // ── Шаг 5: отправляем на Core 1 ─────────────────────────────────────────
    // Передаём владение raw — Core 1 освободит его после stbi.
    if (c3d->decodeThread != NULL &&
        DecodeThread_dispatch(c3d->decodeThread, pageId, raw, txtr->blobSize))
    {
        // Текстура будет белой 1–2 кадра. Core 0 продолжает без фриза.
        return;
    }

    // ── Fallback: очередь полна или нет треда — грузим синхронно ─────────────
    // raw передаётся в loadTextureSync и освобождается там
    fprintf(stderr, "[C3D] Texture %d: queue full, loading synchronously.\n", pageId);
    loadTextureSync(c3d, pageId, raw, txtr->blobSize);
}

#endif // __3DS__