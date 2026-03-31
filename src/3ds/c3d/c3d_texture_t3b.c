//
// Created by Pugemon on 30.03.2026.
//

#ifdef __3DS__

#include "c3d_texture_t3b.h"
#include "c3d_texture.h"
#include "c3d_constants.h"
#include "citro3d_renderer.h"
#include "data_win.h"

#include <tex3ds.h>
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// Открытие / закрытие архива
// ─────────────────────────────────────────────────────────────────────────────

bool TexArchive_open(Citro3dRenderer *c3d, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[T3B] Cannot open archive: %s\n", path);
        return false;
    }

    uint32_t count, dataOff;
    if (fread(&count, 4, 1, f) != 1 || fread(&dataOff, 4, 1, f) != 1) {
        fprintf(stderr, "[T3B] Header read failed\n");
        fclose(f);
        return false;
    }

    // Проверяем совместимость с txtr.count из data.win.
    // count в бандле должен = txtr.count (кол-во EmbeddedTextures после скрипта).
    if (count != c3d->texCount) {
        fprintf(stderr, "[T3B] WARNING: bundle has %u slots, data.win has %u textures!\n",
                count, c3d->texCount);
        // Не фатально: продолжаем с min(count, texCount)
    }

    // offsets[count+1]: offsets[i] = start, offsets[i+1] = end для i-й текстуры
    uint32_t  nOffsets = count + 1;
    uint32_t *offsets  = malloc(nOffsets * sizeof(uint32_t));
    if (!offsets) {
        fprintf(stderr, "[T3B] OOM for offsets table (%u entries)\n", nOffsets);
        fclose(f);
        return false;
    }

    if (fread(offsets, sizeof(uint32_t), nOffsets, f) != nOffsets) {
        fprintf(stderr, "[T3B] Offsets read failed\n");
        free(offsets);
        fclose(f);
        return false;
    }

    c3d->archiveFile    = f;
    c3d->archiveOffsets = offsets;
    c3d->archiveDataOff = dataOff;
    c3d->vramUsed       = 0;
    strncpy(c3d->archivePath, path, sizeof(c3d->archivePath) - 1);

    fprintf(stderr, "[T3B] Opened: %s (%u slots, dataOff=0x%X)\n",
            path, count, dataOff);
    return true;
}

void TexArchive_close(Citro3dRenderer *c3d) {
    if (c3d->archiveFile) {
        fclose(c3d->archiveFile);
        c3d->archiveFile = NULL;
    }
    free(c3d->archiveOffsets);
    c3d->archiveOffsets = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Внутренние утилиты
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Освобождает одну текстуру. Вызывает правильный free в зависимости от пула.
 *
 * C3D_TexDelete ВСЕГДА вызывает linearFree(tex->data).
 * Если tex->data указывает в VRAM → linearFree(vram_addr) → corruption heap.
 * Поэтому для VRAM-текстур вызываем vramFree + memset вручную.
 */
static size_t freeTexture(Citro3dRenderer *c3d, int id) {
    if (!c3d->textures[id].data) return 0;

    size_t freed = c3d->textures[id].size;

    // ВАЖНО: Мы полностью доверяем C3D_TexDelete.
    // Внутри она вызовет addrIsVRAM(tex->data) и сделает правильный free.
    C3D_TexDelete(&c3d->textures[id]);

    // Очищаем структуру и сбрасываем флаг пула
    memset(&c3d->textures[id], 0, sizeof(C3D_Tex));
    c3d->texInVram[id] = false;
    c3d->textures[id].data = NULL;

    c3d->vramUsed -= freed;
    return freed;
}

/**
 * Ищет LRU-текстуру из конкретного пула (VRAM или linear).
 * Возвращает id или -1 если нет текстур в этом пуле.
 */
static int findLRU(const Citro3dRenderer *c3d, bool fromVram) {
    int      oldestId   = -1;
    uint32_t oldestTime = UINT32_MAX;
    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (!c3d->textures[i].data) continue;
        if (c3d->texInVram[i] != fromVram) continue;
        if (c3d->texLastUsed[i] < oldestTime) {
            oldestTime = c3d->texLastUsed[i];
            oldestId   = (int)i;
        }
    }
    return oldestId;
}

/**
 * Вытесняет одну LRU-текстуру из указанного пула.
 * Перед удалением сбрасывает батч — GPU может ещё держать ссылку на эту текстуру.
 */
