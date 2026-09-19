#ifndef DISTILL_PLATFORM_DAUDIO_H
#define DISTILL_PLATFORM_DAUDIO_H

/* PCM output, for the audio core. The core decodes; the platform owns the
   device and the thread that asks for samples. On Linux that is miniaudio's
   device layer; on Muroxi it is the audio service (mx_sio_*).

   The environment variable DISTILL_AUDIO_BACKEND=null asks for an output
   that consumes samples in real time and plays nothing, on every platform:
   the terminal suite exercises playback with it on machines with no sound
   card and without making a noise on ones that have one. */

#include <stddef.h>

/* Fill `frames` frames of interleaved 32-bit float samples. Runs on the
   platform's audio thread: it must not block, allocate or lock. */
typedef void (*DAudioFill)(void *opaque, float *out, size_t frames);

typedef struct DAudioOutput DAudioOutput;

/* An output at `sample_rate` Hz with `channels` interleaved channels,
   stopped. NULL with `*error` set (caller frees) when there is no device. */
DAudioOutput *d_audio_open(unsigned sample_rate, unsigned channels, DAudioFill fill,
                           void *opaque, char **error);

/* Start and stop asking for samples. `fill` is not called while stopped,
   and not after d_audio_stop returns. */
bool d_audio_start(DAudioOutput *output);
void d_audio_stop(DAudioOutput *output);

void d_audio_close(DAudioOutput *output);

#endif
