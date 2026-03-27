#include "sdl12_audio_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>

static bool use_mixer = true;

#define MAX_MIXER_CHANNELS 32
#define MAX_CACHED_CHUNKS 32
#define MUSIC_INSTANCE_ID_BASE 200000

#define STREAMING_SIZE_THRESHOLD (32 * 1024)

typedef struct {
    FILE *fp;       // Указатель на data.win
    uint32_t base;  // Смещение начала звука в файле
    uint32_t size;  // Размер звуковых данных
    uint32_t pos;   // Текущая позиция внутри звука (от 0 до size)
} RWDataWinContext;

// Кастомные функции для SDL_RWops, чтобы читать кусок data.win как отдельный файл
static int rw_datawin_seek(SDL_RWops *context, int offset, int whence) {
    RWDataWinContext *ctx = (RWDataWinContext *)context->hidden.unknown.data1;
    uint32_t new_pos = ctx->pos;

    if (whence == SEEK_SET) new_pos = (uint32_t)offset;
    else if (whence == SEEK_CUR) new_pos += (uint32_t)offset;
    else if (whence == SEEK_END) new_pos = ctx->size + (uint32_t)offset;

    if (new_pos > ctx->size) new_pos = ctx->size;
    ctx->pos = new_pos;

    // Перемещаем реальный указатель в файле
    fseek(ctx->fp, (long)(ctx->base + ctx->pos), SEEK_SET);
    return (int)ctx->pos;
}

static int rw_datawin_read(SDL_RWops *context, void *ptr, int size, int maxnum) {
    RWDataWinContext *ctx = (RWDataWinContext *)context->hidden.unknown.data1;

    if (ctx->pos >= ctx->size) return 0;

    // Проверяем, чтобы не вылезти за пределы выделенного куска данных
    uint32_t remaining = ctx->size - ctx->pos;
    uint32_t total_to_read = (uint32_t)size * maxnum;

    if (total_to_read > remaining) {
        maxnum = remaining / size;
    }

    if (maxnum <= 0) return 0;

    int read = fread(ptr, size, maxnum, ctx->fp);
    ctx->pos += (uint32_t)(read * size);
    return read;
}

static int rw_datawin_close(SDL_RWops *context) {
    if (context) {
        // Освобождаем наш вспомогательный контекст
        if (context->hidden.unknown.data1) {
            free(context->hidden.unknown.data1);
        }
        SDL_FreeRW(context);
    }
    return 0;
}

static SDL_RWops *SDL_RWFromDataWin(FILE* fp, uint32_t offset, uint32_t size) {
    SDL_RWops *rw = SDL_AllocRW();
    if (!rw) return NULL;

    RWDataWinContext *ctx = (RWDataWinContext *)safeMalloc(sizeof(RWDataWinContext));
    ctx->fp = fp;
    ctx->base = offset;
    ctx->size = size;
    ctx->pos = 0;

    rw->hidden.unknown.data1 = ctx; // Кладем наш контекст сюда

    rw->read = rw_datawin_read;
    rw->seek = rw_datawin_seek;
    rw->write = NULL; // Мы только читаем
    rw->close = rw_datawin_close;

    // В SDL 1.2 важно проставить тип, чтобы SDL не пытался использовать свои дефолтные функции
    rw->type = 0;

    return rw;
}

static char* resolveExternalPath(SdlMixerAudioSystem* sys, Sound* sound) {
    if (!use_mixer) return "";

    const char* file = sound->file;
    if (file == nullptr || file[0] == '\0') return nullptr;

    bool hasExtension = (strchr(file, '.') != nullptr);
    char filename[512];
    if (hasExtension) {
        snprintf(filename, sizeof(filename), "%s", file);
    } else {
        snprintf(filename, sizeof(filename), "%s.ogg", file); // Fallback to .ogg
    }

    return sys->fileSystem->vtable->resolvePath(sys->fileSystem, filename);
}

