/* daudio on Linux: miniaudio's device layer, over PulseAudio or ALSA.

   This translation unit is also where miniaudio's implementation is
   compiled, decoders included, so the audio core links against it. */

#include "miniaudio_config.h"

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_NULL

#include "../audio_miniaudio.inc"
