//
// Created by Pugemon on 28.03.2026.
//

#ifdef __3DS__

#include "decode_thread.h"
#include "c3d/c3d_utils.h"    // nextPOT, calcMortonOffset

#include "stb_image.h"

#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// stbi использует рекурсию и временные буферы внутри.
// 128 KiB — достаточно даже для больших PNG Undertale (512×512, атласы).
#define DT_STACK_SIZE  (128u * 1024u)

// Приоритет ниже, чем у RenderThread (0x31), но выше idle (0x3F).
// Когда RenderThread ждёт GPU — он спит в OS event wait, и Core 1 полностью
// свободен для декодирования. Когда GPU готов — RenderThread вытесняет нас.
#define DT_PRIORITY    0x32

// ─────────────────────────────────────────────────────────────────────────────
// Morton-свизл в обычный malloc-буфер (не linearAlloc)
//
// Почему не writePixelMorton_RGBA4 из c3d_texture.c:
//   - Та функция пишет в tex->data, который живёт в linear heap (linearAlloc).
//   - linearAlloc не thread-safe — нельзя вызывать с Core 1.
//   - Здесь мы свизлим в обычный malloc, а Core 0 потом сделает memcpy.
// ─────────────────────────────────────────────────────────────────────────────

static void swizzleToMalloc_RGBA4(u16 *dst, uint32_t potW, uint32_t potH,
                                   const uint8_t *rgba, uint32_t srcW, uint32_t srcH)
{
    memset(dst, 0, potW * potH * sizeof(u16));

    uint32_t tilesX = potW >> 3;

    for (uint32_t y = 0; y < srcH; y++) {
        uint32_t tileY      = y >> 3;
        uint32_t tileRowOff = (tileY * tilesX) << 6; // * 64 пикселей на тайл

        for (uint32_t x = 0; x < srcW; x++) {
            uint32_t srcOff = (y * srcW + x) * 4;
            uint8_t  r = rgba[srcOff + 0];
            uint8_t  g = rgba[srcOff + 1];
            uint8_t  b = rgba[srcOff + 2];
            uint8_t  a = rgba[srcOff + 3];

            uint32_t tileX   = x >> 3;
            uint32_t tileOff = tileRowOff + (tileX << 6);
            uint32_t morOff  = calcMortonOffset(x & 7, y & 7);

            // GPU_RGBA4: биты [15:12]=R, [11:8]=G, [7:4]=B, [3:0]=A
            dst[tileOff + morOff] = ((u16)(r >> 4) << 12)
                                  | ((u16)(g >> 4) <<  8)
                                  | ((u16)(b >> 4) <<  4)
                                  |  (u16)(a >> 4);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Точка входа треда (Core 1)
// ─────────────────────────────────────────────────────────────────────────────

static void decodeThreadEntry(void *arg) {
    DecodeThread *dt = (DecodeThread *)arg;

    while (true) {
        // Спим, пока Core 0 не подаст сигнал о новом job'е.
        // RESET_ONESHOT гарантирует: если Core 0 подал два сигнала пока мы
        // работали, мы всё равно пройдём по всем слотам и найдём оба job'а.
        LightEvent_Wait(&dt->jobAvailable);

        if (dt->shouldExit) break;

        // Обрабатываем все PENDING слоты за один проход.
        // (Несколько текстур могут быть поставлены в очередь между двумя Wake)
        for (int i = 0; i < DECODE_QUEUE_SIZE; i++) {
            DecodeJob *job = &dt->jobs[i];

            if (job->state != DECODE_JOB_PENDING) continue;

            // Сначала меняем state, потом работаем.
            // __sync_synchronize — DMB: все предыдущие записи Core 0 (pngRaw,
            // pngSize) видны нам до того, как мы начинаем их читать.
            __sync_synchronize();
            job->state = DECODE_JOB_RUNNING;

            // ── Шаг 1: PNG-декод ─────────────────────────────────────────────
            int w = 0, h = 0, ch = 0;
            uint8_t *px = stbi_load_from_memory(
                job->pngRaw, (int)job->pngSize, &w, &h, &ch, 4);

            // pngRaw больше не нужен — освобождаем немедленно
            free(job->pngRaw);
            job->pngRaw = NULL;

            if (!px) {
                fprintf(stderr, "[DecodeThread] stbi failed: pageId=%d\n", job->pageId);
                job->state = DECODE_JOB_ERROR;
                continue;
            }

            // ── Шаг 2: Morton-свизл в обычный malloc ─────────────────────────
            uint32_t potW = nextPOT((uint32_t)w);
            uint32_t potH = nextPOT((uint32_t)h);

            u16 *swizzled = (u16 *)malloc(potW * potH * sizeof(u16));
            if (!swizzled) {
                fprintf(stderr, "[DecodeThread] malloc failed: pageId=%d (%ux%u POT)\n",
                        job->pageId, potW, potH);
                stbi_image_free(px);
                job->state = DECODE_JOB_ERROR;
                continue;
            }

            swizzleToMalloc_RGBA4(swizzled, potW, potH, px, (uint32_t)w, (uint32_t)h);
            stbi_image_free(px);

            // ── Шаг 3: публикация результата ─────────────────────────────────
            job->swizzled = swizzled;
            job->potW     = potW;
            job->potH     = potH;
            job->realW    = w;
            job->realH    = h;

            // DMB: все записи выше (swizzled, potW, ...) должны быть видны
            // Core 0 ДО того, как он увидит state == DONE.
            __sync_synchronize();
            job->state = DECODE_JOB_DONE;

            fprintf(stderr, "[DecodeThread] done: pageId=%d (%d×%d → POT %u×%u)\n",
                    job->pageId, w, h, potW, potH);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Публичный API
// ─────────────────────────────────────────────────────────────────────────────

void DecodeThread_init(DecodeThread *dt) {
    memset(dt, 0, sizeof(*dt));
    LightEvent_Init(&dt->jobAvailable, RESET_ONESHOT);
    dt->shouldExit = false;

    dt->handle = threadCreate(
        decodeThreadEntry,
        dt,
        DT_STACK_SIZE,
        DT_PRIORITY,
        1,      // affinity: Core 1 (как и RenderThread, но они не конкурируют —
                // RenderThread спит пока Core 1 декодирует, и наоборот)
        false   // detached = false, нужен threadJoin при завершении
    );

    if (!dt->handle) {
        fprintf(stderr, "[DecodeThread] ОШИБКА: threadCreate не удался!\n");
    } else {
        fprintf(stderr, "[DecodeThread] запущен на Core 1 (priority=0x%02X)\n",
                DT_PRIORITY);
    }
}

void DecodeThread_destroy(DecodeThread *dt) {
    if (!dt->handle) return;

    dt->shouldExit = true;
    LightEvent_Signal(&dt->jobAvailable); // будим тред, чтобы он вышел из Wait

    threadJoin(dt->handle, UINT64_MAX);
    threadFree(dt->handle);
    dt->handle = NULL;

    // Освобождаем всё, что могло остаться в очереди (crash-safety)
    for (int i = 0; i < DECODE_QUEUE_SIZE; i++) {
        free(dt->jobs[i].pngRaw);
        dt->jobs[i].pngRaw = NULL;
        free(dt->jobs[i].swizzled);
        dt->jobs[i].swizzled = NULL;
    }
}

bool DecodeThread_isInFlight(const DecodeThread *dt, int pageId) {
    for (int i = 0; i < DECODE_QUEUE_SIZE; i++) {
        DecodeJobState s = dt->jobs[i].state;
        if (dt->jobs[i].pageId == pageId &&
            (s == DECODE_JOB_PENDING || s == DECODE_JOB_RUNNING)) {
            return true;
        }
    }
    return false;
}

bool DecodeThread_dispatch(DecodeThread *dt, int pageId,
                            uint8_t *pngRaw, uint32_t pngSize)
{
    for (int i = 0; i < DECODE_QUEUE_SIZE; i++) {
        DecodeJob *job = &dt->jobs[i];

        // IDLE и ERROR — оба считаются свободными слотами
        if (job->state == DECODE_JOB_IDLE || job->state == DECODE_JOB_ERROR) {
            job->pageId   = pageId;
            job->pngRaw   = pngRaw;
            job->pngSize  = pngSize;
            job->swizzled = NULL;

            // DMB: записать данные ДО изменения state на PENDING.
            // Без барьера Core 1 может увидеть PENDING раньше, чем pngRaw.
            __sync_synchronize();
            job->state = DECODE_JOB_PENDING;

            LightEvent_Signal(&dt->jobAvailable);
            return true;
        }
    }
    return false; // все слоты заняты
}

bool DecodeThread_tryCollect(DecodeThread *dt, int pageId,
                              u16 **swizzled, uint32_t *potW, uint32_t *potH,
                              int *realW, int *realH)
{
    for (int i = 0; i < DECODE_QUEUE_SIZE; i++) {
        DecodeJob *job = &dt->jobs[i];

        if (job->pageId != pageId || job->state != DECODE_JOB_DONE) continue;

        // DMB: читаем данные ПОСЛЕ того, как убедились, что state == DONE
        __sync_synchronize();

        *swizzled = job->swizzled;
        *potW     = job->potW;
        *potH     = job->potH;
        *realW    = job->realW;
        *realH    = job->realH;

        // Освобождаем слот
        job->swizzled = NULL;
        job->pngRaw   = NULL;
        job->state    = DECODE_JOB_IDLE;

        return true;
    }
    return false;
}

#endif // __3DS__
