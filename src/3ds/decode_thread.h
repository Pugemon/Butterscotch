//
// Created by Pugemon on 28.03.2026.
//

#pragma once
#ifdef __3DS__

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// DecodeThread — асинхронный PNG-декодер на Core 1
//
// Проблема: stbi_load_from_memory для PNG 512×512 занимает ~10–15ms на ARM11.
// За это время игра фризит, потому что всё происходит на Core 0.
//
// Решение — разделить конвейер по ядрам:
//
//   Core 0  │ fread PNG bytes (~0.2ms)  │  C3D_TexInit + memcpy (~1ms)  │
//            └───────────────────────────┘  ↑ collect результат
//                                           │
//   Core 1                                  │ stbi_load + swizzle (~12ms) │
//
// Пока Core 1 декодирует, Core 0 рисует текстуру белой заглушкой (1–2 кадра).
// После сбора результата текстура появляется нормально.
//
// Почему разделение именно так:
//   - fread:        использует dw->file (FILE*), не thread-safe → только Core 0
//   - stbi:         чистые вычисления над malloc-буфером → безопасно на Core 1
//   - Morton swizzle: чистые вычисления → безопасно на Core 1
//   - C3D_TexInit:  вызывает linearAlloc (не thread-safe) → только Core 0
//   - memcpy в tex->data: пишем в linear heap → только Core 0
//   - GSPGPU_FlushDataCache: взаимодействует с GSP → только Core 0
//
// ─────────────────────────────────────────────────────────────────────────────

// Размер очереди декода. 4 слота хватает для одновременной подгрузки
// нескольких текстур при смене комнаты.
#define DECODE_QUEUE_SIZE 4

typedef enum {
    DECODE_JOB_IDLE    = 0,  ///< Слот свободен
    DECODE_JOB_PENDING = 1,  ///< Core 0 заполнил, Core 1 ещё не начал
    DECODE_JOB_RUNNING = 2,  ///< Core 1 работает
    DECODE_JOB_DONE    = 3,  ///< Core 1 завершил, Core 0 должен забрать
    DECODE_JOB_ERROR   = 4,  ///< Декод завершился ошибкой
} DecodeJobState;

typedef struct {
    volatile DecodeJobState state;

    int      pageId;    ///< Индекс текстуры в DataWin->txtr

    // Владение pngRaw: Core 0 → PENDING → Core 1 (освобождает после stbi)
    uint8_t *pngRaw;
    uint32_t pngSize;

    // Владение swizzled: Core 1 → DONE → Core 0 (освобождает после memcpy)
    u16     *swizzled;  ///< Morton-swizzled RGBA4, размер potW*potH*sizeof(u16)
    uint32_t potW;
    uint32_t potH;
    int      realW;     ///< Исходный размер (до POT-округления)
    int      realH;
} DecodeJob;

typedef struct {
    DecodeJob    jobs[DECODE_QUEUE_SIZE];
    LightEvent   jobAvailable;  ///< Core 0 → Core 1: появился новый job
    volatile bool shouldExit;
    Thread       handle;
} DecodeThread;

// ─────────────────────────────────────────────────────────────────────────────
// API (все функции вызываются только с Core 0)
// ─────────────────────────────────────────────────────────────────────────────

/// Создаёт и запускает тред декодирования на Core 1.
void DecodeThread_init(DecodeThread *dt);

/// Корректно останавливает тред и освобождает ресурсы.
void DecodeThread_destroy(DecodeThread *dt);

/// Возвращает true, если для pageId уже есть job в состоянии PENDING или RUNNING.
/// Используется для предотвращения двойного dispatch.
bool DecodeThread_isInFlight(const DecodeThread *dt, int pageId);

/// Отправляет задачу на декодирование.
/// Передаёт владение pngRaw — треду декодирования.
/// Возвращает false, если все слоты заняты (нужен синхронный fallback).
bool DecodeThread_dispatch(DecodeThread *dt, int pageId,
                            uint8_t *pngRaw, uint32_t pngSize);

/// Проверяет, готов ли результат для pageId.
/// Если да — заполняет out-параметры, освобождает слот и возвращает true.
/// Владение swizzled передаётся вызывающему (он должен free() его после memcpy).
bool DecodeThread_tryCollect(DecodeThread *dt, int pageId,
                              u16 **swizzled, uint32_t *potW, uint32_t *potH,
                              int *realW, int *realH);

#endif // __3DS__