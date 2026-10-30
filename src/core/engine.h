#ifndef WN_CORE_ENGINE_H
#define WN_CORE_ENGINE_H

#include "core/config.h"

/* Owns the observable state (playing, type, volume) and the fades. This is the
 * only entry point the audio callback may call.
 *
 * M1 implements create/destroy/render/is_playing with no fades and no atomics.
 * Fades and the atomic parameter handoff land in M6; wn_engine_command in M5. */
typedef struct wn_engine wn_engine;

wn_engine *wn_engine_create(const wn_config *cfg);
void wn_engine_destroy(wn_engine *e);

/* Reseeds both channel generators. Only used by --render, where the output must
 * depend on the seed alone (PRD NFR-06). */
void wn_engine_set_seed(wn_engine *e, uint64_t seed);

/* Applies one command line ("noise pink"). Returns 0 on success, non-zero for
 * an unknown or invalid command. */
int wn_engine_command(wn_engine *e, const char *line);

/* Writes `frames` interleaved stereo frames. Must not allocate, lock, log, or
 * perform I/O. Parameters are read once per buffer. */
void wn_engine_render(wn_engine *e, float *out, unsigned frames);

int wn_engine_is_playing(const wn_engine *e);

/* Diagnostics: frames produced since the engine was created. Read by the
 * playback test to prove the audio callback actually ran. */
unsigned long wn_engine_frames_rendered(const wn_engine *e);

#endif /* WN_CORE_ENGINE_H */
