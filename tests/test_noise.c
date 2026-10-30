/* Generator tests. Plain C, no framework: the project allows no dependencies
 * beyond libc/libm. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/noise.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#define SAMPLE_COUNT 4096

static void test_same_seed_is_identical(void)
{
    wn_noise a;
    wn_noise b;
    int i;

    wn_noise_init(&a, WN_NOISE_WHITE, 42u, 0u);
    wn_noise_init(&b, WN_NOISE_WHITE, 42u, 0u);

    for (i = 0; i < SAMPLE_COUNT; i++) {
        float sa = wn_noise_next(&a);
        float sb = wn_noise_next(&b);

        if (sa != sb) {
            CHECK(sa == sb);
            return;
        }
    }
}

static void test_different_stream_differs(void)
{
    wn_noise left;
    wn_noise right;
    int i;
    int differs = 0;

    wn_noise_init(&left, WN_NOISE_WHITE, 42u, 0u);
    wn_noise_init(&right, WN_NOISE_WHITE, 42u, 1u);

    for (i = 0; i < SAMPLE_COUNT; i++) {
        if (wn_noise_next(&left) != wn_noise_next(&right)) {
            differs = 1;
        }
    }

    CHECK(differs);
}

static void test_different_seed_differs(void)
{
    wn_noise a;
    wn_noise b;
    int i;
    int differs = 0;

    wn_noise_init(&a, WN_NOISE_WHITE, 1u, 0u);
    wn_noise_init(&b, WN_NOISE_WHITE, 2u, 0u);

    for (i = 0; i < SAMPLE_COUNT; i++) {
        if (wn_noise_next(&a) != wn_noise_next(&b)) {
            differs = 1;
        }
    }

    CHECK(differs);
}

static void test_range(void)
{
    wn_noise n;
    int i;

    wn_noise_init(&n, WN_NOISE_WHITE, 7u, 0u);

    for (i = 0; i < 100000; i++) {
        float s = wn_noise_next(&n);

        if (!(s >= -1.0f && s < 1.0f)) {
            CHECK(s >= -1.0f && s < 1.0f);
            return;
        }
    }
}

/* RMS at -18 dBFS is the PRD FR-02 target. The gain is only correct if the
 * generator is genuinely uniform, so this also catches a biased mapping. */
static void test_rms(void)
{
    wn_noise n;
    double sum_sq = 0.0;
    double rms;
    double dbfs;
    float gain;
    int i;
    const int count = 1000000;

    wn_noise_init(&n, WN_NOISE_WHITE, 12345u, 0u);
    gain = wn_noise_gain_for(WN_NOISE_WHITE);

    for (i = 0; i < count; i++) {
        double s = (double)wn_noise_next(&n) * (double)gain;

        sum_sq += s * s;
    }

    rms = sqrt(sum_sq / (double)count);
    dbfs = 20.0 * log10(rms);

    CHECK(dbfs > -18.5);
    CHECK(dbfs < -17.5);
}

static void test_mean_is_zero(void)
{
    wn_noise n;
    double sum = 0.0;
    double mean;
    int i;
    const int count = 1000000;

    wn_noise_init(&n, WN_NOISE_WHITE, 999u, 0u);

    for (i = 0; i < count; i++) {
        sum += (double)wn_noise_next(&n);
    }

    mean = sum / (double)count;

    /* A DC offset would eat headroom and show up as a peak at 0 Hz in M2. */
    CHECK(mean > -0.002);
    CHECK(mean < 0.002);
}

static void test_type_names(void)
{
    CHECK(strcmp(wn_noise_type_name(WN_NOISE_WHITE), "white") == 0);
    CHECK(wn_noise_type_name(WN_NOISE_NONE) == NULL);
    CHECK(wn_noise_type_name(WN_NOISE_COUNT) == NULL);

    CHECK(wn_noise_type_parse("white") == WN_NOISE_WHITE);
    CHECK(wn_noise_type_parse("WHITE") == WN_NOISE_WHITE);
    CHECK(wn_noise_type_parse("white ") == WN_NOISE_NONE);
    CHECK(wn_noise_type_parse("") == WN_NOISE_NONE);
    CHECK(wn_noise_type_parse("gray") == WN_NOISE_NONE);
    CHECK(wn_noise_type_parse(NULL) == WN_NOISE_NONE);
}

int main(void)
{
    test_same_seed_is_identical();
    test_different_stream_differs();
    test_different_seed_differs();
    test_range();
    test_rms();
    test_mean_is_zero();
    test_type_names();

    if (failures != 0) {
        fprintf(stderr, "test_noise: %d check(s) failed\n", failures);
        return 1;
    }

    printf("test_noise: ok\n");
    return 0;
}
