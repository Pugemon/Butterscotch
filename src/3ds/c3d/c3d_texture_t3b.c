//
// Created by Pugemon on 30.03.2026.
//

#ifdef __3DS__

// ─────────────────────────────────────────────────────────────────────────────
// c3d_texture_t3b.c — загрузка текстур из .t3b бандла (ETC1A4 / t3x формат)
//
// Заменяет ensureTextureLoaded из c3d_texture.c при сборке с -DT3B_BUNDLE.
//
// Изменения в citro3d_renderer.h (добавить поля в Citro3dRenderer):
//
//   FILE     *archiveFile;    ///< .t3b открыт на всё время жизни рендерера
//   uint32_t *archiveOffsets; ///< offsets[0..texCount] из заголовка бандла
//   uint32_t  archiveDataOff; ///< абсолютный оффсет начала DATA-секции в файле
//   char      archivePath[512]; ///< путь для открытия
//
// Изменения в c3d_init.c:
//   В C3DRenderer_initTextures — вызвать TexArchive_open()
//   В C3DRenderer_destroy     — вызвать TexArchive_close()
// ─────────────────────────────────────────────────────────────────────────────

#include "c3d_texture.h"
#include "c3d_constants.h"
#include "citro3d_renderer.h"

#include <tex3ds.h>
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────────────────────
// Открыть архив — один раз при инициализации рендерера
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
        fprintf(stderr, "[T3B] MISMATCH: bundle has %u slots, data.win has %u textures!\n",
                count, c3d->texCount);
        // Не фатально — продолжаем с min(count, texCount), но логируем.
    }

    uint32_t nOffsets = count + 1; // offsets[i] = start, offsets[i+1] = end
    uint32_t *offsets = malloc(nOffsets * sizeof(uint32_t));
    if (!offsets) {
        fprintf(stderr, "[T3B] OOM for offsets table\n");
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
    strncpy(c3d->archivePath, path, sizeof(c3d->archivePath) - 1);

    fprintf(stderr, "[T3B] Opened: %s (%u slots, dataOff=0x%X)\n",
            path, count, dataOff);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Закрыть архив — при уничтожении рендерера
// ─────────────────────────────────────────────────────────────────────────────

void TexArchive_close(Citro3dRenderer *c3d) {
    if (c3d->archiveFile) {
        fclose(c3d->archiveFile);
        c3d->archiveFile = NULL;
    }
    free(c3d->archiveOffsets);
    c3d->archiveOffsets = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Вытеснение LRU (идентично оригиналу из c3d_texture.c)
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

    fprintf(stderr, "[T3B] Evicting tex %d (last frame %u)\n", oldestId, oldestTime);

    // Сброс батча ДО удаления — в нём могут быть вершины с этой текстурой
    extern void flushBatch(Citro3dRenderer *c3d);
    flushBatch(c3d);

    C3D_TexDelete(&c3d->textures[oldestId]);
    c3d->textures[oldestId].data = NULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureAtlasLoaded — заменяет ensureTextureLoaded
//
// atlasId = tpag->texturePageId = индекс EmbeddedTexture в data.win
//         = индекс слота в .t3b бандле
//         (это гарантируется скриптом UndertaleModTool: EmbeddedTextures[i]
//          создаётся для TexturePageItems[i], поэтому texturePageId[i] == i)
// ─────────────────────────────────────────────────────────────────────────────

void ensureAtlasLoaded(Citro3dRenderer *c3d, int atlasId) {
    // ── Проверка границ ──────────────────────────────────────────────────────
    if (atlasId < 0 || atlasId >= (int)c3d->texCount) {
        fprintf(stderr, "[T3B] atlasId %d out of bounds (texCount=%u)\n",
                atlasId, c3d->texCount);
        return;
    }

    // ── LRU touch ────────────────────────────────────────────────────────────
    c3d->texLastUsed[atlasId] = c3d->currentFrame;

    // ── Уже загружена ────────────────────────────────────────────────────────
    if (c3d->textures[atlasId].data != NULL) return;

    // ── Архив не открыт (инициализация не вызвана) ───────────────────────────
    if (!c3d->archiveFile || !c3d->archiveOffsets) {
        fprintf(stderr, "[T3B] Archive not open! Call TexArchive_open() in init.\n");
        return;
    }

    // ── Проверяем, что слот существует в бандле ──────────────────────────────
    // archiveOffsets содержит count+1 элементов; atlasId < texCount ≤ count
    uint32_t start = c3d->archiveOffsets[atlasId];
    uint32_t end   = c3d->archiveOffsets[atlasId + 1];
    uint32_t size  = end - start;

    if (size == 0) {
        fprintf(stderr, "[T3B] Tex %d: slot empty in bundle (size=0)\n", atlasId);
        return;
    }

    fprintf(stderr, "[T3B] Loading tex %d (offset=0x%X, size=%u)...\n",
            atlasId, c3d->archiveDataOff + start, size);

    // ── LRU: вытесняем если кэш полон ────────────────────────────────────────
    int loadedCount = 0;
    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (c3d->textures[i].data != NULL) loadedCount++;
    }
    if (loadedCount >= MAX_CACHED_TEXTURES) {
        evictLRU(c3d);
    }

    // ── Читаем t3x из открытого архива ───────────────────────────────────────
    void *tempBuf = malloc(size);
    if (!tempBuf) {
        fprintf(stderr, "[T3B] OOM: malloc(%u) failed for tex %d\n", size, atlasId);
        return;
    }

    // Архив открыт постоянно — просто seek + read, без fopen/fclose
    if (fseek(c3d->archiveFile, (long)(c3d->archiveDataOff + start), SEEK_SET) != 0) {
        fprintf(stderr, "[T3B] fseek failed for tex %d\n", atlasId);
        free(tempBuf);
        return;
    }

    size_t got = fread(tempBuf, 1, size, c3d->archiveFile);
    if (got != size) {
        fprintf(stderr, "[T3B] Short read for tex %d: want %u, got %zu\n",
                atlasId, size, got);
        free(tempBuf);
        return;
    }

    // ── Импорт t3x → C3D_Tex ─────────────────────────────────────────────────
    // Tex3DS_TextureImport аллоцирует VRAM через linearAlloc и копирует данные.
    // После вызова tempBuf больше не нужен.
    C3D_Tex *tex = &c3d->textures[atlasId];
    Tex3DS_Texture t3x = Tex3DS_TextureImport(tempBuf, size, tex, NULL, false);

    free(tempBuf); // освобождаем СРАЗУ после импорта, до любого return

    if (!t3x) {
        fprintf(stderr, "[T3B] Tex3DS_TextureImport failed for tex %d\n", atlasId);
        // tex->data остался NULL — при следующем обращении попробуем снова
        return;
    }

    // ── Сохраняем логический (не-POT) размер ─────────────────────────────────
    // sub->width/height — это РЕАЛЬНЫЙ размер спрайта внутри POT-текстуры.
    // tex->width/height — это POT-размер (используется в calcUV).
    // Оба нужны: texRealW для sprite_width/height в GML,
    //            tex->width для правильных UV-координат.
    const Tex3DS_SubTexture *sub = Tex3DS_GetSubTexture(t3x, 0);
    c3d->texRealW[atlasId] = (int)sub->width;
    c3d->texRealH[atlasId] = (int)sub->height;

    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    Tex3DS_TextureFree(t3x); // освобождаем обёртку Tex3DS (не сами данные VRAM)

    fprintf(stderr, "[T3B] Tex %d OK: sprite=%dx%d GPU=%ux%u VRAM=%zu bytes\n",
            atlasId,
            c3d->texRealW[atlasId], c3d->texRealH[atlasId],
            tex->width, tex->height, tex->size);
}

#endif // __3DS__