static bool ensureSoundLoaded(SdlMixerAudioSystem* sys, int32_t soundIndex) {
    if (!use_mixer) return true;
    if (soundIndex < 0 || (uint32_t)soundIndex >= sys->base.dataWin->sond.count) return false;

    // Запоминаем, когда звук использовался последний раз
    sys->chunkLastUsed[soundIndex] = sys->audioFrameCounter;

    if (sys->chunks[soundIndex] != nullptr || sys->music[soundIndex] != nullptr) return true;

    Sound* sound = &sys->base.dataWin->sond.sounds[soundIndex];
    bool isEmbedded = (sound->flags & 0x01) != 0;

    if (isEmbedded) {
        AudioEntry* entry = &sys->base.dataWin->audo.entries[sound->audioFile];
        bool isMusic = (entry->dataSize > STREAMING_SIZE_THRESHOLD);

        if (!isMusic) {
            // --- ЛОГИКА ВЫТЕСНЕНИЯ СТАРЫХ ЗВУКОВ ---
            int loadedChunks = 0;
            for (uint32_t i = 0; i < sys->base.dataWin->sond.count; i++) {
                if (sys->chunks[i]) loadedChunks++;
            }

            if (loadedChunks >= MAX_CACHED_CHUNKS) {
                int oldestId = -1;
                uint32_t oldestTime = 0xFFFFFFFF;

                for (uint32_t i = 0; i < sys->base.dataWin->sond.count; i++) {
                    if (sys->chunks[i]) {
                        bool isPlaying = false;
                        for (int ch = 0; ch < MAX_MIXER_CHANNELS; ch++) {
                            if (Mix_Playing(ch) && Mix_GetChunk(ch) == sys->chunks[i]) {
                                isPlaying = true;
                                break;
                            }
                        }

                        if (!isPlaying && sys->chunkLastUsed[i] < oldestTime) {
                            oldestTime = sys->chunkLastUsed[i];
                            oldestId = i;
                        }
                    }
                }

                if (oldestId != -1) {
                    Mix_FreeChunk(sys->chunks[oldestId]);
                    sys->chunks[oldestId] = nullptr;
                }
            }

            // --- БЕЗОПАСНАЯ ЗАГРУЗКА ---
            uint8_t* tempBuf = safeMalloc(entry->dataSize);
            // ВАЖНО: ИСПОЛЬЗУЕМ ВТОРОЙ УКАЗАТЕЛЬ ФАЙЛА, чтобы не мешать музыке
            fseek(sys->sfxDataWinFile, (long)entry->dataOffset, SEEK_SET);
            fread(tempBuf, 1, entry->dataSize, sys->sfxDataWinFile);

            SDL_RWops* rw = SDL_RWFromConstMem(tempBuf, entry->dataSize);
            sys->chunks[soundIndex] = Mix_LoadWAV_RW(rw, 1);
            free(tempBuf);

            if (!sys->chunks[soundIndex]) {
                fprintf(stderr, "Audio Error: Failed to load SFX '%s': %s\n", sound->name, Mix_GetError());
            }
        } else {
            // Музыка использует первый указатель
            SDL_RWops* rw = SDL_RWFromDataWin(sys->dataWinFile, entry->dataOffset, entry->dataSize);
            sys->music[soundIndex] = Mix_LoadMUS_RW(rw);
        }
    } else {
        char* path = resolveExternalPath(sys, sound);
        if (path != nullptr) {
            if (strstr(path, ".wav") != nullptr) {
                sys->chunks[soundIndex] = Mix_LoadWAV(path);
            } else {
                sys->music[soundIndex] = Mix_LoadMUS(path);
            }
            free(path);
        }
    }

    return (sys->chunks[soundIndex] != nullptr || sys->music[soundIndex] != nullptr);
}

