#include "data_win.h"
#include "ctr_texture_cache.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0777)
#endif

char g_current_cache_dir[256];

static bool path_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void dirname_of(const char *path, char *out, size_t outSize) {
    if (!path || !out || outSize == 0) return;
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    size_t len = slash ? (size_t)(slash - path) : 0;
    if (len == 0) {
        snprintf(out, outSize, ".");
        return;
    }
    if (len >= outSize) len = outSize - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static void join_path(char *out, size_t outSize, const char *a, const char *b) {
    if (!out || outSize == 0) return;
    size_t len = a ? strlen(a) : 0;
    const char *sep = (len > 0 && a[len - 1] != '/' && a[len - 1] != '\\') ? "/" : "";
    snprintf(out, outSize, "%s%s%s", a ? a : "", sep, b ? b : "");
}

static void cache_progress(uint32_t pageIndex, uint32_t pageCount, const char *path, void *user) {
    (void)path;
    (void)user;
    if (pageCount == 0) return;
    uint32_t pct = (pageIndex * 100u) / pageCount;
    fprintf(stderr, "\rBuilding texture cache: %3u%% (%lu/%lu)",
            pct, (unsigned long)pageIndex, (unsigned long)pageCount);
    if (pageIndex >= pageCount) fputc('\n', stderr);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <game-dir|data.win> [cache-dir]\n"
            "\n"
            "Examples:\n"
            "  %s undertale\n"
            "  %s undertale/data.win\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 2;
    }

    char dataWinPath[512];
    char gameDir[512];

    if (path_is_dir(argv[1])) {
        snprintf(gameDir, sizeof(gameDir), "%s", argv[1]);
        join_path(dataWinPath, sizeof(dataWinPath), gameDir, "data.win");
    } else {
        snprintf(dataWinPath, sizeof(dataWinPath), "%s", argv[1]);
        dirname_of(dataWinPath, gameDir, sizeof(gameDir));
    }

    if (!file_exists(dataWinPath)) {
        fprintf(stderr, "data.win not found: %s\n", dataWinPath);
        return 1;
    }

    if (argc == 3) snprintf(g_current_cache_dir, sizeof(g_current_cache_dir), "%s", argv[2]);
    else join_path(g_current_cache_dir, sizeof(g_current_cache_dir), gameDir, "cache");

    if (MKDIR(g_current_cache_dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "failed to create cache dir: %s\n", g_current_cache_dir);
        return 1;
    }

    DataWinParserOptions opt = {
        .parseGen8 = true,
        .parseSprt = true,
        .parseBgnd = true,
        .parseFont = true,
        .parseTpag = true,
        .parseTxtr = true,
        .parseStrg = true,
        .skipLoadingPreciseMasksForNonPreciseSprites = true,
        .skipTextureBlobData = false,
        .skipAudioBlobData = true,
    };

    fprintf(stderr, "Reading %s\n", dataWinPath);
    DataWin *dw = DataWin_parse(dataWinPath, opt);
    if (!dw) {
        fprintf(stderr, "failed to parse data.win\n");
        return 1;
    }

    CtrTextureCache_setProgressCallback(cache_progress, NULL);
    CtrTextureCache_prepare(dw);
    CtrTextureCache_setProgressCallback(NULL, NULL);

    char atlasPath[512];
    CtrTextureCache_indexPath(atlasPath, sizeof(atlasPath));
    bool ok = CtrTextureCache_indexIsCurrentPath(atlasPath);
    DataWin_free(dw);

    if (!ok) {
        fprintf(stderr, "cache build failed or produced stale atlas: %s\n", atlasPath);
        return 1;
    }

    fprintf(stderr, "Ready: %s\n", atlasPath);
    return 0;
}
