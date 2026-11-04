#include "core/engine.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "core/noise.h"

/* Frames in a fade of `ms` milliseconds. The operand order keeps the product in
 * unsigned 32-bit: 60000 ms * 48000 Hz would overflow int. */
static unsigned frames_for_ms(unsigned ms)
{
    return ms * (unsigned)WN_SAMPLE_RATE / 1000u;
}

#define WN_FADE_FRAMES ((unsigned)WN_FADE_MS * (unsigned)WN_SAMPLE_RATE / 1000u)

/* Envelope target from the per-mille volume. Clamped rather than trusted: the
 * render path must never produce a value above full scale. */
static float milli_to_gain(int milli)
{
    if (milli < 0) {
        milli = 0;
    } else if (milli > 1000) {
        milli = 1000;
    }
    return (float)milli / 1000.0f;
}

enum {
    WN_FADE_OUT_NONE = 0,
    /* A stop fade longer than this is indistinguishable from silence anyway,
     * and the cap keeps frames_for_ms() well inside 32 bits. */
    WN_FADE_OUT_MAX_MS = 60000,
    /* Opening ramps longer than this feel like a malfunction. */
    WN_OPENING_FADE_MAX_MS = 5000
};

/* Everything the engine needs between calls.
 *
 * Sections: values written by the main thread and read by the audio thread
 * (atomics, AGENTS.md 1.2); one-shot requests the audio thread consumes; a
 * diagnostic counter; and the audio thread's own state, which no other thread
 * touches. Relaxed ordering is enough everywhere: each atomic is independent
 * state, and nothing depends on the order in which updates become visible. */
struct wn_engine {
    /* --- main thread writes, audio thread reads ------------------------- */
    _Atomic int type; /* wn_noise_type the output must reach */
    _Atomic int playing;
    _Atomic int volume_milli; /* 0..1000, per-mille of full scale */
    _Atomic int mono;

    /* --- main thread requests, audio thread consumes -------------------- */
    _Atomic int fade_in_req;     /* 1: restart the envelope from silence */
    _Atomic int fade_out_ms;     /* >0: fade to silence over N ms, then pause */
    _Atomic int stop_cancel_req; /* 1: the user resumed during the stop fade */

    /* --- audio thread writes, any thread reads (diagnostics) ------------ */
    _Atomic unsigned long frames_rendered;

    /* --- audio thread only ---------------------------------------------- */
    wn_noise gen[WN_CHANNELS];      /* generators for the target type */
    wn_noise fade_gen[WN_CHANNELS]; /* copy kept live across a crossfade */
    int active_type;                /* type gen[] currently produce */
    int stopping;                   /* a stop fade runs; pause when it lands */
    int stop_target_milli;          /* envelope target when the stop began */

    /* Gain envelope: `gain` is the value at the next sample. A fade moves it
     * toward gain_to over gain_total frames with gain_left remaining. */
    float gain;
    float gain_from;
    float gain_to;
    int gain_to_milli;
    unsigned gain_total;
    unsigned gain_left;

    /* Noise crossfade, in frames. xfade_ms is fixed at creation. */
    unsigned xfade_ms;
    unsigned xfade_total;
    unsigned xfade_left;
    float xfade_t;
};

/* Starts one linear gain fade from the current envelope value. Everything the
 * render loop needs to advance it is in the engine, so a fade can be replaced
 * at any buffer boundary without a discontinuity. */
static void start_gain_fade(wn_engine *e, int to_milli, unsigned frames)
{
    e->gain_from = e->gain;
    e->gain_to_milli = to_milli;
    e->gain_to = milli_to_gain(to_milli);
    e->gain_total = frames > 0 ? frames : 1;
    e->gain_left = e->gain_total;
}

