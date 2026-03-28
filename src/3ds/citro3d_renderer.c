#ifdef __3DS__

/**
 * @file citro3d_renderer.c
 * @brief Сборка vtable и конструктор рендерера Citro3D.
 *
 * Этот файл намеренно тонкий: только vtable + Citro3dRenderer_create().
 * Вся логика реализована в отдельных модулях:
 *
 *   c3d_init.c     — init, destroy, beginFrame, endFrame, beginView, endView
 *   c3d_draw.c     — drawSprite, drawSpritePart, drawRectangle, drawLine, drawText
 *   c3d_batch.c    — flushBatch, checkBatch, calcUV
 *   c3d_texture.c  — uploadTexture, createWhiteTexture, ensureTextureLoaded
 *   c3d_utils.c    — nextPOT, colorToABGR
 */

#include "citro3d_renderer.h"
#include "c3d/c3d_init.h"
#include "c3d/c3d_draw.h"
#include "c3d/c3d_constants.h"
#include "utils.h"

// ─────────────────────────────────────────────────────────────────────────────
// VTable
// ─────────────────────────────────────────────────────────────────────────────

static RendererVtable c3d_vtable = {
    .init                    = C3DRenderer_init,
    .destroy                 = C3DRenderer_destroy,
    .beginFrame              = C3DRenderer_beginFrame,
    .endFrame                = C3DRenderer_endFrame,
    .beginView               = C3DRenderer_beginView,
    .endView                 = C3DRenderer_endView,
    .drawSprite              = C3DRenderer_drawSprite,
    .drawSpritePart          = C3DRenderer_drawSpritePart,
    .drawRectangle           = C3DRenderer_drawRectangle,
    .drawLine                = C3DRenderer_drawLine,
    .drawLineColor           = C3DRenderer_drawLineColor,
    .drawText                = C3DRenderer_drawText,
    .flush                   = C3DRenderer_flush,
    .createSpriteFromSurface = NULL,  // не реализовано на 3DS
    .deleteSprite            = NULL,  // не реализовано на 3DS
    .drawTile                = NULL,  // используется дефолтный путь через drawSpritePart
};

// ─────────────────────────────────────────────────────────────────────────────
// Конструктор
// ─────────────────────────────────────────────────────────────────────────────

Renderer *Citro3dRenderer_create(void) {
    Citro3dRenderer *c3d = safeCalloc(1, sizeof(Citro3dRenderer));

    c3d->base.vtable    = &c3d_vtable;
    c3d->base.drawColor = DEFAULT_DRAW_COLOR;
    c3d->base.drawAlpha = DEFAULT_DRAW_ALPHA;
    c3d->base.drawFont  = DEFAULT_DRAW_FONT;

    return (Renderer *)c3d;
}

#endif // __3DS__
