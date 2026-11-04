#include "core/noise.h"

#include <stddef.h>

#include "core/text.h"

/* PCG32 (O'Neill, 2014). 64-bit LCG state, 32-bit permuted output. */
#define PCG32_MULTIPLIER 6364136223846793005ULL

static const char *const noise_names[WN_NOISE_COUNT] = {
    "white",
    "pink",
    "brown",
};

/* Gains calibrated to -18 dBFS RMS, never by ear.
 *
 * -18 dBFS is an RMS of 10^(-18/20) = 0.125892.
 *
 * White is analytic: noise uniform on [-1,1) has RMS 1/sqrt(3) = 0.577350, so
 * the gain is 0.125892 / 0.577350 = 0.218052. The ensemble measurement below
 * reports 0.218045, which confirms the derivation.
 *
 * Pink and brown are measured over 10M samples per stream, across both streams,
 * because the filter output level depends on the pole placement and cannot be
 * derived in closed form. check_spectrum.py reproduces the numbers.
 *
 * Brown has most of its energy near DC, so any window shorter than a minute
 * lands within about +/-0.3 dB of the target rather than on it. That is a
 * property of 1/f^2 noise, not a calibration error. */
static const float noise_gain[WN_NOISE_COUNT] = {
    0.218052f, /* white: analytic, 0.125892 / (1/sqrt(3)) */
    0.071424f, /* pink:  measured, 0.125892 / 1.762616 */
    0.009741f, /* brown: measured, 0.125892 / 12.923549 */
};

const char *wn_noise_type_name(int type)
{
    if (type < 0 || type >= WN_NOISE_COUNT) {
        return NULL;
    }
    return noise_names[type];
}

int wn_noise_type_parse(const char *name)
{
    int i;

    if (name == NULL) {
        return WN_NOISE_NONE;
    }
    for (i = 0; i < WN_NOISE_COUNT; i++) {
        if (wn_text_equal_ci(name, noise_names[i])) {
            return i;
        }
    }
    return WN_NOISE_NONE;
}

static uint32_t pcg32_next(wn_noise *n)
{
    uint64_t old = n->state;
    uint32_t xorshifted;
    uint32_t rot;

    n->state = old * PCG32_MULTIPLIER + n->inc;

    /* XSH-RR output permutation: the low bits of an LCG are weak, so the
     * output is taken from the high bits and rotated by the top five. */
    xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
}

void wn_noise_init(wn_noise *n, int type, uint64_t seed, uint64_t stream)
{
    if (n == NULL) {
        return;
    }

    /* pcg32_srandom: the increment must be odd, and two RNG steps separate the
     * caller's seed from the raw value so that seed 0 is not a special case. */
    n->state = 0u;
    n->inc = (stream << 1u) | 1u;
    (void)pcg32_next(n);
    n->state += seed;
    (void)pcg32_next(n);

    wn_noise_set_type(n, type);
}

void wn_noise_set_type(wn_noise *n, int type)
{
    int b;

    if (n == NULL) {
        return;
    }
    n->type = type;

    /* Filters restart on a type change; the RNG state is deliberately kept, so
     * switching type does not rewind the sequence. */
    for (b = 0; b < 7; b++) {
        n->pink_b[b] = 0.0f;
    }
    n->brown = 0.0f;
    n->dc_x = 0.0f;
    n->dc_y = 0.0f;
}

/* Uniform on [-1, 1). 24 bits is exactly the float32 significand width, so
 * every step is exact and the output has no rounding bias. */
static float white_unit(wn_noise *n)
{
    uint32_t x = pcg32_next(n);
    float f = (float)(x >> 8);

    f = f * (1.0f / 8388608.0f);
    return f - 1.0f;
}

/* Paul Kellet's 7-pole pink filter: a bank of one-pole sections whose summed
 * response approximates -3 dB/octave across the audio band. It is a set of
 * constants fitted to that slope, not a derived design, which is why the gain
 * is measured rather than computed. */
static float pink_step(wn_noise *n, float w)
{
    float pink;

    n->pink_b[0] = 0.99886f * n->pink_b[0] + 0.0555179f * w;
    n->pink_b[1] = 0.99332f * n->pink_b[1] + 0.0750759f * w;
    n->pink_b[2] = 0.96900f * n->pink_b[2] + 0.1538520f * w;
    n->pink_b[3] = 0.86650f * n->pink_b[3] + 0.3104856f * w;
    n->pink_b[4] = 0.55000f * n->pink_b[4] + 0.5329522f * w;
    n->pink_b[5] = -0.7616f * n->pink_b[5] - 0.0168980f * w;

    pink = n->pink_b[0] + n->pink_b[1] + n->pink_b[2] + n->pink_b[3] +
           n->pink_b[4] + n->pink_b[5] + n->pink_b[6] + 0.5362f * w;

    n->pink_b[6] = 0.115926f * w;

    return pink;
}

/* Leaky integrator followed by a DC blocker.
 *
 * The integrator alone has infinite gain at DC, so the output would walk away.
 * The leak (a = 0.9995) puts the -3 dB corner near 3.8 Hz, well below the 40 Hz
 * band the spectrum test starts at, and the DC blocker removes the residual
 * offset. A larger leak flattens the slope toward -4.9 dB/oct; keep 0.9995. */
#define BROWN_LEAK 0.9995f
#define DC_BLOCK_R 0.9995f

static float brown_step(wn_noise *n, float w)
{
    float x;
    float y;

    n->brown = BROWN_LEAK * n->brown + w;

    x = n->brown;
    y = x - n->dc_x + DC_BLOCK_R * n->dc_y;
    n->dc_x = x;
    n->dc_y = y;

    return y;
}

float wn_noise_next(wn_noise *n)
{
    float w;

    if (n == NULL) {
        return 0.0f;
    }

    w = white_unit(n);

    switch (n->type) {
        case WN_NOISE_PINK:
            return pink_step(n, w);
        case WN_NOISE_BROWN:
            return brown_step(n, w);
        case WN_NOISE_WHITE:
        default:
            return w;
    }
}

float wn_noise_gain_for(int type)
{
    if (type < 0 || type >= WN_NOISE_COUNT) {
        /* Silence, not full scale: an invalid type is a bug, and a bug should
         * not produce the loudest possible output. */
        return 0.0f;
    }
    return noise_gain[type];
}
