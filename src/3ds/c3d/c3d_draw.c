#ifdef __3DS__

#include "c3d_draw.h"
#include "c3d_batch.h"
#include "c3d_texture.h"
#include "c3d_utils.h"
#include "c3d_constants.h"
#include "citro3d_renderer.h"
#include "text_utils.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>

#include "c3d_texture_t3b.h"

// ─────────────────────────────────────────────────────────────────────────────
// Внутренний хелпер: получить текстуру и UV для TPAG-индекса
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Подготавливает рендеринг по TPAG-индексу:
 * гарантирует загрузку текстуры, проверяет батч, заполняет UV.
 * Возвращает указатель на буфер вершин или NULL при ошибке.
 */
static inline C3D_Vertex *prepareQuad(Citro3dRenderer *c3d,
                                Renderer *renderer,
                                int32_t tpagIndex,
                                float srcX, float srcY,
                                float srcW, float srcH,
                                float *u0, float *v0,
                                float *u1, float *v1)
{
    if (tpagIndex < 0 || tpagIndex >= (int)renderer->dataWin->tpag.count) return NULL;

    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];
    int pid = tpag->texturePageId;

    /// Теперь текстуры загружает checkBatch
    //ensureAtlasLoaded(c3d, pid);
    //ensureTextureLoaded(c3d, pid);
    if (!checkBatch(c3d, 6, tpagIndex)) {
        return NULL; // VBO переполнен, отказываемся выдавать вершины
    }

    calcUV(&c3d->textures[pid], srcX, srcY, srcW, srcH, u0, v0, u1, v1);
    return &c3d->vboData[c3d->vertexCount];
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw-функции
// ─────────────────────────────────────────────────────────────────────────────

void C3DRenderer_drawSprite(Renderer *renderer,
                             int32_t tpagIndex,
                             float x, float y,
                             float originX, float originY,
                             float xscale, float yscale,
                             float angleDeg,
                             uint32_t color, float alpha)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];

    // ── 2D FRUSTUM CULLING ───────────────────────────────────────────────────
    // Локальные координаты четырёх углов спрайта (относительно точки x,y).
    // targetX/targetY — смещение контента внутри bounding rect'а TPAG;
    // originX/originY — pivot, вокруг которого происходит вращение.
    // Без учёта targetX/Y culling даёт ложные отсечения у края экрана.
    float lx0 = ((float)tpag->targetX - originX) * xscale;
    float ly0 = ((float)tpag->targetY - originY) * yscale;
    float lx1 = lx0 + (float)tpag->sourceWidth  * xscale;
    float ly1 = ly0 + (float)tpag->sourceHeight * yscale;

    if (angleDeg == 0.0f) {
        // FAST PATH: точный AABB без поворота. fminf/fmaxf нужны при отрицательном scale.
        float worldL = x + fminf(lx0, lx1);
        float worldR = x + fmaxf(lx0, lx1);
        float worldT = y + fminf(ly0, ly1);
        float worldB = y + fmaxf(ly0, ly1);
        if (worldR < c3d->viewX || worldL > c3d->viewX + c3d->viewW ||
            worldB < c3d->viewY || worldT > c3d->viewY + c3d->viewH) return;
    } else {
        // ROTATED PATH: описывающая окружность вокруг центра спрайта.
        // Центр спрайта в локальных координатах:
        float cx = (lx0 + lx1) * 0.5f;
        float cy = (ly0 + ly1) * 0.5f;
        // Радиус описывающей окружности (половина диагонали bounding rect'а):
        float hw = fabsf(lx1 - lx0) * 0.5f;
        float hh = fabsf(ly1 - ly0) * 0.5f;
        float radius = sqrtf(hw * hw + hh * hh);
        // Центр в мировых координатах:
        float worldCX = x + cx;
        float worldCY = y + cy;
        if (worldCX + radius < c3d->viewX || worldCX - radius > c3d->viewX + c3d->viewW ||
            worldCY + radius < c3d->viewY || worldCY - radius > c3d->viewY + c3d->viewH) return;
    }

    float u0, v0, u1, v1;
    C3D_Vertex *v = prepareQuad(c3d, renderer, tpagIndex,
                                 tpag->sourceX, tpag->sourceY,
                                 tpag->sourceWidth, tpag->sourceHeight,
                                 &u0, &v0, &u1, &v1);
    if (!v) return;

    // lx0/ly0/lx1/ly1 уже вычислены выше для culling — переиспользуем.
    float rx0, ry0, rx1, ry1, rx2, ry2, rx3, ry3;

    // FAST PATH: Пропускаем тяжелую математику для неповёрнутых спрайтов
    if (angleDeg == 0.0f) {
        rx0 = lx0; ry0 = ly0;
        rx1 = lx1; ry1 = ly0;
        rx2 = lx0; ry2 = ly1;
        rx3 = lx1; ry3 = ly1;
    } else {
        float sn, cs;
        fast_sincosf(angleDeg, &sn, &cs);
        rotatePoint(lx0, ly0, cs, sn, &rx0, &ry0);
        rotatePoint(lx1, ly0, cs, sn, &rx1, &ry1);
        rotatePoint(lx0, ly1, cs, sn, &rx2, &ry2);
        rotatePoint(lx1, ly1, cs, sn, &rx3, &ry3);
    }

    u32 clr = colorToABGR(color, alpha);

    // 2 треугольника (v0-v1-v2, v1-v3-v2) = 1 quad без index buffer
    v[0] = (C3D_Vertex){x+rx0, y+ry0, SPRITE_Z_DEPTH, u0, v0, clr};
    v[1] = (C3D_Vertex){x+rx1, y+ry1, SPRITE_Z_DEPTH, u1, v0, clr};
    v[2] = (C3D_Vertex){x+rx2, y+ry2, SPRITE_Z_DEPTH, u0, v1, clr};
    v[3] = (C3D_Vertex){x+rx1, y+ry1, SPRITE_Z_DEPTH, u1, v0, clr};
    v[4] = (C3D_Vertex){x+rx3, y+ry3, SPRITE_Z_DEPTH, u1, v1, clr};
    v[5] = (C3D_Vertex){x+rx2, y+ry2, SPRITE_Z_DEPTH, u0, v1, clr};

    c3d->vertexCount += 6;
}