wn_engine *wn_engine_create(const wn_config *cfg)
{
    wn_engine *e;
    int volume_milli;
    int ch;

    if (cfg == NULL) {
        return NULL;
    }

    e = malloc(sizeof(*e));
    if (e == NULL) {
        return NULL;
    }

    volume_milli = cfg->volume;
    if (volume_milli < 0) {
        volume_milli = 0;
    } else if (volume_milli > 100) {
        volume_milli = 100;
    }
    volume_milli *= 10;

    atomic_store_explicit(&e->type, cfg->noise, memory_order_relaxed);
    atomic_store_explicit(&e->playing, 1, memory_order_relaxed);
    atomic_store_explicit(&e->volume_milli, volume_milli, memory_order_relaxed);
    atomic_store_explicit(&e->mono, cfg->mono != 0, memory_order_relaxed);
    atomic_store_explicit(&e->fade_in_req, 0, memory_order_relaxed);
    atomic_store_explicit(&e->fade_out_ms, 0, memory_order_relaxed);
    atomic_store_explicit(&e->stop_cancel_req, 0, memory_order_relaxed);
    atomic_store_explicit(&e->frames_rendered, 0, memory_order_relaxed);

    /* One generator per channel, decorrelated by PCG32 stream rather than by
     * offsetting the seed, so the sequences are provably disjoint. The fade
     * copies start out identical to the live ones and are only used once a
     * crossfade begins. */
    for (ch = 0; ch < WN_CHANNELS; ch++) {
        wn_noise_init(&e->gen[ch], cfg->noise, 1u, (uint64_t)ch);
        wn_noise_init(&e->fade_gen[ch], cfg->noise, 1u, (uint64_t)ch);
    }
    e->active_type = cfg->noise;
    e->stopping = 0;
    e->stop_target_milli = 0;

    e->xfade_ms = 0;
    if (cfg->fade_ms > 0) {
        e->xfade_ms = (unsigned)cfg->fade_ms;
        if (e->xfade_ms > 2000u) {
            e->xfade_ms = 2000u;
        }
    }
    e->xfade_total = 0;
    e->xfade_left = 0;
    e->xfade_t = 1.0f;

    /* Click-free start: the envelope opens from silence over WN_FADE_MS, the
     * same fade a re-opened device gets. Playback and --render share this, so
     * a render matches what a DAC would receive (ARCHITECTURE 12). */
    e->gain = 0.0f;
    e->gain_from = 0.0f;
    e->gain_to_milli = volume_milli;
    e->gain_to = milli_to_gain(volume_milli);
    e->gain_total = WN_FADE_FRAMES;
    e->gain_left = WN_FADE_FRAMES;

    return e;
}

void wn_engine_destroy(wn_engine *e)
{
    free(e);
}

void wn_engine_set_seed(wn_engine *e, uint64_t seed)
{
    int ch;

    if (e == NULL) {
        return;
    }

    /* Called before the audio thread exists, so touching the generators
     * directly is safe. */
    for (ch = 0; ch < WN_CHANNELS; ch++) {
        wn_noise_init(&e->gen[ch], e->gen[ch].type, seed, (uint64_t)ch);
    }
}

void wn_engine_set_opening_fade(wn_engine *e, unsigned ms)
{
    unsigned frames;

    if (e == NULL ||
        atomic_load_explicit(&e->frames_rendered, memory_order_relaxed) != 0) {
        return;
    }

    if (ms == 0) {
        ms = WN_FADE_MS;
    } else if (ms > WN_OPENING_FADE_MAX_MS) {
        ms = WN_OPENING_FADE_MAX_MS;
    }
    frames = frames_for_ms(ms);

    /* The envelope is already positioned at silence by wn_engine_create; only
     * the length of the ramp changes. */
    e->gain_total = frames;
    e->gain_left = frames;
}

