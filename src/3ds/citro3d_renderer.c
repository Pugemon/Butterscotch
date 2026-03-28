#ifdef __3DS__

#include "citro3d_renderer.h"
#include "text_utils.h"

#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "stb_image.h"
#include "binary_reader.h"
#include <butterscotch_shader_shbin.h>

// ─────────────────────────────────────────────
// Утилиты
// ─────────────────────────────────────────────

// Следующая степень двойки, минимум 8 (требование PICA200)
static u32 nextPOT(u32 n) {
    if (n < 8) return 8;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
    return n + 1;
}

// BGR цвет движка + alpha → ABGR для PICA200
static u32 colorToABGR(uint32_t bgr, float alpha) {
    u8 r = (bgr)       & 0xFF;
    u8 g = (bgr >> 8)  & 0xFF;
    u8 b = (bgr >> 16) & 0xFF;
    u8 a = (u8)(alpha * 255.0f);
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | r;
}

// Правильный расчет смещения для текстур PICA200 (полный Morton-свиззл)
static inline u32 get_pica_swizzle_offset(u32 x, u32 y, u32 potW, u32 potH) {
    u32 offset = 0;
    u32 shift = 0;
    u32 mask = 1;

    // Переплетаем биты X и Y. Если одна сторона больше другой,
    // её оставшиеся биты просто дописываются в старшие разряды.
    while (mask < potW || mask < potH) {
        if (mask < potW) {
            if (x & mask) offset |= (1 << shift);
            shift++;
        }
        if (mask < potH) {
            if (y & mask) offset |= (1 << shift);
            shift++;
        }
        mask <<= 1;
    }
    return offset;
}

// Morton (Z-order) свиззл: линейный ABGR-буфер → тайловый формат PICA200.
static void swizzleToTex(C3D_Tex *tex, const u32 *src, u32 srcW, u32 srcH) {
    u32 *dst = (u32 *)tex->data;
    u32 potW = tex->width;
    u32 potH = tex->height;
    u32 tilesX = potW >> 3;

    memset(dst, 0, potW * potH * 4);

    for (u32 y = 0; y < srcH; y++) {
        for (u32 x = 0; x < srcW; x++) {
            u32 tileX  = x >> 3;
            u32 tileY  = y >> 3;
            u32 localX = x & 7;
            u32 localY = y & 7;

            // ИСПРАВЛЕННЫЙ Morton Z-order
            u32 localOff = (localX & 1)        |
                           ((localY & 1) << 1) |
                           ((localX & 2) << 1) |
                           ((localY & 2) << 2) |
                           ((localX & 4) << 2) |
                           ((localY & 4) << 3);

            u32 tileOff = (tileY * tilesX + tileX) << 6;

            dst[tileOff + localOff] = src[y * srcW + x];
        }
    }

    GSPGPU_FlushDataCache(tex->data, tex->size);
}

static inline void rotatePoint(float x, float y,
                                float cs, float sn,
                                float *ox, float *oy) {
    *ox = cs * x + sn * y;
    *oy = -sn * x + cs * y;
}

// ─────────────────────────────────────────────
// Батч
// ─────────────────────────────────────────────

static void flushBatch(Citro3dRenderer *c3d) {
    if (c3d->vertexCount == c3d->batchStart) return;

    int count = c3d->vertexCount - c3d->batchStart;

    // 1. КРИТИЧЕСКИ ВАЖНО: Принудительно выгружаем данные вершин из CPU в RAM!
    GSPGPU_FlushDataCache(&c3d->vboData[c3d->batchStart], count * sizeof(C3D_Vertex));

    C3D_Tex *texToBind = NULL;
    if (c3d->currentTexIndex >= 0) {
        if (c3d->textures[c3d->currentTexIndex].data != NULL) {
            texToBind = &c3d->textures[c3d->currentTexIndex];
        } else {
            texToBind = &c3d->textures[c3d->whiteTexIndex];
        }
    }

    C3D_TexBind(0, texToBind);
    C3D_DrawArrays(GPU_TRIANGLES, c3d->batchStart, count);

    // Сдвигаем начало для следующего батча
    c3d->batchStart = c3d->vertexCount;
}

