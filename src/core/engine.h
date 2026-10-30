#ifndef WN_CORE_ENGINE_H
#define WN_CORE_ENGINE_H

#include "core/config.h"

/* Owns the observable state (playing, type, volume) and the fades. This is the
 * only entry point the audio callback may call.
 *
 * Implementation starts in M5 (create/command/render) and is completed in M6
 * (atomics, fades, auto-stop). The declarations are fixed now so audio/ and
 * platform/ can be written against them. */
typedef struct wn_engine wn_engine;

wn_engine *wn_engine_create(const wn_config *cfg);
void wn_engine_destroy(wn_engine *e);

/* Applies one command line ("noise pink"). Returns 0 on success, non-zero for
 * an unknown or invalid command. */
int wn_engine_command(wn_engine *e, const char *line);

/* Writes `frames` interleaved stereo frames. Must not allocate, lock, log, or
 * perform I/O. Parameters are read once per buffer. */
void wn_engine_render(wn_engine *e, float *out, unsigned frames);

int wn_engine_is_playing(const wn_engine *e);

#endif /* WN_CORE_ENGINE_H */
