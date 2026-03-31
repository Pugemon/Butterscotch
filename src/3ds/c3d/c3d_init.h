#pragma once
#ifdef __3DS__

#include "renderer.h"
#include "data_win.h"

/**
 * @file c3d_init.h
 * @brief Инициализация, уничтожение и управление кадром рендерера Citro3D.
 *
 * Инициализация разбита на независимые подфункции, каждая из которых
 * отвечает за один аспект GPU-системы. Порядок вызова важен:
 *
 *   1. initRenderTarget  — C3D + render target (куда рисуем)
 *   2. initShader        — компиляция и привязка DVLB-шейдера
 *   3. initVertexLayout  — описание формата вершины для GPU
 *   4. initVBO           — выделение вершинного буфера в linearAlloc
 *   5. initRenderState   — глобальные GPU-флаги (blend, cull, depth)
 *   6. initTextures      — выделение массивов текстур + белая заглушка
 */

// ─────────────────────────────────────────────────────────────────────────────
// Подфункции инициализации (не вызывать напрямую — используй C3DRenderer_init)
// ─────────────────────────────────────────────────────────────────────────────

typedef struct Citro3dRenderer Citro3dRenderer;

/** Инициализирует Citro3D и создаёт render target для top-экрана. */
void C3DRenderer_initRenderTarget(Citro3dRenderer *c3d);

/** Загружает DVLB-шейдер, инициализирует программу и получает uniform-локации. */
void C3DRenderer_initShader(Citro3dRenderer *c3d);

/**
 * Описывает формат вершины (AttrInfo) для GPU.
 * Должна вызываться после initShader, так как slot-нумерация
 * определяется объявлениями .in в шейдере.
 */
void C3DRenderer_initVertexLayout(Citro3dRenderer *c3d);

/**
 * Выделяет VBO в linearAlloc и регистрирует его в BufInfo.
 * linearAlloc — обязательное требование: только этот регион памяти
 * физически непрерывен и доступен DMA-контроллеру GPU.
 */
void C3DRenderer_initVBO(Citro3dRenderer *c3d);

/**
 * Настраивает глобальный render state:
 *  - Depth test: отключён (2D, порядок = порядок draw-вызовов)
 *  - Face culling: отключён (спрайты не имеют "задней" грани)
 *  - Alpha blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA (стандартная прозрачность)
 *  - TexEnv slot 0: TEXTURE0 * PRIMARY_COLOR (GPU_MODULATE)
 */
void C3DRenderer_initRenderState(Citro3dRenderer *c3d);

/**
 * Выделяет массивы метаданных текстур, создаёт белую заглушку
 * и сбрасывает состояние батча.
 */
void C3DRenderer_initTextures(Citro3dRenderer *c3d, DataWin *dataWin);

// ─────────────────────────────────────────────────────────────────────────────
// Публичные vtable-функции
// ─────────────────────────────────────────────────────────────────────────────

/** Полная инициализация рендерера. Вызывается один раз после создания. */
void C3DRenderer_init(Renderer *renderer, DataWin *dataWin);

/** Освобождает все GPU- и CPU-ресурсы рендерера. */
void C3DRenderer_destroy(Renderer *renderer);

/**
 * Начало кадра: инкремент счётчика, очистка буфера, сброс состояния батча.
 * Параметры gameW/H и windowW/H игнорируются (экран 3DS фиксирован).
 */
void C3DRenderer_beginFrame(Renderer *renderer,
                             int32_t gameW, int32_t gameH,
                             int32_t windowW, int32_t windowH,
                             int eye, float iod);

/** Конец кадра: сброс оставшегося батча и отправка кадра на экран. */
void C3DRenderer_endFrame(Renderer *renderer);

/**
 * Начало вьюпорта: настройка scissor/viewport и проекционной матрицы.
 *
 * Система координат после этого вызова:
 *  - (0, 0) = левый верхний угол view-области
 *  - X растёт вправо, Y растёт вниз
 *  - viewX/viewY задают смещение камеры ("скролл")
 */
void C3DRenderer_beginView(Renderer *renderer,
                            int32_t viewX, int32_t viewY,
                            int32_t viewW, int32_t viewH,
                            int32_t portX, int32_t portY,
                            int32_t portW, int32_t portH,
                            float   viewAngle);

/** Конец вьюпорта: сброс батча. */
void C3DRenderer_endView(Renderer *renderer);

/** Принудительный сброс батча (flush без смены кадра). */
void C3DRenderer_flush(Renderer *renderer);

#endif // __3DS__
