/* The audio core: the one medium this reader plays itself.
 *
 * Public state carries no miniaudio types, so every front end -- the GTK
 * shell today, the framebuffer and TUI shells later -- sees the same C23 API
 * and links the same implementation. Playback belongs to the long-lived shell
 * process: build/distill is one-shot and exits after emitting its artifact,
 * so it can only ever fetch the file.
 *
 * One stream plays at a time, and nothing plays by itself.
 * See src/documents/MEDIA_PLAYBACK_PLAN.md.
 */
#ifndef DISTILL_MEDIA_AUDIO_H
#define DISTILL_MEDIA_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MEDIA_AUDIO_STOPPED = 0,
    MEDIA_AUDIO_LOADING,
    MEDIA_AUDIO_PLAYING,
    MEDIA_AUDIO_PAUSED,
    MEDIA_AUDIO_FINISHED,
    MEDIA_AUDIO_ERROR
} MediaAudioPhase;

#define MEDIA_AUDIO_ERROR_MAX 160
#define MEDIA_AUDIO_ITEM_MAX 512

/* A snapshot, never a live view: the front end reads this on its own tick and
   is never handed a pointer the audio thread is writing through. */
typedef struct {
    MediaAudioPhase phase;
    double position_seconds;
    double duration_seconds;
    unsigned sample_rate;
    float volume;
    bool seekable;
    char item[MEDIA_AUDIO_ITEM_MAX];
    char error_message[MEDIA_AUDIO_ERROR_MAX];
} MediaAudioState;

/* Open a decoded-from-disk file. `item` is the caller's own identifier for
   what is playing, carried through snapshots so a front end can tell which
   card the transport belongs to. Replaces whatever was playing. */
bool media_audio_open(const char *path, const char *item);

bool media_audio_play(void);
void media_audio_pause(void);
void media_audio_stop(void);          /* rewinds; keeps the file open */
bool media_audio_seek(double seconds);
void media_audio_set_volume(float volume);   /* clamped to 0.0 .. 1.0 */
void media_audio_snapshot(MediaAudioState *out);
void media_audio_close(void);         /* releases the device and the file */

/* True while a device is attached -- that is, after an open succeeded and
   before the next close. Not a probe: before the first open it is false
   whether or not the machine has a device. */
bool media_audio_available(void);

/* Test seam. Opens the decoder with no device at all, so position, duration
   and end-of-stream can be asserted exactly rather than raced against a
   backend that paces itself in wall clock. */
bool media_audio_open_silent(const char *path, const char *item);
size_t media_audio_pull(size_t frames);

#endif
