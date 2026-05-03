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
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "ctr_renderer.h"
#include "ctr_file_system.h"
#include "sdl12_audio_system.h"
#include "render2d_shader_shbin.h"
#include "launcher.h"
u32 __ctru_heap_size        = 35 * 1024 * 1024;
u32 __ctru_linear_heap_size = 25 * 1024 * 1024;
u32 __stacksize__           = 64 * 1024;

char g_current_data_path[256];

char g_current_cache_dir[256];

char g_next_game_path[256] = "";

bool g_game_change_requested = false;

//Shaders
DVLB_s *g_vshaderDvlb = NULL;

shaderProgram_s g_shaderProg;

static void map_key(RunnerKeyboardState *kb, u32 down, u32 up, u32 held, u32 mask, int gml) {
    if (down & mask)             RunnerKeyboard_onKeyDown(kb, gml);
    else if ((up & mask) && !(held & mask)) RunnerKeyboard_onKeyUp(kb, gml);
}

static void setup_logging(void) {
    freopen("sdmc:/3ds/butter_out.txt", "w", stdout);
    freopen("sdmc:/3ds/butter_err.txt", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

#include <malloc.h>
void printMemoryStats() {
    struct mallinfo mi = mallinfo();

    u32 linearFree = linearSpaceFree();

    float heapUsedMB = (float)mi.uordblks / 1024.0f / 1024.0f;
    float linearFreeMB = (float)linearFree / 1024.0f / 1024.0f;

    printf("[MEMORY] Heap Used: %.2f MB | LINEAR RAM FREE: %.2f MB\n",
           heapUsedMB, linearFreeMB);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    setup_logging();
    cfguInit();
    gfxInitDefault();
    gfxSet3D(false);

    PrintConsole bottomConsole;
    consoleInit(GFX_BOTTOM, &bottomConsole);

    APT_SetAppCpuTimeLimit(30);

    osSetSpeedupEnable(1);

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        printf("C3D_Init failed!\n");
        gfxExit();
        return 1;
    }

    g_vshaderDvlb = DVLB_ParseFile((u32 *)render2d_shader_shbin, render2d_shader_shbin_size);
    shaderProgramInit(&g_shaderProg);
    shaderProgramSetVsh(&g_shaderProg, &g_vshaderDvlb->DVLE[0]);

    LauncherGfx gfx;
    bool gfx_ready = launcher_gfx_init(&gfx);

    int selected_game = run_launcher_menu(&gfx);
    if (selected_game < 0) {
        if (gfx_ready) launcher_gfx_destroy(&gfx);
        free_game_icons();

        shaderProgramFree(&g_shaderProg);
        DVLB_Free(g_vshaderDvlb);

        C3D_Fini();
        cfguExit();
        gfxExit();
        return 0;
    }

    strncpy(g_current_data_path, g_games[selected_game].path, 255);
    g_current_data_path[255] = '\0';

    bool keep_playing = true;

    while (keep_playing && aptMainLoop()) {
        g_game_change_requested = false;
        int frameCounter = 0;
        char base_game_dir[256];
        char *slash = strrchr(g_current_data_path, '/');
        size_t baselen = slash ? (size_t)(slash - g_current_data_path) : 0;
        if (baselen >= sizeof(base_game_dir)) baselen = sizeof(base_game_dir) - 1;
        memcpy(base_game_dir, g_current_data_path, baselen);
        base_game_dir[baselen] = '\0';

        snprintf(g_current_cache_dir, sizeof(g_current_cache_dir), "%s/cache", base_game_dir);
        mkdir(g_current_cache_dir, 0777);

        char code_cache_path[256];
        snprintf(code_cache_path, sizeof(code_cache_path), "%s/code.cache", g_current_cache_dir);

        char cache_flag_path[256];
        snprintf(cache_flag_path, sizeof(cache_flag_path), "%s/cache_ready.flag", g_current_cache_dir);

        consoleClear();
        const char *display_name = strrchr(base_game_dir, '/');
        display_name = display_name ? display_name + 1 : base_game_dir;
        printf("\x1b[10;5H\x1b[32mLoading %s...\x1b[0m\n", display_name);

        free_game_icons();

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name, "PREPARING", 0, 0, 2.f);
        }

        FILE *flag = fopen(cache_flag_path, "r");
        bool cached = flag != NULL;
        if (flag) fclose(flag);

        if (!cached) {
            printf("\n\nGenerating texture cache...\nThis may take a minute.");
            if (gfx_ready) {
                launcher_render_loading(&gfx, display_name, "READING TEXTURE TABLE", 0, 0, 4.f);
            }
            LoadingScreenState prePassState = {
                .gfx = gfx_ready ? &gfx : NULL,
                .gameName = display_name,
                .basePercent = 4.f,
                .spanPercent = 4.f
            };
            DataWinParserOptions opt = {
                .parseGen8=1, .parseTpag=1, .parseTxtr=1, .skipTextureBlobData=1,
                .progressCallback = gfx_ready ? launcher_datawin_progress : NULL,
                .progressCallbackUserData = &prePassState
            };
            DataWin *dw = DataWin_parse(g_current_data_path, opt);
            if (dw) {
                LoadingScreenState cacheState = {
                    .gfx = gfx_ready ? &gfx : NULL,
                    .gameName = display_name,
                    .basePercent = 8.f,
                    .spanPercent = 76.f
                };
                CtrRenderer_setCacheProgressCallback(gfx_ready ? launcher_cache_progress : NULL, &cacheState);
                CtrRenderer_prepareTextureCache(dw);
                CtrRenderer_setCacheProgressCallback(NULL, NULL);
                DataWin_free(dw);
            }
            flag = fopen(cache_flag_path, "r");
            cached = flag != NULL;
            if (flag) fclose(flag);
        }

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name,
                                    cached ? "CACHE READY" : "CACHE SKIPPED", 0, 0,
                                    cached ? 84.f : 12.f);
        }

        float fullBase = cached ? 84.f : 12.f;
        LoadingScreenState fullParseState = {
            .gfx = gfx_ready ? &gfx : NULL,
            .gameName = display_name,
            .basePercent = fullBase,
            .spanPercent = 99.f - fullBase
        };
        DataWinParserOptions full_opt = {
            .parseGen8=1, .parseOptn=1, .parseLang=1, .parseExtn=1, .parseSond=1,
            .parseAgrp=1, .parseSprt=1, .parseBgnd=1, .parsePath=1, .parseScpt=1,
            .parseGlob=1, .parseShdr=1, .parseFont=1, .parseTmln=1, .parseObjt=1,
            .parseRoom=1, .parseTpag=1, .parseCode=1, .parseVari=1, .parseFunc=1,
            .parseStrg=1, .parseTxtr=1, .parseAudo=1,
            .skipLoadingPreciseMasksForNonPreciseSprites=1,
            .skipTextureBlobData=cached, .skipAudioBlobData=1,

            .lazyLoadRooms=0,
            .codeCachePath=code_cache_path,
            .progressCallback = gfx_ready ? launcher_datawin_progress : NULL,
            .progressCallbackUserData = &fullParseState
        };

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name, "LOADING DATA.WIN", 0, 0, fullBase);
        }
        DataWin *dw = DataWin_parse(g_current_data_path, full_opt);
        if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0 || !dw) {
            printf("\nBoot failed.\nCheck data.win at %s\nPress START to quit.\n", g_current_data_path);
            while (aptMainLoop()) {
                hidScanInput();
                if (hidKeysDown() & KEY_START) break;
                gspWaitForVBlank();
            }
            if (dw) DataWin_free(dw);
            break;
        }

        fprintf(stderr, "Loaded \"%s\" (ID: %d)\n", dw->gen8.name, dw->gen8.gameID);

        if (gfx_ready) {
            launcher_render_loading(&gfx, display_name, "LAUNCHING GAME!", 0, 0, 100.f);
        }

        if (gfx_ready) { launcher_gfx_destroy(&gfx); gfx_ready = false; }

        VMContext      *vm  = VM_create(dw);
        N3dsFileSystem *fs  = N3dsFileSystem_create(g_current_data_path);
        Renderer       *ren = CtrRenderer_create();
        AudioSystem    *snd = SdlMixerAudioSystem_create();
        if (snd) snd->dataWin = dw;

        Runner *run = Runner_create(dw, vm, ren, (FileSystem *)fs, snd);
        run->osType = OS_WINDOWS;

        Runner_initFirstRoom(run);

        while (aptMainLoop() && !run->shouldExit) {
            u64 t_start = osGetTime();
            hidScanInput();
            u32 d = hidKeysDown(), u = hidKeysUp(), h = hidKeysHeld();

            if ((h & KEY_START) && (h & KEY_SELECT) && (h & KEY_A)) {
                printf("Ret to launcher\n");
                g_game_change_requested = false;
                run->shouldExit = true;
            }

            RunnerKeyboard_beginFrame(run->keyboard);
            map_key(run->keyboard, d, u, h, KEY_CPAD_UP    | KEY_DUP,    VK_UP);
            map_key(run->keyboard, d, u, h, KEY_CPAD_DOWN  | KEY_DDOWN,  VK_DOWN);
            map_key(run->keyboard, d, u, h, KEY_CPAD_LEFT  | KEY_DLEFT,  VK_LEFT);
            map_key(run->keyboard, d, u, h, KEY_CPAD_RIGHT | KEY_DRIGHT, VK_RIGHT);
            map_key(run->keyboard, d, u, h, KEY_A,          'Z');
            map_key(run->keyboard, d, u, h, KEY_B,          'X');
            map_key(run->keyboard, d, u, h, KEY_X,          'C');
            map_key(run->keyboard, d, u, h, KEY_Y,          VK_SHIFT);
            map_key(run->keyboard, d, u, h, KEY_L,          VK_ENTER);
            map_key(run->keyboard, d, u, h, KEY_R,          VK_SPACE);
            map_key(run->keyboard, d, u, h, KEY_SELECT,     VK_ESCAPE);

            Runner_step(run);
            if (run->audioSystem)
                run->audioSystem->vtable->update(run->audioSystem, 1.f / 30.f);

            Room *rm = run->currentRoom;
            int gw = dw->gen8.defaultWindowWidth;
            int gh = dw->gen8.defaultWindowHeight;
            bool views_en = rm->flags & 1;

            if (views_en) {
                int maxR = 0, maxB = 0;
                for (int i = 0; i < MAX_VIEWS; i++) {
                    if (!run->views[i].enabled) continue;
                    int r = run->views[i].portX + run->views[i].portWidth;
                    int b = run->views[i].portY + run->views[i].portHeight;
                    if (r > maxR) maxR = r;
                    if (b > maxB) maxB = b;
                }
                if (maxR > 0 && maxB > 0) { gw = maxR; gh = maxB; }
            }

            ren->vtable->beginFrame(ren, gw, gh, 400, 240);
            if (run->drawBackgroundColor) {
                ren->vtable->clearTarget(ren, run->backgroundColor, 1.f);
            }

            bool drawn = false;
            if (views_en) {
                for (int i = 0; i < MAX_VIEWS; i++) {
                    RuntimeView *v = &run->views[i];
                    if (!v->enabled) continue;
                    run->viewCurrent = i;

                    ren->vtable->beginView(ren, v->viewX, v->viewY, v->viewWidth, v->viewHeight,
                                           v->portX, v->portY, v->portWidth, v->portHeight, v->viewAngle);
                    Runner_draw(run);
                    ren->vtable->endView(ren);

                    ren->vtable->beginGUI(ren,
                                          run->guiWidth  > 0 ? run->guiWidth  : v->portWidth,
                                          run->guiHeight > 0 ? run->guiHeight : v->portHeight,
                                          v->portX, v->portY, v->portWidth, v->portHeight);
                    Runner_drawGUI(run);
                    ren->vtable->endGUI(ren);
                    ren->vtable->flush(ren);
                    drawn = true;
                }
            }

            if (!drawn) {
                run->viewCurrent = 0;
                ren->vtable->beginView(ren, 0, 0, gw, gh, 0, 0, gw, gh, 0.f);
                Runner_draw(run);
                ren->vtable->endView(ren);

                ren->vtable->beginGUI(ren,
                                      run->guiWidth  > 0 ? run->guiWidth  : gw,
                                      run->guiHeight > 0 ? run->guiHeight : gh,
                                      0, 0, gw, gh);
                Runner_drawGUI(run);
                ren->vtable->endGUI(ren);
                ren->vtable->flush(ren);
            }

            run->viewCurrent = 0;
            ren->vtable->endFrame(ren);
            if (frameCounter % 60 == 0) {
                printMemoryStats();
            }
            frameCounter++;

            while (osGetTime() - t_start < 33) gspWaitForVBlank();
        }

        run->audioSystem->vtable->destroy(run->audioSystem);
        ren->vtable->destroy(ren);
        Runner_free(run);
        N3dsFileSystem_destroy(fs);
        VM_free(vm);
        DataWin_free(dw);
        SDL_Quit();

        if (!gfx_ready) gfx_ready = launcher_gfx_init(&gfx);

        if (g_game_change_requested) {
            resolve_new_game_path(g_next_game_path, g_current_data_path);
        } else {
            int new_selection = run_launcher_menu(&gfx);
            if (new_selection < 0) {
                keep_playing = false;
            } else {
                strncpy(g_current_data_path, g_games[new_selection].path, 255);
                g_current_data_path[255] = '\0';
            }
        }
    }

    if (gfx_ready) launcher_gfx_destroy(&gfx);
    free_game_icons();

    shaderProgramFree(&g_shaderProg);
    DVLB_Free(g_vshaderDvlb);

    C3D_Fini();
    cfguExit();
    gfxExit();
    return 0;
}
