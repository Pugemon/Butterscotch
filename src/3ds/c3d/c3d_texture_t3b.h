//
// Created by Pugemon on 30.03.2026.
//

#pragma once

#ifdef __3DS__

#include <stdbool.h>
#include <stdint.h>

#include "data_win.h"

// Forward-declarations: полные определения в citro3d_renderer.h и data_win.h
typedef struct Citro3dRenderer Citro3dRenderer;

/**
 * @file c3d_texture_t3b.h
 * @brief Загрузка текстур из .t3b бандла (ETC1A4 / Tex3DS формат).
 *
 * ## Формат .t3b
 *
 * Самодельный бандл-формат: один файл содержит все текстуры игры.
 * Структура файла:
 *
 *   [uint32 count]             — число текстур (= DataWin->txtr.count)
 *   [uint32 dataOff]           — абсолютное смещение начала DATA-секции
 *   [uint32 offsets[count+1]]  — offsets[i]..offsets[i+1] = диапазон i-й текстуры
 *   [DATA: t3x blob'ы подряд]
 *
 * Каждый blob — это стандартный .t3x файл (формат Tex3DS).
 * Tex3DS_TextureImport() загружает его напрямую в linearAlloc (VRAM-совместимая память),
 * уже в Morton-упорядоченном формате ETC1A4. Morton-свиззл делать не нужно.
 *
 * ## LRU по байтам VRAM
 *
 * Лимит задаётся в байтах (VRAM_BUDGET_BYTES), не в штуках.
 * При загрузке новой текстуры:
 *   1. Если vramUsed + tex->size > VRAM_BUDGET_BYTES — вытесняем LRU-текстуры
 *      в цикле, пока не освободится место.
 *   2. Загружаем через Tex3DS_TextureImport и прибавляем tex->size к vramUsed.
 *
 * ## Preload
 *
 * TexArchive_preloadForTPAGs() загружает все уникальные текстуры нужные для
 * набора TPAG-элементов (например, всей комнаты) до начала рендеринга.
 * Это устраняет подгрузки в рантайме — SD-карта на 3DS медленная (~1–5 MB/s).
 */

/**
 * @brief Открывает .t3b архив. Вызывать один раз в C3DRenderer_initTextures.
 *
 * Читает заголовок и таблицу смещений. Файл остаётся открытым на всё время
 * жизни рендерера — последующие ensureAtlasLoaded делают только fseek+fread.
 *
 * @param c3d  Рендерер (должен иметь basePath и texCount заполненными)
 * @param path Путь к файлу, например "sdmc:/game/undertale.t3b"
 * @return true при успехе
 */
bool TexArchive_open(Citro3dRenderer *c3d, const char *path);

/**
 * @brief Закрывает архив. Вызывать в C3DRenderer_destroy перед удалением текстур.
 */
void TexArchive_close(Citro3dRenderer *c3d);

/**
 * @brief Гарантирует, что текстура atlasId загружена в VRAM.
 *
 * Если уже загружена — только обновляет texLastUsed (LRU touch).
 * Если не загружена — вытесняет LRU при необходимости и загружает.
 *
 * @param c3d     Рендерер
 * @param atlasId Индекс текстуры = tpag->texturePageId [0, texCount)
 */
void ensureAtlasLoaded(Citro3dRenderer *c3d, int atlasId);

/**
 * @brief Предзагружает текстуры по списку texturePageId.
 *
 * Загружает каждый уникальный ID из pageIds[] через ensureAtlasLoaded
 * (с VRAM-бюджетом и LRU). Уже загруженные пропускаются.
 *
 * Вызывать при загрузке комнаты, передавая ID страниц тайлов и объектов.
 * После вызова все нужные текстуры будут в VRAM — рендеринг без пауз.
 *
 * @param pageIds   Массив texturePageId (могут быть дубли — дедупликация внутри)
 * @param pageCount Число элементов
 */
void TexArchive_preloadPageIds(Citro3dRenderer *c3d,
                                const uint32_t *pageIds,
                                uint32_t pageCount);

#endif // __3DS__
