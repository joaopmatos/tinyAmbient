#ifndef WN_CORE_STATUS_H
#define WN_CORE_STATUS_H

#include <stddef.h>

/* Status text for PRD FR-08. Formatting lives in core/ so the daemon reply and
 * the no-daemon fallback in ui/cli.c cannot drift apart: both produce the same
 * six lines from the same function. */

/* Remaining stop-after seconds meaning "no timer armed". */
#define WN_STOP_AFTER_SECONDS_OFF (-1L)

/* Writes the six key=value lines, each terminated by a newline. Returns the
 * byte count the full text needs, snprintf-style, so a result >= cap signals
 * truncation. `noise` is a wn_noise_type; `stop_after_seconds` is either
 * WN_STOP_AFTER_SECONDS_OFF or a positive count. */
size_t wn_status_format(char *out, size_t cap, int playing, int noise,
                        int volume, int mono, long stop_after_seconds);

#endif /* WN_CORE_STATUS_H */
