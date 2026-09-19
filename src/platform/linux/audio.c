/* daudio on Linux: miniaudio's device layer, over PulseAudio or ALSA.

   This translation unit is also where miniaudio's implementation is
   compiled, decoders included, so the audio core links against it. */

#include "miniaudio_config.h"

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_NULL

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "daudio.h"

#include "cutil.h"

struct DAudioOutput {
    ma_device device;
    ma_context context;      /* only for the null backend */
    bool context_ready;
    DAudioFill fill;
    void *opaque;
};

static void data_callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    DAudioOutput *audio = device->pUserData;
    audio->fill(audio->opaque, output, frames);
}

DAudioOutput *d_audio_open(unsigned sample_rate, unsigned channels, DAudioFill fill,
                           void *opaque, char **error)
{
    if (error) *error = NULL;
    DAudioOutput *audio = d_xcalloc(1, sizeof(*audio));
    audio->fill = fill;
    audio->opaque = opaque;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate = sample_rate;
    config.dataCallback = data_callback;
    config.pUserData = audio;
    ma_context *context = NULL;
    const char *backend = getenv("DISTILL_AUDIO_BACKEND");
    if (backend && !strcmp(backend, "null")) {
        ma_backend null_backend = ma_backend_null;
        if (ma_context_init(&null_backend, 1, NULL, &audio->context) == MA_SUCCESS) {
            audio->context_ready = true;
            context = &audio->context;
        }
    }
    if (ma_device_init(context, &config, &audio->device) != MA_SUCCESS) {
        if (audio->context_ready) ma_context_uninit(&audio->context);
        free(audio);
        if (error) *error = d_strdup("no audio device is available");
        return NULL;
    }
    return audio;
}

bool d_audio_start(DAudioOutput *audio)
{
    return audio && ma_device_start(&audio->device) == MA_SUCCESS;
}

void d_audio_stop(DAudioOutput *audio)
{
    if (audio) ma_device_stop(&audio->device);
}

void d_audio_close(DAudioOutput *audio)
{
    if (!audio) return;
    ma_device_uninit(&audio->device);
    if (audio->context_ready) ma_context_uninit(&audio->context);
    free(audio);
}