static size_t evictLRU(Citro3dRenderer *c3d, bool fromVram) {
    int id = findLRU(c3d, fromVram);
    if (id < 0) return 0;

    fprintf(stderr, "[T3B] Evict tex %d from %s (frame %u, %zu bytes)\n",
            id, fromVram ? "VRAM" : "linear", c3d->texLastUsed[id],
            c3d->textures[id].size);

    // Сброс батча ДО удаления: GPU мог получить draw call с этой текстурой.
    extern void flushBatch(Citro3dRenderer *c3d);
    flushBatch(c3d);

    return freeTexture(c3d, id);
}

/**
 * Освобождает linear heap, вытесняя linear-текстуры, пока не хватит места.
 * Намеренно не трогает VRAM-текстуры — они linear не занимают.
 */
static bool evictUntilLinearFits(Citro3dRenderer *c3d, size_t needed) {
    while (linearSpaceFree() < needed + LINEAR_SAFE_RESERVE) {
        size_t freed = evictLRU(c3d, false /* linear */);
        if (freed == 0) {
            fprintf(stderr, "[T3B] WARNING: no linear textures to evict for %zu bytes "
                    "(freeLinear=%lu)\n", needed, (unsigned long)linearSpaceFree());
            return false;
        }
    }
    return true;
}

/**
 * Читает t3x-блоб из открытого архива в malloc-буфер.
 * Возвращает указатель (caller освобождает через free) и размер в *outSize.
 * При ошибке возвращает NULL.
 */
static void *readBlob(Citro3dRenderer *c3d, int atlasId, uint32_t *outSize) {
    if (!c3d->archiveFile || !c3d->archiveOffsets) {
        fprintf(stderr, "[T3B] Archive not open! Call TexArchive_open() in init.\n");
        return NULL;
    }

    uint32_t start = c3d->archiveOffsets[atlasId];
    uint32_t end   = c3d->archiveOffsets[atlasId + 1];
    uint32_t size  = end - start;

    if (size == 0) {
        fprintf(stderr, "[T3B] Tex %d: empty slot in bundle\n", atlasId);
        return NULL;
    }

    void *buf = malloc(size);
    if (!buf) {
        fprintf(stderr, "[T3B] OOM: malloc(%u) for tex %d\n", size, atlasId);
        return NULL;
    }

    if (fseek(c3d->archiveFile, (long)(c3d->archiveDataOff + start), SEEK_SET) != 0) {
        fprintf(stderr, "[T3B] fseek failed for tex %d\n", atlasId);
        free(buf);
        return NULL;
    }

    size_t got = fread(buf, 1, size, c3d->archiveFile);
    if (got != size) {
        fprintf(stderr, "[T3B] Short read for tex %d: want %u, got %zu\n",
                atlasId, size, got);
        free(buf);
        return NULL;
    }

    *outSize = size;
    return buf;
}

/**
 * Импортирует t3x-блоб в GPU-текстуру.
 *
 * Шаг 1 — всегда загружаем в linear heap (vram=false, безопасно, никакого DMA):
 *   Tex3DS_TextureImport(vram=false) делает memcpy прямо в tex->data (linear).
 *   Нет GX_TextureCopy, нет race condition с рендер-тредом.
 *
 * Шаг 2 — опциональный promote в VRAM через CPU memcpy (только если gpuDmaSafe):
 *   Когда Core 1 спит (между beginFrame и endFrame), CPU может писать в VRAM
 *   напрямую по AXI-шине без кэш-флашей. Это освобождает linear heap для VM.
 *   НЕ используем Tex3DS с vram=true — он делает асинхронный GX_TextureCopy
 *   который конкурирует с C3D_FrameEnd на Core 1 за GSP-очередь.
 *
 * Шаг 3 — записываем texInVram[atlasId] для корректного освобождения позднее.
 */
