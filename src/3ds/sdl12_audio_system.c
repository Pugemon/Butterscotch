#include "sdl12_audio_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <malloc.h>
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>

#include "stb_ds.h"

// ===[ Globals ]===
// SDL_mixer + SDL audio subsystem are initialised once per app lifetime. Tear
// down is deferred to SdlMixer_globalShutdown at exit because Mix_OpenAudio /
// Mix_CloseAudio cycles inside the same process are not reliable on 3DS — the
// second Mix_OpenAudio used to crash inside Mix_HaltMusic when launching a
// second game. Keeping the mixer alive avoids the issue entirely.
static bool s_mixerReady = false;

bool SdlMixer_globalInit(void) {
    if (s_mixerReady) return true;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "[AUDIO] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
    }
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        fprintf(stderr, "[AUDIO] Mix_OpenAudio failed: %s\n", Mix_GetError());
        return false;
    }
    Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    s_mixerReady = true;
    fprintf(stderr, "[AUDIO] SDL_mixer ready (44100/stereo/4096)\n");
    return true;
}

void SdlMixer_globalShutdown(void) {
    if (!s_mixerReady) return;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    Mix_CloseAudio();
    Mix_Quit();
    s_mixerReady = false;
}

#define MAX_CHANS 32
#define STREAM_THRES (512 * 1024)
#define MAX_CACHE 64

// ===[ Helpers ]===

static DataWin* groupOf(SysMixer *sm, int32_t groupIndex) {
    if (groupIndex < 0) return NULL;
    if ((size_t) groupIndex >= arrlenu(sm->base.audioGroups)) return NULL;
    return sm->base.audioGroups[groupIndex];
}

static const char* archiveOf(SysMixer *sm, int32_t groupIndex) {
    if (groupIndex < 0) return NULL;
    if ((size_t) groupIndex >= arrlenu(sm->archivePaths)) return NULL;
    return sm->archivePaths[groupIndex];
}

static bool readEntryBytes(const char *archive, uint32_t offset, uint32_t size, uint8_t *out) {
    if (!archive || !size || !out) return false;
    FILE *fp = fopen(archive, "rb");
    if (!fp) {
        fprintf(stderr, "[AUDIO] open archive failed: %s\n", archive);
        return false;
    }
    if (fseek(fp, offset, SEEK_SET) != 0) { fclose(fp); return false; }
    size_t got = fread(out, 1, size, fp);
    fclose(fp);
    if (got != size) {
        fprintf(stderr, "[AUDIO] short read %u/%u from %s @%u\n",
                (unsigned) got, (unsigned) size, archive, (unsigned) offset);
        return false;
    }
    return true;
}

// Drop the oldest finished cached chunk so we don't blow heap on big games.
static void evict_old(SysMixer *sm) {
    int cnt = 0;
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (sm->chunks[i]) cnt++;
    }
    if (cnt < MAX_CACHE) return;

    int victim = -1;
    uint32_t oldest = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (!sm->chunks[i]) continue;
        bool playing = false;
        for (int c = 0; c < MAX_CHANS; c++) {
            if (Mix_Playing(c) && Mix_GetChunk(c) == sm->chunks[i]) {
                playing = true;
                break;
            }
        }
        if (!playing && sm->lastUsed[i] < oldest) {
            oldest = sm->lastUsed[i];
            victim = (int) i;
        }
    }

    if (victim >= 0) {
        Mix_FreeChunk(sm->chunks[victim]);
        sm->chunks[victim] = NULL;
        if (sm->sfxBuf[victim]) {
            linearFree(sm->sfxBuf[victim]);
            sm->sfxBuf[victim] = NULL;
        }
    }
}