// ===[ Vtable Implementations ]===
static void sdlmInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    sys->base.dataWin = dataWin;
    sys->fileSystem = fileSystem;

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        fprintf(stderr, "Audio: Failed to init SDL_mixer: %s\n", Mix_GetError());
        use_mixer = false;
    }

    if (use_mixer) {
        Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
        Mix_AllocateChannels(MAX_MIXER_CHANNELS);

        uint32_t soundCount = dataWin->sond.count;
        sys->chunks = safeCalloc(soundCount, sizeof(Mix_Chunk*));
        sys->music = safeCalloc(soundCount, sizeof(Mix_Music*));
        sys->compressedMusicBuf = safeCalloc(soundCount, sizeof(uint8_t*));

        sys->currentMusicSoundIndex = -1;

        // ДВА независимых указателя на файл!
        sys->dataWinFile = fopen("data.win", "rb");
        sys->sfxDataWinFile = fopen("data.win", "rb");

        sys->chunkLastUsed = safeCalloc(soundCount, sizeof(uint32_t));
        sys->audioFrameCounter = 1;

        fprintf(stderr, "Audio: SDL_mixer 1.2 initialized (%d channels)\n", MAX_MIXER_CHANNELS);
    }
}

static void sdlmDestroy(AudioSystem* audio) {
    if (use_mixer) {
        SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

        Mix_HaltChannel(-1);
        Mix_HaltMusic();

        uint32_t soundCount = sys->base.dataWin->sond.count;
        for (uint32_t i = 0; i < soundCount; i++) {
            if (sys->chunks[i]) Mix_FreeChunk(sys->chunks[i]);
            if (sys->music[i]) Mix_FreeMusic(sys->music[i]);
            if (sys->compressedMusicBuf[i]) free(sys->compressedMusicBuf[i]);
        }

        free(sys->chunks);
        free(sys->music);
        free(sys->compressedMusicBuf);
        if (sys->chunkLastUsed) free(sys->chunkLastUsed);

        if (sys->dataWinFile) fclose(sys->dataWinFile);
        if (sys->sfxDataWinFile) fclose(sys->sfxDataWinFile);

        Mix_CloseAudio();
        Mix_Quit();
        free(sys);
    }
}

static void sdlmUpdate(AudioSystem* audio, float deltaTime) {
    if (use_mixer) {
        SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
        sys->audioFrameCounter++;
        (void)deltaTime;
    }
}

static int32_t sdlmPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    if (!use_mixer) return 0;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (!ensureSoundLoaded(sys, soundIndex)) return -1;

    Sound* sound = &sys->base.dataWin->sond.sounds[soundIndex];
    int volume = (int)(sound->volume * MIX_MAX_VOLUME);

    if (sys->music[soundIndex] != nullptr) {
        Mix_VolumeMusic(volume);
        Mix_PlayMusic(sys->music[soundIndex], loop ? -1 : 0);
        sys->currentMusicSoundIndex = soundIndex;
        return MUSIC_INSTANCE_ID_BASE;
    }

    if (sys->chunks[soundIndex] != nullptr) {
        int channel = Mix_PlayChannel(-1, sys->chunks[soundIndex], loop ? -1 : 0);
        if (channel >= 0) {
            Mix_Volume(channel, volume);
            return SOUND_INSTANCE_ID_BASE + channel;
        } else {
            fprintf(stderr, "Audio Warning: No free channels for '%s'!\n", sound->name);
        }
    }

    return -1;
}

static void sdlmStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_HaltMusic();
        sys->currentMusicSoundIndex = -1;
    }
    else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) {
            Mix_HaltChannel(channel);
        }
    }
    else {
        if (sys->currentMusicSoundIndex == soundOrInstance) {
            Mix_HaltMusic();
            sys->currentMusicSoundIndex = -1;
        }
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk != nullptr) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_Playing(i) && Mix_GetChunk(i) == targetChunk) {
                    Mix_HaltChannel(i);
                }
            }
        }
    }
}

static void sdlmStopAll(AudioSystem* audio) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    sys->currentMusicSoundIndex = -1;
}

static bool sdlmIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return false;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        return Mix_PlayingMusic() != 0;
    }
    else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) {
            return Mix_Playing(channel) != 0;
        }
    }
    else {
        if (sys->currentMusicSoundIndex == soundOrInstance && Mix_PlayingMusic()) return true;

        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk != nullptr) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_Playing(i) && Mix_GetChunk(i) == targetChunk) {
                    return true;
                }
            }
        }
    }
    return false;
}

