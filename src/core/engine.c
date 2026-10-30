#include "core/engine.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "core/noise.h"

/* Everything the engine needs between calls. The audio thread reads the atomic
 * counter and, from M6, the atomic parameters; everything else is main-thread
 * only until then. */
struct wn_engine {
    wn_noise noise[WN_CHANNELS];
    uint64_t seed;
    float gain; /* calibrated type gain scaled by volume */
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
    e->mono = cfg->mono != 0;
    e->playing = 1;
    e->gain = wn_noise_gain_for(cfg->noise) * volume_to_linear(cfg->volume);
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

void wn_engine_render(wn_engine *e, float *out, unsigned frames)
{
    unsigned i;

    if (e == NULL || out == NULL) {
        return;
    }

    for (i = 0; i < frames; i++) {
        float left = wn_noise_next(&e->noise[0]) * e->gain;
        float right;

        /* Mono generates one channel and duplicates it, so both channels stay
         * bit-identical (PRD FR-13). */
        if (e->mono) {
            right = left;
        } else {
            right = wn_noise_next(&e->noise[1]) * e->gain;
        }

        /* Soft clip is a safety net: the calibrated gain keeps the signal well
         * below it. Pending M2/M6, it guards against a bad constant. */
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

unsigned long wn_engine_frames_rendered(const wn_engine *e)
{
    if (e == NULL) {
        return 0;
    }
    return atomic_load_explicit(&e->frames_rendered, memory_order_relaxed);
}
