#ifdef __3DS__

#include "c3d_init.h"
#include "c3d_constants.h"
#include "c3d_batch.h"
#include "c3d_texture.h"
#include "c3d_texture_t3b.h"
#include "citro3d_renderer.h"

#include <3ds.h>
#include <citro3d.h>
#include <butterscotch_shader_shbin.h>
#include <stdio.h>
#include <stdlib.h>

// ─────────────────────────────────────────────────────────────────────────────
// Подфункции инициализации
// ─────────────────────────────────────────────────────────────────────────────

void C3DRenderer_initRenderTarget(Citro3dRenderer *c3d) {
    // C3D_Init выделяет командный буфер GPU указанного размера.
    // C3D_DEFAULT_CMDBUF_SIZE = 0x40000 байт — стандартный размер для большинства игр.
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    // Создаём render target — область VRAM, куда GPU будет писать пиксели.
    // Размеры в "памятной" системе координат (ширина и высота поменяны местами
    // из-за аппаратного поворота экрана на 90°).
    c3d->target = C3D_RenderTargetCreate(SCREEN_MEM_WIDTH, SCREEN_MEM_HEIGHT,
                                          GPU_RB_RGBA8,
                                          GPU_RB_DEPTH24_STENCIL8);
    if (!c3d->target) {
        fprintf(stderr, "[C3D] RenderTargetCreate failed!\n");
        return;
    }
    fprintf(stderr, "[C3D] Render target created: %p\n", (void *)c3d->target);

    // Привязываем render target к физическому top-экрану (GFX_TOP, GFX_LEFT).
    // Флаги transfer описывают, как GPU копирует данные из framebuffer в LCD:
    //   FLIP_VERT(0)     — не переворачивать по вертикали
    //   OUT_TILED(0)     — выходной буфер линейный (LCD ожидает линейный формат)
    //   RAW_COPY(0)      — выполнять конвертацию формата
    //   IN_FORMAT RGBA8  — входной формат нашего render target
    //   OUT_FORMAT RGB8  — LCD принимает RGB8 (без альфа-канала)
    //   SCALING NONE     — без масштабирования
    C3D_RenderTargetSetOutput(c3d->target, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0)   |
        GX_TRANSFER_OUT_TILED(0)   |
        GX_TRANSFER_RAW_COPY(0)    |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)  |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)  |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

void C3DRenderer_initShader(Citro3dRenderer *c3d) {
    // DVLB — скомпилированный бинарник шейдера (.shbin), линкуется прямо в executable.
    // butterscotch_shader_shbin и butterscotch_shader_shbin_size — символы из линкера.
    c3d->dvlb = DVLB_ParseFile((u32 *)butterscotch_shader_shbin,
                                butterscotch_shader_shbin_size);

    shaderProgramInit(&c3d->shader);
    // DVLE[0] — первый (и единственный) vertex shader в DVLB
    shaderProgramSetVsh(&c3d->shader, &c3d->dvlb->DVLE[0]);
    C3D_BindProgram(&c3d->shader);

    // Получаем числовые локации uniform-переменных шейдера по именам.
    // Эти локации используются при обновлении матриц каждый кадр.
    c3d->uLoc_projection = shaderInstanceGetUniformLocation(
        c3d->shader.vertexShader, "projMtx");

    fprintf(stderr, "[C3D] Shader loaded. projMtx=%d\n", c3d->uLoc_projection);
}

void C3DRenderer_initVertexLayout(Citro3dRenderer *c3d) {
    (void)c3d;  // AttrInfo — глобальный GPU-стейт, не привязан к структуре рендерера

    // AttrInfo описывает формат структуры C3D_Vertex для GPU:
    //   attrib 0: position  — 3 × float  (x, y, z)
    //   attrib 1: texcoord  — 2 × float  (u, v)
    //   attrib 2: color     — 4 × ubyte  (R, G, B, A упакованы в u32)
    //
    // Индексы аттрибутов должны совпадать с .in v0/v1/v2 в шейдере.
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT,         3);  // position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT,         2);  // UV
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  // color (4 байта = u32)
}

