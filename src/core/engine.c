#include "core/engine.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "core/noise.h"

/* Everything the engine needs between calls. The audio thread reads the atomic
 * counter and, from M6, the atomic parameters; everything else is main-thread
 * only until then. */
struct wn_engine {
    wn_noise noise[WN_CHANNELS];
    uint64_t seed;
    int type;
    int volume;
    int mono;
    int playing;
    /* Written by the audio thread, read by diagnostics. Relaxed is enough:
     * nothing depends on ordering, only on the value eventually being seen. */
    _Atomic unsigned long frames_rendered;
};

/* Volume is linear, 0..100 -> 0.0..1.0 (PRD FR-03). Anything outside the range
 * is a usage error handled by the parser, so it is clamped rather than trusted
 * here: the render path must never emit a value above full scale. */
static float volume_to_linear(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return (float)volume / 100.0f;
}

wn_engine *wn_engine_create(const wn_config *cfg)
{
    wn_engine *e;
    int ch;

    if (cfg == NULL) {
        return NULL;
    }

    e = malloc(sizeof(*e));
    if (e == NULL) {
        return NULL;
    }

    e->seed = 1u;
    e->type = cfg->noise;
    e->volume = cfg->volume;
    e->mono = cfg->mono != 0;
    e->playing = 1;
    atomic_store_explicit(&e->frames_rendered, 0, memory_order_relaxed);

    /* One generator per channel, decorrelated by PCG32 stream rather than by
     * offsetting the seed, so the sequences are provably disjoint. */
    for (ch = 0; ch < WN_CHANNELS; ch++) {
        wn_noise_init(&e->noise[ch], cfg->noise, e->seed, (uint64_t)ch);
    }

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
    e->seed = seed;
    for (ch = 0; ch < WN_CHANNELS; ch++) {
        wn_noise_init(&e->noise[ch], e->noise[ch].type, seed, (uint64_t)ch);
    }
}

static void apply_command(wn_engine *e, const wn_cmd *cmd)
{
    int ch;

    switch (cmd->kind) {
        case WN_CMD_NOISE:
            e->type = cmd->ivalue;
            for (ch = 0; ch < WN_CHANNELS; ch++) {
                wn_noise_set_type(&e->noise[ch], cmd->ivalue);
            }
            break;
        case WN_CMD_VOLUME:
            e->volume = cmd->ivalue;
            break;
        case WN_CMD_PLAY:
            e->playing = 1;
            break;
        case WN_CMD_PAUSE:
            e->playing = 0;
            break;
        case WN_CMD_TOGGLE:
            e->playing = !e->playing;
            break;
        case WN_CMD_MONO:
            e->mono = 1;
            break;
        case WN_CMD_STEREO:
            e->mono = 0;
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

void wn_engine_render(wn_engine *e, float *out, unsigned frames)
{
    float gain;
    int mono;
    int playing;
    unsigned i;

    if (e == NULL || out == NULL) {
        return;
    }

    /* Parameter reads happen once per buffer, never mid-sample-loop. They are
     * plain fields at M5; M6 turns them into atomics. */
    gain = wn_noise_gain_for(e->type) * volume_to_linear(e->volume);
    mono = e->mono;
    playing = e->playing;

    if (!playing) {
        /* A paused engine writes silence and stops generating. M6 replaces this
         * with a 20 ms fade so the transition is click-free. */
        memset(out, 0, sizeof(float) * WN_CHANNELS * frames);
        atomic_fetch_add_explicit(&e->frames_rendered, frames,
                                  memory_order_relaxed);
        return;
    }

    for (i = 0; i < frames; i++) {
        float left = wn_noise_next(&e->noise[0]) * gain;
        float right;

        /* Mono generates one channel and duplicates it, so both channels stay
         * bit-identical (PRD FR-13). */
        if (mono) {
            right = left;
        } else {
            right = wn_noise_next(&e->noise[1]) * gain;
        }

        /* Soft clip is a safety net: the calibrated gain keeps the signal well
         * below it. It guards against a bad constant. */
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

        /* A zero gain still multiplies negative samples into -0.0. Normalize
         * the sign so volume 0 is byte-for-byte silence (FR-03). */
        if (left == 0.0f) {
            left = 0.0f;
        }
        if (right == 0.0f) {
            right = 0.0f;
        }

        out[2 * i] = left;
        out[2 * i + 1] = right;
    }

    atomic_fetch_add_explicit(&e->frames_rendered, frames,
                              memory_order_relaxed);
}

int wn_engine_is_playing(const wn_engine *e)
{
    return e != NULL && e->playing != 0;
}

int wn_engine_noise(const wn_engine *e)
{
    return e != NULL ? e->type : WN_NOISE_NONE;
}

int wn_engine_volume(const wn_engine *e)
{
    return e != NULL ? e->volume : 0;
}

int wn_engine_is_mono(const wn_engine *e)
{
    return e != NULL && e->mono != 0;
}

unsigned long wn_engine_frames_rendered(const wn_engine *e)
{
    if (e == NULL) {
        return 0;
    }
    return atomic_load_explicit(&e->frames_rendered, memory_order_relaxed);
}
