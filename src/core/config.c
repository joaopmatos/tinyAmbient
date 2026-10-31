#include "core/config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/noise.h"

/* A view into a line. No copying: the parser only needs lengths. */
typedef struct {
    const char *ptr;
    size_t len;
} slice;

static unsigned char lower_ascii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c - 'A' + 'a');
    }
    return c;
}

static int slice_eq_ci(slice s, const char *literal)
{
    size_t i;

    for (i = 0; i < s.len; i++) {
        if (literal[i] == '\0') {
            return 0;
        }
        if (lower_ascii((unsigned char)s.ptr[i]) !=
            lower_ascii((unsigned char)literal[i])) {
            return 0;
        }
    }

    return literal[s.len] == '\0';
}

static void trim(slice *s)
{
    while (s->len > 0 &&
           (s->ptr[0] == ' ' || s->ptr[0] == '\t' || s->ptr[0] == '\r')) {
        s->ptr++;
        s->len--;
    }
    while (s->len > 0 &&
           (s->ptr[s->len - 1] == ' ' || s->ptr[s->len - 1] == '\t' ||
            s->ptr[s->len - 1] == '\r')) {
        s->len--;
    }
}

static void set_warning(char *warn, size_t cap, const char *fmt, ...)
{
    va_list args;

    if (warn == NULL || cap == 0 || warn[0] != '\0') {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(warn, cap, fmt, args);
    va_end(args);
}

/* Copies a short value into a NUL-terminated buffer so libc can parse it. */
static int slice_to_text(slice s, char *out, size_t cap)
{
    if (s.len == 0 || s.len >= cap) {
        return -1;
    }
    memcpy(out, s.ptr, s.len);
    out[s.len] = '\0';
    return 0;
}

static int parse_long(slice s, long *out)
{
    char text[32];
    char *end = NULL;
    long value;

    if (slice_to_text(s, text, sizeof(text)) != 0) {
        return -1;
    }

    value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return -1;
    }

    *out = value;
    return 0;
}

static int parse_int_range(slice s, long lo, long hi, int *out)
{
    long value;

    if (parse_long(s, &value) != 0 || value < lo || value > hi) {
        return -1;
    }

    *out = (int)value;
    return 0;
}

static int known_general_key(slice key)
{
    return slice_eq_ci(key, "noise") || slice_eq_ci(key, "volume") ||
           slice_eq_ci(key, "mono") || slice_eq_ci(key, "fade_ms");
}

/* Returns 1 for [general], 0 for any other valid section, -1 for a malformed
 * header. `body` is the text between the brackets. */
static int parse_section(slice line, slice *body)
{
    slice inner;

    if (line.len < 2 || line.ptr[0] != '[' || line.ptr[line.len - 1] != ']') {
        return -1;
    }

    inner.ptr = line.ptr + 1;
    inner.len = line.len - 2;
    trim(&inner);
    *body = inner;

    return slice_eq_ci(inner, "general");
}

void wn_config_defaults(wn_config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->noise = WN_NOISE_PINK;
    cfg->volume = 40;
    cfg->mono = 0;
    cfg->fade_ms = 250;
}

static void parse_key_value(slice key, slice value, wn_config *out, char *warn,
                            size_t warn_cap)
{
    int parsed;

    if (slice_eq_ci(key, "noise")) {
        char text[16];
        int type;

        if (slice_to_text(value, text, sizeof(text)) != 0) {
            set_warning(warn, warn_cap,
                        "invalid noise value; using the default");
            return;
        }

        type = wn_noise_type_parse(text);
        if (type == WN_NOISE_NONE) {
            set_warning(warn, warn_cap,
                        "invalid noise value; using the default");
            return;
        }
        out->noise = type;
    } else if (slice_eq_ci(key, "volume")) {
        if (parse_int_range(value, 0, 100, &parsed) != 0) {
            set_warning(warn, warn_cap, "invalid volume; using the default");
            return;
        }
        out->volume = parsed;
    } else if (slice_eq_ci(key, "mono")) {
        if (slice_eq_ci(value, "true")) {
            out->mono = 1;
        } else if (slice_eq_ci(value, "false")) {
            out->mono = 0;
        } else {
            set_warning(warn, warn_cap,
                        "invalid mono value; using the default");
        }
    } else if (slice_eq_ci(key, "fade_ms")) {
        long value_ms;

        if (parse_long(value, &value_ms) != 0) {
            set_warning(warn, warn_cap,
                        "invalid fade_ms value; using the default");
        } else if (value_ms < 0) {
            out->fade_ms = 0;
            set_warning(warn, warn_cap, "fade_ms below 0; clamped to 0");
        } else if (value_ms > 2000) {
            out->fade_ms = 2000;
            set_warning(warn, warn_cap, "fade_ms above 2000; clamped to 2000");
        } else {
            out->fade_ms = (int)value_ms;
        }
    }
}

