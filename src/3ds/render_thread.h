#pragma once
#ifdef __3DS__

#include <3ds.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
// RenderThread — выполняет C3D_FrameEnd на Core 1
//
// Временная схема кадра (пример при 30fps, C3D_FrameEnd ≈ 8ms):
//
//  Core 0: ──[step N + draw N + flushBatch N]──▶signal──[step N+1 + draw N+1]──▶signal──
//  Core 1:                                      ◀wait──[C3D_FrameEnd N]──▶signal
//                                                       ◀wait──[C3D_FrameEnd N+1]──▶signal
//
// Пока Core 1 ждёт GPU/VBlank в C3D_FrameEnd, Core 0 уже считает следующий шаг.
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    LightEvent drawDone;      ///< Core 0 → Core 1: command list готов, можно FrameEnd
    LightEvent endFrameDone;  ///< Core 1 → Core 0: GPU flush завершён, Core 0 может beginFrame
    volatile bool shouldExit;
    Thread handle;
} RenderThread;

/// Инициализирует и запускает рендер-тред на Core 1.
void RenderThread_init(RenderThread* rt);

/// Core 0: ждёт завершения предыдущего C3D_FrameEnd перед beginFrame.
/// На первой итерации возвращается сразу (тред сигналит при старте).
void RenderThread_waitEndFrame(RenderThread* rt);

/// Core 0: сигналит Core 1 вызвать C3D_FrameEnd.
/// Вызывать после endFrame (т.е. после flushBatch).
void RenderThread_signalDraw(RenderThread* rt);

/// Останавливает тред корректно (дожидается текущего C3D_FrameEnd) и освобождает ресурсы.
void RenderThread_destroy(RenderThread* rt);

#endif // __3DS__
