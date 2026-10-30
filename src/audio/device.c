/* The single translation unit that compiles miniaudio.
 *
 * The MA_NO_* defines strip the decoders, encoders, resource manager, node
 * graph, engine, and waveform generators before compilation, so --gc-sections
 * has less to remove and the size budget stays reachable. See AGENTS.md 4.
 *
 * ALSA and PulseAudio are resolved with dlopen by miniaudio, so this file does
 * not need their development headers. */

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_MP3
#define MA_NO_FLAC
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio/device.h"

/* wn_audio_start and wn_audio_stop land in M4. Keeping the miniaudio
 * implementation in the build from M0 onwards means every size measurement
 * already includes the real cost of the audio backend. */