static void checkBatch(Citro3dRenderer *c3d, int verts, int texIdx) {
    if (c3d->currentTexIndex != texIdx || c3d->vertexCount + verts > MAX_VERTICES) {
        flushBatch(c3d);
        c3d->currentTexIndex = texIdx;

        if (c3d->vertexCount + verts > MAX_VERTICES) {
            c3d->vertexCount = 0;
            c3d->batchStart = 0;
        }
    }
}

// ─────────────────────────────────────────────
// Загрузка одной текстуры
// ─────────────────────────────────────────────

static bool uploadTexture(C3D_Tex *tex, int *realW, int *realH, const u8 *rgba, int w, int h) {
    u32 potW = nextPOT((u32)w);
    u32 potH = nextPOT((u32)h);

    if (!C3D_TexInit(tex, (u16)potW, (u16)potH, GPU_RGBA4)) {
        fprintf(stderr, "[TEX] C3D_TexInit failed\n");
        return false;
    }

    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    u16 *dst = (u16 *)tex->data;
    u32 tilesX = potW >> 3;

    memset(dst, 0, potW * potH * 2);

    for (u32 y = 0; y < (u32)h; y++) {
        for (u32 x = 0; x < (u32)w; x++) {
            u32 tileX  = x >> 3;
            u32 tileY  = y >> 3;
            u32 localX = x & 7;
            u32 localY = y & 7;

            // ИСПРАВЛЕННЫЙ Morton Z-order (X на четных битах, Y на нечетных)
            u32 localOff = (localX & 1)        |
                           ((localY & 1) << 1) |
                           ((localX & 2) << 1) |
                           ((localY & 2) << 2) |
                           ((localX & 4) << 2) |
                           ((localY & 4) << 3);

            u32 tileOff = (tileY * tilesX + tileX) << 6;

            u32 srcOff = (y * w + x) * 4;
            u8 r = rgba[srcOff + 0];
            u8 g = rgba[srcOff + 1];
            u8 b = rgba[srcOff + 2];
            u8 a = rgba[srcOff + 3];

            // Формат цвета GPU_RGBA4
            u16 color16 = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
            dst[tileOff + localOff] = color16;
        }
    }

    GSPGPU_FlushDataCache(tex->data, tex->size);

    *realW = w;
    *realH = h;
    return true;
}

// ─────────────────────────────────────────────
// Белая текстура (для примитивов без текстуры)
// ─────────────────────────────────────────────

static bool createWhiteTexture(C3D_Tex *tex) {
    if (!C3D_TexInit(tex, 8, 8, GPU_RGBA8)) return false;
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    u32 *tmp = (u32 *)linearAlloc(8 * 8 * 4);
    if (!tmp) { C3D_TexDelete(tex); return false; }

    for (int i = 0; i < 64; i++) tmp[i] = 0xFFFFFFFFu; // ABGR белый

    swizzleToTex(tex, tmp, 8, 8);
    linearFree(tmp);
    return true;
}

// ─────────────────────────────────────────────
// init / destroy
// ─────────────────────────────────────────────