void C3DRenderer_initVBO(struct Citro3dRenderer *c3d) {
    // linearAlloc выделяет память в физически непрерывном регионе (Linear Heap),
    // который доступен DMA-контроллеру GPU без копирования.
    // Обычный malloc() возвращает виртуальный адрес; GPU его не видит.
    c3d->vboData = (C3D_Vertex *)linearAlloc(sizeof(C3D_Vertex) * MAX_VERTICES);

    // BufInfo регистрирует VBO в GPU:
    //   - указатель на буфер
    //   - шаг (stride) — sizeof(C3D_Vertex) байт между вершинами
    //   - количество аттрибутов (VERTEX_ATTR_COUNT)
    //   - permutation — маппинг слотов шейдера на аттрибуты (VERTEX_ATTR_PERMUTATION)
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, c3d->vboData, sizeof(C3D_Vertex),
                VERTEX_ATTR_COUNT, VERTEX_ATTR_PERMUTATION);
}

void C3DRenderer_initRenderState(Citro3dRenderer *c3d) {
    (void)c3d;

    // Глубина: отключена для 2D. Порядок спрайтов = порядок draw-вызовов.
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

    // Отсечение граней: отключено. У спрайтов нет "задней" грани.
    C3D_CullFace(GPU_CULL_NONE);

    // Альфа-блендинг: стандартный режим "over" (premultiplied alpha не используется).
    // Формула: out = src.rgb * src.a + dst.rgb * (1 - src.a)
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    // TexEnv slot 0: finalColor = TEXTURE0 * PRIMARY_COLOR (GPU_MODULATE).
    // PRIMARY_COLOR — это interpolated vertex color (то, что мы пишем в C3D_Vertex.color).
    // Умножение цвета вершины на цвет текстуры позволяет тинтовать спрайты
    // без дополнительных шейдерных uniform-переменных.
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

void C3DRenderer_initTextures(Citro3dRenderer *c3d, DataWin *dataWin) {
    uint32_t texCount = dataWin->txtr.count;
    c3d->texCount     = texCount;
    c3d->currentFrame = 0;

    // +1 для белой заглушки, которая хранится сразу после игровых текстур.
    // safeCalloc обнуляет память — все tex.data == NULL (= не загружена).
    c3d->textures    = (C3D_Tex  *)safeCalloc(texCount + 1, sizeof(C3D_Tex));
    c3d->texRealW    = (int      *)safeCalloc(texCount + 1, sizeof(int));
    c3d->texRealH    = (int      *)safeCalloc(texCount + 1, sizeof(int));
    c3d->texLastUsed = (uint32_t *)safeCalloc(texCount + 1, sizeof(uint32_t));

    // Белая текстура — всегда загружена, занимает слот сразу за игровыми
    c3d->whiteTexIndex = (int)texCount;
    if (!createWhiteTexture(&c3d->textures[c3d->whiteTexIndex])) {
        fprintf(stderr, "[C3D] Failed to create white texture!\n");
    }
    c3d->texRealW[c3d->whiteTexIndex] = WHITE_TEX_SIZE;
    c3d->texRealH[c3d->whiteTexIndex] = WHITE_TEX_SIZE;

    // Начальное состояние батча
    c3d->vertexCount     = 0;
    c3d->batchStart      = 0;
    c3d->currentTexIndex = -1;

    c3d->archiveFile    = NULL;
    c3d->archiveOffsets = NULL;
    c3d->vramUsed       = 0;

    // Путь к .t3b бандлу строим рядом с data.win: basePath уже содержит
    // директорию с data.win (включая trailing slash), добавляем имя файла.
    char archivePath[512];
    snprintf(archivePath, sizeof(archivePath), "%sundertale.t3b", c3d->basePath);
    TexArchive_open(c3d, archivePath);

    fprintf(stderr, "[C3D] Init done: %u game textures + 1 white stub.\n", texCount);
    c3d->decodeThread = (DecodeThread *)malloc(sizeof(DecodeThread));
    if (c3d->decodeThread) {
        DecodeThread_init(c3d->decodeThread);
    }

}

// ─────────────────────────────────────────────────────────────────────────────
// Публичные vtable-функции
// ─────────────────────────────────────────────────────────────────────────────

void C3DRenderer_init(Renderer *renderer, DataWin *dataWin) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    renderer->dataWin = dataWin;

    // gfxInitDefault() уже вызван в main() до создания рендерера.
    // Порядок подфункций важен — каждая следующая зависит от предыдущей.
    C3DRenderer_initRenderTarget(c3d);
    C3DRenderer_initShader(c3d);
    C3DRenderer_initVertexLayout(c3d);
    C3DRenderer_initVBO(c3d);
    C3DRenderer_initRenderState(c3d);
    C3DRenderer_initTextures(c3d, dataWin);
}

