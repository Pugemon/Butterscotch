#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS

#include <SDL2/SDL.h>

#include "sdl_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

#define DYNAMIC_TPAG_OFFSET_BASE 0xD0000000u

#define FORCE_INTERNAL_3DS_RES 0
#define FORCE_INTERNAL_3DS_W 400
#define FORCE_INTERNAL_3DS_H 240

#define MAX_CACHED_TEXTURES 128

static int g_renderParity = -1;
static int g_presentW = 0;
static int g_presentH = 0;
static int g_interlaceParity = 0;

static uint32_t g_currentFrame = 0;
static uint32_t* g_texLastUsed = NULL;
static int g_loadedTexCount = 0;

static inline void transformWorldToViewF(SDLRenderer* sdl, float wx, float wy, float* vx, float* vy) {
    float lx = wx - sdl->currentViewX;
    float ly = wy - sdl->currentViewY;

    if (sdl->currentViewAngle != 0.0f) {
        lx -= sdl->camCX;
        ly -= sdl->camCY;
        float nx = lx * sdl->camCos - ly * sdl->camSin;
        float ny = lx * sdl->camSin + ly * sdl->camCos;
        lx = nx + sdl->camCX;
        ly = ny + sdl->camCY;
    }

    *vx = (lx * sdl->camScaleX + sdl->currentPortX);
    *vy = (ly * sdl->camScaleY + sdl->currentPortY);
}

static inline int shouldDrawF(SDLRenderer* sdl, float x, float y, float w, float h) {
    if (!sdl || !sdl->fboTexture) return 0;
    float x1 = x, x2 = x + w, y1 = y, y2 = y + h;
    if (x1 > x2) { float t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { float t = y1; y1 = y2; y2 = t; }

    SDL_Rect cr;
    SDL_RenderGetClipRect(sdl->renderer, &cr);

    if (SDL_RectEmpty(&cr)) {
        cr.x = 0; cr.y = 0;
        SDL_QueryTexture(sdl->fboTexture, NULL, NULL, &cr.w, &cr.h);
    }

    float margin = 32.0f;
    return (x2 > cr.x - margin && x1 < cr.x + cr.w + margin &&
            y2 > cr.y - margin && y1 < cr.y + cr.h + margin);
}

static void ensureTextureLoaded(SDLRenderer* sdl, int16_t pageId) {
    if (pageId < 0 || pageId >= sdl->textureCount) return;
    g_texLastUsed[pageId] = g_currentFrame;
    if (sdl->sdlTextures[pageId]) return;

    Texture* txtr = &sdl->base.dataWin->txtr.textures[pageId];
    if (!txtr->blobData || !txtr->blobSize) return;

    int current_loaded_originals = 0;
    for (int i = 0; i < sdl->originalTexturePageCount; i++) {
        if (sdl->sdlTextures[i]) current_loaded_originals++;
    }

    if (current_loaded_originals >= MAX_CACHED_TEXTURES) {
        int oldest_id = -1;
        uint32_t oldest_time = 0xFFFFFFFF;
        for (int i = 0; i < sdl->originalTexturePageCount; i++) {
            if (sdl->sdlTextures[i] && g_texLastUsed[i] < oldest_time) {
                oldest_time = g_texLastUsed[i];
                oldest_id = i;
            }
        }
        if (oldest_id != -1) {
            SDL_DestroyTexture(sdl->sdlTextures[oldest_id]);
            sdl->sdlTextures[oldest_id] = NULL;
        }
    }

    int w, h, ch;
    uint8_t* px = stbi_load_from_memory(txtr->blobData, (int)txtr->blobSize, &w, &h, &ch, 4);
    if (!px) return;

    SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    if (tmp) {
        sdl->sdlTextures[pageId] = SDL_CreateTextureFromSurface(sdl->renderer, tmp);
        if (sdl->sdlTextures[pageId]) {
            SDL_SetTextureBlendMode(sdl->sdlTextures[pageId], SDL_BLENDMODE_BLEND);
        }
        SDL_FreeSurface(tmp);
    }
    stbi_image_free(px);

    sdl->textureWidths[pageId]  = w;
    sdl->textureHeights[pageId] = h;
}

static void sdlInit(Renderer* r, DataWin* dw) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    r->dataWin = dw;
    sdl->textureCount = dw->txtr.count;
    sdl->sdlTextures    = safeCalloc(sdl->textureCount, sizeof(SDL_Texture*));
    sdl->textureWidths  = safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->textureHeights = safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->originalTexturePageCount = sdl->textureCount;
    sdl->originalTpagCount        = dw->tpag.count;
    sdl->originalSpriteCount      = dw->sprt.count;

    g_texLastUsed = safeCalloc(sdl->textureCount, sizeof(uint32_t));
    g_loadedTexCount = 0;
    g_currentFrame = 0;
}