static void C3DRenderer_init(Renderer *renderer, DataWin *dataWin) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    renderer->dataWin = dataWin;

    // gfxInitDefault() уже вызван в main() до создания рендерера.
    // Здесь только инициализируем C3D создаём render target.

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);


    c3d->target = C3D_RenderTargetCreate(240, 400,
                                         GPU_RB_RGBA8,
                                         GPU_RB_DEPTH24_STENCIL8);
    if (!c3d->target) {
        fprintf(stderr, "[C3D] RenderTargetCreate failed!\n");
        return;
    }
    fprintf(stderr, "[C3D] target = %p\n", (void *)c3d->target);

    C3D_RenderTargetSetOutput(c3d->target, GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_RAW_COPY(0)  |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    // ── Шейдер ──────────────────────────────────────────────────────────
    c3d->dvlb = DVLB_ParseFile((u32 *)butterscotch_shader_shbin,
                                butterscotch_shader_shbin_size);
    shaderProgramInit(&c3d->shader);
    shaderProgramSetVsh(&c3d->shader, &c3d->dvlb->DVLE[0]);
    C3D_BindProgram(&c3d->shader);

    c3d->uLoc_projection = shaderInstanceGetUniformLocation(
        c3d->shader.vertexShader, "projMtx");
    c3d->uLoc_modelview = shaderInstanceGetUniformLocation(
        c3d->shader.vertexShader, "mdlvMtx");

    // ── Атрибуты вершин ─────────────────────────────────────────────────
    // Структура C3D_Vertex: [float x,y,z] [float u,v] [u32 color]
    //   attrib 0: position  — 3 float
    //   attrib 1: texcoord  — 2 float
    //   attrib 2: color     — 4 unsigned byte (= u32)
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT,         3); // position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT,         2); // UV
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); // color (4 байта)

    // ── VBO ─────────────────────────────────────────────────────────────
    c3d->vboData = (C3D_Vertex *)linearAlloc(sizeof(C3D_Vertex) * MAX_VERTICES);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    // 3 атрибута, permutation 0x210 (attrib2=slot2, attrib1=slot1, attrib0=slot0)
    BufInfo_Add(buf, c3d->vboData, sizeof(C3D_Vertex), 3, 0x210);

    // ── Render state ─────────────────────────────────────────────────────
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    // ── Текстуры ─────────────────────────────────────────────────────────
    uint32_t texCount = dataWin->txtr.count;
    c3d->texCount = texCount;

    // Выделяем память под структуру (calloc сам забьет всё нулями) +1 для текстуры заглушки
    c3d->textures = (C3D_Tex *)safeCalloc(texCount + 1, sizeof(C3D_Tex));
    c3d->texRealW = (int *)safeCalloc(texCount + 1, sizeof(int));
    c3d->texRealH = (int *)safeCalloc(texCount + 1, sizeof(int));
    c3d->texLastUsed = (uint32_t *)safeCalloc(texCount + 1, sizeof(uint32_t));
    c3d->currentFrame = 0;




    // Белая текстура для примитивов
    c3d->whiteTexIndex = (int)texCount;
    if (!createWhiteTexture(&c3d->textures[c3d->whiteTexIndex])) {
        fprintf(stderr, "[C3D] white texture failed\n");
    }
    c3d->texRealW[c3d->whiteTexIndex] = 8;
    c3d->texRealH[c3d->whiteTexIndex] = 8;

    // ── Состояние батча ──────────────────────────────────────────────────
    c3d->vertexCount    = 0;
    c3d->currentTexIndex = -1;

    fprintf(stderr, "[C3D] init done, %u textures\n", texCount);
}

static void C3DRenderer_destroy(Renderer *renderer) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;

    for (uint32_t i = 0; i <= c3d->texCount; i++) {
        if (c3d->textures[i].data)
            C3D_TexDelete(&c3d->textures[i]);
    }
    free(c3d->textures);
    free(c3d->texRealW);
    free(c3d->texRealH);

    linearFree(c3d->vboData);
    shaderProgramFree(&c3d->shader);
    DVLB_Free(c3d->dvlb);
    C3D_RenderTargetDelete(c3d->target);

    // C3D_Fini() и gfxExit() вызываются в main(), не здесь
    free(c3d);
}

// ─────────────────────────────────────────────
// Frame
// ─────────────────────────────────────────────

static void C3DRenderer_beginFrame(Renderer *renderer,
                                   int32_t gameW, int32_t gameH,
                                   int32_t windowW, int32_t windowH)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    c3d->currentFrame++;
    (void)gameW; (void)gameH; (void)windowW; (void)windowH;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(c3d->target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(c3d->target);

    c3d->vertexCount     = 0;
    c3d->batchStart      = 0;
    c3d->currentTexIndex = -1;
}

