#pragma once
#include "obs-module.h"
#include <cstdint>

struct obs_source {};
typedef struct obs_source obs_source_t;

struct obs_source_audio {
    const uint8_t *data[8];
    uint32_t frames;
    int speakers;
    int format;
    uint32_t samples_per_sec;
    uint64_t timestamp;
};

#define SPEAKERS_MONO 1
#define AUDIO_FORMAT_16BIT 1
#define OBS_SOURCE_TYPE_INPUT 1
#define OBS_SOURCE_AUDIO (1 << 1)
#define OBS_SOURCE_DO_NOT_DUPLICATE (1 << 7)

struct obs_data;
typedef struct obs_data obs_data_t;

struct obs_source_info {
    const char *id;
    int type;
    int output_flags;
    const char *(*get_name)(void *type_data);
    void *(*create)(obs_data_t *settings, obs_source_t *source);
    void (*destroy)(void *data);
    void (*activate)(void *data);
    void (*deactivate)(void *data);
};

inline void obs_register_source(struct obs_source_info *) {}
inline void obs_source_output_audio(obs_source_t *, const struct obs_source_audio *) {}
