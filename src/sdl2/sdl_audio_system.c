#include "sdl_audio_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_mixer.h>

#define MAX_MIXER_CHANNELS 64
#define MUSIC_INSTANCE_ID_BASE 200000

#define STREAMING_SIZE_THRESHOLD (512 * 1024)

static char* resolveExternalPath(SdlMixerAudioSystem* sys, Sound* sound) {
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
    if (soundIndex < 0 || (uint32_t)soundIndex >= sys->base.dataWin->sond.count) return false;

    if (sys->chunks[soundIndex] != nullptr || sys->music[soundIndex] != nullptr) return true;

    Sound* sound = &sys->base.dataWin->sond.sounds[soundIndex];
    bool isEmbedded = (sound->flags & 0x01) != 0;

    if (isEmbedded) {
        if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= sys->base.dataWin->audo.count) return false;
        
        AudioEntry* entry = &sys->base.dataWin->audo.entries[sound->audioFile];
        if (entry->dataSize == 0 || sys->dataWinFile == nullptr) return false;

        bool isMusic = (entry->dataSize > STREAMING_SIZE_THRESHOLD);

        if (isMusic) {
            sys->compressedMusicBuf[soundIndex] = safeMalloc(entry->dataSize);
            fseek(sys->dataWinFile, (long)entry->dataOffset, SEEK_SET);
            fread(sys->compressedMusicBuf[soundIndex], 1, entry->dataSize, sys->dataWinFile);

            SDL_RWops* rw = SDL_RWFromConstMem(sys->compressedMusicBuf[soundIndex], entry->dataSize);
            sys->music[soundIndex] = Mix_LoadMUS_RW(rw, 1);
            
            fprintf(stderr, "Audio: Lazy-loaded Music '%s' from RAM stream (%.2f MB)\n", sound->name, entry->dataSize / 1024.0f / 1024.0f);
        } else {
            uint8_t* tempBuf = safeMalloc(entry->dataSize);
            fseek(sys->dataWinFile, (long)entry->dataOffset, SEEK_SET);
            fread(tempBuf, 1, entry->dataSize, sys->dataWinFile);

            SDL_RWops* rw = SDL_RWFromConstMem(tempBuf, entry->dataSize);
            sys->chunks[soundIndex] = Mix_LoadWAV_RW(rw, 1);
            
            free(tempBuf);
            fprintf(stderr, "Audio: Lazy-loaded SFX '%s' (decoded)\n", sound->name);
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
        return;
    }
    
    Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    Mix_AllocateChannels(MAX_MIXER_CHANNELS);

    uint32_t soundCount = dataWin->sond.count;
    sys->chunks = safeCalloc(soundCount, sizeof(Mix_Chunk*));
    sys->music = safeCalloc(soundCount, sizeof(Mix_Music*));
    sys->compressedMusicBuf = safeCalloc(soundCount, sizeof(uint8_t*));
    sys->channelToSoundIndex = safeCalloc(MAX_MIXER_CHANNELS, sizeof(int32_t));

    for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
        sys->channelToSoundIndex[i] = -1;
    }
    sys->currentMusicSoundIndex = -1;

    sys->dataWinFile = fopen("data.win", "rb");

    fprintf(stderr, "Audio: SDL_mixer initialized (%d channels)\n", MAX_MIXER_CHANNELS);
}

static void sdlmDestroy(AudioSystem* audio) {
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
    free(sys->channelToSoundIndex);

    if (sys->dataWinFile) fclose(sys->dataWinFile);

    Mix_CloseAudio();
    Mix_Quit();
    free(sys);
}

static void sdlmUpdate(AudioSystem* audio, float deltaTime) {
    (void)audio;
    (void)deltaTime;
}

static int32_t sdlmPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
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
            sys->channelToSoundIndex[channel] = soundIndex;
            return SOUND_INSTANCE_ID_BASE + channel;
        }
    }

    return -1;
}

static void sdlmStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_HaltMusic();
        sys->currentMusicSoundIndex = -1;
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) {
            Mix_HaltChannel(channel);
            sys->channelToSoundIndex[channel] = -1;
        }
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) {
            Mix_HaltMusic();
            sys->currentMusicSoundIndex = -1;
        }
        for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
            if (sys->channelToSoundIndex[i] == soundOrInstance) {
                Mix_HaltChannel(i);
                sys->channelToSoundIndex[i] = -1;
            }
        }
    }
}

static void sdlmStopAll(AudioSystem* audio) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    Mix_HaltChannel(-1);
    Mix_HaltMusic();
    sys->currentMusicSoundIndex = -1;
    for (int i = 0; i < MAX_MIXER_CHANNELS; i++) sys->channelToSoundIndex[i] = -1;
}

static bool sdlmIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        return Mix_PlayingMusic();
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) {
            return Mix_Playing(channel);
        }
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance && Mix_PlayingMusic()) return true;
        for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
            if (sys->channelToSoundIndex[i] == soundOrInstance && Mix_Playing(i)) return true;
        }
    }
    return false;
}

static void sdlmPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_PauseMusic();
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Pause(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_PauseMusic();
        for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
            if (sys->channelToSoundIndex[i] == soundOrInstance) Mix_Pause(i);
        }
    }
}

static void sdlmResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_ResumeMusic();
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Resume(channel);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_ResumeMusic();
        for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
            if (sys->channelToSoundIndex[i] == soundOrInstance) Mix_Resume(i);
        }
    }
}

static void sdlmPauseAll(AudioSystem* audio) {
    (void)audio;
    Mix_Pause(-1);
    Mix_PauseMusic();
}

static void sdlmResumeAll(AudioSystem* audio) {
    (void)audio;
    Mix_Resume(-1);
    Mix_ResumeMusic();
}

static void sdlmSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    int vol = (int)(gain * MIX_MAX_VOLUME);
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;

    (void)timeMs;

    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE) {
        Mix_VolumeMusic(vol);
    } else if (soundOrInstance >= SOUND_INSTANCE_ID_BASE) {
        int channel = soundOrInstance - SOUND_INSTANCE_ID_BASE;
        if (channel >= 0 && channel < MAX_MIXER_CHANNELS) Mix_Volume(channel, vol);
    } else {
        if (sys->currentMusicSoundIndex == soundOrInstance) Mix_VolumeMusic(vol);
        for (int i = 0; i < MAX_MIXER_CHANNELS; i++) {
            if (sys->channelToSoundIndex[i] == soundOrInstance) Mix_Volume(i, vol);
        }
    }
}

static float sdlmGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
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
    SdlMixerAudioSystem* sys = (SdlMixerAudioSystem*) audio;
    if (soundOrInstance == MUSIC_INSTANCE_ID_BASE || sys->currentMusicSoundIndex == soundOrInstance) {
        Mix_SetMusicPosition((double)positionSeconds);
    }
}

static void sdlmSetMasterGain(AudioSystem* audio, float gain) {
    (void)audio;
    int vol = (int)(gain * MIX_MAX_VOLUME);
    Mix_Volume(-1, vol);
    Mix_VolumeMusic(vol);
}

static void sdlmSetChannelCount(AudioSystem* audio, int32_t count) {
    (void)audio;
    Mix_AllocateChannels(count);
}

static void sdlmGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    (void)audio; (void)groupIndex;
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