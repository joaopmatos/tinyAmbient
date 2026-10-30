#include "core/noise.h"

#include <string.h>

static const char *const noise_names[WN_NOISE_COUNT] = {
    "white",
    "pink",
    "brown",
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
