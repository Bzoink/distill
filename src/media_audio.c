/* miniaudio behind a C23 API. See src/media_audio.h for the contract and
 * src/documents/MEDIA_PLAYBACK_PLAN.md for why audio is in scope and video
 * is not.
 *
 * The threading contract, which is the part that bites:
 *
 *   The fill callback runs on a real-time audio thread this process does not
 *   control. It allocates nothing, locks nothing, and calls nothing belonging
 *   to a front end. Everything the front end reads is published through
 *   atomics and collected by media_audio_snapshot on the front end's own tick.
 *
 *   The decoder is touched from the audio thread only, and only while the
 *   device is running; open and close happen with the device stopped. That is
 *   why a seek is an atomic request the callback honours rather than a call
 *   that reaches into the decoder from under the UI. Decoding does read from
 *   the file on the audio thread, which is the one liberty taken here -- it is
 *   affordable precisely because the plan fetches media to a local file first
 *   rather than streaming it.
 */

/* miniaudio decodes here; the device that plays what it decodes belongs to
   the platform (daudio.h), which also compiles miniaudio's implementation. */
#include "miniaudio_config.h"
#include "miniaudio.h"

#include "daudio.h"
#include "media_audio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEDIA_AUDIO_CHANNELS 2
#define MEDIA_AUDIO_FORMAT ma_format_f32
#define SEEK_NONE ((unsigned long long)-1)

static struct {
    bool open;            /* a decoder is loaded */
    DAudioOutput *output; /* a device is attached */
    ma_decoder decoder;
    unsigned long long total_frames;
    unsigned sample_rate;
    char item[MEDIA_AUDIO_ITEM_MAX];
    char error[MEDIA_AUDIO_ERROR_MAX];

    /* Written by the audio thread, read by the front end. */
    _Atomic unsigned long long position_frames;
    _Atomic unsigned long long seek_request;
    _Atomic int phase;
    _Atomic int volume_milli;
    _Atomic bool volume_chosen;
} core;

static void set_phase(MediaAudioPhase phase)
{
    atomic_store_explicit(&core.phase, (int)phase, memory_order_release);
}

static MediaAudioPhase phase_now(void)
{
    return (MediaAudioPhase)atomic_load_explicit(&core.phase, memory_order_acquire);
}

static void set_error(const char *message)
{
    snprintf(core.error, sizeof core.error, "%s", message ? message : "");
    set_phase(MEDIA_AUDIO_ERROR);
}

/* The one routine the audio thread runs. Everything it touches is either
   thread-local, atomic, or the decoder it alone owns while playing. */
static size_t fill_frames(float *out, size_t frames)
{
    if (!core.open) {
        if (out) memset(out, 0, frames * MEDIA_AUDIO_CHANNELS * sizeof *out);
        return 0;
    }
    unsigned long long requested = atomic_exchange_explicit(&core.seek_request, SEEK_NONE,
                                                            memory_order_acq_rel);
    if (requested != SEEK_NONE) {
        if (ma_decoder_seek_to_pcm_frame(&core.decoder, (ma_uint64)requested) == MA_SUCCESS)
            atomic_store_explicit(&core.position_frames, requested, memory_order_release);
    }
    if (phase_now() != MEDIA_AUDIO_PLAYING) {
        if (out) memset(out, 0, frames * MEDIA_AUDIO_CHANNELS * sizeof *out);
        return 0;
    }
    ma_uint64 read = 0;
    float scratch[512 * MEDIA_AUDIO_CHANNELS];
    float *target = out;
    if (!target) {
        /* Silent pull: decode into scratch in chunks so the test seam moves
           the decoder exactly as the device would. */
        size_t done = 0;
        while (done < frames) {
            size_t chunk = frames - done;
            if (chunk > 512) chunk = 512;
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&core.decoder, scratch, chunk, &got);
            done += (size_t)got;
            if (got < chunk) break;
        }
        read = done;
    } else {
        ma_decoder_read_pcm_frames(&core.decoder, target, frames, &read);
        float gain = (float)atomic_load_explicit(&core.volume_milli, memory_order_acquire) / 1000.0f;
        if (gain != 1.0f)
            for (ma_uint64 i = 0; i < read * MEDIA_AUDIO_CHANNELS; ++i) target[i] *= gain;
        if (read < frames)
            memset(target + read * MEDIA_AUDIO_CHANNELS, 0,
                   (frames - read) * MEDIA_AUDIO_CHANNELS * sizeof *target);
    }
    atomic_fetch_add_explicit(&core.position_frames, (unsigned long long)read, memory_order_acq_rel);
    if (read < frames) {
        /* End of stream: stop at the end and rewind, but never start again.
           Autoplay is a non-goal, and a loop is autoplay by another name. */
        set_phase(MEDIA_AUDIO_FINISHED);
        atomic_store_explicit(&core.position_frames, 0, memory_order_release);
        atomic_store_explicit(&core.seek_request, 0, memory_order_release);
    }
    return (size_t)read;
}