static void sdlDestroy(Renderer* r) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    if (sdl->fboTexture) SDL_DestroyTexture(sdl->fboTexture);

    for (int i = 0; i < sdl->textureCount; i++) {
        if (sdl->sdlTextures[i]) SDL_DestroyTexture(sdl->sdlTextures[i]);
    }
    free(sdl->sdlTextures);
    free(sdl->textureWidths);
    free(sdl->textureHeights);
    if (g_texLastUsed) { free(g_texLastUsed); g_texLastUsed = NULL; }
    free(sdl);
}

static void sdlBeginFrame(Renderer* r, int32_t gw, int32_t gh, int32_t ww, int32_t wh) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    g_currentFrame++;
    sdl->windowW = ww;
    sdl->windowH = wh;

#if FORCE_INTERNAL_3DS_RES
    sdl->gameW = FORCE_INTERNAL_3DS_W;
    sdl->gameH = FORCE_INTERNAL_3DS_H;
#else
    sdl->gameW = (gw > 0) ? gw : ww;
    sdl->gameH = (gh > 0) ? gh : wh;
#endif

    if (sdl->fboWidth != sdl->gameW || sdl->fboHeight != sdl->gameH || !sdl->fboTexture) {
        if (sdl->fboTexture) SDL_DestroyTexture(sdl->fboTexture);
        sdl->fboTexture = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_TARGET, sdl->gameW, sdl->gameH);

        SDL_SetTextureScaleMode(sdl->fboTexture, SDL_ScaleModeNearest);

        sdl->fboWidth  = sdl->gameW;
        sdl->fboHeight = sdl->gameH;
    }

    SDL_SetRenderTarget(sdl->renderer, sdl->fboTexture);
    SDL_SetRenderDrawColor(sdl->renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl->renderer);
}

static void sdlBeginView(Renderer* r, int32_t vx, int32_t vy, int32_t vw, int32_t vh,
                         int32_t px, int32_t py, int32_t pw, int32_t ph, float va) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    sdl->currentViewX = vx; sdl->currentViewY = vy;
    sdl->currentViewW = vw; sdl->currentViewH = vh;
    sdl->currentPortX = px; sdl->currentPortY = py;
    sdl->currentPortW = pw; sdl->currentPortH = ph;
    sdl->currentViewAngle = va;
    sdl->camScaleX = (float)pw / vw;
    sdl->camScaleY = (float)ph / vh;
    sdl->camCX = vw * 0.5f; sdl->camCY = vh * 0.5f;

    if (va != 0.0f) {
        float rad = -va * 3.14159265f / 180.0f;
        sdl->camCos = cosf(rad); sdl->camSin = sinf(rad);
    } else {
        sdl->camCos = 1.0f; sdl->camSin = 0.0f;
    }

    SDL_Rect cr = {px, py, pw, ph};
    if (cr.x < 0) cr.x = 0; if (cr.y < 0) cr.y = 0;
    if (cr.x + cr.w > sdl->fboWidth) cr.w = sdl->fboWidth - cr.x;
    if (cr.y + cr.h > sdl->fboHeight) cr.h = sdl->fboHeight - cr.y;
    if (cr.w < 0) cr.w = 0; if (cr.h < 0) cr.h = 0;

    SDL_RenderSetClipRect(sdl->renderer, &cr);
}

static void sdlEndView(Renderer* r) {
    SDL_RenderSetClipRect(((SDLRenderer*)r)->renderer, NULL);
}

static void sdlEndFrame(Renderer* r) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    if (!sdl->renderer || !sdl->fboTexture) return;

    SDL_SetRenderTarget(sdl->renderer, NULL);
    SDL_SetRenderDrawColor(sdl->renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl->renderer);

    float ga = (float)sdl->gameW / sdl->gameH;
    float sa = (float)sdl->windowW / sdl->windowH;
    SDL_Rect dr;

    if (sa > ga) {
        dr.h = sdl->windowH; dr.w = (int)(sdl->windowH * ga);
        dr.x = (sdl->windowW - dr.w) / 2; dr.y = 0;
    } else {
        dr.w = sdl->windowW; dr.h = (int)(sdl->windowW / ga);
        dr.x = 0; dr.y = (sdl->windowH - dr.h) / 2;
    }

    SDL_RenderCopy(sdl->renderer, sdl->fboTexture, NULL, &dr);
    SDL_RenderPresent(sdl->renderer);
}

