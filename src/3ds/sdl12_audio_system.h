#pragma once

#include "../audio_system.h"
#include <SDL/SDL_mixer.h>
#include <stdio.h>

#define SND_ID_BASE 100000
#define MUS_ID_BASE 200000

typedef struct {
    AudioSystem base;
    FileSystem *fs;

    // Per-sound caches indexed by SOND id (base.dataWin->sond.count entries).
    Mix_Chunk **chunks;
    Mix_Music **music;
    uint8_t **musicBuf; // raw ogg/mp3 bytes backing a Mix_Music (linearAlloc)
    void **sfxBuf; // linearAlloc'd PCM backing a Mix_QuickLoad_RAW chunk
    float *pitches;
    uint32_t *lastUsed;
    // One bit per SOND id: set the first time the game asks to play it.
    // Used by sys_on_room_changed to warm-load anything the player has
    // already heard at least once, eliminating the in-battle stutter.
    uint8_t *playedOnce;

    // Per-audiogroup archive paths. archivePaths[N] is the file the AUDO offsets
    // for audioGroups[N] resolve into. archivePaths[0] = data.win/game.unx.
    // Grown lazily by groupLoad. Stored as a stb_ds dynamic array.
    char **archivePaths;

    FILE **archiveFiles;

    int32_t curMusicId;
    uint32_t frame;
} SysMixer;

AudioSystem *SdlMixerAudioSystem_create(void);

// One-time audio backend init (SDL audio + Mix_OpenAudio + Mix_Init). Safe to
// call multiple times; subsequent calls are no-ops. Tearing the mixer down
// between games is unreliable on 3DS so we keep it alive for the whole app.
bool SdlMixer_globalInit(void);

void SdlMixer_globalShutdown(void);

static bool is_track_music(Sound *snd, uint32_t dataSize);
