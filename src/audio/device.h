#ifndef WN_AUDIO_DEVICE_H
#define WN_AUDIO_DEVICE_H

#include "core/engine.h"

/* 48 kHz, float32, stereo. Opens the default output device and starts calling
 * wn_engine_render from the audio thread.
 *
 * Implementation lands in M4. */

/* Returns 0 on success, non-zero when no device could be opened. */
int wn_audio_start(wn_engine *engine);

/* Stops the device and waits for the callback to return. Safe to call when no
 * device is open. */
void wn_audio_stop(void);

/* Set by the miniaudio notification callback when the device stops or is
 * removed. The daemon loop (M7) polls this and reinitializes with backoff. */
int wn_audio_needs_restart(void);

#endif /* WN_AUDIO_DEVICE_H */
