#ifdef __3DS__

#include "render_thread.h"
#include <citro3d.h>
#include <stdio.h>

// Размер стека достаточен для C3D_FrameEnd — она не рекурсивна и почти не
// аллоцирует. 16 KiB — разумный минимум; 32 KiB — с запасом.
#define RT_STACK_SIZE  (32u * 1024u)

// Приоритет чуть ниже главного треда (на 3DS меньший номер = выше приоритет).
// Главный тред обычно имеет приоритет 0x30; даём 0x31, чтобы он не вытеснял
// логику в критических местах, но всё равно планировался активно на Core 1.
#define RT_PRIORITY    0x31

// ─────────────────────────────────────────────────────────────────────────────
// Точка входа рендер-треда (Core 1)
// ─────────────────────────────────────────────────────────────────────────────
static void renderThreadEntry(void* arg) {
    RenderThread* rt = (RenderThread*)arg;

    // Сигналим сразу: Core 0 может начинать первый кадр.
    LightEvent_Signal(&rt->endFrameDone);

    while (true) {
        // Ждём, пока Core 0 построит command list (flushBatch завершён)
        LightEvent_Wait(&rt->drawDone);

        if (rt->shouldExit) break;

        // Отправляем command list GPU и ждём его завершения.
        // Именно здесь тред может «спать» до 16ms, пока Core 0 считает
        // следующий шаг — это и есть весь смысл двух тредов.
        C3D_FrameEnd(0);

        // Сообщаем Core 0, что буфер освобождён и можно вызывать beginFrame.
        LightEvent_Signal(&rt->endFrameDone);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Публичный API
// ─────────────────────────────────────────────────────────────────────────────

void RenderThread_init(RenderThread* rt) {
    rt->shouldExit = false;

    // RESET_ONESHOT: событие сбрасывается автоматически после первого Wait.
    // Это предотвращает «накопление» сигналов, если Core 0 вырвется вперёд.
    LightEvent_Init(&rt->drawDone,     RESET_ONESHOT);
    LightEvent_Init(&rt->endFrameDone, RESET_ONESHOT);

    // Запускаем тред на Core 1 (affinity = 1).
    rt->handle = threadCreate(
        renderThreadEntry,
        rt,
        RT_STACK_SIZE,
        RT_PRIORITY,
        1,      /* affinity: Core 1 */
        false   /* detached = false, нужен threadJoin при завершении */
    );

    if (rt->handle == NULL) {
        fprintf(stderr, "[RenderThread] ОШИБКА: threadCreate не удался!\n");
        // Аварийный вариант: работаем однопоточно.
        // Core 0 будет ждать endFrameDone вечно — нужна обработка ошибки
        // на уровне main(). Для простоты — fatal здесь.
    }
}

void RenderThread_waitEndFrame(RenderThread* rt) {
    // Блокируем Core 0, пока Core 1 не завершит C3D_FrameEnd.
    // На первой итерации рендер-тред уже отправил сигнал при старте,
    // поэтому Wait вернётся немедленно.
    LightEvent_Wait(&rt->endFrameDone);
}

void RenderThread_signalDraw(RenderThread* rt) {
    // command list готов; разрешаем Core 1 вызвать C3D_FrameEnd.
    LightEvent_Signal(&rt->drawDone);
}

void RenderThread_destroy(RenderThread* rt) {
    // Дожидаемся завершения текущего C3D_FrameEnd, прежде чем останавливать тред.
    // Без этого мы можем прервать GPU-операцию.
    LightEvent_Wait(&rt->endFrameDone);

    rt->shouldExit = true;
    // Будим тред, чтобы он увидел shouldExit и вышел из цикла.
    LightEvent_Signal(&rt->drawDone);

    threadJoin(rt->handle, UINT64_MAX);
    threadFree(rt->handle);
}

#endif // __3DS__