static void sdlRendererFlush(Renderer* r) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    for (int i = 0; i < sdl->originalTexturePageCount; i++) {
        if (sdl->sdlTextures[i]) {
            SDL_DestroyTexture(sdl->sdlTextures[i]);
            sdl->sdlTextures[i] = NULL;
        }
    }
}

static void sdlDrawSprite(Renderer* r, int32_t tpagIdx, float x, float y,
                          float ox, float oy, float xs, float ys,
                          float angle, uint32_t col, float alpha) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;

    if (tpagIdx < 0 || tpagIdx >= (int)dw->tpag.count || alpha <= 0.0f) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
    ensureTextureLoaded(sdl, tpag->texturePageId);
    SDL_Texture* tex = sdl->sdlTextures[tpag->texturePageId];
    if (!tex) return;

    float vx, vy;
    transformWorldToViewF(sdl, x, y, &vx, &vy);

    float abs_xs = fabsf(xs * sdl->camScaleX);
    float abs_ys = fabsf(ys * sdl->camScaleY);
    float dstW = tpag->sourceWidth * abs_xs;
    float dstH = tpag->sourceHeight * abs_ys;
    if (dstW <= 0.001f || dstH <= 0.001f) return;

    float cx = vx - ox * (xs * sdl->camScaleX);
    float cy = vy - oy * (ys * sdl->camScaleY);

    float dstX = (xs >= 0) ? (cx + tpag->targetX * abs_xs)
                           : (cx - (tpag->targetX + tpag->sourceWidth) * abs_xs);

    float dstY = (ys >= 0) ? (cy + tpag->targetY * abs_ys)
                           : (cy - (tpag->targetY + tpag->sourceHeight) * abs_ys);

    if (!shouldDrawF(sdl, dstX, dstY, dstW, dstH)) return;

    SDL_Rect src = {tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight};
    SDL_FRect dst = {dstX, dstY, dstW, dstH};

    SDL_RendererFlip flip = SDL_FLIP_NONE;
    if (xs < 0) flip |= SDL_FLIP_HORIZONTAL;
    if (ys < 0) flip |= SDL_FLIP_VERTICAL;

    uint8_t tr = (col >> 16) & 0xFF;
    uint8_t tg = (col >> 8) & 0xFF;
    uint8_t tb = col & 0xFF;

    SDL_SetTextureColorMod(tex, tr, tg, tb);
    SDL_SetTextureAlphaMod(tex, (uint8_t)(alpha * 255.0f));

    SDL_RenderCopyExF(sdl->renderer, tex, &src, &dst, angle, NULL, flip);
}

