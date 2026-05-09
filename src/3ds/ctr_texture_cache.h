#pragma once

#include "data_win.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTR_TEXTURE_CACHE_MAGIC      0x4B415052u
#define CTR_TEXTURE_CACHE_VERSION    15u
#define CTR_TEXTURE_CACHE_ATLAS_SIZE 512u
#define CTR_TEXTURE_CACHE_FILE       "atlas.bin"
#define CTR_TEXTURE_CACHE_READY_FLAG "cache_ready_v16.flag"

#define CTR_TEXTURE_CACHE_FORMAT_RGBA4 4u
#define CTR_TEXTURE_CACHE_FORMAT_LA4   9u
#define CTR_TEXTURE_CACHE_FORMAT_A4    11u
#define CTR_TEXTURE_CACHE_FORMAT_ETC1   12u
#define CTR_TEXTURE_CACHE_FORMAT_ETC1A4 13u

typedef struct CtrRenderer CtrRenderer;

typedef void (*CtrTextureCacheProgressFn)(uint32_t pageIndex, uint32_t pageCount,
                                          const char *path, void *user);

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t tpagCount;
    uint32_t basePageId;
    uint32_t atlasCount;
    uint32_t atlasSize;
    uint32_t segmentCount;
    uint32_t itemsOffset;
    uint32_t segmentsOffset;
    uint32_t atlasInfosOffset;
    uint32_t atlasDataOffset;
    uint64_t srcSize;
    uint64_t srcMtime;
    uint32_t srcSampleHash;
    uint32_t reserved;
} CtrTextureCacheHeader;

void CtrTextureCache_indexPath(char *out, size_t outSize);

bool CtrTextureCache_indexIsCurrentPath(const char *path);

void CtrTextureCache_setProgressCallback(CtrTextureCacheProgressFn callback, void *user);

void CtrTextureCache_prepare(DataWin *dw);

bool CtrTextureCache_apply(CtrRenderer *ctx, DataWin *dw);