// Read a SFX entry from its audiogroup archive into a Mix_Chunk.
static bool load_sfx(SysMixer *sm, int id, AudioEntry *ent, const char *archive) {
    evict_old(sm);

    uint8_t *raw = linearAlloc(ent->dataSize);
    if (!raw) {
        fprintf(stderr, "[AUDIO] linearAlloc(%u) failed for sfx %d\n",
                (unsigned) ent->dataSize, id);
        return false;
    }
    if (!readEntryBytes(archive, ent->dataOffset, ent->dataSize, raw)) {
        linearFree(raw);
        return false;
    }

    SDL_RWops *rw = SDL_RWFromConstMem(raw, ent->dataSize);
    sm->chunks[id] = Mix_LoadWAV_RW(rw, 1);
    linearFree(raw);
    return sm->chunks[id] != NULL;
}

// GMS Sound::flags layout (per UndertaleModTool):
//   bit 0 (0x01): IsEmbedded         — sample lives in AUDO chunk
//   bit 1 (0x02): IsCompressed       — OGG/MP3 instead of raw PCM
//   "Regular" mask is 0x64 (= bits 6 + 5 + 2). When ALL of those bits are set
//   the sound is a regular cached SFX; when any of them are missing it is a
//   streamed BGM track. Without this check we used to misclassify long
//   embedded SFX (impacts, ambient loops, voice clips > 512 KB) as music
//   tracks, which then cannibalised the actual BGM through stop_other_music.
static inline bool sound_is_streamed(const Sound *snd) {
    return (snd->flags & 0x64u) != 0x64u;
}

// LRU-trim the cached MUSIC tracks down to a small cap. We never touch the
// currently-playing music here — Mix_PlayMusic naturally replaces it when a
// new track is started, so there's no reason to halt it pre-emptively.
//
// Old behaviour freed every other music slot eagerly (and Halted the playing
// one if its slot was being freed) — that meant every newly-loaded streamed
// track would kill BGM mid-play, which is exactly the symptom the user kept
// hitting (song stops after a flurry of SFX activity).
#define MAX_CACHED_MUSIC 3

static void evict_old_music(SysMixer *sm, int keepId) {
    int cnt = 0;
    for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
        if (sm->music[i]) cnt++;
    }
    if (cnt <= MAX_CACHED_MUSIC) return;

    while (cnt > MAX_CACHED_MUSIC) {
        int victim = -1;
        uint32_t oldest = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
            if (!sm->music[i]) continue;
            if ((int) i == keepId) continue;
            if (sm->curMusicId == (int32_t) i) continue; // never evict the playing track
            if (sm->lastUsed[i] < oldest) {
                oldest = sm->lastUsed[i];
                victim = (int) i;
            }
        }
        if (victim < 0) break; // nothing safe to drop

        Mix_FreeMusic(sm->music[victim]);
        sm->music[victim] = NULL;
        if (sm->musicBuf[victim]) {
            free(sm->musicBuf[victim]);
            sm->musicBuf[victim] = NULL;
        }
        cnt--;
    }
}