static void C3DRenderer_endFrame(Renderer *renderer) {
    flushBatch((Citro3dRenderer *)renderer);
    // C3D_FrameEnd(0) автоматически сбрасывает весь linear heap через
    // GSPGPU_FlushDataCache(__ctru_linear_heap, __ctru_linear_heap_size)
    C3D_FrameEnd(0);
}

// ─────────────────────────────────────────────
// View
// ─────────────────────────────────────────────

static void C3DRenderer_beginView(Renderer *renderer,
                                  int32_t viewX, int32_t viewY,
                                  int32_t viewW, int32_t viewH,
                                  int32_t portX, int32_t portY,
                                  int32_t portW, int32_t portH,
                                  float   viewAngle)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    flushBatch(c3d);
    (void)viewAngle; // Упростим и пока уберем поворот камеры

    // 3DS: физический экран повёрнут — ширина 400, высота 240,
    // но в памяти: width=240, height=400.
    // C3D_SetViewport(x, y, w, h) — в физических пикселях буфера (240×400).
    C3D_SetViewport((u32)portY,
                    (u32)(400 - portX - portW),
                    (u32)portH,
                    (u32)portW);

    // СОЗДАЕМ ПРОЕКЦИЮ В СТИЛЕ citro2d:
    // Мы создаем "виртуальный экран" размером с view (например, 320x240).
    // Координаты (0,0) будут в левом верхнем углу этого "экрана".
    // А сдвиг камеры (viewX, viewY) мы применим к матрице позже.
    Mtx_OrthoTilt(&c3d->projection, 0.0f, (float)viewW, (float)viewH, 0.0f, 1.0f, -1.0f, true);

    // Теперь сдвигаем всю сцену на -viewX, -viewY, чтобы "камера" смотрела на нужную область
    Mtx_Translate(&c3d->projection, -(float)viewX, -(float)viewY, 0.0f, true);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, c3d->uLoc_projection, &c3d->projection);
}

static void C3DRenderer_endView(Renderer *renderer) {
    flushBatch((Citro3dRenderer *)renderer);
}

static void C3DRenderer_flush(Renderer *renderer) {
    flushBatch((Citro3dRenderer *)renderer);
}