void wn_config_parse(const char *text, size_t len, wn_config *out, char *warn,
                     size_t warn_cap)
{
    size_t pos = 0;
    int in_general = 0;

    if (out == NULL) {
        return;
    }

    wn_config_defaults(out);
    if (warn != NULL && warn_cap > 0) {
        warn[0] = '\0';
    }
    if (text == NULL) {
        return;
    }

    while (pos < len) {
        size_t start = pos;
        size_t end;
        slice line;
        const char *equals;

        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        end = pos;
        if (pos < len && text[pos] == '\n') {
            pos++;
        }

        line.ptr = text + start;
        line.len = end - start;
        trim(&line);

        if (line.len == 0 || line.ptr[0] == '#' || line.ptr[0] == ';') {
            continue;
        }

        if (line.ptr[0] == '[') {
            slice body;
            int general = parse_section(line, &body);

            (void)body;
            if (general < 0) {
                in_general = 0;
                set_warning(warn, warn_cap,
                            "malformed section header; ignoring it");
            } else {
                in_general = general;
            }
            continue;
        }

        equals = memchr(line.ptr, '=', line.len);
        if (equals == NULL) {
            set_warning(warn, warn_cap, "ignoring a malformed config line");
            continue;
        }

        if (!in_general) {
            continue;
        }

        {
            slice key;
            slice value;

            key.ptr = line.ptr;
            key.len = (size_t)(equals - line.ptr);
            value.ptr = equals + 1;
            value.len = (size_t)((line.ptr + line.len) - (equals + 1));
            trim(&key);
            trim(&value);

            if (key.len > 0) {
                parse_key_value(key, value, out, warn, warn_cap);
            }
        }
    }
}

/* Appends formatted text and returns the byte count the full text needs. With
 * out == NULL and cap == 0 it only measures, which is how the caller checks for
 * truncation before allocating or writing. */
static void append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list args;
    int written;

    va_start(args, fmt);
    if (*used < cap) {
        written = vsnprintf(out + *used, cap - *used, fmt, args);
    } else {
        written = vsnprintf(NULL, 0, fmt, args);
    }
    va_end(args);

    if (written > 0) {
        *used += (size_t)written;
    }
}

/* Unknown keys under [general] survive a rewrite. They are emitted before any
 * other section, so they cannot drift into the section that follows them. */
static void preserve_general_keys(const char *original, char *out, size_t cap,
                                  size_t *used)
{
    const char *p = original;
    int in_general = 0;

    while (*p != '\0') {
        const char *start = p;
        size_t len;
        slice line;
        const char *equals;

        while (*p != '\0' && *p != '\n') {
            p++;
        }
        len = (size_t)(p - start);
        if (*p == '\n') {
            p++;
        }

        line.ptr = start;
        line.len = len;
        trim(&line);
        if (line.len == 0 || line.ptr[0] == '#' || line.ptr[0] == ';') {
            continue;
        }

        if (line.ptr[0] == '[') {
            slice body;
            int general = parse_section(line, &body);

            (void)body;
            in_general = general > 0;
            continue;
        }

        if (!in_general) {
            continue;
        }

        equals = memchr(line.ptr, '=', line.len);
        if (equals == NULL) {
            continue;
        }

        {
            slice key;

            key.ptr = line.ptr;
            key.len = (size_t)(equals - line.ptr);
            trim(&key);

            if (key.len == 0 || known_general_key(key)) {
                continue;
            }
        }

        append(out, cap, used, "%.*s\n", (int)line.len, line.ptr);
    }
}

/* Unknown sections are preserved verbatim after the canonical [general] block.
 * Keys inside them do not affect the configuration. */
static void preserve_other_sections(const char *original, char *out, size_t cap,
                                    size_t *used)
{
    const char *p = original;
    int in_other = 0;

    while (*p != '\0') {
        const char *start = p;
        size_t len;
        slice line;
        const char *equals;

        while (*p != '\0' && *p != '\n') {
            p++;
        }
        len = (size_t)(p - start);
        if (*p == '\n') {
            p++;
        }

        line.ptr = start;
        line.len = len;
        trim(&line);
        if (line.len == 0 || line.ptr[0] == '#' || line.ptr[0] == ';') {
            continue;
        }

        if (line.ptr[0] == '[') {
            slice body;
            int general = parse_section(line, &body);

            (void)body;
            in_other = general == 0;
            if (in_other) {
                append(out, cap, used, "%.*s\n", (int)line.len, line.ptr);
            }
            continue;
        }

        if (!in_other) {
            continue;
        }

        equals = memchr(line.ptr, '=', line.len);
        if (equals == NULL) {
            continue;
        }

        append(out, cap, used, "%.*s\n", (int)line.len, line.ptr);
    }
}

size_t wn_config_format(const wn_config *cfg, const char *original, char *out,
                        size_t cap)
{
    const char *noise_name;
    int volume;
    int fade_ms;
    size_t used = 0;

    if (cfg == NULL) {
        return 0;
    }

    noise_name = wn_noise_type_name(cfg->noise);
    if (noise_name == NULL) {
        noise_name = "pink";
    }

    volume = cfg->volume;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    fade_ms = cfg->fade_ms;
    if (fade_ms < 0) {
        fade_ms = 0;
    } else if (fade_ms > 2000) {
        fade_ms = 2000;
    }

    append(out, cap, &used, "[general]\n");
    append(out, cap, &used, "noise=%s\n", noise_name);
    append(out, cap, &used, "volume=%d\n", volume);
    append(out, cap, &used, "mono=%s\n", cfg->mono != 0 ? "true" : "false");
    append(out, cap, &used, "fade_ms=%d\n", fade_ms);

    if (original != NULL) {
        preserve_general_keys(original, out, cap, &used);
        preserve_other_sections(original, out, cap, &used);
    }

    return used;
}
