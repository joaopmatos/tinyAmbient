#include "core/status.h"

#include <stdio.h>

#include "core/noise.h"

/* Provided by CMake (PUBLIC on ta_core), so every layer reports one version
 * (PRD FR-15). The fallback keeps a standalone compile working. */
#ifndef TA_VERSION
#define TA_VERSION "0.0.0"
#endif

size_t wn_status_format(char *out, size_t cap, int playing, int noise,
                        int volume, int mono, long stop_after_seconds)
{
    const char *name = wn_noise_type_name(noise);
    size_t used;

    if (name == NULL) {
        name = "pink";
    }

    /* One snprintf for the fixed part, then the two possible tails. The return
     * value keeps measuring even when `out` is too small, exactly like the
     * config formatter, so the caller can size a buffer once. */
    used = (size_t)snprintf(out, cap,
                            "version=%s\n"
                            "playing=%d\n"
                            "noise=%s\n"
                            "volume=%d\n"
                            "mono=%d\n"
                            "stop_after=",
                            TA_VERSION, playing != 0 ? 1 : 0, name, volume,
                            mono != 0 ? 1 : 0);

    if (stop_after_seconds < 0) {
        used += (size_t)snprintf(out != NULL && used < cap ? out + used : NULL,
                                 out != NULL && used < cap ? cap - used : 0,
                                 "off\n");
    } else {
        used += (size_t)snprintf(out != NULL && used < cap ? out + used : NULL,
                                 out != NULL && used < cap ? cap - used : 0,
                                 "%ld\n", stop_after_seconds);
    }

    return used;
}
