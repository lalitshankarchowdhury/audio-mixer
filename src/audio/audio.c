#include "../audio/audio.h"
#include "../log/log.h"

#include <malloc.h>
#include <memory.h>
#include <unistd.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <sndfile.h>

enum internal_audio_subsystem_failure_statuses {
    IAS_CLEANUP_NORMALLY,
    IAS_MAKE_CONTEXT_CURRENT_FAILURE,
    IAS_CREATE_DEVICE_CONTEXT_FAILURE,
    IAS_OPEN_DEFAULT_DEVICE_FAILURE
};

enum internal_audio_track_failure_statuses {
    IAC_CLEANUP_NORMALLY,
    IAC_OPEN_CLIP_FILE_FAILURE,
    IAC_GENERATE_GLOBAL_SOURCE_FAILURE,
    IAC_GENERATE_CLIP_BUFFER_FAILURE,
    IAC_ALLOCATE_MEMORY_TO_TEMP_CLIP_DATA_BUFFER_FAILURE,
    IAC_READ_CLIP_FILE_COMPLETELY_FAILURE,
    IAC_COPY_CLIP_FILE_DATA_TO_CLIP_BUFFER_FAILURE
};

static ALCdevice* global_device;
static ALCcontext* global_context;
static ALuint global_source;

static void internal_audio_subsystem_cleanup(int failure_status)
{
    switch (failure_status) {
    case IAS_CLEANUP_NORMALLY:
        alcMakeContextCurrent(NULL);

    case IAS_MAKE_CONTEXT_CURRENT_FAILURE:
        alcDestroyContext(global_context);

    case IAS_CREATE_DEVICE_CONTEXT_FAILURE:
        alcCloseDevice(global_device);

    case IAS_OPEN_DEFAULT_DEVICE_FAILURE:
        break;
    }
}

static void internal_audio_clip_cleanup(int failure_status, AudioClip* clip)
{
    switch (failure_status) {
    case IAC_CLEANUP_NORMALLY:
    case IAC_COPY_CLIP_FILE_DATA_TO_CLIP_BUFFER_FAILURE:
    case IAC_READ_CLIP_FILE_COMPLETELY_FAILURE:
    case IAC_ALLOCATE_MEMORY_TO_TEMP_CLIP_DATA_BUFFER_FAILURE:
        sf_close(clip->file);

    case IAC_OPEN_CLIP_FILE_FAILURE:
        alDeleteSources(1, &global_source);

    case IAC_GENERATE_GLOBAL_SOURCE_FAILURE:
        alDeleteBuffers(1, &clip->buffer);

    case IAC_GENERATE_CLIP_BUFFER_FAILURE:
        break;
    }
}

int audioInitSubsystem()
{
    log_info("Initialize audio subsystem");

    global_device = alcOpenDevice(NULL);

    if (global_device == NULL) {
        log_error("Failed to open default audio device");

        internal_audio_subsystem_cleanup(IAS_OPEN_DEFAULT_DEVICE_FAILURE);

        return AUDIO_FAILURE;
    }

    global_context = alcCreateContext(global_device, NULL);

    if (global_context == NULL) {
        log_error("Failed to create device context");

        internal_audio_subsystem_cleanup(IAS_CREATE_DEVICE_CONTEXT_FAILURE);

        return AUDIO_FAILURE;
    }

    if (alcMakeContextCurrent(global_context) == ALC_FALSE) {
        log_error("Failed to make context current");

        internal_audio_subsystem_cleanup(IAS_MAKE_CONTEXT_CURRENT_FAILURE);

        return AUDIO_FAILURE;
    }

    return AUDIO_SUCCESS;
}

int audioLoadClip(AudioClip* clip, char const* clip_file_name)
{
    log_info("Load audio clip: %s", clip_file_name);

    // Clear old error state
    alGetError();

    alGenBuffers(1, &clip->buffer);

    if (alGetError() != AL_NO_ERROR) {
        log_error("Failed to generate clip buffer");

        internal_audio_clip_cleanup(IAC_GENERATE_CLIP_BUFFER_FAILURE, clip);

        return AUDIO_FAILURE;
    }

    alGenSources(1, &global_source);

    if (alGetError() != AL_NO_ERROR) {
        log_error("Failed to generate global source");

        internal_audio_clip_cleanup(IAC_GENERATE_GLOBAL_SOURCE_FAILURE, clip);

        return AUDIO_FAILURE;
    }

    SF_INFO clip_file_info;

    clip->file = sf_open(clip_file_name, SFM_READ, &clip_file_info);

    if (clip->file == NULL) {
        log_error("Failed to open clip file");

        internal_audio_clip_cleanup(IAC_OPEN_CLIP_FILE_FAILURE, clip);

        return AUDIO_FAILURE;
    }

    // Set audio clip info
    clip->frames = clip_file_info.frames;
    clip->sample_rate = clip_file_info.samplerate;
    clip->channels = clip_file_info.channels;

    int bit_depth;

    // Mask to extract bit-depth and signedness from clip_file_info.format
    unsigned int pcm_format_mask = 0xF;

    switch (clip_file_info.format & pcm_format_mask) {
    case SF_FORMAT_PCM_S8:
    case SF_FORMAT_PCM_U8:
        bit_depth = 8;

        clip->format = (clip->channels == 1) ? AL_FORMAT_MONO8 : AL_FORMAT_STEREO8;

        break;

    case SF_FORMAT_PCM_16:
        bit_depth = 16;

        clip->format = (clip->channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

        break;
    }

    // Allocate memory of size frames * channels * bytes per-sample
    void* clip_file_data = calloc(clip->frames * clip->channels, bit_depth / 2);

    if (clip_file_data == NULL) {
        log_error("Failed to allocate memory to temporary clip data buffer");

        internal_audio_clip_cleanup(
            IAC_ALLOCATE_MEMORY_TO_TEMP_CLIP_DATA_BUFFER_FAILURE,
            clip);

        return AUDIO_FAILURE;
    }

    if (sf_readf_short(clip->file, (short*)clip_file_data, clip->frames) != clip->frames) {
        log_error("Failed to read clip file completely");

        internal_audio_clip_cleanup(IAC_READ_CLIP_FILE_COMPLETELY_FAILURE, clip);

        return AUDIO_FAILURE;
    }

    alBufferData(
        clip->buffer,
        clip->format,
        clip_file_data,
        clip->frames * clip->channels * bit_depth / 2,
        clip->sample_rate);

    free(clip_file_data);

    if (alGetError() != AL_NO_ERROR) {
        log_error("Failed to copy clip file data to clip buffer");

        internal_audio_clip_cleanup(IAC_COPY_CLIP_FILE_DATA_TO_CLIP_BUFFER_FAILURE, clip);

        return AUDIO_FAILURE;
    }

    return AUDIO_SUCCESS;
}

void audioPlayClip(AudioClip* clip)
{
    // Attach clip buffer to global source
    alSourcei(global_source, AL_BUFFER, clip->buffer);

    alSourcePlay(global_source);

    ALenum source_state;

    do {
        sleep(1);

        alGetSourcei(global_source, AL_SOURCE_STATE, &source_state);
    } while (source_state == AL_PLAYING);
}

void audioUnloadClip(AudioClip* clip)
{
    internal_audio_clip_cleanup(IAC_CLEANUP_NORMALLY, clip);
}

void audioQuitSubsystem()
{
    internal_audio_subsystem_cleanup(IAS_CLEANUP_NORMALLY);
}