void C3DRenderer_destroy(Renderer *renderer) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;

    TexArchive_close(c3d);

    // Сначала останавливаем декод-тред
    // Важно: до C3D_TexDelete, иначе тред может обращаться к уже
    // освобождённым структурам DataWin.
    if (c3d->decodeThread) {
        DecodeThread_destroy(c3d->decodeThread);
        free(c3d->decodeThread);
        c3d->decodeThread = NULL;
    }

    // Удаляем все загруженные текстуры (включая белую заглушку = texCount-й слот)
    for (uint32_t i = 0; i <= c3d->texCount; i++) {
        if (c3d->textures[i].data) {
            C3D_TexDelete(&c3d->textures[i]);
        }
    }
    free(c3d->textures);
    free(c3d->texRealW);
    free(c3d->texRealH);
    free(c3d->texLastUsed);

    linearFree(c3d->vboData);  // VBO выделялся через linearAlloc
    shaderProgramFree(&c3d->shader);
    DVLB_Free(c3d->dvlb);
    C3D_RenderTargetDelete(c3d->target);

    // C3D_Fini() и gfxExit() вызываются в main() — не здесь
    free(c3d);
}

void C3DRenderer_beginFrame(Renderer *renderer,
                             int32_t gameW, int32_t gameH,
                             int32_t windowW, int32_t windowH)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    (void)gameW; (void)gameH; (void)windowW; (void)windowH;

    c3d->currentFrame++;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(c3d->target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(c3d->target);

    // Сбрасываем состояние батча в начале каждого кадра
    c3d->vertexCount     = 0;
    c3d->batchStart      = 0;
    c3d->currentTexIndex = -1;
}

void C3DRenderer_endFrame(Renderer *renderer) {
    flushBatch((Citro3dRenderer *)renderer);
    // C3D_FrameEnd автоматически сбрасывает весь linear heap через
    // GSPGPU_FlushDataCache(__ctru_linear_heap, __ctru_linear_heap_size),
    // поэтому отдельный flush VBO здесь не нужен.

    // НЕ вызываем C3D_FrameEnd здесь.
    // C3D_FrameEnd теперь вызывается рендер-тредом (Core 1) через RenderThread_signalDraw().
    // Это позволяет Core 0 немедленно перейти к Runner_step следующего кадра,
    // пока Core 1 ожидает GPU/VBlank.
    // C3D_FrameEnd(0);
}

void C3DRenderer_beginView(Renderer *renderer,
                            int32_t viewX, int32_t viewY,
                            int32_t viewW, int32_t viewH,
                            int32_t portX, int32_t portY,
                            int32_t portW, int32_t portH,
                            float   viewAngle)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    flushBatch(c3d);
    (void)viewAngle;  // TODO: поворот камеры не реализован

    // 3DS: экран повёрнут на 90° аппаратно.
    // C3D_SetViewport принимает координаты в "памятной" системе (MEM):
    //   x → portY (т.к. физический X = памятный Y)
    //   y → SCREEN_MEM_HEIGHT - portX - portW (инверсия из-за поворота)
    //   w → portH
    //   h → portW
    C3D_SetViewport((u32)portY,
                    (u32)(SCREEN_MEM_HEIGHT - portX - portW),
                    (u32)portH,
                    (u32)portW);

    // Ортографическая проекция: (0,0) в левом верхнем углу, Y растёт вниз.
    // Mtx_OrthoTilt учитывает поворот экрана на 90° автоматически.
    // Диапазон глубины [1, -1] (reversed) — стандарт Citro3D.
    Mtx_OrthoTilt(&c3d->projection,
                  0.0f, (float)viewW, (float)viewH, 0.0f,
                  1.0f, -1.0f, true);

    // Применяем смещение камеры: сдвигаем всю сцену на -(viewX, viewY).
    // Это эквивалентно "скроллингу": если камера смотрит на viewX=100, viewY=0,
    // весь мир сдвигается на -100 по X.
    Mtx_Translate(&c3d->projection, -(float)viewX, -(float)viewY, 0.0f, true);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, c3d->uLoc_projection, &c3d->projection);
}

void C3DRenderer_endView(Renderer *renderer) {
    flushBatch((Citro3dRenderer *)renderer);
}

void C3DRenderer_flush(Renderer *renderer) {
    flushBatch((Citro3dRenderer *)renderer);
}

#endif // __3DS__