static bool ensure_snd(SysMixer *sm, int id) {
    if (!s_mixerReady || id < 0 || (uint32_t) id >= sm->base.dataWin->sond.count) return false;
    sm->lastUsed[id] = sm->frame;
    if (sm->chunks[id] || sm->music[id]) return true;

    Sound *snd = &sm->base.dataWin->sond.sounds[id];

    // Embedded sounds live in the AUDO chunk of their audiogroup.
    if (snd->flags & 1) {
        DataWin *gw = groupOf(sm, snd->audioGroup);
        const char *archive = archiveOf(sm, snd->audioGroup);
        if (!gw || !archive) {
            // Group not loaded yet — most games call audio_group_load() lazily
            // before playing sounds from a group. Log once so we can spot games
            // that forget to do that.
            static int warned[16] = {0};
            int idx = (snd->audioGroup >= 0 && snd->audioGroup < 16) ? snd->audioGroup : 0;
            if (!warned[idx]) {
                warned[idx] = 1;
                fprintf(stderr, "[AUDIO] sound '%s' wants group %d but it isn't loaded\n",
                        snd->name ? snd->name : "?", snd->audioGroup);
            }
            return false;
        }
        if (snd->audioFile < 0 || (uint32_t) snd->audioFile >= gw->audo.count) {
            fprintf(stderr, "[AUDIO] sound '%s' bad audioFile %d (group %d has %u)\n",
                    snd->name ? snd->name : "?", snd->audioFile, snd->audioGroup,
                    (unsigned) gw->audo.count);
            return false;
        }

        AudioEntry *ent = &gw->audo.entries[snd->audioFile];
        if (!ent->dataSize) return false;

        // GMS-flag-driven classification (see sound_is_streamed comment).
        // Size threshold remains as a safety net for sounds that DON'T have
        // proper Regular flagging (older bytecode or hand-rolled mods) so
        // we don't decode a 50 MB OGG into RAM by mistake — but we no longer
        // demote a long Regular SFX into the music slot, which is what was
        // killing BGM in Deltarune/Undertale on this port.
        bool asStream = sound_is_streamed(snd) && ent->dataSize > STREAM_THRES;

        if (asStream) {
            evict_old_music(sm, id);

            uint8_t *mb = malloc(ent->dataSize);
            if (!mb) {
                fprintf(stderr, "[AUDIO] malloc(%u) failed for music %d\n",
                        (unsigned) ent->dataSize, id);
                return false;
            }
            if (!readEntryBytes(archive, ent->dataOffset, ent->dataSize, mb)) {
                free(mb);
                return false;
            }

            SDL_RWops *rw = SDL_RWFromConstMem(mb, ent->dataSize);
            sm->music[id] = Mix_LoadMUS_RW(rw);
            if (sm->music[id]) {
                sm->musicBuf[id] = mb;
            } else {
                fprintf(stderr, "[AUDIO] Mix_LoadMUS_RW failed for '%s': %s\n",
                        snd->name ? snd->name : "?", Mix_GetError());
                free(mb);
            }
        } else {
            if (!load_sfx(sm, id, ent, archive)) {
                fprintf(stderr, "[AUDIO] load_sfx failed for '%s' (group %d, file %d)\n",
                        snd->name ? snd->name : "?", snd->audioGroup, snd->audioFile);
            }
        }
    } else {
        // External file referenced via Sound::file (no AUDO entry).
        if (!snd->file || !snd->file[0]) return false;
        char name[512];
        snprintf(name, sizeof(name), "%s%s", snd->file,
                 strchr(snd->file, '.') ? "" : ".ogg");
        char *path = sm->fs->vtable->resolvePath(sm->fs, name);
        if (path) {
            // External .wav -> always SFX. Anything else (typically .ogg
            // per the synth above) is treated as a stream UNLESS the GMS
            // Regular flag explicitly tags it as a regular sound, in which
            // case we still pull it into a Mix_Chunk so it doesn't take
            // over the music channel.
            if (strstr(path, ".wav") || !sound_is_streamed(snd)) {
                sm->chunks[id] = Mix_LoadWAV(path);
            } else {
                evict_old_music(sm, id);
                sm->music[id] = Mix_LoadMUS(path);
            }
            free(path);
        }
    }
    return sm->chunks[id] || sm->music[id];
}

// ===[ Vtable ]===