static void apply_command(wn_engine *e, const wn_cmd *cmd)
{
    switch (cmd->kind) {
        case WN_CMD_NOISE:
            /* Only the target moves. The audio thread copies the outgoing
             * generators and starts the crossfade at a buffer boundary. */
            atomic_store_explicit(&e->type, cmd->ivalue, memory_order_relaxed);
            break;
        case WN_CMD_VOLUME:
            atomic_store_explicit(&e->volume_milli, cmd->ivalue * 10,
                                  memory_order_relaxed);
            break;
        case WN_CMD_PLAY:
            atomic_store_explicit(&e->playing, 1, memory_order_relaxed);
            /* A resume during a stop fade is the user still working: the
             * audio thread cancels the pending pause at the next buffer. */
            atomic_store_explicit(&e->stop_cancel_req, 1, memory_order_relaxed);
            break;
        case WN_CMD_PAUSE:
            atomic_store_explicit(&e->playing, 0, memory_order_relaxed);
            break;
        case WN_CMD_TOGGLE:
            atomic_store_explicit(
                &e->playing,
                !atomic_load_explicit(&e->playing, memory_order_relaxed),
                memory_order_relaxed);
            /* The cancel path fades to whatever `playing` now targets, so a
             * toggle-to-pause behaves exactly like the volume-change cancel. */
            atomic_store_explicit(&e->stop_cancel_req, 1, memory_order_relaxed);
            break;
        case WN_CMD_MONO:
            atomic_store_explicit(&e->mono, 1, memory_order_relaxed);
            break;
        case WN_CMD_STEREO:
            atomic_store_explicit(&e->mono, 0, memory_order_relaxed);
            break;
        default:
            break;
    }
}

static int command_supported(wn_cmd_kind kind)
{
    return kind == WN_CMD_NOISE || kind == WN_CMD_VOLUME ||
           kind == WN_CMD_PLAY || kind == WN_CMD_PAUSE ||
           kind == WN_CMD_TOGGLE || kind == WN_CMD_MONO ||
           kind == WN_CMD_STEREO;
}

int wn_engine_apply_cmd(wn_engine *e, const wn_cmd *cmd)
{
    if (e == NULL || cmd == NULL || !command_supported(cmd->kind)) {
        return -1;
    }
    apply_command(e, cmd);
    return 0;
}

int wn_engine_command(wn_engine *e, const char *line)
{
    char buf[WN_LINE_MAX];
    wn_cmd cmds[WN_CMD_MAX];
    char err[96];
    size_t len;
    int n;
    int i;

    if (e == NULL || line == NULL) {
        return -1;
    }

    len = strlen(line);
    if (len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, line, len + 1);

    n = wn_cmd_parse_line(buf, cmds, WN_CMD_MAX, err, sizeof(err));
    if (n <= 0) {
        return -1;
    }

    /* Validate the whole line before touching state: a command with an
     * unsupported part must not leave the engine half-applied. */
    for (i = 0; i < n; i++) {
        if (!command_supported(cmds[i].kind)) {
            return -1;
        }
    }

    for (i = 0; i < n; i++) {
        apply_command(e, &cmds[i]);
    }

    return 0;
}

void wn_engine_fade_out(wn_engine *e, unsigned ms)
{
    if (e == NULL) {
        return;
    }
    if (ms == 0) {
        ms = 1;
    } else if (ms > WN_FADE_OUT_MAX_MS) {
        ms = WN_FADE_OUT_MAX_MS;
    }
    atomic_store_explicit(&e->fade_out_ms, (int)ms, memory_order_relaxed);
}

void wn_engine_fade_in(wn_engine *e)
{
    if (e == NULL) {
        return;
    }
    atomic_store_explicit(&e->fade_in_req, 1, memory_order_relaxed);
}