static bool importBlob(Citro3dRenderer *c3d, int atlasId, void *buf, uint32_t size) {
    C3D_Tex *tex = &c3d->textures[atlasId];

    // Шаг 1: загружаем в linear heap
    Tex3DS_Texture t3x = Tex3DS_TextureImport(buf, size, tex, NULL, false);
    if (!t3x) {
        fprintf(stderr, "[T3B] Tex3DS_TextureImport failed for tex %d\n", atlasId);
        return false;
    }

    const Tex3DS_SubTexture *sub = Tex3DS_GetSubTexture(t3x, 0);
    c3d->texRealW[atlasId] = (int)sub->width;
    c3d->texRealH[atlasId] = (int)sub->height;
    Tex3DS_TextureFree(t3x);  // освобождает обёртку; tex->data (linear) остаётся

    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    // Шаг 2: promote в VRAM если Core 1 спит
    c3d->texInVram[atlasId] = false;
    if (c3d->gpuDmaSafe) {
        // Если VRAM полон — вытесняем старейшую VRAM-текстуру
        if (vramSpaceFree() < tex->size + VRAM_SAFE_RESERVE) {
            evictLRU(c3d, true /* VRAM */);
        }
        void *vramPtr = vramAlloc(tex->size);
        if (vramPtr) {
            memcpy(vramPtr, tex->data, tex->size);  // CPU copy, нет DMA
            linearFree(tex->data);
            tex->data = vramPtr;
            c3d->texInVram[atlasId] = true;
        }
    }

    c3d->vramUsed += tex->size;

    fprintf(stderr, "[T3B] Tex %d OK: sprite=%dx%d GPU=%ux%u "
            "size=%zu bytes in %s (freeVRAM=%lu freeLinear=%lu)\n",
            atlasId, c3d->texRealW[atlasId], c3d->texRealH[atlasId],
            tex->width, tex->height, tex->size,
            c3d->texInVram[atlasId] ? "VRAM" : "linear",
            (unsigned long)vramSpaceFree(), (unsigned long)linearSpaceFree());

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Публичный API
// ─────────────────────────────────────────────────────────────────────────────

void ensureAtlasLoaded(Citro3dRenderer *c3d, int atlasId) {
    if (atlasId < 0 || atlasId >= (int)c3d->texCount) return;

    // LRU touch — всегда, даже если текстура уже загружена
    c3d->texLastUsed[atlasId] = c3d->currentFrame;

    if (c3d->textures[atlasId].data != NULL) return;  // уже в VRAM

    fprintf(stderr, "[T3B] Frame %u: loading tex %d...\n",
            c3d->currentFrame, atlasId);

    uint32_t size;
    void *buf = readBlob(c3d, atlasId, &size);
    if (!buf) return;

    evictUntilLinearFits(c3d, (size_t)size);

    bool ok = importBlob(c3d, atlasId, buf, size);
    free(buf);  // освобождаем сразу: Tex3DS_TextureImport уже скопировал в linearAlloc

    (void)ok;   // ошибка уже залоггирована внутри importBlob
}

void TexArchive_preloadPageIds(Citro3dRenderer *c3d,
                               const uint32_t *pageIds,
                               uint32_t pageCount)
{
    if (!pageIds || pageCount == 0) return;

    uint32_t maxId = c3d->texCount;

    // Bitset для дедупликации: O(pageCount + maxId), без сортировки
    uint8_t *seen = calloc(maxId, sizeof(uint8_t));
    if (!seen) {
        fprintf(stderr, "[T3B] preload: OOM for bitset (%u bytes)\n", maxId);
        return;
    }

    uint32_t uniqueCount = 0;
    for (uint32_t i = 0; i < pageCount; i++) {
        if (pageIds[i] < maxId && !seen[pageIds[i]]) {
            seen[pageIds[i]] = 1;
            uniqueCount++;
        }
    }

    fprintf(stderr, "[T3B] Preload: %u unique pages from %u ids "
            "(VRAM used=%zu/%u)\n",
            uniqueCount, pageCount, c3d->vramUsed, VRAM_BUDGET_BYTES);

    uint32_t loaded = 0, alreadyCached = 0;
    for (uint32_t pid = 0; pid < maxId; pid++) {
        if (!seen[pid]) continue;

        bool wasLoaded = (c3d->textures[pid].data != NULL);
        ensureAtlasLoaded(c3d, (int)pid);

        if (wasLoaded)                             alreadyCached++;
        else if (c3d->textures[pid].data != NULL)  loaded++;
    }

    free(seen);

    fprintf(stderr, "[T3B] Preload done: %u loaded, %u cached, "
            "VRAM used=%zu/%u bytes\n",
            loaded, alreadyCached, c3d->vramUsed, VRAM_BUDGET_BYTES);
}

#endif // __3DS__