static void sdlDrawSpritePart(Renderer* r, int32_t tpagIdx, int32_t sx, int32_t sy,
                              int32_t sw, int32_t sh, float x, float y,
                              float xs, float ys, uint32_t col, float alpha) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;

    if (tpagIdx < 0 || tpagIdx >= (int)dw->tpag.count || alpha <= 0.0f) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
    ensureTextureLoaded(sdl, tpag->texturePageId);
    SDL_Texture* tex = sdl->sdlTextures[tpag->texturePageId];
    if (!tex) return;

    int actual_sx = sx - tpag->targetX;
    int actual_sy = sy - tpag->targetY;
    int actual_sw = sw, actual_sh = sh;

    if (actual_sx < 0) { actual_sw += actual_sx; actual_sx = 0; }
    if (actual_sy < 0) { actual_sh += actual_sy; actual_sy = 0; }
    if (actual_sx + actual_sw > tpag->sourceWidth) actual_sw = tpag->sourceWidth - actual_sx;
    if (actual_sy + actual_sh > tpag->sourceHeight) actual_sh = tpag->sourceHeight - actual_sy;
    if (actual_sw <= 0 || actual_sh <= 0) return;

    int offset_x = (sx < tpag->targetX) ? (tpag->targetX - sx) : 0;
    int offset_y = (sy < tpag->targetY) ? (tpag->targetY - sy) : 0;

    float vx, vy;
    transformWorldToViewF(sdl, x, y, &vx, &vy);

    float abs_xs = fabsf(xs * sdl->camScaleX);
    float abs_ys = fabsf(ys * sdl->camScaleY);
    float dstW = actual_sw * abs_xs;
    float dstH = actual_sh * abs_ys;
    if (dstW <= 0.001f || dstH <= 0.001f) return;

    float dstX = (xs >= 0) ? (vx + (offset_x * abs_xs)) : (vx - ((offset_x + actual_sw) * abs_xs));
    float dstY = (ys >= 0) ? (vy + (offset_y * abs_ys)) : (vy - ((offset_y + actual_sh) * abs_ys));

    if (!shouldDrawF(sdl, dstX, dstY, dstW, dstH)) return;

    SDL_Rect src = {tpag->sourceX + actual_sx, tpag->sourceY + actual_sy, actual_sw, actual_sh};
    SDL_FRect dst = {dstX, dstY, dstW, dstH};

    SDL_RendererFlip flip = SDL_FLIP_NONE;
    if (xs < 0) flip |= SDL_FLIP_HORIZONTAL;
    if (ys < 0) flip |= SDL_FLIP_VERTICAL;

    SDL_SetTextureColorMod(tex, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
    SDL_SetTextureAlphaMod(tex, (uint8_t)(alpha * 255.0f));

    SDL_RenderCopyExF(sdl->renderer, tex, &src, &dst, 0.0, NULL, flip);
}