void wn_engine_render(wn_engine *e, float *out, unsigned frames)
{
    int noise_type;
    int playing;
    int volume_milli;
    int is_mono;
    int target_milli;
    int fade_in;
    int stop_ms;
    int stop_cancel;
    int ch;

    if (e == NULL || out == NULL) {
        return;
    }

    /* One read per parameter per buffer (AGENTS.md 1.2, PRD NFR-02). */
    noise_type = atomic_load_explicit(&e->type, memory_order_relaxed);
    playing = atomic_load_explicit(&e->playing, memory_order_relaxed);
    volume_milli = atomic_load_explicit(&e->volume_milli, memory_order_relaxed);
    is_mono = atomic_load_explicit(&e->mono, memory_order_relaxed);
    target_milli = playing ? volume_milli : 0;

    /* Consume the main thread's fade requests. A stop request wins over a
     * resume request: expiry is authoritative, and the envelope starts from
     * silence either way, which is exactly what a fresh DAC needs. */
    fade_in =
        atomic_exchange_explicit(&e->fade_in_req, 0, memory_order_relaxed);
    stop_ms =
        atomic_exchange_explicit(&e->fade_out_ms, 0, memory_order_relaxed);
    stop_cancel =
        atomic_exchange_explicit(&e->stop_cancel_req, 0, memory_order_relaxed);

    if (fade_in) {
        e->gain = 0.0f;
        start_gain_fade(e, e->stopping ? 0 : target_milli, WN_FADE_FRAMES);
    }
    if (stop_ms > 0) {
        e->stop_target_milli = target_milli;
        start_gain_fade(e, 0, frames_for_ms((unsigned)stop_ms));
        e->stopping = 1;
    } else if (e->stopping) {
        /* A playing or volume change during the stop fade means the user is
         * still working, so the pause is cancelled. Expiry is authoritative
         * when both land in the same buffer: the branch above wins and this
         * one is not reached. */
        if (stop_cancel || target_milli != e->stop_target_milli) {
            start_gain_fade(e, target_milli, WN_FADE_FRAMES);
            e->stopping = 0;
        }
    } else if (target_milli != e->gain_to_milli) {
        /* Play, pause, and volume all ride the same 20 ms envelope. */
        start_gain_fade(e, target_milli, WN_FADE_FRAMES);
    }

    /* A type change crossfades over fade_ms (FR-04). The outgoing generators
     * are copied first, so the old signal continues from exactly the sample it
     * would have produced next and the mix starts at the audible value instead
     * of stepping. While silent the switch is instant. A second change during
     * a running crossfade waits for it to finish, so the mix never jumps. */
    if (noise_type >= 0 && noise_type < WN_NOISE_COUNT &&
        noise_type != e->active_type && e->xfade_left == 0) {
        if (e->xfade_ms > 0 && !(e->gain == 0.0f && e->gain_to_milli == 0)) {
            for (ch = 0; ch < WN_CHANNELS; ch++) {
                e->fade_gen[ch] = e->gen[ch];
                wn_noise_set_type(&e->gen[ch], noise_type);
            }
            e->active_type = noise_type;
            e->xfade_total = frames_for_ms(e->xfade_ms);
            e->xfade_left = e->xfade_total;
            e->xfade_t = 0.0f;
        } else {
            for (ch = 0; ch < WN_CHANNELS; ch++) {
                wn_noise_set_type(&e->gen[ch], noise_type);
            }
            e->active_type = noise_type;
            e->xfade_left = 0;
            e->xfade_t = 1.0f;
        }
    }

    /* Paused, muted, or finished fading out: write exact silence and skip
     * generation entirely. The bookkeeping still advances so a pending stop
     * fade lands and a silent type switch is already applied above. */
    if (e->gain == 0.0f && e->gain_to_milli == 0) {
        memset(out, 0, sizeof(float) * WN_CHANNELS * frames);

        if (e->gain_left > frames) {
            e->gain_left -= frames;
        } else {
            e->gain_left = 0;
            e->gain = e->gain_to;
            if (e->stopping) {
                atomic_store_explicit(&e->playing, 0, memory_order_relaxed);
                e->stopping = 0;
            }
        }

        atomic_fetch_add_explicit(&e->frames_rendered, frames,
                                  memory_order_relaxed);
        return;
    }

    {
        float active_gain = wn_noise_gain_for(e->active_type);
        float old_gain = 0.0f;
        float xfade_t = e->xfade_t;
        float xfade_step = 0.0f;
        float gain = e->gain;
        float gain_step = 0.0f;
        unsigned gain_left = e->gain_left;
        unsigned xfade_left = e->xfade_left;
        unsigned i;
        int landed = 0;

        if (xfade_left > 0) {
            old_gain = wn_noise_gain_for(e->fade_gen[0].type);
            xfade_step = 1.0f / (float)e->xfade_total;
        }
        if (gain_left > 0) {
            gain_step = (e->gain_to - e->gain_from) / (float)e->gain_total;
        }

        for (i = 0; i < frames; i++) {
            float left;
            float right;

            if (xfade_left > 0) {
                float a = wn_noise_next(&e->fade_gen[0]) * old_gain;
                float b = wn_noise_next(&e->gen[0]) * active_gain;

                left = a + (b - a) * xfade_t;
                if (is_mono) {
                    right = left;
                } else {
                    a = wn_noise_next(&e->fade_gen[1]) * old_gain;
                    b = wn_noise_next(&e->gen[1]) * active_gain;
                    right = a + (b - a) * xfade_t;
                }
            } else {
                left = wn_noise_next(&e->gen[0]) * active_gain;

                /* Mono generates one channel and duplicates it, so both
                 * channels stay bit-identical (PRD FR-13). */
                right =
                    is_mono ? left : wn_noise_next(&e->gen[1]) * active_gain;
            }

            left *= gain;
            right *= gain;

            /* Soft clip is a safety net: the calibrated gain keeps the signal
             * well below it. It guards against a bad constant. */
            if (left > 1.0f) {
                left = 1.0f;
            } else if (left < -1.0f) {
                left = -1.0f;
            }
            if (right > 1.0f) {
                right = 1.0f;
            } else if (right < -1.0f) {
                right = -1.0f;
            }

            /* A zero gain still multiplies negative samples into -0.0.
             * Normalize the sign so volume 0 is byte-for-byte silence. */
            if (left == 0.0f) {
                left = 0.0f;
            }
            if (right == 0.0f) {
                right = 0.0f;
            }

            out[2 * i] = left;
            out[2 * i + 1] = right;

            if (gain_left > 0) {
                gain += gain_step;
                gain_left--;
                if (gain_left == 0) {
                    gain = e->gain_to;
                    landed = 1;
                }
            }
            if (xfade_left > 0) {
                xfade_t += xfade_step;
                xfade_left--;
                if (xfade_left == 0) {
                    xfade_t = 1.0f;
                }
            }
        }

        e->gain = gain;
        e->gain_left = gain_left;
        e->xfade_t = xfade_t;
        e->xfade_left = xfade_left;

        /* The stop fade reached silence: pause, so the daemon observes
         * playing=0 without inspecting the envelope (PRD FR-05). */
        if (landed && e->stopping) {
            atomic_store_explicit(&e->playing, 0, memory_order_relaxed);
            e->stopping = 0;
        }

        atomic_fetch_add_explicit(&e->frames_rendered, frames,
                                  memory_order_relaxed);
    }
}

int wn_engine_is_playing(const wn_engine *e)
{
    return e != NULL &&
           atomic_load_explicit(&e->playing, memory_order_relaxed) != 0;
}

int wn_engine_noise(const wn_engine *e)
{
    return e != NULL ? atomic_load_explicit(&e->type, memory_order_relaxed)
                     : WN_NOISE_NONE;
}

int wn_engine_volume(const wn_engine *e)
{
    if (e == NULL) {
        return 0;
    }
    /* volume_milli is always a multiple of 10, so the division is exact. */
    return atomic_load_explicit(&e->volume_milli, memory_order_relaxed) / 10;
}

int wn_engine_is_mono(const wn_engine *e)
{
    return e != NULL &&
           atomic_load_explicit(&e->mono, memory_order_relaxed) != 0;
}

unsigned long wn_engine_frames_rendered(const wn_engine *e)
{
    if (e == NULL) {
        return 0;
    }
    return atomic_load_explicit(&e->frames_rendered, memory_order_relaxed);
}