void C3DRenderer_drawSpritePart(Renderer *renderer,
                                 int32_t tpagIndex,
                                 int32_t srcOffX, int32_t srcOffY,
                                 int32_t srcW, int32_t srcH,
                                 float x, float y,
                                 float xscale, float yscale,
                                 uint32_t color, float alpha)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;

    // ── 2D FRUSTUM CULLING (FAST PATH) ───────────────────────────────────────
    float scaledW = (float)srcW * xscale;
    float scaledH = (float)srcH * yscale;

    // Поддержка отрицательного скейла (отражение спрайта)
    float worldL = x + fminf(0.0f, scaledW);
    float worldR = x + fmaxf(0.0f, scaledW);
    float worldT = y + fminf(0.0f, scaledH);
    float worldB = y + fmaxf(0.0f, scaledH);

    // Если кусок спрайта вне экрана — пропускаем! (Это спасёт FPS на больших картах)
    if (worldR < c3d->viewX || worldL > c3d->viewX + c3d->viewW ||
        worldB < c3d->viewY || worldT > c3d->viewY + c3d->viewH) return;

    TexturePageItem *tpag = &renderer->dataWin->tpag.items[tpagIndex];

    float u0, v0, u1, v1;
    // ВАЖНО: Убедитесь, что prepareQuad внутри вызывает checkBatch(c3d, 6, tpagIndex)!
    C3D_Vertex *v = prepareQuad(c3d, renderer, tpagIndex,
                                 (float)(tpag->sourceX + srcOffX),
                                 (float)(tpag->sourceY + srcOffY),
                                 (float)srcW, (float)srcH,
                                 &u0, &v0, &u1, &v1);
    if (!v) return;

    float x2 = x + scaledW;
    float y2 = y + scaledH;
    u32 clr  = colorToABGR(color, alpha);

    v[0] = (C3D_Vertex){x,  y,  SPRITE_Z_DEPTH, u0, v0, clr};
    v[1] = (C3D_Vertex){x2, y,  SPRITE_Z_DEPTH, u1, v0, clr};
    v[2] = (C3D_Vertex){x,  y2, SPRITE_Z_DEPTH, u0, v1, clr};
    v[3] = (C3D_Vertex){x2, y,  SPRITE_Z_DEPTH, u1, v0, clr};
    v[4] = (C3D_Vertex){x2, y2, SPRITE_Z_DEPTH, u1, v1, clr};
    v[5] = (C3D_Vertex){x,  y2, SPRITE_Z_DEPTH, u0, v1, clr};

    c3d->vertexCount += 6;
}

