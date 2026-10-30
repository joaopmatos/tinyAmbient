#ifndef WN_CORE_NOISE_H
#define WN_CORE_NOISE_H

#include <stdint.h>

#define WN_SAMPLE_RATE 48000
#define WN_CHANNELS 2

/* Noise generators, in the order the CLI accepts them. WN_NOISE_NONE is the
 * "unset" value returned by wn_noise_type_parse on failure. */
typedef enum {
    WN_NOISE_NONE = -1,
    WN_NOISE_WHITE = 0,
    WN_NOISE_PINK = 1,
    WN_NOISE_BROWN = 2,
    WN_NOISE_COUNT = 3
} wn_noise_type;

/* One generator: PCG32 state plus the filter state for the current type.
 *
 * `inc` is the PCG32 increment, which selects a stream. Two generators with the
 * same seed and different streams produce disjoint sequences, so the channels
 * are decorrelated by construction rather than by offsetting one seed. */
typedef struct {
    uint64_t state;
    uint64_t inc;
    int type;
    float pink_b[7];  /* Paul Kellet, 7 poles */
    float brown;      /* leaky integrator */
    float dc_x, dc_y; /* DC blocker */
} wn_noise;

/* Static name in lowercase, or NULL for an out-of-range value. */
const char *wn_noise_type_name(int type);

/* Parses a lowercase or mixed-case name. Returns WN_NOISE_NONE on failure. */
int wn_noise_type_parse(const char *name);

/* Seeds the generator. `stream` selects the PCG32 sequence; callers pass the
 * channel index. Same (type, seed, stream) produces an identical sequence. */
void wn_noise_init(wn_noise *n, int type, uint64_t seed, uint64_t stream);

/* Switches type without touching the RNG state. */
void wn_noise_set_type(wn_noise *n, int type);

/* Returns the next sample in [-1.0, 1.0). Advances the RNG. */
float wn_noise_next(wn_noise *n);

/* Calibrated linear gain that puts the type at -18 dBFS RMS. */
float wn_noise_gain_for(int type);

#endif /* WN_CORE_NOISE_H */
