#pragma once

#include "renderer.h"

#ifdef __3DS__
#include <c3d/renderqueue.h>
#include <citro3d.h>
#include "decode_thread.h"

#include "c3d/c3d_constants.h"

/**
 * @file citro3d_renderer.h
 * @brief Структура рендерера Citro3D и публичный конструктор.
 *
 * Citro3dRenderer — конкретная реализация абстрактного Renderer для Nintendo 3DS.
 * Использует аппаратное ускорение PICA200 через библиотеку Citro3D.
 *
 * Для создания рендерера используй Citro3dRenderer_create().
 * Для инициализации GPU-ресурсов — renderer->vtable->init(renderer, dataWin).
 */

// ─────────────────────────────────────────────────────────────────────────────
// Структура вершины
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Вершина, передаваемая в GPU. Должна строго соответствовать AttrInfo в c3d_init.c
 * и объявлениям .in в шейдере (shader.v.pica):
 *
 *   v0 (attrib 0): position  — float3 (x, y, z)
 *   v1 (attrib 1): texcoord  — float2 (u, v)
 *   v2 (attrib 2): color     — ubyte4 упакован в u32 (формат ABGR)
 */
typedef struct {
    float x, y, z;  ///< Позиция в мировых координатах
    float u, v;      ///< UV-координаты текстуры [0..1]
    u32   color;     ///< Цвет-тинт в формате ABGR (A=старший байт)
} C3D_Vertex;

// ─────────────────────────────────────────────────────────────────────────────
// Структура рендерера
// ─────────────────────────────────────────────────────────────────────────────

typedef struct Citro3dRenderer {
    Renderer          base;      ///< Базовый класс (ДОЛЖЕН быть первым полем)
    char              basePath[512]; /// Путь до папки с data.win (нужен для загрузки t3b)

    // ── GPU-ресурсы ──────────────────────────────────────────────────────────
    C3D_RenderTarget *target;    ///< Render target (буфер кадра в VRAM)
    DVLB_s           *dvlb;      ///< Скомпилированный бинарник шейдера
    shaderProgram_s   shader;    ///< Программа шейдера (vertex only)
    int8_t            uLoc_projection; ///< Локация uniform projMtx в шейдере
    C3D_Mtx           projection;      ///< Текущая проекционная матрица
    float viewX, viewY, viewW, viewH; ///< Viewport Culling

    // ── Массивы текстур (размер texCount + 1 для белой заглушки) ─────────────
    C3D_Tex  *textures;    ///< C3D_Tex[texCount+1]: GPU-текстуры (NULL.data = не загружена)
    int      *texRealW;    ///< Исходная ширина до POT-округления
    int      *texRealH;    ///< Исходная высота до POT-округления
    uint32_t  texCount;    ///< Число игровых текстур из DataWin->txtr
    int       whiteTexIndex; ///< Индекс белой заглушки (= texCount)

    // ── LRU-кэш текстур ───────────────────────────────────────────────────────
    uint32_t *texLastUsed;  ///< texLastUsed[i] = currentFrame последнего использования
    uint32_t  currentFrame; ///< Счётчик кадров (инкрементируется в beginFrame)
    size_t    vramUsed;     ///< Суммарный tex->size загруженных текстур (без белой заглушки)

    // ── VBO и состояние батча ─────────────────────────────────────────────────
    C3D_Vertex *vboData;       ///< Вершинный буфер в linearAlloc (MAX_VERTICES вершин)
    int         vertexCount;   ///< Количество записанных вершин (хвост буфера)
    int         batchStart;    ///< Начало текущего незакоммиченного батча
    int         currentTexIndex; ///< Текстура текущего батча (-1 = не задана)
    DecodeThread *decodeThread; ///< NULL если не инициализирован

    // ── .t3b бандл (ETC1A4 T3X текстуры) ────────────────────────────────────────
    FILE     *archiveFile;       ///< Файл открыт на всё время жизни рендерера
    uint32_t *archiveOffsets;    ///< offsets[0..texCount]: start/end в DATA-секции
    uint32_t  archiveDataOff;    ///< Абсолютный оффсет начала DATA в файле
    char      archivePath[512];  ///< Путь к .t3b (для логов/переоткрытия)
} Citro3dRenderer;

// ─────────────────────────────────────────────────────────────────────────────
// Публичный API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Создаёт экземпляр рендерера Citro3D.
 *
 * Выделяет память и настраивает vtable. GPU-ресурсы (текстуры, VBO и т.д.)
 * инициализируются позже через renderer->vtable->init(renderer, dataWin).
 *
 * Вызывать один раз после gfxInitDefault() в main().
 *
 * @return Указатель на Renderer (реально Citro3dRenderer*, приведённый к базовому типу)
 */
Renderer *Citro3dRenderer_create(void);

#endif // __3DS__