static void sdlDrawRectangle(Renderer* r, float x1, float y1, float x2, float y2,
                             uint32_t col, float alpha, bool outline) {
    if (alpha <= 0.0f) return;
    SDLRenderer* sdl = (SDLRenderer*)r;
    float vx1, vy1, vx2, vy2;
    transformWorldToViewF(sdl, x1, y1, &vx1, &vy1);
    transformWorldToViewF(sdl, x2, y2, &vx2, &vy2);

    if (vx1 > vx2) { float t = vx1; vx1 = vx2; vx2 = t; }
    if (vy1 > vy2) { float t = vy1; vy1 = vy2; vy2 = t; }

    float w = vx2 - vx1 + 1.0f;
    float h = vy2 - vy1 + 1.0f;
    if (!shouldDrawF(sdl, vx1, vy1, w, h)) return;

    SDL_FRect rect = {vx1, vy1, w, h};

    SDL_SetRenderDrawBlendMode(sdl->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl->renderer, (col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF, (uint8_t)(alpha * 255));

    if (outline) {
        SDL_RenderDrawRectF(sdl->renderer, &rect);
    } else {
        SDL_RenderFillRectF(sdl->renderer, &rect);
    }
}

static void sdlDrawLineColor(Renderer* r, float x1, float y1, float x2, float y2,
                             float w, uint32_t c1, uint32_t c2, float alpha) {
    (void)w; (void)c2;
    if (alpha <= 0.0f) return;
    SDLRenderer* sdl = (SDLRenderer*)r;
    float vx1, vy1, vx2, vy2;
    transformWorldToViewF(sdl, x1, y1, &vx1, &vy1);
    transformWorldToViewF(sdl, x2, y2, &vx2, &vy2);

    SDL_SetRenderDrawBlendMode(sdl->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl->renderer, (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF, c1 & 0xFF, (uint8_t)(alpha * 255));
    SDL_RenderDrawLineF(sdl->renderer, vx1, vy1, vx2, vy2);
}

static void sdlDrawLine(Renderer* r, float x1, float y1, float x2, float y2,
                        float w, uint32_t c, float alpha) {
    sdlDrawLineColor(r, x1, y1, x2, y2, w, c, c, alpha);
}

static void sdlDrawText(Renderer* r, const char* text, float x, float y,
                        float xscale, float yscale, float angle) {
    (void)angle;
    if (!text || !text[0] || r->drawAlpha <= 0.0f) return;

    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;

    int fnt = r->drawFont;
    if (fnt < 0 || fnt >= (int)dw->font.count) return;
    Font* font = &dw->font.fonts[fnt];
    if (!font) return;

    int ti = DataWin_resolveTPAG(dw, font->textureOffset);
    if (ti < 0 || ti >= (int)dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[ti];
    ensureTextureLoaded(sdl, tpag->texturePageId);
    SDL_Texture* tex = sdl->sdlTextures[tpag->texturePageId];
    if (!tex) return;

    char* proc = TextUtils_preprocessGmlText(text);
    if (!proc) return;
    int len = strlen(proc);
    if (len == 0 || len > 2048) { free(proc); return; }

    int lines = TextUtils_countLines(proc, len);
    if (lines <= 0 || lines > 100) lines = 1;

    float th = lines * font->emSize;
    float voff = (r->drawValign==1)?-th/2.0f:(r->drawValign==2)?-th:0.0f;
    float cy = voff;
    int pos = 0;
    float fsx = xscale * sdl->camScaleX, fsy = yscale * sdl->camScaleY;

    SDL_SetTextureColorMod(tex, (r->drawColor >> 16) & 0xFF, (r->drawColor >> 8) & 0xFF, r->drawColor & 0xFF);
    SDL_SetTextureAlphaMod(tex, (uint8_t)(r->drawAlpha * 255));

    for (int li = 0; li < lines && pos < len; li++) {
        int end = pos, ls = 0;
        while (end < len && !TextUtils_isNewlineChar(proc[end]) && ls < 1000) { end++; ls++; }
        int linelen = end - pos;
        if (linelen > 1000) linelen = 1000;

        float lw = TextUtils_measureLineWidth(font, proc + pos, linelen);
        float hoff = (r->drawHalign==1)?-lw/2.0f:(r->drawHalign==2)?-lw:0.0f;
        float cx = hoff;
        int p = 0, cs = 0;

        while (p < linelen && cs < 1000) {
            int pp = p;
            uint16_t ch = TextUtils_decodeUtf8(proc + pos, linelen, (int32_t*)&p);
            if (p <= pp) p = pp + 1;

            FontGlyph* g = TextUtils_findGlyph(font, ch);
            if (!g) { cx += font->emSize * 0.5f; cs++; continue; }

            if (g->sourceWidth > 0 && g->sourceHeight > 0) {
                float gw = g->sourceWidth * fsx;
                float gh = g->sourceHeight * fsy;
                float vx, vy;
                transformWorldToViewF(sdl, x + (cx + g->offset) * xscale, y + cy * yscale, &vx, &vy);

                SDL_Rect src = {tpag->sourceX + g->sourceX, tpag->sourceY + g->sourceY, g->sourceWidth, g->sourceHeight};
                SDL_FRect dst = {vx, vy, gw, gh};

                SDL_RenderCopyF(sdl->renderer, tex, &src, &dst);
            }
            cx += g->shift; cs++;
        }
        cy += font->emSize; pos = end;
        if (pos < len && TextUtils_isNewlineChar(proc[pos])) pos = TextUtils_skipNewline(proc, pos, len);
    }
    free(proc);
}

static int findOrAllocTex(SDLRenderer* sdl) {
    for (uint32_t i = sdl->originalTexturePageCount; i < sdl->textureCount; i++) {
        if (!sdl->sdlTextures[i]) return i;
    }
    int id = sdl->textureCount++;
    sdl->sdlTextures = safeRealloc(sdl->sdlTextures, sdl->textureCount * sizeof(SDL_Texture*));
    sdl->textureWidths = safeRealloc(sdl->textureWidths, sdl->textureCount * sizeof(int32_t));
    sdl->textureHeights = safeRealloc(sdl->textureHeights, sdl->textureCount * sizeof(int32_t));
    g_texLastUsed = safeRealloc(g_texLastUsed, sdl->textureCount * sizeof(uint32_t));
    g_texLastUsed[id] = g_currentFrame;
    sdl->sdlTextures[id] = NULL;
    return id;
}

static int findOrAllocTpag(DataWin* dw, uint32_t orig) {
    for (uint32_t i = orig; i < dw->tpag.count; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    int id = dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[id], 0, sizeof(TexturePageItem));
    dw->tpag.items[id].texturePageId = -1;
    return id;
}

static int findOrAllocSpr(DataWin* dw, uint32_t orig) {
    for (uint32_t i = orig; i < dw->sprt.count; i++) {
        if (dw->sprt.sprites[i].textureCount == 0) return i;
    }
    int id = dw->sprt.count++;
    dw->sprt.sprites = safeRealloc(dw->sprt.sprites, dw->sprt.count * sizeof(Sprite));
    memset(&dw->sprt.sprites[id], 0, sizeof(Sprite));
    return id;
}

static int32_t sdlCreateSpriteFromSurface(Renderer* r, int32_t x, int32_t y, int32_t w, int32_t h,
                                          bool rb, bool sm, int32_t xo, int32_t yo) {
    (void)rb; (void)sm;
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;
    if (w <= 0 || h <= 0 || !sdl->fboTexture) return -1;

    SDL_Surface* tempSurf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!tempSurf) return -1;

    SDL_Rect sr = {x, y, w, h};
    SDL_RenderReadPixels(sdl->renderer, &sr, SDL_PIXELFORMAT_ABGR8888, tempSurf->pixels, tempSurf->pitch);

    SDL_Texture* newTex = SDL_CreateTextureFromSurface(sdl->renderer, tempSurf);
    SDL_SetTextureBlendMode(newTex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(tempSurf);

    int pid = findOrAllocTex(sdl);
    sdl->sdlTextures[pid] = newTex;
    sdl->textureWidths[pid] = w;
    sdl->textureHeights[pid] = h;

    int ti = findOrAllocTpag(dw, sdl->originalTpagCount);
    TexturePageItem* tp = &dw->tpag.items[ti];
    tp->sourceX = 0; tp->sourceY = 0;
    tp->sourceWidth = w; tp->sourceHeight = h;
    tp->texturePageId = pid;

    uint32_t foff = DYNAMIC_TPAG_OFFSET_BASE + ti;
    hmput(dw->tpagOffsetMap, foff, ti);

    int si = findOrAllocSpr(dw, sdl->originalSpriteCount);
    Sprite* sp = &dw->sprt.sprites[si];
    sp->width = w; sp->height = h;
    sp->originX = xo; sp->originY = yo;
    sp->textureCount = 1;
    sp->textureOffsets = safeMalloc(sizeof(uint32_t));
    sp->textureOffsets[0] = foff;

    return si;
}

static void sdlDeleteSprite(Renderer* r, int32_t idx) {
    SDLRenderer* sdl = (SDLRenderer*)r;
    DataWin* dw = r->dataWin;
    if (idx < 0 || idx >= (int)dw->sprt.count || idx < (int)sdl->originalSpriteCount) return;
    Sprite* sp = &dw->sprt.sprites[idx];
    if (!sp->textureCount) return;

    for (int i = 0; i < sp->textureCount; i++) {
        uint32_t off = sp->textureOffsets[i];
        if (off >= DYNAMIC_TPAG_OFFSET_BASE) {
            int ti = DataWin_resolveTPAG(dw, off);
            if (ti >= 0) {
                int pid = dw->tpag.items[ti].texturePageId;
                if (pid >= 0 && pid < sdl->textureCount && sdl->sdlTextures[pid]) {
                    SDL_DestroyTexture(sdl->sdlTextures[pid]);
                    sdl->sdlTextures[pid] = NULL;
                }
                dw->tpag.items[ti].texturePageId = -1;
            }
            hmdel(dw->tpagOffsetMap, off);
        }
    }
    free(sp->textureOffsets);
    memset(sp, 0, sizeof(Sprite));
}

static RendererVtable sdlVtable = {
    .init = sdlInit, .destroy = sdlDestroy, .beginFrame = sdlBeginFrame, .endFrame = sdlEndFrame,
    .beginView = sdlBeginView, .endView = sdlEndView, .drawSprite = sdlDrawSprite,
    .drawSpritePart = sdlDrawSpritePart, .drawRectangle = sdlDrawRectangle, .drawLine = sdlDrawLine,
    .drawLineColor = sdlDrawLineColor, .drawText = sdlDrawText, .flush = sdlRendererFlush,
    .createSpriteFromSurface = sdlCreateSpriteFromSurface, .deleteSprite = sdlDeleteSprite, .drawTile = NULL,
};

Renderer* SDLRenderer_create(SDL_Window* window, SDL_Renderer* sdl_renderer) {
    SDLRenderer* sdl = safeCalloc(1, sizeof(SDLRenderer));
    sdl->base.vtable = &sdlVtable;
    sdl->base.drawColor = 0xFFFFFF; sdl->base.drawAlpha = 1.0f; sdl->base.drawFont = -1;
    sdl->renderer = sdl_renderer;
    sdl->frameTimeAvg = 0.0f;
    sdl->lastTicks = 0;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // 0 = nearest
    //SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    return (Renderer*)sdl;
}