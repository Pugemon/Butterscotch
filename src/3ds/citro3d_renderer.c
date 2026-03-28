#ifdef __3DS__

#include "citro3d_renderer.h"
#include "text_utils.h"

#include <3ds.h>
#include <citro3d.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "stb_image.h"

// Генерируется CMake из shader.v.pica
#include "binary_reader.h"
#include <butterscotch_shader_shbin.h>

#define MAX_VERTICES 8192

#define CLEAR_COLOR 0xFF000000 // Черный (ABGR)

#define DISPLAY_TRANSFER_FLAGS \
(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Флаги переноса текстуры: меняем формат в RGBA8 и включаем тайлинг (Morton order)
#define TEX_TRANSFER_FLAGS \
  (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
   GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    float x, y, z;
    float u, v;
    u32 color; // ABGR
} C3D_Vertex;

static uint32_t nextPOT(uint32_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

typedef struct {
    Renderer base;
    C3D_RenderTarget *target;

    DVLB_s *dvlb;
    shaderProgram_s shader;
    int8_t uLoc_projection;

    C3D_Tex *textures;
    int whitePixelTexIndex;

    int *texRealW; // реальный размер до POT
    int *texRealH;

    // Батчинг (Batching)
    C3D_Vertex *vboData;
    int vertexCount;
    int currentTextureIndex;

    C3D_Mtx projection;
} Citro3dRenderer;

// ===[ Вспомогательные функции ]===

static void C3DRenderer_flush(Renderer *renderer) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    if (c3d->vertexCount == 0) return;

    if (c3d->currentTextureIndex >= 0) {
        C3D_TexBind(0, &c3d->textures[c3d->currentTextureIndex]);
    } else {
        C3D_TexBind(0, NULL);
    }

    C3D_DrawArrays(GPU_TRIANGLES, 0, c3d->vertexCount);
    c3d->vertexCount = 0;
}

static void checkBatchLimit(Citro3dRenderer *c3d, int numVerticesNeeded, int textureIndex) {
    // Если сменилась текстура или буфер переполнен - сбрасываем батч на видеокарту
    if (c3d->currentTextureIndex != textureIndex || c3d->vertexCount + numVerticesNeeded > MAX_VERTICES) {
        C3DRenderer_flush((Renderer *) c3d);
        c3d->currentTextureIndex = textureIndex;
    }
}

static u32 colorToABGR(uint32_t bgrColor, float alpha) {
    u8 r = (bgrColor) & 0xFF;
    u8 g = (bgrColor >> 8) & 0xFF;
    u8 b = (bgrColor >> 16) & 0xFF;
    u8 a = (u8) (alpha * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static inline void rotatePoint(float x, float y, float originX, float originY, float cs, float sn, float *outX,
                               float *outY) {
    float dx = x - originX;
    float dy = y - originY;
    // Поворот против часовой стрелки для координатной системы (Y-вниз)
    *outX = originX + (cs * dx + sn * dy);
    *outY = originY + (-sn * dx + cs * dy);
}

// ===[ Vtable Реализация ]===

static void initGraphics(Citro3dRenderer *c3d) {
    gfxInitDefault();
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    c3d->target = C3D_RenderTargetCreate(240, 400,
                                         GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);

    C3D_RenderTargetSetOutput(
        c3d->target,
        GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) |
        GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO)
    );
}

static void initShader(Citro3dRenderer *c3d) {
    c3d->dvlb = DVLB_ParseFile((u32 *) butterscotch_shader_shbin, butterscotch_shader_shbin_size);

    shaderProgramInit(&c3d->shader);
    shaderProgramSetVsh(&c3d->shader, &c3d->dvlb->DVLE[0]);

    C3D_BindProgram(&c3d->shader);

    c3d->uLoc_projection =
            shaderInstanceGetUniformLocation(
                c3d->shader.vertexShader,
                "proj_mtx"
            );
}

static void initBuffers(Citro3dRenderer *c3d) {
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);

    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);

    c3d->vboData = linearAlloc(sizeof(C3D_Vertex) * MAX_VERTICES);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, c3d->vboData, sizeof(C3D_Vertex), 3, 0x210);
}

static void initRenderState(void) {
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);

    C3D_AlphaBlend(
        GPU_BLEND_ADD, GPU_BLEND_ADD,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA
    );

    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);

    C3D_TexEnvSrc(env, C3D_Both,
                  GPU_TEXTURE0,
                  GPU_PRIMARY_COLOR,
                  0
    );

    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

