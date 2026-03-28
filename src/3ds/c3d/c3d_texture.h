#pragma once
#ifdef __3DS__

#include <citro3d.h>
#include <stdbool.h>
#include <3ds.h>

// Forward-declaration: полное определение в citro3d_renderer.h
typedef struct Citro3dRenderer Citro3dRenderer;

/**
 * @file c3d_texture.h
 * @brief Управление текстурами: загрузка, выгрузка, LRU-кэш.
 *
 * ## Формат текстур на PICA200
 *
 * PICA200 не может читать текстуры в линейном (обычном) формате.
 * Все текстуры должны быть в Morton/Z-order тайловом формате (см. c3d_utils.h).
 * Кроме того, PICA200 поддерживает несколько форматов цвета; мы используем:
 *
 *   GPU_RGBA8  — 32 бит/пиксель, для белой заглушки (небольшая, 8×8)
 *   GPU_RGBA4  — 16 бит/пиксель, для игровых текстур (экономия VRAM)
 *
 * ## LRU-кэш текстур
 *
 * Всего VRAM на 3DS ≈ 4–6 MiB. Большие атласы (1024×1024 RGBA4 = 512 KiB)
 * не умещаются все сразу. Кэш хранит не более MAX_CACHED_TEXTURES текстур.
 * При нехватке места вытесняется текстура с наименьшим значением texLastUsed
 * (кадр последнего использования).
 *
 * ## Важно: сброс D-cache
 *
 * ARM11 (CPU 3DS) и PICA200 (GPU) работают с разными кэшами.
 * После записи данных через CPU нужно вызвать GSPGPU_FlushDataCache(),
 * иначе GPU может прочитать устаревшие данные из кэша DMA.
 */

/**
 * @brief Morton-свиззл: копирует линейный ABGR-буфер в тайловый формат текстуры.
 *
 * Заполняет tex->data Morton-упорядоченными пикселями из src, затем
 * сбрасывает D-cache CPU (GSPGPU_FlushDataCache) для корректного видения GPU.
 *
 * Область за пределами [srcW × srcH] заполняется нулями (прозрачный чёрный).
 *
 * @param tex  Целевая текстура (уже инициализирована C3D_TexInit, формат RGBA8)
 * @param src  Исходный буфер в формате ABGR, линейный, srcW × srcH пикселей
 * @param srcW Исходная ширина
 * @param srcH Исходная высота
 */
void swizzleToTex(C3D_Tex *tex, const u32 *src, u32 srcW, u32 srcH);

/**
 * @brief Загружает декодированные RGBA8-пиксели в GPU-текстуру формата RGBA4.
 *
 * Шаги:
 *  1. Вычисляет POT-размер (nextPOT).
 *  2. Инициализирует C3D_Tex через C3D_TexInit (GPU_RGBA4).
 *  3. Настраивает фильтрацию (LINEAR при увеличении, NEAREST при уменьшении)
 *     и wrapping (CLAMP_TO_EDGE по обоим осям).
 *  4. Конвертирует каждый пиксель RGBA8 → RGBA4 (старшие 4 бита каждого канала).
 *  5. Записывает пиксели в Morton-порядке напрямую в tex->data.
 *  6. Сбрасывает D-cache.
 *
 * @param tex   [out] Неинициализированная C3D_Tex
 * @param realW [out] Оригинальная ширина (до POT-округления)
 * @param realH [out] Оригинальная высота
 * @param rgba  Пиксели RGBA8
 * @param w     Ширина в пикселях
 * @param h     Высота в пикселях
 * @return true при успехе, false если C3D_TexInit провалился
 */
bool uploadTexture(C3D_Tex *tex, int *realW, int *realH, const u8 *rgba, int w, int h);

/**
 * @brief Создаёт белую 8×8 текстуру (GPU_RGBA8) для рисования примитивов.
 *
 * Эта текстура используется для прямоугольников, линий и любых примитивов,
 * которые должны рисоваться "чистым" цветом без текстуры.
 * Шейдер перемножает цвет вершины с цветом текстуры (GPU_MODULATE),
 * а белый = (1,1,1,1), что нейтрально при умножении.
 *
 * @param tex [out] Неинициализированная C3D_Tex
 * @return true при успехе
 */
bool createWhiteTexture(C3D_Tex *tex);

/**
 * @brief Гарантирует, что текстура с индексом pageId загружена в VRAM.
 *
 * Если уже загружена — только обновляет texLastUsed для LRU.
 * Если не загружена:
 *  1. Считает загруженные текстуры; если >= MAX_CACHED_TEXTURES,
 *     вытесняет текстуру с минимальным texLastUsed (самую давнюю).
 *  2. Читает PNG-blob из DataWin->txtr по blobOffset/blobSize.
 *  3. Декодирует PNG через stb_image (RGBA8).
 *  4. Загружает через uploadTexture() → GPU_RGBA4.
 *
 * Вызывать перед каждым draw-вызовом, использующим данную текстуру.
 *
 * @param c3d    Рендерер
 * @param pageId Индекс в DataWin->txtr [0, texCount)
 */
void ensureTextureLoaded(Citro3dRenderer *c3d, int pageId);

#endif // __3DS__
