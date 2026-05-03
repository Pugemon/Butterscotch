#pragma once

#include "renderer.h"
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>

#define CTR_REPACK_ATLAS_SIZE   512
#define CTR_MAX_CHUNKS_X        4
#define CTR_MAX_CHUNKS_Y        4
#define CTR_TARGET_STACK_DEPTH  8

typedef struct {
    bool     valid;
    C3D_Tex  tex;
    int      srcX, srcY;
    int      width, height;
    int      potW,  potH;
} CtrAtlasChunk;

typedef struct {
    bool     loaded;
    bool     keepResident;
    int      origW, origH;
    int      chunksX, chunksY;
    CtrAtlasChunk chunks[CTR_MAX_CHUNKS_X][CTR_MAX_CHUNKS_Y];
    uint32_t lastFrame;
} CtrPage;

typedef struct {
    bool     loaded;
    bool     keepResident;
    int      width, height;
    int      potW,  potH;
    uint32_t fileOffset;
    C3D_Tex  tex;
    uint32_t lastFrame;
} CtrSourcePage;

typedef struct {
    bool     valid;
    uint32_t segmentStart;
    uint32_t segmentCount;
} CtrCachedTpag;

typedef struct {
    uint32_t atlasIndex;
    uint16_t sourceX, sourceY;
    uint16_t width, height;
    uint16_t atlasX, atlasY;
} CtrCachedSegment;

typedef struct {
    bool              used;
    int               width, height;
    int               potW,  potH;
    C3D_Tex           tex;
    C3D_RenderTarget *target;
} CtrSurface;

typedef struct {
    C3D_RenderTarget *target;
    int               viewport[4];
    bool              hasScissor;
    int               scissor[4];
    C3D_Mtx           projection;
} CtrTargetState;

typedef struct CtrRenderer {
    Renderer base;

    // Citro3D shader pipeline
    int              uLoc_projection;
    C3D_AttrInfo     attrInfo;
    bool             pipelineReady;

    // Screen render targets
    C3D_RenderTarget *topTarget;     // GFX_TOP
    C3D_RenderTarget *bottomTarget;  // GFX_BOTTOM
    bool              gfxOwned;

    // Game-sized render target
    bool              appReady;
    int               appLogicW, appLogicH;
    int               appPotW,   appPotH;
    C3D_Tex           appTex;
    C3D_RenderTarget *appTarget;
    bool              appFrameCleared;

    // Screen size
    int               winW, winH;
    int               gameW, gameH;

    // Texture cache
    CtrPage          *pages;
    uint32_t          pageCount;
    uint32_t          originalTpagCount;
    uint32_t          originalSpriteCount;
    CtrSourcePage    *sourcePages;
    uint32_t          sourcePageCount;
    uint32_t          repackBasePageId;
    uint32_t          repackPageCount;
    CtrCachedTpag    *cacheItems;
    uint32_t          cacheItemCount;
    CtrCachedSegment *cacheSegments;
    uint32_t          cacheSegmentCount;

    // Game surfaces
    CtrSurface       *surfaces;
    uint32_t          surfaceCount;

    // Vertex batch
    void             *vbuf;          // linearAlloc
    uint32_t          vbufCap;
    uint32_t          vbufHead;
    uint32_t          batchStart;
    uint32_t          batchVerts;
    C3D_Tex          *batchTex;

    // Solid-color texture
    C3D_Tex           whiteTex;

    // Target stack
    CtrTargetState    targetStack[CTR_TARGET_STACK_DEPTH];
    int               targetStackDepth;

    // Current target state
    C3D_RenderTarget *activeTarget;
    C3D_Mtx           currentProjection;
    int               currentViewport[4];

    // Frame state
    bool              inFrame;
    int               currentBlendMode;
} CtrRenderer;

Renderer *CtrRenderer_create(void);

typedef void (*CtrRendererCacheProgressFn)(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user);

void CtrRenderer_setCacheProgressCallback(CtrRendererCacheProgressFn callback, void *user);

// Standalone texture cache build. Не трогает Citro3D state/pipeline — можно
// безопасно вызывать пока активен другой рендер (например, лаунчер).
void CtrRenderer_prepareTextureCache(DataWin *dw);

void CtrRenderer_prefetchSprite(Renderer *ren, int32_t sprIdx);

// ---- Live appearance / layout controls --------------------------------------

typedef enum {
    CTR_GAME_SCREEN_TOP    = 0,
    CTR_GAME_SCREEN_BOTTOM = 1
} CtrGameScreen;

typedef enum {
    CTR_BACKDROP_GRADIENT = 0,
    CTR_BACKDROP_BLUR,
    CTR_BACKDROP_BLACK,
    CTR_BACKDROP_STRETCH,
} CtrBackdropMode;

void CtrRenderer_setGameScreen(CtrGameScreen which);
CtrGameScreen CtrRenderer_getGameScreen(void);
void CtrRenderer_setBackdropMode(CtrBackdropMode mode);
CtrBackdropMode CtrRenderer_getBackdropMode(void);

void CtrRenderer_setLetterboxTheme(float topR, float topG, float topB,
                                   float botR, float botG, float botB,
                                   float accentR, float accentG, float accentB,
                                   float blurAlpha, float particleAlpha);

C3D_RenderTarget *CtrRenderer_getTopTarget(Renderer *ren);
C3D_RenderTarget *CtrRenderer_getBottomTarget(Renderer *ren);