void C3DRenderer_drawRectangle(Renderer *renderer,
                                float x1, float y1, float x2, float y2,
                                uint32_t color, float alpha, bool outline)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;

    // ── CULLING ─────────────────────────────────────────────────────────────
    float worldL = fminf(x1, x2);
    float worldR = fmaxf(x1, x2);
    float worldT = fminf(y1, y2);
    float worldB = fmaxf(y1, y2);

    if (worldR < c3d->viewX || worldL > c3d->viewX + c3d->viewW ||
        worldB < c3d->viewY || worldT > c3d->viewY + c3d->viewH) return;

    if (outline) {
        // Обводка = 4 линии по периметру (drawLine сама проверит culling, если нужно,
        // но мы уже отсекли прямоугольник целиком, что быстрее)
        renderer->vtable->drawLine(renderer, x1, y1, x2, y1, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x2, y1, x2, y2, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x2, y2, x1, y2, 1.0f, color, alpha);
        renderer->vtable->drawLine(renderer, x1, y2, x1, y1, 1.0f, color, alpha);
        return;
    }

    // Защита от переполнения VBO!
    if (!checkBatch(c3d, 6, c3d->whiteTexIndex)) return;

    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x1, y1, SPRITE_Z_DEPTH, 0.0f, 0.0f, clr};
    v[1] = (C3D_Vertex){x2, y1, SPRITE_Z_DEPTH, 1.0f, 0.0f, clr};
    v[2] = (C3D_Vertex){x1, y2, SPRITE_Z_DEPTH, 0.0f, 1.0f, clr};
    v[3] = (C3D_Vertex){x2, y1, SPRITE_Z_DEPTH, 1.0f, 0.0f, clr};
    v[4] = (C3D_Vertex){x2, y2, SPRITE_Z_DEPTH, 1.0f, 1.0f, clr};
    v[5] = (C3D_Vertex){x1, y2, SPRITE_Z_DEPTH, 0.0f, 1.0f, clr};

    c3d->vertexCount += 6;
}

void C3DRenderer_drawLine(Renderer *renderer,
                           float x1, float y1, float x2, float y2,
                           float width, uint32_t color, float alpha)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    float dx = x2 - x1, dy = y2 - y1;
    float lenSq = dx * dx + dy * dy;
    if (lenSq < (LINE_MIN_LENGTH * LINE_MIN_LENGTH)) return;

    float invLen = 1.0f / sqrtf(lenSq);
    float halfWidth = width * 0.5f;

    // Нормаль к линии (перпендикуляр), масштабированная на полуширину
    float nx = -dy * invLen * halfWidth;
    float ny =  dx * invLen * halfWidth;

    float worldL = fminf(x1, x2) - halfWidth;
    float worldR = fmaxf(x1, x2) + halfWidth;
    float worldT = fminf(y1, y2) - halfWidth;
    float worldB = fmaxf(y1, y2) + halfWidth;

    if (worldR < c3d->viewX || worldL > c3d->viewX + c3d->viewW ||
        worldB < c3d->viewY || worldT > c3d->viewY + c3d->viewH) return;

    // ЗАЩИТА VBO: checkBatch должен возвращать bool. Если буфер полон и
    // flushBatch не смог освободить место (например, сброс запрещен), выходим!
    if (!checkBatch(c3d, 6, c3d->whiteTexIndex)) return;

    u32 clr = colorToABGR(color, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x1-nx, y1-ny, SPRITE_Z_DEPTH, 0.0f, 0.0f, clr};
    v[1] = (C3D_Vertex){x1+nx, y1+ny, SPRITE_Z_DEPTH, 1.0f, 0.0f, clr};
    v[2] = (C3D_Vertex){x2-nx, y2-ny, SPRITE_Z_DEPTH, 0.0f, 1.0f, clr};
    v[3] = (C3D_Vertex){x1+nx, y1+ny, SPRITE_Z_DEPTH, 1.0f, 0.0f, clr};
    v[4] = (C3D_Vertex){x2+nx, y2+ny, SPRITE_Z_DEPTH, 1.0f, 1.0f, clr};
    v[5] = (C3D_Vertex){x2-nx, y2-ny, SPRITE_Z_DEPTH, 0.0f, 1.0f, clr};

    c3d->vertexCount += 6;
}