static void sys_init(AudioSystem *sys, DataWin *dw, FileSystem *fs) {
    SysMixer *sm = (SysMixer *) sys;
    sm->base.dataWin = dw;
    sm->fs = fs;

    if (!SdlMixer_globalInit()) return;

    Mix_AllocateChannels(MAX_CHANS);

    int cnt = (int) dw->sond.count;
    sm->chunks   = calloc(cnt, sizeof(Mix_Chunk *));
    sm->music    = calloc(cnt, sizeof(Mix_Music *));
    sm->musicBuf = calloc(cnt, sizeof(uint8_t *));
    sm->sfxBuf   = calloc(cnt, sizeof(void *));
    sm->lastUsed = calloc(cnt, sizeof(uint32_t));
    sm->pitches  = calloc(cnt, sizeof(float));
    for (int i = 0; i < cnt; i++) sm->pitches[i] = 1.0f;

    sm->curMusicId = -1;
    sm->frame = 1;

    // Group 0: the main data.win itself. Resolve its on-disk path so AUDO
    // offsets can be turned into byte ranges later.
    char *primary = fs->vtable->resolvePath(fs, "data.win");
    if (!primary) primary = fs->vtable->resolvePath(fs, "game.unx");
    arrput(sm->base.audioGroups, dw);
    arrput(sm->archivePaths, primary);
}

static void sys_destroy(AudioSystem *sys) {
    SysMixer *sm = (SysMixer *) sys;

    if (s_mixerReady) {
        Mix_HaltChannel(-1);
        Mix_HaltMusic();

        for (uint32_t i = 0; i < sm->base.dataWin->sond.count; i++) {
            if (sm->chunks[i])   Mix_FreeChunk(sm->chunks[i]);
            if (sm->music[i])    Mix_FreeMusic(sm->music[i]);
            if (sm->musicBuf[i]) free(sm->musicBuf[i]);
            if (sm->sfxBuf[i])   linearFree(sm->sfxBuf[i]);
        }
    }

    free(sm->chunks);
    free(sm->music);
    free(sm->musicBuf);
    free(sm->sfxBuf);
    free(sm->lastUsed);
    free(sm->pitches);

    // Skip group 0 (owned by main); free everything we loaded ourselves.
    if (arrlen(sm->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(sm->base.audioGroups); i++) {
            if (sm->base.audioGroups[i]) DataWin_free(sm->base.audioGroups[i]);
        }
    }
    for (int32_t i = 0; i < (int32_t) arrlen(sm->archivePaths); i++) {
        if (sm->archivePaths[i]) free(sm->archivePaths[i]);
    }
    arrfree(sm->base.audioGroups);
    arrfree(sm->archivePaths);

    free(sm);
}

static void sys_update(AudioSystem *sys, MAYBE_UNUSED float dt) {
    if (s_mixerReady) ((SysMixer *) sys)->frame++;
}

static int32_t sys_play(AudioSystem *sys, int32_t id, MAYBE_UNUSED int32_t prio, bool loop) {
    if (!s_mixerReady) return -1;
    SysMixer *sm = (SysMixer *) sys;
    if (!ensure_snd(sm, id)) return -1;

    int vol = (int) (sm->base.dataWin->sond.sounds[id].volume * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
    if (vol < 0) vol = 0;

    if (sm->music[id]) {
        Mix_VolumeMusic(vol);
        Mix_PlayMusic(sm->music[id], loop ? -1 : 0);
        sm->curMusicId = id;
        return MUS_ID_BASE;
    }

    if (sm->chunks[id]) {
        int ch = Mix_PlayChannel(-1, sm->chunks[id], loop ? -1 : 0);
        if (ch >= 0) {
            Mix_Volume(ch, vol);
            return SND_ID_BASE + ch;
        }
    }
    return -1;
}

static void sys_stop(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) {
        Mix_HaltMusic();
        sm->curMusicId = -1;
    } else if (id >= SND_ID_BASE) {
        Mix_HaltChannel(id - SND_ID_BASE);
    } else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) {
            if (Mix_GetChunk(i) == sm->chunks[id]) Mix_HaltChannel(i);
        }
    }
}

static void sys_stop_all(AudioSystem *sys) {
    if (!s_mixerReady) return;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    ((SysMixer *) sys)->curMusicId = -1;
}

static bool sys_is_playing(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return false;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || (sm->curMusicId == id && Mix_PlayingMusic())) return Mix_PlayingMusic();
    if (id >= SND_ID_BASE) return Mix_Playing(id - SND_ID_BASE);

    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) {
            if (Mix_Playing(i) && Mix_GetChunk(i) == sm->chunks[id]) return true;
        }
    }
    return false;
}

