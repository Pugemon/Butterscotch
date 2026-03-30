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
 * Вытесняет одну LRU-текстуру. Возвращает сколько байт освободилось, или 0
 * если вытеснять нечего (все слоты пусты).
 *
 * ВАЖНО: сбрасывает батч ДО удаления — VBO может содержать вершины,
 * ссылающиеся на эту текстуру. Рисование с удалённой текстурой = мусор на экране.
 */
static size_t evictOneLRU(Citro3dRenderer *c3d) {
    int      oldestId   = -1;
    uint32_t oldestTime = UINT32_MAX;

    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (c3d->textures[i].data != NULL && c3d->texLastUsed[i] < oldestTime) {
            oldestTime = c3d->texLastUsed[i];
            oldestId   = (int)i;
        }
    }

    if (oldestId < 0) return 0;

    size_t freed = c3d->textures[oldestId].size;
    fprintf(stderr, "[T3B] Evict tex %d (frame %u, %zu bytes). VRAM: %zu → %zu\n",
            oldestId, oldestTime, freed, c3d->vramUsed, c3d->vramUsed - freed);

    // Сброс батча ДО удаления — обязательно
    extern void flushBatch(Citro3dRenderer *c3d);
    flushBatch(c3d);

    C3D_TexDelete(&c3d->textures[oldestId]);
    c3d->textures[oldestId].data = NULL;
    c3d->vramUsed -= freed;

    return freed;
}

/**
 * Освобождает VRAM в цикле, пока vramUsed + needed <= VRAM_BUDGET_BYTES.
 * Возвращает false если места всё равно не хватило (некого вытеснять),
 * но загрузку не блокирует — лучше превысить бюджет, чем показать чёрный экран.
 */
static bool evictUntilFits(Citro3dRenderer *c3d, size_t needed) {
    while (c3d->vramUsed + needed > VRAM_BUDGET_BYTES) {
        size_t freed = evictOneLRU(c3d);
        if (freed == 0) {
            // Нечего вытеснять, но места нет. Загружаем всё равно.
            fprintf(stderr, "[T3B] WARNING: budget exceeded! "
                    "needed=%zu, used=%zu, budget=%u\n",
                    needed, c3d->vramUsed, VRAM_BUDGET_BYTES);
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
 * Импортирует t3x-блоб в GPU-текстуру. Обновляет vramUsed и texRealW/H.
 * Возвращает true при успехе.
 */
static bool importBlob(Citro3dRenderer *c3d, int atlasId, void *buf, uint32_t size) {
    C3D_Tex *tex = &c3d->textures[atlasId];

    // Tex3DS_TextureImport аллоцирует linearAlloc и копирует данные.
    // Формат ETC1A4 — Morton-упорядоченный по умолчанию, свиззл не нужен.
    Tex3DS_Texture t3x = Tex3DS_TextureImport(buf, size, tex, NULL, false);
    if (!t3x) {
        fprintf(stderr, "[T3B] Tex3DS_TextureImport failed for tex %d\n", atlasId);
        return false;
    }

    // sub->width/height — логический размер спрайта внутри POT-текстуры.
    // tex->width/height — POT-размер, нужен calcUV для правильных UV-координат.
    const Tex3DS_SubTexture *sub = Tex3DS_GetSubTexture(t3x, 0);
    c3d->texRealW[atlasId] = (int)sub->width;
    c3d->texRealH[atlasId] = (int)sub->height;

    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    Tex3DS_TextureFree(t3x);  // освобождаем обёртку; данные в linearAlloc остаются

    c3d->vramUsed += tex->size;

    fprintf(stderr, "[T3B] Tex %d OK: sprite=%dx%d GPU=%ux%u "
            "size=%zu VRAM used=%zu/%u\n",
            atlasId,
            c3d->texRealW[atlasId], c3d->texRealH[atlasId],
            tex->width, tex->height,
            tex->size, c3d->vramUsed, VRAM_BUDGET_BYTES);

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

    // Оцениваем нужный VRAM по размеру блоба: для ETC1A4 это точный размер.
    // evictUntilFits вытеснит LRU-текстуры пока не освободится место.
    evictUntilFits(c3d, (size_t)size);

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