void C3DRenderer_drawLineColor(Renderer *renderer,
                                float x1, float y1, float x2, float y2,
                                float width,
                                uint32_t color1, uint32_t color2, float alpha)
{
    Citro3dRenderer *c3d = (Citro3dRenderer *)renderer;
    float dx = x2 - x1, dy = y2 - y1;
    float lenSq = dx * dx + dy * dy; // Используем квадрат длины

    if (lenSq < (LINE_MIN_LENGTH * LINE_MIN_LENGTH)) return;

    // Компиляторы ARM часто превращают (1.0f / sqrtf) в очень быструю
    // аппаратную инструкцию rsqrte (Reverse Square Root Estimate).
    float invLen = 1.0f / sqrtf(lenSq);
    float halfWidth = width * 0.5f;

    float nx = -dy * invLen * halfWidth;
    float ny =  dx * invLen * halfWidth;

    float worldL = fminf(x1, x2) - halfWidth;
    float worldR = fmaxf(x1, x2) + halfWidth;
    float worldT = fminf(y1, y2) - halfWidth;
    float worldB = fmaxf(y1, y2) + halfWidth;

    if (worldR < c3d->viewX || worldL > c3d->viewX + c3d->viewW ||
        worldB < c3d->viewY || worldT > c3d->viewY + c3d->viewH) return;

    // ЗАЩИТА VBO: checkBatch должен возвращать bool. Если буфер полон и
    // flushBatch не смог освободить место (например, сброс запрещен), выходим!
    if (!checkBatch(c3d, 6, c3d->whiteTexIndex)) return;
    u32 clr1 = colorToABGR(color1, alpha);
    u32 clr2 = colorToABGR(color2, alpha);
    C3D_Vertex *v = &c3d->vboData[c3d->vertexCount];

    v[0] = (C3D_Vertex){x1-nx, y1-ny, SPRITE_Z_DEPTH, 0.0f, 0.0f, clr1};
    v[1] = (C3D_Vertex){x1+nx, y1+ny, SPRITE_Z_DEPTH, 1.0f, 0.0f, clr1};
    v[2] = (C3D_Vertex){x2-nx, y2-ny, SPRITE_Z_DEPTH, 0.0f, 1.0f, clr2};
    v[3] = (C3D_Vertex){x1+nx, y1+ny, SPRITE_Z_DEPTH, 1.0f, 0.0f, clr1};
    v[4] = (C3D_Vertex){x2+nx, y2+ny, SPRITE_Z_DEPTH, 1.0f, 1.0f, clr2};
    v[5] = (C3D_Vertex){x2-nx, y2-ny, SPRITE_Z_DEPTH, 0.0f, 1.0f, clr2};

    c3d->vertexCount += 6;
}

void C3DRenderer_drawText(Renderer *renderer,
                           const char *text,
                           float x, float y,
                           float xscale, float yscale,
                           float angleDeg)
{
    (void)angleDeg;  // TODO: повёрнутый текст не реализован
    DataWin *dw = renderer->dataWin;

    int32_t fontIdx = renderer->drawFont;
    if (fontIdx < 0 || fontIdx >= (int)dw->font.count) return;

    Font   *font = &dw->font.fonts[fontIdx];
    int32_t len  = (int32_t)strlen(text);
    int32_t pos  = 0;
    float curX = x, curY = y;

    while (pos < len) {
        if (text[pos] == '\n') {
            curX = x;
            curY += font->emSize * yscale;
            pos++;
            continue;
        }

        uint16_t  ch = TextUtils_decodeUtf8(text, len, &pos);
        FontGlyph *g  = TextUtils_findGlyph(font, ch);
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

        // Кернинг: корректируем расстояние до следующего символа
        if (pos < len) {
            int32_t  savedPos = pos;
            uint16_t nextCh   = TextUtils_decodeUtf8(text, len, &pos);
            pos = savedPos;
            curX += TextUtils_getKerningOffset(g, nextCh) * xscale;
        }
    }
}

#endif // __3DS__
