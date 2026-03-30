//
// Created by Pugemon on 30.03.2026.
//

#pragma once

#ifdef __3DS__

typedef struct Citro3dRenderer Citro3dRenderer;

bool TexArchive_open(Citro3dRenderer *c3d, const char *path);
void TexArchive_close(Citro3dRenderer *c3d);
void ensureAtlasLoaded(Citro3dRenderer *c3d, int atlasId);

#endif