static void ensureTextureLoaded(Citro3dRenderer *c3d, int pageId) {
    if (pageId < 0 || pageId >= (int)c3d->texCount) return;

    // Обновляем время использования
    c3d->texLastUsed[pageId] = c3d->currentFrame;

    // Если текстура уже загружена (есть данные) — ничего не делаем
    if (c3d->textures[pageId].data != NULL) return;

    fprintf(stderr, "==> Frame %u: Requesting texture %d...\n", c3d->currentFrame, pageId);

    // 1. Проверяем лимит кэша
    int loadedCount = 0;
    for (uint32_t i = 0; i < c3d->texCount; i++) {
        if (c3d->textures[i].data != NULL && i != c3d->whiteTexIndex) {
            loadedCount++;
        }
    }

    // 2. Если лимит превышен, удаляем самую старую
    if (loadedCount >= MAX_CACHED_TEXTURES) {
        int oldestId = -1;
        uint32_t oldestTime = 0xFFFFFFFF;
        for (uint32_t i = 0; i < c3d->texCount; i++) {
            if (c3d->textures[i].data != NULL && i != c3d->whiteTexIndex && c3d->texLastUsed[i] < oldestTime) {
                oldestTime = c3d->texLastUsed[i];
                oldestId = i;
            }
        }
        if (oldestId != -1) {
            fprintf(stderr, "[C3D] Cache full. Evicting texture %d (last used on frame %u).\n", oldestId, oldestTime);
            // КРИТИЧЕСКИ ВАЖНО: сбросить батч перед удалением текстуры!
            flushBatch(c3d);
            C3D_TexDelete(&c3d->textures[oldestId]);
            c3d->textures[oldestId].data = NULL; // Помечаем как выгруженную
        }
    }

    // 3. Загружаем новую текстуру
    DataWin *dw = c3d->base.dataWin;
    Texture *txtr = &dw->txtr.textures[pageId];
    if (txtr->blobSize == 0) {
        fprintf(stderr, "[C3D] Texture %d has blobSize == 0.\n", pageId);
        return;
    }

    // Читаем бинарник из файла
    BinaryReader reader = BinaryReader_create(dw->file, dw->fileSize);
    uint8_t *raw = BinaryReader_readBytesAt(&reader, txtr->blobOffset, txtr->blobSize);
    if (!raw) {
        fprintf(stderr, "[C3D] Could not read blob for texture %d.\n", pageId);
        return;
    }

    int w, h, ch;
    stbi_set_flip_vertically_on_load(1); // TODO: Есть вариант переворачивать текстуру шейдером, или где то ещё, не забыть убрать при переходе на tex3d (формат для 3ds)
    uint8_t *px = stbi_load_from_memory(raw, (int)txtr->blobSize, &w, &h, &ch, 4);
    free(raw);

    if (!px) {
        fprintf(stderr, "    [FAIL] stbi_load_from_memory failed for texture %d.\n", pageId);
        return;
    }
    if (uploadTexture(&c3d->textures[pageId], &c3d->texRealW[pageId], &c3d->texRealH[pageId], px, w, h)) {
        fprintf(stderr, "    [SUCCESS] Texture %d uploaded.\n", pageId);
    } else {
        // uploadTexture уже пишет ошибку, но мы добавим для ясности
        fprintf(stderr, "    [FAIL] uploadTexture failed for texture %d.\n", pageId);
        // Важно: если C3D_TexInit провалился, tex.data будет NULL, и защита сработает.
    }

    stbi_image_free(px);
}

// ─────────────────────────────────────────────
// UV-хелпер: реальный размер пикселей / POT-размер текстуры
// ─────────────────────────────────────────────

// UV в citro3d с ручным свиззлом и Y-инверсией:
//   u = srcX / potW          (X не инвертируется)
//   v = 1.0 - srcY / potH   (Y инвертируется из-за OpenGL-конвенции свиззла)
// где potW = tex->width, potH = tex->height

static inline void calcUV(const C3D_Tex *tex,
                           float srcX, float srcY,
                           float srcW, float srcH,
                           float *u0, float *v0,
                           float *u1, float *v1)
{
    if (tex->width == 0 || tex->height == 0) {
        *u0 = *v0 = *u1 = *v1 = 0.0f;
        return;
    }

    float potW = (float)tex->width;
    float potH = (float)tex->height;

    *u0 = srcX / potW;
    *u1 = (srcX + srcW) / potW;
    *v0 = srcY / potH;
    *v1 = (srcY + srcH) / potH;
}

// ─────────────────────────────────────────────
// Draw calls
// ─────────────────────────────────────────────

