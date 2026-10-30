#include "core/noise.h"

#include <stddef.h>

/* PCG32 (O'Neill, 2014). 64-bit LCG state, 32-bit permuted output. */
#define PCG32_MULTIPLIER 6364136223846793005ULL

static const char *const noise_names[WN_NOISE_COUNT] = {
    "white",
    "pink",
    "brown",
};

/* Gains calibrated to -18 dBFS RMS, never by ear.
 *
 * -18 dBFS is an RMS of 10^(-18/20) = 0.125892. White noise uniform on [-1,1)
 * has RMS 1/sqrt(3) = 0.577350, so the gain is 0.125892 / 0.577350 = 0.218052.
 *
 * Pink and brown land in M2. Their rows stay at zero until check_spectrum.py
 * has measured them, so a wrong gain cannot ship silently. */
static const float noise_gain[WN_NOISE_COUNT] = {
    0.218052f, /* white */
    0.0f,      /* pink,  M2 */
    0.0f,      /* brown, M2 */
};

const char *wn_noise_type_name(int type)
{
    if (type < 0 || type >= WN_NOISE_COUNT) {
        return NULL;
    }
    return noise_names[type];
}

/* Case-insensitive compare without <strings.h>: strcasecmp is POSIX, not C11,
 * and core/ is restricted to the C11 standard library. */
static int ascii_equal_ci(const char *a, const char *b)
{
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
    }
}

int wn_noise_type_parse(const char *name)
{
    int i;

    if (name == NULL) {
        return WN_NOISE_NONE;
    }
    for (i = 0; i < WN_NOISE_COUNT; i++) {
        if (ascii_equal_ci(name, noise_names[i])) {
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
    if (n == NULL) {
        return;
    }
    n->type = type;
    /* Filter state resets land here in M2. The RNG state is deliberately kept:
     * switching type must not rewind the sequence. */
}

float wn_noise_next(wn_noise *n)
{
    uint32_t x;
    float f;

    if (n == NULL) {
        return 0.0f;
    }

    x = pcg32_next(n);

    switch (n->type) {
        case WN_NOISE_WHITE:
        default:
            /* 24 bits, which is exactly the float32 significand width, so every
             * step below is exact: no rounding, and the result is uniform on a
             * 2^24-point grid over [-1, 1). */
            f = (float)(x >> 8);
            f = f * (1.0f / 8388608.0f);
            return f - 1.0f;
    }
}

float wn_noise_gain_for(int type)
{
    if (type < 0 || type >= WN_NOISE_COUNT) {
        return 1.0f;
    }
    return noise_gain[type];
}
