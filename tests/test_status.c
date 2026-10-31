/* Status formatter tests. The six-line format in PRD FR-08 is shared by the
 * daemon reply and the no-daemon fallback, so it is a core contract. */

#include <stdio.h>
#include <string.h>

#include "core/noise.h"
#include "core/status.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

#ifndef TA_VERSION
#define TA_VERSION "0.0.0"
#endif

static void test_overflow(void)
{
    char expected[256];
    char actual[256];
    size_t needed;

    (void)snprintf(expected, sizeof(expected),
                   "version=%s\n"
                   "playing=1\n"
                   "noise=pink\n"
                   "volume=40\n"
                   "mono=0\n"
                   "stop_after=off\n",
                   TA_VERSION);

    needed = wn_status_format(actual, sizeof(actual), 1, WN_NOISE_PINK, 40, 0,
                              WN_STOP_AFTER_SECONDS_OFF);
    CHECK(needed == strlen(expected));
    CHECK(strcmp(actual, expected) == 0);
}

static void test_remaining_seconds(void)
{
    char actual[256];

    (void)wn_status_format(actual, sizeof(actual), 0, WN_NOISE_BROWN, 7, 1,
                           3600);
    CHECK(strstr(actual, "playing=0\n") != NULL);
    CHECK(strstr(actual, "noise=brown\n") != NULL);
    CHECK(strstr(actual, "volume=7\n") != NULL);
    CHECK(strstr(actual, "mono=1\n") != NULL);
    CHECK(strstr(actual, "stop_after=3600\n") != NULL);
}

static void test_measure_and_truncate(void)
{
    char small[8];
    size_t needed;
    size_t written;

    needed = wn_status_format(NULL, 0, 1, WN_NOISE_WHITE, 100, 0,
                              WN_STOP_AFTER_SECONDS_OFF);
    CHECK(needed > sizeof(small));

    written = wn_status_format(small, sizeof(small), 1, WN_NOISE_WHITE, 100, 0,
                               WN_STOP_AFTER_SECONDS_OFF);
    /* snprintf semantics: the return value is the full length, not the number
     * of bytes stored, so a truncated call is detectable. */
    CHECK(written == needed);
    CHECK(strlen(small) == sizeof(small) - 1);
}

int main(void)
{
    test_overflow();
    test_remaining_seconds();
    test_measure_and_truncate();

    if (failures != 0) {
        fprintf(stderr, "test_status: %d check(s) failed\n", failures);
        return 1;
    }

    printf("test_status: ok\n");
    return 0;
}