static void uploadTexture(C3D_Tex *tex, uint8_t *pixels, int w, int h) {

    // RGBA -> ABGR
    for (int i = 0; i < w * h * 4; i += 4) {
        uint8_t r = pixels[i];
        uint8_t g = pixels[i + 1];
        uint8_t b = pixels[i + 2];
        uint8_t a = pixels[i + 3];

        pixels[i] = a;
        pixels[i + 1] = b;
        pixels[i + 2] = g;
        pixels[i + 3] = r;
    }

    C3D_TexInit(tex, w, h, GPU_RGBA8);
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_NEAREST);

    GSPGPU_FlushDataCache(pixels, w * h * 4);

    C3D_SyncDisplayTransfer(
        (u32 *) pixels, GX_BUFFER_DIM(w, h),
        (u32 *) tex->data, GX_BUFFER_DIM(w, h),
        TEX_TRANSFER_FLAGS
    );
}

static void flush(Citro3dRenderer *c3d) {
    if (!c3d->vertexCount) return;

    C3D_TexBind(0,
                c3d->currentTextureIndex >= 0
                    ? &c3d->textures[c3d->currentTextureIndex]
                    : NULL
    );

    C3D_DrawArrays(GPU_TRIANGLES, 0, c3d->vertexCount);

    c3d->vertexCount = 0;
}

static inline void ensureBatch(
    Citro3dRenderer *c3d,
    int verts,
    int tex
) {
    if (c3d->currentTextureIndex != tex ||
        c3d->vertexCount + verts > MAX_VERTICES) {
        flush(c3d);
        c3d->currentTextureIndex = tex;
    }
}

static void C3DRenderer_init(Renderer *renderer, DataWin *dataWin) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    c3d->base.dataWin = dataWin;

    // --- Инициализация графики ---
    fprintf(stderr,"[C3D] Before gfxInitDefault\n");
    gfxInitDefault();
    fprintf(stderr,"[C3D] Before C3D_Init\n");
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    fprintf(stderr,"[C3D] C3D_Init done\n");

    c3d->target = C3D_RenderTargetCreate(
        240, 400,
        GPU_RB_RGBA8,
        GPU_RB_DEPTH24_STENCIL8
    );

    fprintf(stderr,"[C3D] target = %p\n", (void*)c3d->target);

    C3D_RenderTargetSetOutput(
        c3d->target,
        GFX_TOP, GFX_LEFT,
        GX_TRANSFER_FLIP_VERT(0) |
        GX_TRANSFER_OUT_TILED(0) |
        GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO)
    );

    // --- Шейдер ---
    c3d->dvlb = DVLB_ParseFile((u32 *) butterscotch_shader_shbin, butterscotch_shader_shbin_size);

    shaderProgramInit(&c3d->shader);
    shaderProgramSetVsh(&c3d->shader, &c3d->dvlb->DVLE[0]);
    C3D_BindProgram(&c3d->shader);

    c3d->uLoc_projection =
            shaderInstanceGetUniformLocation(
                c3d->shader.vertexShader,
                "proj_mtx"
            );

    // --- Атрибуты вершин ---
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); // position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); // UV
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); // color

    // --- VBO ---
    c3d->vboData = linearAlloc(sizeof(C3D_Vertex) * MAX_VERTICES);

    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, c3d->vboData, sizeof(C3D_Vertex), 3, 0x210);

    // --- Render state ---
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);

    C3D_AlphaBlend(
        GPU_BLEND_ADD, GPU_BLEND_ADD,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
        GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA
    );

    // Texture combiner: texture * vertex color
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    // --- Текстуры ---
    uint32_t texCount = dataWin->txtr.count;
    c3d->textures = safeCalloc(texCount + 1, sizeof(C3D_Tex)); // +1 white

    BinaryReader reader = BinaryReader_create(
        dataWin->file,
        dataWin->fileSize
    );

    for (uint32_t i = 0; i < texCount; i++) {
        Texture *tex = &dataWin->txtr.textures[i];
        if (tex->blobSize == 0) continue;

        uint8_t *raw = BinaryReader_readBytesAt(
            &reader,
            tex->blobOffset,
            tex->blobSize
        );

        int w, h, ch;
        uint8_t *pixels = stbi_load_from_memory(raw, tex->blobSize, &w, &h, &ch, 4);
        free(raw);
        if (!pixels) continue;

        c3d->texRealW[i] = w;
        c3d->texRealH[i] = h;
        int potW = nextPOT(w);
        int potH = nextPOT(h);

        // Выделяем POT-буфер, заполненный нулями
        uint8_t *linBuf = (uint8_t*)linearAlloc(potW * potH * 4);
        if (!linBuf) { stbi_image_free(pixels); continue; }
        memset(linBuf, 0, potW * potH * 4);

        // Копируем построчно с конвертацией RGBA→ABGR
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                int src = (row * w + col) * 4;
                int dst = (row * potW + col) * 4;
                linBuf[dst + 0] = pixels[src + 3]; // A
                linBuf[dst + 1] = pixels[src + 2]; // B
                linBuf[dst + 2] = pixels[src + 1]; // G
                linBuf[dst + 3] = pixels[src + 0]; // R
            }
        }
        stbi_image_free(pixels);

        // Теперь C3D_TexInit получит корректные POT размеры
        if (!C3D_TexInit(&c3d->textures[i], potW, potH, GPU_RGBA8)) {
            linearFree(linBuf);
            continue; // хотя бы не крашимся
        }
        C3D_TexSetFilter(&c3d->textures[i], GPU_LINEAR, GPU_NEAREST);

        GSPGPU_FlushDataCache(linBuf, potW * potH * 4);
        C3D_SyncDisplayTransfer(
            (u32*)linBuf, GX_BUFFER_DIM(potW, potH),
            (u32*)c3d->textures[i].data, GX_BUFFER_DIM(potW, potH),
            TEX_TRANSFER_FLAGS
        );
        linearFree(linBuf);
    }

    // --- Белая текстура (для примитивов) ---
    c3d->whitePixelTexIndex = texCount;

    C3D_TexInit(&c3d->textures[c3d->whitePixelTexIndex], 8, 8, GPU_RGBA8);

    u32 *white = (u32*)linearAlloc(8 * 8 * 4);
    for (int i = 0; i < 64; i++) white[i] = 0xFFFFFFFF;
    GSPGPU_FlushDataCache(white, 64 * 4);

    C3D_SyncDisplayTransfer(
        white, GX_BUFFER_DIM(8, 8),
        (u32 *) c3d->textures[c3d->whitePixelTexIndex].data,
        GX_BUFFER_DIM(8, 8),
        TEX_TRANSFER_FLAGS
    );

    linearFree(white);

    // --- Состояние батча ---
    c3d->vertexCount = 0;
    c3d->currentTextureIndex = -1;
}