static void C3DRenderer_drawSprite(Renderer *renderer,
                                   int32_t tpagIndex,
                                   float x, float y,
                                   float originX, float originY,
                                   float xscale, float yscale,
                                   float angleDeg,
                                   uint32_t color, float alpha)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    if (tpagIndex < 0 || tpagIndex >= (int)renderer->dataWin->tpag.count) return;

    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];
    int pid = tpag->texturePageId;
    ensureTextureLoaded(c3d, pid);
    checkBatch(c3d, 6, pid);

    C3D_Tex *tex = &c3d->textures[pid];
    float u0, v0, u1, v1;
    calcUV(tex,
           (float)tpag->sourceX, (float)tpag->sourceY,
           (float)tpag->sourceWidth, (float)tpag->sourceHeight,
           &u0, &v0, &u1, &v1);

    float rad = angleDeg * (float)(M_PI / 180.0);
    float cs = cosf(rad), sn = sinf(rad);

    float lx0 = -originX * xscale,                     ly0 = -originY * yscale;
    float lx1 = (tpag->sourceWidth  - originX) * xscale;
    float ly1 = (tpag->sourceHeight - originY) * yscale;

    float rx0, ry0, rx1, ry1, rx2, ry2, rx3, ry3;
    rotatePoint(lx0, ly0, cs, sn, &rx0, &ry0);
    rotatePoint(lx1, ly0, cs, sn, &rx1, &ry1);
    rotatePoint(lx0, ly1, cs, sn, &rx2, &ry2);
    rotatePoint(lx1, ly1, cs, sn, &rx3, &ry3);

    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x+rx0, y+ry0, 0.5f, u0, v0, clr};
    v[1] = (C3D_Vertex){x+rx1, y+ry1, 0.5f, u1, v0, clr};
    v[2] = (C3D_Vertex){x+rx2, y+ry2, 0.5f, u0, v1, clr};
    v[3] = (C3D_Vertex){x+rx1, y+ry1, 0.5f, u1, v0, clr};
    v[4] = (C3D_Vertex){x+rx3, y+ry3, 0.5f, u1, v1, clr};
    v[5] = (C3D_Vertex){x+rx2, y+ry2, 0.5f, u0, v1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawSpritePart(Renderer *renderer,
                                       int32_t tpagIndex,
                                       int32_t srcOffX, int32_t srcOffY,
                                       int32_t srcW, int32_t srcH,
                                       float x, float y,
                                       float xscale, float yscale,
                                       uint32_t color, float alpha)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    if (tpagIndex < 0 || tpagIndex >= (int)renderer->dataWin->tpag.count) return;

    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];
    int pid = tpag->texturePageId;
    ensureTextureLoaded(c3d, pid);
    checkBatch(c3d, 6, pid);

    C3D_Tex *tex = &c3d->textures[pid];
    float u0, v0, u1, v1;
    calcUV(tex,
           (float)(tpag->sourceX + srcOffX), (float)(tpag->sourceY + srcOffY),
           (float)srcW, (float)srcH,
           &u0, &v0, &u1, &v1);

    float x2 = x + srcW * xscale;
    float y2 = y + srcH * yscale;
    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x,  y,  0.5f, u0, v0, clr};
    v[1] = (C3D_Vertex){x2, y,  0.5f, u1, v0, clr};
    v[2] = (C3D_Vertex){x,  y2, 0.5f, u0, v1, clr};
    v[3] = (C3D_Vertex){x2, y,  0.5f, u1, v0, clr};
    v[4] = (C3D_Vertex){x2, y2, 0.5f, u1, v1, clr};
    v[5] = (C3D_Vertex){x,  y2, 0.5f, u0, v1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawRectangle(Renderer *renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;

    if (outline) {
        renderer->vtable->drawLine(renderer, x1, y1, x2, y1, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x2, y1, x2, y2, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x2, y2, x1, y2, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x1, y2, x1, y1, 1.0f, color, alpha);
        return;
    }

    checkBatch(c3d, 6, c3d->whiteTexIndex);
    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    // Белая текстура берется из левого верхнего пикселя (0,0)
    v[0] = (C3D_Vertex){x1, y1, 0.5f, 0, 0, clr};
    v[1] = (C3D_Vertex){x2, y1, 0.5f, 1, 0, clr};
    v[2] = (C3D_Vertex){x1, y2, 0.5f, 0, 1, clr};
    v[3] = (C3D_Vertex){x2, y1, 0.5f, 1, 0, clr};
    v[4] = (C3D_Vertex){x2, y2, 0.5f, 1, 1, clr};
    v[5] = (C3D_Vertex){x1, y2, 0.5f, 0, 1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawLine(Renderer *renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return;

    float nx = -(dy / len) * (width * 0.5f);
    float ny =  (dx / len) * (width * 0.5f);

    checkBatch(c3d, 6, c3d->whiteTexIndex);
    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x1-nx, y1-ny, 0.5f, 0, 0, clr};
    v[1] = (C3D_Vertex){x1+nx, y1+ny, 0.5f, 1, 0, clr};
    v[2] = (C3D_Vertex){x2-nx, y2-ny, 0.5f, 0, 1, clr};
    v[3] = (C3D_Vertex){x1+nx, y1+ny, 0.5f, 1, 0, clr};
    v[4] = (C3D_Vertex){x2+nx, y2+ny, 0.5f, 1, 1, clr};
    v[5] = (C3D_Vertex){x2-nx, y2-ny, 0.5f, 0, 1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawLineColor(Renderer *renderer,
                                      float x1, float y1,
                                      float x2, float y2,
                                      float width,
                                      uint32_t color1, uint32_t color2,
                                      float alpha)
{
    // Упрощённо: используем первый цвет
    (void)color2;
    C3DRenderer_drawLine(renderer, x1, y1, x2, y2, width, color1, alpha);
}

static void C3DRenderer_drawText(Renderer *renderer,
                                 const char *text,
                                 float x, float y,
                                 float xscale, float yscale,
                                 float angleDeg)
{
    (void)angleDeg;
    DataWin *dw = renderer->dataWin;
    int32_t fontIdx = renderer->drawFont;
    if (fontIdx < 0 || fontIdx >= (int)dw->font.count) return;

    Font *font = &dw->font.fonts[fontIdx];
    int32_t pos = 0, len = (int32_t)strlen(text);
    float curX = x, curY = y;

    while (pos < len) {
        if (text[pos] == '\n') {
            curX = x;
            curY += font->emSize * yscale;
            pos++;
            continue;
        }

        uint16_t ch = TextUtils_decodeUtf8(text, len, &pos);
        FontGlyph *g = TextUtils_findGlyph(font, ch);
        if (!g) continue;

        int32_t ti = DataWin_resolveTPAG(dw, font->textureOffset);
        if (ti >= 0 && g->sourceWidth > 0 && g->sourceHeight > 0) {
            C3DRenderer_drawSpritePart(renderer, ti,
                                       g->sourceX, g->sourceY,
                                       g->sourceWidth, g->sourceHeight,
                                       curX + g->offset * xscale, curY,
                                       xscale, yscale,
                                       renderer->drawColor, renderer->drawAlpha);
        }
        curX += g->shift * xscale;

        if (pos < len) {
            int32_t savedPos = pos;
            uint16_t nextCh = TextUtils_decodeUtf8(text, len, &pos);
            pos = savedPos;
            curX += TextUtils_getKerningOffset(g, nextCh) * xscale;
        }
    }
}

// ─────────────────────────────────────────────
// VTable и конструктор
// ─────────────────────────────────────────────

static RendererVtable c3d_vtable = {
    .init                  = C3DRenderer_init,
    .destroy               = C3DRenderer_destroy,
    .beginFrame            = C3DRenderer_beginFrame,
    .endFrame              = C3DRenderer_endFrame,
    .beginView             = C3DRenderer_beginView,
    .endView               = C3DRenderer_endView,
    .drawSprite            = C3DRenderer_drawSprite,
    .drawSpritePart        = C3DRenderer_drawSpritePart,
    .drawRectangle         = C3DRenderer_drawRectangle,
    .drawLine              = C3DRenderer_drawLine,
    .drawLineColor         = C3DRenderer_drawLineColor,
    .drawText              = C3DRenderer_drawText,
    .flush                 = C3DRenderer_flush,
    .createSpriteFromSurface = NULL,
    .deleteSprite          = NULL,
    .drawTile              = NULL,
};

Renderer *Citro3dRenderer_create(void) {
    Citro3dRenderer *c3d = (Citro3dRenderer *)safeCalloc(1, sizeof(Citro3dRenderer));
    c3d->base.vtable     = &c3d_vtable;
    c3d->base.drawColor  = 0xFFFFFF;
    c3d->base.drawAlpha  = 1.0f;
    c3d->base.drawFont   = -1;
    return (Renderer *)c3d;
}

#endif // __3DS__