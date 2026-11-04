#ifndef WN_CORE_ENGINE_H
#define WN_CORE_ENGINE_H

#include "core/command.h"
#include "core/config.h"
#include "core/noise.h"

/* Owns the observable state (playing, type, volume, mono) and the fades. This
 * is the only entry point the audio callback may call.
 *
 * The main thread applies commands through wn_engine_command and requests
 * fades through wn_engine_fade_in/wn_engine_fade_out. Parameters cross the
 * thread boundary as integer atomics and are read once per buffer; the
 * generators and the fade envelopes belong to the audio thread alone. */
typedef struct wn_engine wn_engine;

/* Fade timings from the PRD. Play, pause, volume, and device resume share the
 * short fade (FR-01, FR-03); a noise switch uses the configured fade_ms
 * (FR-04); session expiry fades out over 20 s (FR-05). */
#define WN_FADE_MS 20
#define WN_STOP_FADE_MS 20000

/* The opening ramp a session uses instead of WN_FADE_MS. A fresh daemon should
 * not jump to the saved level, so the first second swells from silence
 * (FR-09). Rendering keeps the short fade, so offline output stays identical. */
#define WN_OPENING_FADE_MS 1000

wn_engine *wn_engine_create(const wn_config *cfg);
void wn_engine_destroy(wn_engine *e);

/* Reseeds both channel generators. Only used by --render, where the output must
 * depend on the seed alone (PRD NFR-06). Must run before the audio thread
 * exists. */
void wn_engine_set_seed(wn_engine *e, uint64_t seed);

/* Replaces the opening ramp of `ms` milliseconds, clamped to 0..5000. The ramp
 * still starts at silence, so the first sample is click-free; it just takes
 * longer to reach the saved level. Zero restores the default WN_FADE_MS. Like
 * wn_engine_set_seed, this must run before the audio thread exists. */
void wn_engine_set_opening_fade(wn_engine *e, unsigned ms);

/* Applies one command line ("noise pink", "volume 40", "toggle"). Returns 0 on
 * success, non-zero for an unknown or invalid command. The line is copied, so
 * the caller's buffer is left untouched. Safe to call from the main thread
 * while the audio thread renders: only atomics are written.
 *
 * The daemon parses forwarded lines itself and calls wn_engine_apply_cmd, so
 * this text entry point is for tests and small embeddings; both end in the same
 * apply path and the same validation rules. */
int wn_engine_command(wn_engine *e, const char *line);

/* Applies one already-parsed setting or playback command. Returns 0 on success,
 * non-zero when the kind is not an engine command. The daemon loop uses this
 * after it has validated a whole line, so a forwarded command does not have to
 * be formatted back into text and parsed again. */
int wn_engine_apply_cmd(wn_engine *e, const wn_cmd *cmd);

/* Fades to silence over `ms` milliseconds and pauses when the fade lands.
 * This is the session timer's expiry (PRD FR-05): the daemon requests it and
 * observes playing drop to 0. A play/pause or volume change during the fade
 * cancels the pause, because it means the user is still working. */
void wn_engine_fade_out(wn_engine *e, unsigned ms);

/* Restarts the gain envelope from silence over WN_FADE_MS. Called after the
 * audio device is reinitialized, so the DAC never sees a step (PRD FR-16). */
void wn_engine_fade_in(wn_engine *e);

/* Writes `frames` interleaved stereo frames. Must not allocate, lock, log, or
 * perform I/O. Parameters are read once per buffer. */
void wn_engine_render(wn_engine *e, float *out, unsigned frames);

int wn_engine_is_playing(const wn_engine *e);

/* Observable state for status output and the IPC layer. Values are the
 * requested state; during a fade the audible signal is still moving. */
int wn_engine_noise(const wn_engine *e);
int wn_engine_volume(const wn_engine *e);
int wn_engine_is_mono(const wn_engine *e);

/* Diagnostics: frames produced since the engine was created. Read by the
 * playback test to prove the audio callback actually ran. */
unsigned long wn_engine_frames_rendered(const wn_engine *e);

#endif /* WN_CORE_ENGINE_H */