static void on_fill(void *opaque, float *out, size_t frames)
{
    (void)opaque;
    fill_frames(out, frames);
}

static void release(void)
{
    if (core.output) { d_audio_close(core.output); core.output = NULL; }
    if (core.open) { ma_decoder_uninit(&core.decoder); core.open = false; }
    core.total_frames = 0;
    core.sample_rate = 0;
    core.item[0] = '\0';
    atomic_store_explicit(&core.position_frames, 0, memory_order_release);
    atomic_store_explicit(&core.seek_request, SEEK_NONE, memory_order_release);
}

static bool open_decoder(const char *path, const char *item)
{
    release();
    core.error[0] = '\0';
    /* Full volume until somebody chooses otherwise; a chosen volume then
       survives every later open, which is what a reader expects. */
    if (!atomic_load_explicit(&core.volume_chosen, memory_order_acquire))
        atomic_store_explicit(&core.volume_milli, 1000, memory_order_release);
    set_phase(MEDIA_AUDIO_LOADING);
    ma_decoder_config config = ma_decoder_config_init(MEDIA_AUDIO_FORMAT, MEDIA_AUDIO_CHANNELS, 0);
#ifdef DISTILL_WINDOWS
    ma_result d_win_decoder_init(const char *, const ma_decoder_config *, ma_decoder *);
    ma_result result = d_win_decoder_init(path ? path : "", &config, &core.decoder);
#else
    ma_result result = ma_decoder_init_file(path ? path : "", &config, &core.decoder);
#endif
    if (result != MA_SUCCESS) {
        set_error("this file could not be decoded");
        return false;
    }
    core.open = true;
    core.sample_rate = core.decoder.outputSampleRate;
    ma_uint64 length = 0;
    core.total_frames = ma_decoder_get_length_in_pcm_frames(&core.decoder, &length) == MA_SUCCESS
                        ? (unsigned long long)length : 0;
    snprintf(core.item, sizeof core.item, "%s", item ? item : "");
    atomic_store_explicit(&core.position_frames, 0, memory_order_release);
    atomic_store_explicit(&core.seek_request, SEEK_NONE, memory_order_release);
    set_phase(MEDIA_AUDIO_PAUSED);
    return true;
}

bool media_audio_open(const char *path, const char *item)
{
    if (!open_decoder(path, item)) return false;
    /* DISTILL_AUDIO_BACKEND=null, honoured by every platform's output, plays
       into nothing in real time: position, seeking and end of track all
       behave as they do on a device, with no sound and no device needed. */
    char *error = NULL;
    core.output = d_audio_open(core.sample_rate, MEDIA_AUDIO_CHANNELS, on_fill, NULL, &error);
    if (!core.output) {
        /* No device is a reportable condition, not a crash: the file stays
           open so the front end can still offer a download. */
        set_error(error ? error : "no audio device is available");
        free(error);
        return false;
    }
    set_phase(MEDIA_AUDIO_PAUSED);
    return true;
}