static void C3DRenderer_destroy(Renderer *renderer) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    for (uint32_t i = 0; i <= c3d->base.dataWin->txtr.count; i++) {
        if (c3d->textures[i].data != NULL) C3D_TexDelete(&c3d->textures[i]);
    }
    free(c3d->textures);
    linearFree(c3d->vboData);
    shaderProgramFree(&c3d->shader);
    DVLB_Free(c3d->dvlb);
    C3D_RenderTargetDelete(c3d->target);
    C3D_Fini();
    gfxExit();
    free(c3d);
}

static void C3DRenderer_beginFrame(Renderer *renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    (void) gameW;
    (void) gameH;
    (void) windowW;
    (void) windowH;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    // Очистка экрана (мы рисуем фон вручную, но на всякий случай чистим черным)
    C3D_RenderTargetClear(c3d->target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
    C3D_FrameDrawOn(c3d->target);

    c3d->vertexCount = 0;
    c3d->currentTextureIndex = -1;
}

static void C3DRenderer_endFrame(Renderer *renderer) {
    C3DRenderer_flush(renderer);
    C3D_FrameEnd(0);
}

static void C3DRenderer_beginView(Renderer *renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                                  int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    C3DRenderer_flush(renderer);
    (void) viewAngle;

    // В 3DS физические размеры экрана: X=240, Y=400 (повернуты)
    // portY/portX/portH/portW транслируются в повернутые координаты PICA200
    C3D_SetViewport(portY, 400 - portX - portW, portH, portW);

    Mtx_Identity(&c3d->projection);
    // OrthoTilt выравнивает систему координат, поэтому мы можем рисовать нормально (0,0 слева вверху)
    Mtx_OrthoTilt(&c3d->projection, viewX, viewX + viewW, viewY + viewH, viewY, 1.0f, -1.0f, true);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, c3d->uLoc_projection, &c3d->projection);
}

static void C3DRenderer_endView(Renderer *renderer) {
    C3DRenderer_flush(renderer);
}

