//
// Created by Notebook on 03.05.2026.
//

#ifndef BUTTERSCOTCH_LAUNCHER_H
#define BUTTERSCOTCH_LAUNCHER_H
#include <3ds.h>
#include <citro3d.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <math.h>
#include <ctype.h>

#include "icon_parse.h"
#include "data_win.h"

#define BASE_DIR  "sdmc:/3ds/butterscotch"

#define MAX_GAMES 64


typedef struct {
    char name[64];
    char path[256];
    char exe_path[256];
    bool icon_ready;
    int icon_w, icon_h;
    int icon_pot_w, icon_pot_h;
    C3D_Tex icon_tex;
} GameEntry;

static GameEntry g_games[MAX_GAMES];
static int       g_game_count = 0;

#define LAUNCHER_W 400
#define LAUNCHER_H 240
#define LAUNCHER_VBUF_CAP (8192 * 3)
#define LAUNCHER_DISPLAY_TRANSFER_FLAGS \
(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {
    float x, y, z;
    float u, v;
    float r, g, b, a;
} LauncherVertex;

typedef struct {
    C3D_RenderTarget *target;
    int uLoc_projection;
    C3D_AttrInfo attrInfo;
    C3D_Tex whiteTex;
    LauncherVertex *vbuf;
    uint32_t vbufCap;
    uint32_t vbufHead;
    uint32_t batchStart;
    uint32_t batchVerts;
    C3D_Tex *batchTex;
    bool ready;
    bool inFrame;
} LauncherGfx;

typedef struct {
    LauncherGfx *gfx;
    const char *gameName;
    float basePercent;
    float spanPercent;
} LoadingScreenState;

int run_launcher_menu(LauncherGfx *gfx);

bool launcher_gfx_init(LauncherGfx *gfx);

void launcher_gfx_destroy(LauncherGfx *gfx);

//Progress statesments
void launcher_render_loading(LauncherGfx *gfx, const char *gameName, const char *stage,
                                    int page, int total, float percent);

void launcher_datawin_progress(const char *chunkName, int chunkIndex, int totalChunks,
                                      DataWin *dw, void *user);

void launcher_cache_progress(uint32_t pageIndex, uint32_t pageCount, const char *pagePath, void *user);

//Utils
void resolve_new_game_path(const char *request, char *out_path);

void free_game_icons(void);
#endif //BUTTERSCOTCH_LAUNCHER_H