bool media_audio_open_silent(const char *path, const char *item)
{
    return open_decoder(path, item);
}

bool media_audio_play(void)
{
    if (!core.open) return false;
    if (phase_now() == MEDIA_AUDIO_FINISHED) {
        atomic_store_explicit(&core.seek_request, 0, memory_order_release);
        atomic_store_explicit(&core.position_frames, 0, memory_order_release);
    }
    set_phase(MEDIA_AUDIO_PLAYING);
    if (core.output && !d_audio_start(core.output)) {
        set_error("the audio device would not start");
        return false;
    }
    return true;
}

void media_audio_pause(void)
{
    if (!core.open || phase_now() != MEDIA_AUDIO_PLAYING) return;
    set_phase(MEDIA_AUDIO_PAUSED);
    if (core.output) d_audio_stop(core.output);
}

void media_audio_stop(void)
{
    if (!core.open) return;
    set_phase(MEDIA_AUDIO_STOPPED);
    if (core.output) d_audio_stop(core.output);
    atomic_store_explicit(&core.position_frames, 0, memory_order_release);
    atomic_store_explicit(&core.seek_request, 0, memory_order_release);
}

bool media_audio_seek(double seconds)
{
    if (!core.open || !core.sample_rate) return false;
    if (seconds < 0) seconds = 0;
    unsigned long long frame = (unsigned long long)(seconds * core.sample_rate);
    if (core.total_frames && frame > core.total_frames) frame = core.total_frames;
    /* Publish the new position immediately so a redraw between here and the
       next callback shows where the handle was dragged to, not where playback
       still is. The audio thread performs the seek itself. */
    atomic_store_explicit(&core.position_frames, frame, memory_order_release);
    atomic_store_explicit(&core.seek_request, frame, memory_order_release);
    if (phase_now() == MEDIA_AUDIO_FINISHED) set_phase(MEDIA_AUDIO_PAUSED);
    if (!core.output || phase_now() != MEDIA_AUDIO_PLAYING) {
        /* With no device running nothing will honour the request, so do it
           here -- the decoder is idle and nobody else can be touching it. */
        unsigned long long pending = atomic_exchange_explicit(&core.seek_request, SEEK_NONE,
                                                              memory_order_acq_rel);
        if (pending != SEEK_NONE)
            ma_decoder_seek_to_pcm_frame(&core.decoder, (ma_uint64)pending);
    }
    return true;
}

void media_audio_set_volume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    atomic_store_explicit(&core.volume_milli, (int)(volume * 1000.0f + 0.5f), memory_order_release);
    atomic_store_explicit(&core.volume_chosen, true, memory_order_release);
}

void media_audio_snapshot(MediaAudioState *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->phase = phase_now();
    out->sample_rate = core.sample_rate;
    unsigned long long position = atomic_load_explicit(&core.position_frames, memory_order_acquire);
    out->position_seconds = core.sample_rate ? (double)position / core.sample_rate : 0;
    out->duration_seconds = core.sample_rate && core.total_frames
                            ? (double)core.total_frames / core.sample_rate : 0;
    out->volume = (float)atomic_load_explicit(&core.volume_milli, memory_order_acquire) / 1000.0f;
    out->seekable = core.open && core.total_frames > 0;
    snprintf(out->item, sizeof out->item, "%s", core.item);
    snprintf(out->error_message, sizeof out->error_message, "%s", core.error);
}

void media_audio_close(void)
{
    release();
    core.error[0] = '\0';
    set_phase(MEDIA_AUDIO_STOPPED);
}

bool media_audio_available(void)
{
    return core.output != NULL;
}

size_t media_audio_pull(size_t frames)
{
    return fill_frames(NULL, frames);
}
