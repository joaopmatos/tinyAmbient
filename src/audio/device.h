#ifndef WN_AUDIO_DEVICE_H
#define WN_AUDIO_DEVICE_H

#include "core/engine.h"

/* 48 kHz, float32, stereo. Opens the default output device and starts calling
 * wn_engine_render from the audio thread.
 *
 * Implementation lands in M4. */

/* Returns 0 on success, non-zero when no device could be opened. */
int wn_audio_start(wn_engine *e);

/* Stops the device and waits for the callback to return. Safe to call when no
 * device is open. */
void wn_audio_stop(void);

#endif /* WN_AUDIO_DEVICE_H */
