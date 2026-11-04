#include "core/text.h"

#include <stdio.h>

int wn_text_equal_ci(const char *a, const char *b)
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

void wn_text_vset(char *buf, size_t cap, const char *fmt, va_list args)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    (void)vsnprintf(buf, cap, fmt, args);
}

void wn_text_set(char *buf, size_t cap, const char *fmt, ...)
{
    va_list args;

    if (buf == NULL || cap == 0) {
        return;
    }

    va_start(args, fmt);
    wn_text_vset(buf, cap, fmt, args);
    va_end(args);
}
