#ifndef DISTILL_MINIAUDIO_CONFIG_H
#define DISTILL_MINIAUDIO_CONFIG_H

/* The miniaudio options every translation unit that includes miniaudio.h
   must agree on: the audio core, which decodes, and the platform's audio
   backend, which compiles miniaudio's implementation. Which device backends
   that implementation carries is the platform's choice and is made there;
   where the platform has its own audio service (Muroxi), the implementation
   is built decoding-only with MA_NO_DEVICE_IO, since miniaudio's device
   layer loads its backends with dlopen. */

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#endif
