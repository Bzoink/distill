/* daudio on Windows: miniaudio's WASAPI device layer.

   This translation unit is also where miniaudio's implementation is
   compiled, decoders included, so the audio core links against it. */

#include "miniaudio_config.h"

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_NULL

#include "../audio_miniaudio.inc"
#include "win.h"

ma_result d_win_decoder_init(const char *path, const ma_decoder_config *config, ma_decoder *decoder)
{
    wchar_t *wide = d_win_wide(path);
    if (!wide) return MA_INVALID_ARGS;
    ma_result result = ma_decoder_init_file_w(wide, config, decoder);
    free(wide);
    return result;
}
