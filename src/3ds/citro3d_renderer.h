//
// Created by Pugemon on 27.03.2026.
//

#pragma once

#include "renderer.h"

#ifdef __3DS__
#include <c3d/renderqueue.h>

#define MAX_CACHED_TEXTURES 8 // Для 3DS 8-10 текстур (1024x1024 в 16-бит) — это безопасный максимум
#define MAX_VERTICES  8192
#define CLEAR_COLOR 0x6B3838 // Настоящий черный (R=00, G=00, B=00, A=FF)

// ─────────────────────────────────────────────
// Структуры
// ─────────────────────────────────────────────

typedef struct {
    float x, y, z;
    float u, v;
    u32   color;   // ABGR
} C3D_Vertex;

typedef struct {
    Renderer          base;
    C3D_RenderTarget *target;

    DVLB_s           *dvlb;
    shaderProgram_s   shader;
    int8_t            uLoc_projection;
    int8_t            uLoc_modelview;

    // Текстуры
    C3D_Tex  *textures;
    int      *texRealW;   // исходная ширина до POT-округления
    int      *texRealH;   // исходная высота до POT-округления
    uint32_t  texCount;
    int       whiteTexIndex;

    // Для динамического кэша
    uint32_t *texLastUsed;
    uint32_t  currentFrame;

    // Батч
    C3D_Vertex *vboData;
    int         vertexCount;
    int         batchStart;
    int         currentTexIndex;

    C3D_Mtx projection;
} Citro3dRenderer;

// Создает экземпляр рендерера, использующего аппаратное ускорение 3DS (Citro3D)
Renderer* Citro3dRenderer_create(void);
#endif