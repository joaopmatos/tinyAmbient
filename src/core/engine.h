#ifndef WN_CORE_ENGINE_H
#define WN_CORE_ENGINE_H

#include "core/config.h"

/* Owns the observable state (playing, type, volume, mono) and, from M6, the
 * fades. This is the only entry point the audio callback may call.
 *
 * M5 applies parameter changes immediately through wn_engine_command. The
 * atomics and the 20 ms/250 ms fades land in M6; until then the command path is
 * exercised by test_engine and by the daemon once M7 gives it a caller. */
typedef struct wn_engine wn_engine;

wn_engine *wn_engine_create(const wn_config *cfg);
void wn_engine_destroy(wn_engine *e);

/* Reseeds both channel generators. Only used by --render, where the output must
 * depend on the seed alone (PRD NFR-06). */
void wn_engine_set_seed(wn_engine *e, uint64_t seed);

/* Applies one command line ("noise pink", "volume 40", "toggle"). Returns 0 on
 * success, non-zero for an unknown or invalid command. The line is copied, so
 * the caller's buffer is left untouched. */
int wn_engine_command(wn_engine *e, const char *line);

/* Writes `frames` interleaved stereo frames. Must not allocate, lock, log, or
 * perform I/O. Parameters are read once per buffer. */
void wn_engine_render(wn_engine *e, float *out, unsigned frames);

int wn_engine_is_playing(const wn_engine *e);

/* Observable state for status output and the IPC layer. */
int wn_engine_noise(const wn_engine *e);
int wn_engine_volume(const wn_engine *e);
int wn_engine_is_mono(const wn_engine *e);

/* Diagnostics: frames produced since the engine was created. Read by the
 * playback test to prove the audio callback actually ran. */
unsigned long wn_engine_frames_rendered(const wn_engine *e);

#endif /* WN_CORE_ENGINE_H */