static void C3DRenderer_drawSprite(Renderer *renderer, int32_t tpagIndex, float x, float y, float originX,
                                   float originY, float xscale, float yscale, float angleDeg, uint32_t color,
                                   float alpha) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    if (tpagIndex < 0 || tpagIndex >= renderer->dataWin->tpag.count) return;

    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];
    checkBatchLimit(c3d, 6, tpag->texturePageId);

    C3D_Tex *tex = &c3d->textures[tpag->texturePageId];
    float tw = (float)c3d->texRealW[tpag->texturePageId];
    float th = (float)c3d->texRealH[tpag->texturePageId];

    float u0 = (float) tpag->sourceX / tw;
    float v0 = (float) tpag->sourceY / th;
    float u1 = u0 + (float) tpag->sourceWidth / tw;
    float v1 = v0 + (float) tpag->sourceHeight / th;

    float rad = angleDeg * (M_PI / 180.0f);
    float cs = cosf(rad);
    float sn = sinf(rad);

    float lx0 = -originX * xscale;
    float ly0 = -originY * yscale;
    float lx1 = (tpag->sourceWidth - originX) * xscale;
    float ly1 = (tpag->sourceHeight - originY) * yscale;

    float px0, py0, px1, py1, px2, py2, px3, py3;
    rotatePoint(lx0, ly0, 0, 0, cs, sn, &px0, &py0);
    rotatePoint(lx1, ly0, 0, 0, cs, sn, &px1, &py1);
    rotatePoint(lx0, ly1, 0, 0, cs, sn, &px2, &py2);
    rotatePoint(lx1, ly1, 0, 0, cs, sn, &px3, &py3);

    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x + px0, y + py0, 0.5f, u0, v0, clr};
    v[1] = (C3D_Vertex){x + px1, y + py1, 0.5f, u1, v0, clr};
    v[2] = (C3D_Vertex){x + px2, y + py2, 0.5f, u0, v1, clr};
    v[3] = (C3D_Vertex){x + px1, y + py1, 0.5f, u1, v0, clr};
    v[4] = (C3D_Vertex){x + px3, y + py3, 0.5f, u1, v1, clr};
    v[5] = (C3D_Vertex){x + px2, y + py2, 0.5f, u0, v1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawSpritePart(Renderer *renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY,
                                       int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale,
                                       uint32_t color, float alpha) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;
    if (tpagIndex < 0 || tpagIndex >= renderer->dataWin->tpag.count) return;

    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];
    checkBatchLimit(c3d, 6, tpag->texturePageId);

    float tw = (float)c3d->texRealW[tpag->texturePageId];
    float th = (float)c3d->texRealH[tpag->texturePageId];

    float u0 = (float) (tpag->sourceX + srcOffX) / tw;
    float v0 = (float) (tpag->sourceY + srcOffY) / th;
    float u1 = u0 + (float) srcW / tw;
    float v1 = v0 + (float) srcH / th;

    float x2 = x + (srcW * xscale);
    float y2 = y + (srcH * yscale);

    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x, y, 0.5f, u0, v0, clr};
    v[1] = (C3D_Vertex){x2, y, 0.5f, u1, v0, clr};
    v[2] = (C3D_Vertex){x, y2, 0.5f, u0, v1, clr};
    v[3] = (C3D_Vertex){x2, y, 0.5f, u1, v0, clr};
    v[4] = (C3D_Vertex){x2, y2, 0.5f, u1, v1, clr};
    v[5] = (C3D_Vertex){x, y2, 0.5f, u0, v1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawRectangle(Renderer *renderer, float x1, float y1, float x2, float y2, uint32_t color,
                                      float alpha, bool outline) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;

    if (outline) {
        // Линии рисуем как тонкие прямоугольники
        renderer->vtable->drawLine(renderer, x1, y1, x2, y1, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x2, y1, x2, y2, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x2, y2, x1, y2, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x1, y2, x1, y1, 1.0f, color, alpha);
        return;
    }

    checkBatchLimit(c3d, 6, c3d->whitePixelTexIndex);
    u32 clr = colorToABGR(color, alpha);

    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];
    v[0] = (C3D_Vertex){x1, y1, 0.5f, 0, 0, clr};
    v[1] = (C3D_Vertex){x2, y1, 0.5f, 1, 0, clr};
    v[2] = (C3D_Vertex){x1, y2, 0.5f, 0, 1, clr};
    v[3] = (C3D_Vertex){x2, y1, 0.5f, 1, 0, clr};
    v[4] = (C3D_Vertex){x2, y2, 0.5f, 1, 1, clr};
    v[5] = (C3D_Vertex){x1, y2, 0.5f, 0, 1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawLine(Renderer *renderer, float x1, float y1, float x2, float y2, float width,
                                 uint32_t color, float alpha) {
    Citro3dRenderer *c3d = (Citro3dRenderer *) renderer;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len == 0) return;

    // Вычисляем нормаль линии для придания ей толщины
    float nx = -(dy / len) * (width / 2.0f);
    float ny = (dx / len) * (width / 2.0f);

    checkBatchLimit(c3d, 6, c3d->whitePixelTexIndex);
    u32 clr = colorToABGR(color, alpha);

    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];
    v[0] = (C3D_Vertex){x1 - nx, y1 - ny, 0.5f, 0, 0, clr};
    v[1] = (C3D_Vertex){x1 + nx, y1 + ny, 0.5f, 1, 0, clr};
    v[2] = (C3D_Vertex){x2 - nx, y2 - ny, 0.5f, 0, 1, clr};
    v[3] = (C3D_Vertex){x1 + nx, y1 + ny, 0.5f, 1, 0, clr};
    v[4] = (C3D_Vertex){x2 + nx, y2 + ny, 0.5f, 1, 1, clr};
    v[5] = (C3D_Vertex){x2 - nx, y2 - ny, 0.5f, 0, 1, clr};

    c3d->vertexCount += 6;
}