static void sys_pause(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_PauseMusic();
    else if (id >= SND_ID_BASE) Mix_Pause(id - SND_ID_BASE);
    else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Pause(i);
    }
}

static void sys_resume(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_ResumeMusic();
    else if (id >= SND_ID_BASE) Mix_Resume(id - SND_ID_BASE);
    else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Resume(i);
    }
}

static void sys_pause_all(MAYBE_UNUSED AudioSystem *sys) {
    if (s_mixerReady) {
        Mix_Pause(-1);
        Mix_PauseMusic();
    }
}

static void sys_resume_all(MAYBE_UNUSED AudioSystem *sys) {
    if (s_mixerReady) {
        Mix_Resume(-1);
        Mix_ResumeMusic();
    }
}

static void sys_set_gain(AudioSystem *sys, int32_t id, float g, MAYBE_UNUSED uint32_t t) {
    if (!s_mixerReady) return;
    SysMixer *sm = (SysMixer *) sys;
    int vol = (int) (g * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
    if (vol < 0) vol = 0;

    if (id == MUS_ID_BASE || sm->curMusicId == id) Mix_VolumeMusic(vol);
    else if (id >= SND_ID_BASE) Mix_Volume(id - SND_ID_BASE, vol);
    else if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        for (int i = 0; i < MAX_CHANS; i++) if (Mix_GetChunk(i) == sm->chunks[id]) Mix_Volume(i, vol);
    }
}

static float sys_get_gain(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return 0.f;
    SysMixer *sm = (SysMixer *) sys;
    if (id == MUS_ID_BASE || sm->curMusicId == id) return (float) Mix_VolumeMusic(-1) / MIX_MAX_VOLUME;
    if (id >= SND_ID_BASE) return (float) Mix_Volume(id - SND_ID_BASE, -1) / MIX_MAX_VOLUME;
    return 0.f;
}

static void sys_set_pitch(AudioSystem *sys, int32_t id, float p) {
    SysMixer *sm = (SysMixer *) sys;
    if (s_mixerReady && id >= 0 && (uint32_t) id < sys->dataWin->sond.count && sm->pitches) {
        sm->pitches[id] = p;
    }
}

static float sys_get_pitch(AudioSystem *sys, int32_t id) {
    SysMixer *sm = (SysMixer *) sys;
    if (s_mixerReady && id >= 0 && (uint32_t) id < sys->dataWin->sond.count && sm->pitches) {
        return sm->pitches[id];
    }
    return 1.f;
}

static float sys_get_pos(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t id) {
    return 0.f;
}

static void sys_set_pos(AudioSystem *sys, int32_t id, float pos) {
    if (s_mixerReady && (id == MUS_ID_BASE || ((SysMixer *) sys)->curMusicId == id)) Mix_SetMusicPosition(pos);
}

static float sys_get_length(AudioSystem *sys, int32_t id) {
    if (!s_mixerReady) return 0.f;
    SysMixer *sm = (SysMixer *) sys;

    if (id == MUS_ID_BASE || sm->curMusicId == id) return 0.f;
    if (id >= SND_ID_BASE) return 0.f;

    if (id >= 0 && (uint32_t) id < sm->base.dataWin->sond.count && sm->chunks[id]) {
        int freq, chans;
        Uint16 fmt;
        Mix_QuerySpec(&freq, &fmt, &chans);
        int bytes_per_sample = (fmt & 0xFF) / 8;
        if (freq > 0 && chans > 0 && bytes_per_sample > 0) {
            return (float) sm->chunks[id]->alen / (freq * chans * bytes_per_sample);
        }
    }
    return 0.f;
}

