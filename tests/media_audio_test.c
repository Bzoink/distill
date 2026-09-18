#define _POSIX_C_SOURCE 200809L
/* The audio core, driven with no device at all.
 *
 * miniaudio's null backend paces itself in wall clock, which makes it a smoke
 * test rather than a source of deterministic assertions. These tests pull
 * frames through the same routine the audio callback runs, so position,
 * duration, seeking and end-of-stream are exact.
 *
 * The fixture is a WAV written here rather than a binary checked into the
 * corpus: the point is the decoder and the state machine, and a generated
 * file keeps both deterministic.
 */
#include "../src/media_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>

#define RATE 8000
#define SECONDS 2
#define FRAMES (RATE * SECONDS)

static int failures;

static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) ++failures;
}

static void put32(FILE *fh, unsigned value)
{
    fputc(value & 255, fh); fputc((value >> 8) & 255, fh);
    fputc((value >> 16) & 255, fh); fputc((value >> 24) & 255, fh);
}

static void put16(FILE *fh, unsigned value)
{
    fputc(value & 255, fh); fputc((value >> 8) & 255, fh);
}

/* Two seconds of a quiet sine, 16-bit mono. */
static bool write_wav(const char *path)
{
    FILE *fh = fopen(path, "wb");
    if (!fh) return false;
    unsigned data_bytes = FRAMES * 2;
    fwrite("RIFF", 1, 4, fh); put32(fh, 36 + data_bytes); fwrite("WAVE", 1, 4, fh);
    fwrite("fmt ", 1, 4, fh); put32(fh, 16); put16(fh, 1); put16(fh, 1);
    put32(fh, RATE); put32(fh, RATE * 2); put16(fh, 2); put16(fh, 16);
    fwrite("data", 1, 4, fh); put32(fh, data_bytes);
    for (unsigned i = 0; i < FRAMES; ++i) {
        int sample = (int)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * i / RATE));
        put16(fh, (unsigned)(sample & 0xffff));
    }
    fclose(fh);
    return true;
}

int main(void)
{
    char path[] = "/tmp/distill-media-audio-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { fprintf(stderr, "no temporary file\n"); return 1; }
    close(fd);
    if (!write_wav(path)) { fprintf(stderr, "could not write %s\n", path); return 1; }

    MediaAudioState state;

    check(!media_audio_open_silent("/nonexistent/not-a-file.mp3", "missing"),
          "a missing file fails to open");
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_ERROR, "a failed open reports an error phase");
    check(state.error_message[0] != '\0', "a failed open carries a message");

    check(media_audio_open_silent(path, "episode-14"), "a WAV opens");
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_PAUSED, "an opened file waits rather than playing");
    check(strcmp(state.item, "episode-14") == 0, "the caller's item id comes back");
    check(state.sample_rate == RATE, "the sample rate is reported");
    check(fabs(state.duration_seconds - SECONDS) < 0.01, "the duration is reported");
    check(state.position_seconds == 0, "playback starts at the beginning");
    check(state.seekable, "a file of known length is seekable");

    check(media_audio_pull(1000) == 0, "a paused file decodes nothing");
    media_audio_snapshot(&state);
    check(state.position_seconds == 0, "a paused file does not advance");

    check(media_audio_play(), "play starts");
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_PLAYING, "play reaches the playing phase");

    check(media_audio_pull(RATE / 2) == RATE / 2, "half a second decodes whole");
    media_audio_snapshot(&state);
    check(fabs(state.position_seconds - 0.5) < 0.001, "position follows the frames decoded");

    media_audio_pause();
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_PAUSED, "pause reaches the paused phase");
    media_audio_pull(RATE);
    media_audio_snapshot(&state);
    check(fabs(state.position_seconds - 0.5) < 0.001, "a paused file holds its position");

    media_audio_play();
    check(media_audio_seek(1.5), "a seek is accepted");
    media_audio_snapshot(&state);
    check(fabs(state.position_seconds - 1.5) < 0.001, "the position moves with the seek");
    check(media_audio_pull(RATE / 4) == RATE / 4, "decoding continues after a seek");
    media_audio_snapshot(&state);
    check(fabs(state.position_seconds - 1.75) < 0.01, "the seek moved the decoder, not just the label");

    /* Past the end: the remainder decodes short and stops. */
    size_t got = media_audio_pull(RATE);
    check(got < RATE, "the last pull returns fewer frames than asked");
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_FINISHED, "end of stream reaches the finished phase");
    check(state.position_seconds == 0, "the finished file rewinds");
    check(media_audio_pull(RATE) == 0, "a finished file does not play on by itself");

    check(media_audio_seek(30.0), "a seek past the end is accepted");
    media_audio_snapshot(&state);
    check(state.position_seconds <= SECONDS + 0.001, "a seek past the end clamps to the duration");

    check(media_audio_play(), "a finished file plays again when asked");
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_PLAYING, "replay reaches the playing phase");

    media_audio_stop();
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_STOPPED, "stop reaches the stopped phase");
    check(state.position_seconds == 0, "stop rewinds");

    media_audio_set_volume(0.25f);
    media_audio_snapshot(&state);
    check(fabs(state.volume - 0.25) < 0.001, "volume is remembered");
    media_audio_set_volume(4.0f);
    media_audio_snapshot(&state);
    check(state.volume == 1.0f, "volume clamps at full");
    media_audio_set_volume(-1.0f);
    media_audio_snapshot(&state);
    check(state.volume == 0.0f, "volume clamps at silence");

    /* Repeated open/close must not leak the device or the decoder. */
    for (int i = 0; i < 8; ++i) {
        check(media_audio_open_silent(path, "repeat"), "repeated open succeeds");
        media_audio_play();
        media_audio_pull(RATE / 8);
        media_audio_close();
    }
    media_audio_snapshot(&state);
    check(state.phase == MEDIA_AUDIO_STOPPED, "close returns to stopped");
    check(state.item[0] == '\0', "close forgets what was playing");
    check(media_audio_pull(128) == 0, "a closed core decodes nothing");
    check(!media_audio_play(), "a closed core refuses to play");

    unlink(path);
    printf(failures ? "\nFAILED %d check(s)\n" : "\nOK\n", failures);
    return failures ? 1 : 0;
}