static void C3DRenderer_drawLineColor(Renderer *renderer, float x1, float y1, float x2, float y2, float width,
                                      uint32_t color1, uint32_t color2, float alpha) {
    C3DRenderer_drawLine(renderer, x1, y1, x2, y2, width, color1, alpha); // Упрощено для 3DS
}

static void C3DRenderer_drawText(Renderer *renderer, const char *text, float x, float y, float xscale, float yscale,
                                 float angleDeg) {
    DataWin *dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || fontIndex >= dw->font.count) return;

    Font *font = &dw->font.fonts[fontIndex];
    int32_t pos = 0;
    int32_t len = strlen(text);
    float currentX = x;
    float currentY = y;

    while (pos < len) {
        if (text[pos] == '\n') {
            currentX = x;
            currentY += font->emSize * yscale;
            pos++;
            continue;
        }

        uint16_t ch = TextUtils_decodeUtf8(text, len, &pos);
        FontGlyph *glyph = TextUtils_findGlyph(font, ch);
        if (!glyph) continue;

        int32_t tpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
        if (tpagIndex >= 0) {
            float gx = currentX + (glyph->offset * xscale);
            float gy = currentY;

            // Текст рисуем стандартным методом (уже с батчингом)
            C3DRenderer_drawSpritePart(renderer, tpagIndex, glyph->sourceX, glyph->sourceY,
                                       glyph->sourceWidth, glyph->sourceHeight,
                                       gx, gy, xscale, yscale, renderer->drawColor, renderer->drawAlpha);
        }

        currentX += glyph->shift * xscale;

        if (pos < len) {
            int32_t savedPos = pos;
            uint16_t nextCh = TextUtils_decodeUtf8(text, len, &pos);
            pos = savedPos;
            currentX += TextUtils_getKerningOffset(glyph, nextCh) * xscale;
        }
    }
}

static RendererVtable c3d_vtable = {
    .init = C3DRenderer_init,
    .destroy = C3DRenderer_destroy,
    .beginFrame = C3DRenderer_beginFrame,
    .endFrame = C3DRenderer_endFrame,
    .beginView = C3DRenderer_beginView,
    .endView = C3DRenderer_endView,
    .drawSprite = C3DRenderer_drawSprite,
    .drawSpritePart = C3DRenderer_drawSpritePart,
    .drawRectangle = C3DRenderer_drawRectangle,
    .drawLine = C3DRenderer_drawLine,
    .drawLineColor = C3DRenderer_drawLineColor,
    .drawText = C3DRenderer_drawText,
    .flush = C3DRenderer_flush,
    .createSpriteFromSurface = NULL,
    .deleteSprite = NULL,
    .drawTile = NULL
};

Renderer *Citro3dRenderer_create(void) {
    Citro3dRenderer *c3d = safeCalloc(1, sizeof(Citro3dRenderer));
    c3d->base.vtable = &c3d_vtable;
    return (Renderer *) c3d;
}

#endif // __3DS__