static void sys_set_master(MAYBE_UNUSED AudioSystem *sys, float g) {
    if (s_mixerReady) {
        int v = (int) (g * MIX_MAX_VOLUME);
        if (v > MIX_MAX_VOLUME) v = MIX_MAX_VOLUME;
        if (v < 0) v = 0;
        Mix_Volume(-1, v);
        Mix_VolumeMusic(v);
    }
}

static void sys_set_chans(MAYBE_UNUSED AudioSystem *sys, int32_t cnt) {
    if (s_mixerReady) Mix_AllocateChannels(cnt);
}

// Lazy-load audiogroupN.dat: parse its AUDO chunk (offsets only — we read
// the actual bytes on demand from disk) and stash the file path.
static void sys_grp_load(AudioSystem *sys, int32_t grp) {
    SysMixer *sm = (SysMixer *) sys;
    if (grp <= 0) return; // group 0 is the main data.win, set up in sys_init

    // Already loaded?
    if ((size_t) grp < arrlenu(sm->base.audioGroups) && sm->base.audioGroups[grp]) return;

    char rel[64];
    snprintf(rel, sizeof(rel), "audiogroup%d.dat", grp);
    char *path = sm->fs->vtable->resolvePath(sm->fs, rel);
    if (!path) {
        fprintf(stderr, "[AUDIO] cannot resolve %s\n", rel);
        return;
    }

    DataWinParserOptions opt = (DataWinParserOptions) {
        .parseAudo = 1,
        .skipAudioBlobData = 1,
    };
    DataWin *gw = DataWin_parse(path, opt);
    if (!gw) {
        fprintf(stderr, "[AUDIO] failed to parse %s\n", path);
        free(path);
        return;
    }

    // Pad both arrays up to (grp+1) entries with NULLs so we can index by grp.
    while ((int32_t) arrlen(sm->base.audioGroups) <= grp) arrput(sm->base.audioGroups, NULL);
    while ((int32_t) arrlen(sm->archivePaths)    <= grp) arrput(sm->archivePaths, NULL);
    sm->base.audioGroups[grp] = gw;
    sm->archivePaths[grp]     = path;
    fprintf(stderr, "[AUDIO] loaded audiogroup %d (%s, %u entries)\n",
            grp, path, (unsigned) gw->audo.count);
}

static bool sys_grp_loaded(AudioSystem *sys, int32_t grp) {
    SysMixer *sm = (SysMixer *) sys;
    if (grp < 0) return false;
    if ((size_t) grp >= arrlenu(sm->base.audioGroups)) return false;
    return sm->base.audioGroups[grp] != NULL;
}

static int32_t sys_create_stream(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED const char *filename) {
    return -1;
}

static bool sys_destroy_stream(MAYBE_UNUSED AudioSystem *sys, MAYBE_UNUSED int32_t streamIndex) {
    return false;
}

static AudioSystemVtable vtable = {
    .init = sys_init, .destroy = sys_destroy, .update = sys_update,
    .playSound = sys_play, .stopSound = sys_stop, .stopAll = sys_stop_all,
    .isPlaying = sys_is_playing, .pauseSound = sys_pause, .resumeSound = sys_resume,
    .pauseAll = sys_pause_all, .resumeAll = sys_resume_all, .setSoundGain = sys_set_gain,
    .getSoundGain = sys_get_gain, .setSoundPitch = sys_set_pitch, .getSoundPitch = sys_get_pitch,
    .getTrackPosition = sys_get_pos, .setTrackPosition = sys_set_pos,
    .getSoundLength = sys_get_length,
    .setMasterGain = sys_set_master, .setChannelCount = sys_set_chans,
    .groupLoad = sys_grp_load, .groupIsLoaded = sys_grp_loaded,
    .createStream = sys_create_stream, .destroyStream = sys_destroy_stream
};

AudioSystem *SdlMixerAudioSystem_create(void) {
    SysMixer *sm = calloc(1, sizeof(SysMixer));
    sm->base.vtable = &vtable;
    return (AudioSystem *) sm;
}