static void sdlmPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_PauseMusic();
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Pause(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_PauseMusic();
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_Pause(i);
            }
        }
    }
}

static void sdlmResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_ResumeMusic();
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Resume(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_ResumeMusic();
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_Resume(i);
            }
        }
    }
}

static void sdlmPauseAll(AudioSystem* audio) {
    (void)audio;
    if (use_mixer) { Mix_Pause(-1); Mix_PauseMusic(); }
}

static void sdlmResumeAll(AudioSystem* audio) {
    (void)audio;
    if (use_mixer) { Mix_Resume(-1); Mix_ResumeMusic(); }
}

static void sdlmSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    (void)timeMs;

    int vol = (int)(gain * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_VolumeMusic(vol);
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Volume(channel, vol);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_VolumeMusic(vol);
        Mix_Chunk* targetChunk = sys->chunks[soundOrInstance];
        if (targetChunk) {
            for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
                if (Mix_GetChunk(i) == targetChunk) Mix_Volume(i, vol);
            }
        }
    }
}

static float sdlmGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    if (!use_mixer) return 0.0f;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE || sys->currentMusicSoundIndex == soundOrInstance) {
        return (float)Mix_VolumeMusic(-1) / (float)MIX_MAX_VOLUME;
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) return (float)Mix_Volume(channel, -1) / (float)MIX_MAX_VOLUME;
    }
    return 0.0f;
}

static void sdlmSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    (void)audio; (void)soundOrInstance; (void)pitch;
}

static float sdlmGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    (void)audio; (void)soundOrInstance;
    return 1.0f;
}

static float sdlmGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    (void)audio; (void)soundOrInstance;
    return 0.0f;
}

static void sdlmSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    if (!use_mixer) return;
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE || sys->currentMusicSoundIndex == soundOrInstance) {
        Mix_SetMusicPosition((double)positionSeconds);
    }
}

static void sdlmSetMasterGain(AudioSystem* audio, float gain) {
    if (!use_mixer) return;
    (void)audio;
    int vol = (int)(gain * MIX_MAX_VOLUME);
    Mix_Volume(-1, vol);
    Mix_VolumeMusic(vol);
}

static void sdlmSetChannelCount(AudioSystem* audio, int32_t count) {
    if (!use_mixer) return;
    (void)audio;
    Mix_AllocateChannels(count);
}

static void sdlmGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    (void)audio; (void)groupIndex;
    fprintf(stderr, "Audio: groupLoad bypassed to save RAM\n");
}

static bool sdlmGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    (void)audio; (void)groupIndex;
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable sdlmAudioSystemVtable = {
    .init = sdlmInit,
    .destroy = sdlmDestroy,
    .update = sdlmUpdate,
    .playSound = sdlmPlaySound,
    .stopSound = sdlmStopSound,
    .stopAll = sdlmStopAll,
    .isPlaying = sdlmIsPlaying,
    .pauseSound = sdlmPauseSound,
    .resumeSound = sdlmResumeSound,
    .pauseAll = sdlmPauseAll,
    .resumeAll = sdlmResumeAll,
    .setSoundGain = sdlmSetSoundGain,
    .getSoundGain = sdlmGetSoundGain,
    .setSoundPitch = sdlmSetSoundPitch,
    .getSoundPitch = sdlmGetSoundPitch,
    .getTrackPosition = sdlmGetTrackPosition,
    .setTrackPosition = sdlmSetTrackPosition,
    .setMasterGain = sdlmSetMasterGain,
    .setChannelCount = sdlmSetChannelCount,
    .groupLoad = sdlmGroupLoad,
    .groupIsLoaded = sdlmGroupIsLoaded,
};

// ===[ Lifecycle ]===

AudioSystem* SdlMixerAudioSystem_create(void) {
    SdlMixerAudioSystem* sys = safeCalloc(1, sizeof(SdlMixerAudioSystem));
    sys->base.vtable = &sdlmAudioSystemVtable;
    return (AudioSystem*) sys;
}