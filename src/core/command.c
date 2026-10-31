#include "core/command.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/noise.h"

/* One command line is at most WN_LINE_MAX bytes; the worst case is every
 * command carrying a value, so two tokens per command is enough. */
#define WN_TOKEN_MAX (WN_CMD_MAX * 2)

static int arg_is(const char *arg, const char *name)
{
    return strcmp(arg, name) == 0;
}

static void set_error(char *err, size_t cap, const char *fmt, ...)
{
    va_list args;

    if (err == NULL || cap == 0) {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(err, cap, fmt, args);
    va_end(args);
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

/* Parses a decimal integer in [lo, hi]. Rejects trailing garbage, empty
 * strings, and signs the range does not allow. */
static int parse_int(const char *text, long lo, long hi, int *out)
{
    char *end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }

    value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return -1;
    }
    if (value < lo || value > hi) {
        return -1;
    }

    *out = (int)value;
    return 0;
}

static int emit(wn_cmd *out, int cap, int *count, wn_cmd_kind kind, int ivalue,
                const char *svalue, char *err, size_t err_cap)
{
    if (*count >= cap) {
        set_error(err, err_cap, "too many arguments");
        return -1;
    }

    out[*count].kind = kind;
    out[*count].ivalue = ivalue;
    out[*count].svalue = svalue;
    (*count)++;
    return 0;
}

static int validate(const wn_cmd *cmds, int count, char *err, size_t err_cap)
{
    int has_render = 0;
    int has_status = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (cmds[i].kind == WN_CMD_RENDER) {
            has_render = 1;
        } else if (cmds[i].kind == WN_CMD_STATUS) {
            has_status = 1;
        }
    }

    if (has_render) {
        /* PRD section 7 rule 3: render is offline-only. Help and version are
         * allowed through so the caller can still honour rule 6. */
        for (i = 0; i < count; i++) {
            wn_cmd_kind kind = cmds[i].kind;

            if (kind != WN_CMD_RENDER && kind != WN_CMD_SECONDS &&
                kind != WN_CMD_SEED && kind != WN_CMD_NOISE &&
                kind != WN_CMD_VOLUME && kind != WN_CMD_MONO &&
                kind != WN_CMD_STEREO && kind != WN_CMD_HELP &&
                kind != WN_CMD_VERSION) {
                set_error(err, err_cap,
                          "--render cannot be combined with other commands");
                return -1;
            }
        }
    } else {
        for (i = 0; i < count; i++) {
            if (cmds[i].kind == WN_CMD_SECONDS || cmds[i].kind == WN_CMD_SEED) {
                set_error(err, err_cap,
                          "--seconds and --seed require --render");
                return -1;
            }
        }
    }

    if (has_status) {
        /* PRD section 7 rule 4: status reports, it does not change anything.
         * --portable only changes which file is read, so it is allowed;
         * --foreground would start a session, so it is not. */
        for (i = 0; i < count; i++) {
            wn_cmd_kind kind = cmds[i].kind;

            if (kind == WN_CMD_PLAY || kind == WN_CMD_PAUSE ||
                kind == WN_CMD_TOGGLE || kind == WN_CMD_NOISE ||
                kind == WN_CMD_VOLUME || kind == WN_CMD_STOP_AFTER ||
                kind == WN_CMD_MONO || kind == WN_CMD_STEREO ||
                kind == WN_CMD_FOREGROUND || kind == WN_CMD_QUIT ||
                kind == WN_CMD_RENDER) {
                set_error(err, err_cap,
                          "--status cannot be combined with other commands");
                return -1;
            }
        }
    }

    return 0;
}

/* Parses `count` tokens. bare words and "--" flags are equivalent. Tokens
 * starting with a single '-' are rejected: v1 has no short options. */
static int parse_tokens(const char *const *tok, int count, wn_cmd *out, int cap,
                        char *err, size_t err_cap)
{
    int n = 0;
    int seen_render = 0;
    int seen_noise = 0;
    int seen_volume = 0;
    int seen_stop_after = 0;
    int seen_seconds = 0;
    int seen_seed = 0;
    int i = 0;

    if (out == NULL || cap <= 0) {
        set_error(err, err_cap, "internal: no command buffer");
        return -1;
    }

    while (i < count) {
        const char *arg = tok[i];
        const char *name = arg;
        const char *value = NULL;

        if (arg == NULL || arg[0] == '\0') {
            set_error(err, err_cap, "empty command");
            return -1;
        }

        if (arg[0] == '-') {
            if (arg[1] != '-') {
                set_error(err, err_cap, "unknown option '%s'", arg);
                return -1;
            }
            if (arg[2] == '\0') {
                /* Bare "--" ends option parsing. v1 has no positional
                 * arguments, so anything after it is an error rather than
                 * silently ignored. */
                if (i + 1 < count) {
                    set_error(err, err_cap, "unexpected argument '%s'",
                              tok[i + 1]);
                    return -1;
                }
                break;
            }
            name = arg + 2;
        }

        if (arg_is(name, "render")) {
            if (i + 1 >= count) {
                set_error(err, err_cap, "'%s' requires a path", arg);
                return -1;
            }
            if (seen_render) {
                set_error(err, err_cap, "'%s' given more than once", arg);
                return -1;
            }
            seen_render = 1;
            value = tok[i + 1];
            if (value[0] == '-') {
                /* Every other value option rejects a flag-looking value
                 * through its range check. Render is the only free-form one,
                 * so the check is spelled out; without it, `--render --help`
                 * silently writes a file named "--help". */
                set_error(err, err_cap, "a render path may not start with '-'");
                return -1;
            }
            if (emit(out, cap, &n, WN_CMD_RENDER, 0, value, err, err_cap) !=
                0) {
                return -1;
            }
            i += 2;
        } else if (arg_is(name, "noise")) {
            int type;

            if (i + 1 >= count) {
                set_error(err, err_cap, "'%s' requires a value", arg);
                return -1;
            }
            if (seen_noise) {
                set_error(err, err_cap, "'%s' given more than once", arg);
                return -1;
            }
            seen_noise = 1;
            value = tok[i + 1];
            type = wn_noise_type_parse(value);
            if (type == WN_NOISE_NONE) {
                set_error(err, err_cap,
                          "'%s' is not a noise type (white, pink, brown)",
                          value);
                return -1;
            }
            if (emit(out, cap, &n, WN_CMD_NOISE, type, wn_noise_type_name(type),
                     err, err_cap) != 0) {
                return -1;
            }
            i += 2;
        } else if (arg_is(name, "volume")) {
            int volume;

            if (i + 1 >= count) {
                set_error(err, err_cap, "'%s' requires a value", arg);
                return -1;
            }
            if (seen_volume) {
                set_error(err, err_cap, "'%s' given more than once", arg);
                return -1;
            }
            seen_volume = 1;
            value = tok[i + 1];
            if (parse_int(value, 0, 100, &volume) != 0) {
                set_error(err, err_cap, "'%s' is not a valid volume (0-100)",
                          value);
                return -1;
            }
            if (emit(out, cap, &n, WN_CMD_VOLUME, volume, NULL, err, err_cap) !=
                0) {
                return -1;
            }
            i += 2;
        } else if (arg_is(name, "stop-after")) {
            int minutes = WN_STOP_AFTER_OFF;

            if (i + 1 >= count) {
                set_error(err, err_cap, "'%s' requires a value", arg);
                return -1;
            }
            if (seen_stop_after) {
                set_error(err, err_cap, "'%s' given more than once", arg);
                return -1;
            }
            seen_stop_after = 1;
            value = tok[i + 1];
            if (!ascii_equal_ci(value, "off") &&
                parse_int(value, WN_STOP_AFTER_MIN, WN_STOP_AFTER_MAX,
                          &minutes) != 0) {
                set_error(err, err_cap,
                          "'%s' is not a valid stop-after value (1-%d or off)",
                          value, WN_STOP_AFTER_MAX);
                return -1;
            }
            if (emit(out, cap, &n, WN_CMD_STOP_AFTER, minutes, NULL, err,
                     err_cap) != 0) {
                return -1;
            }
            i += 2;
        } else if (arg_is(name, "seconds")) {
            int seconds;

            if (i + 1 >= count) {
                set_error(err, err_cap, "'%s' requires a value", arg);
                return -1;
            }
            if (seen_seconds) {
                set_error(err, err_cap, "'%s' given more than once", arg);
                return -1;
            }
            seen_seconds = 1;
            value = tok[i + 1];
            if (parse_int(value, WN_RENDER_SECONDS_MIN, WN_RENDER_SECONDS_MAX,
                          &seconds) != 0) {
                set_error(err, err_cap,
                          "'%s' is not a valid seconds value (%d-%d)", value,
                          WN_RENDER_SECONDS_MIN, WN_RENDER_SECONDS_MAX);
                return -1;
            }
            if (emit(out, cap, &n, WN_CMD_SECONDS, seconds, NULL, err,
                     err_cap) != 0) {
                return -1;
            }
            i += 2;
        } else if (arg_is(name, "seed")) {
            int seed;

            if (i + 1 >= count) {
                set_error(err, err_cap, "'%s' requires a value", arg);
                return -1;
            }
            if (seen_seed) {
                set_error(err, err_cap, "'%s' given more than once", arg);
                return -1;
            }
            seen_seed = 1;
            value = tok[i + 1];
            if (parse_int(value, 0, 2147483647L, &seed) != 0) {
                set_error(err, err_cap,
                          "'%s' is not a valid seed value (0-2147483647)",
                          value);
                return -1;
            }
            if (emit(out, cap, &n, WN_CMD_SEED, seed, NULL, err, err_cap) !=
                0) {
                return -1;
            }
            i += 2;
        } else if (arg_is(name, "play")) {
            if (emit(out, cap, &n, WN_CMD_PLAY, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "pause")) {
            if (emit(out, cap, &n, WN_CMD_PAUSE, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "toggle")) {
            if (emit(out, cap, &n, WN_CMD_TOGGLE, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "mono")) {
            if (emit(out, cap, &n, WN_CMD_MONO, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "stereo")) {
            if (emit(out, cap, &n, WN_CMD_STEREO, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "status")) {
            if (emit(out, cap, &n, WN_CMD_STATUS, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "portable")) {
            if (emit(out, cap, &n, WN_CMD_PORTABLE, 0, NULL, err, err_cap) !=
                0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "foreground")) {
            if (emit(out, cap, &n, WN_CMD_FOREGROUND, 0, NULL, err, err_cap) !=
                0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "version")) {
            if (emit(out, cap, &n, WN_CMD_VERSION, 0, NULL, err, err_cap) !=
                0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "help")) {
            if (emit(out, cap, &n, WN_CMD_HELP, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else if (arg_is(name, "quit")) {
            if (emit(out, cap, &n, WN_CMD_QUIT, 0, NULL, err, err_cap) != 0) {
                return -1;
            }
            i++;
        } else {
            if (arg[0] == '-') {
                set_error(err, err_cap, "unknown option '%s'", arg);
            } else {
                set_error(err, err_cap, "unknown command '%s'", arg);
            }
            return -1;
        }
    }

    if (validate(out, n, err, err_cap) != 0) {
        return -1;
    }

    return n;
}

int wn_cmd_parse_argv(int argc, const char *const *argv, wn_cmd *out, int cap,
                      char *err, size_t err_cap)
{
    if (argv == NULL || argc < 1) {
        return 0;
    }

    /* argv[0] is the program name. */
    return parse_tokens(argv + 1, argc - 1, out, cap, err, err_cap);
}

int wn_cmd_takes_value(const char *name)
{
    return arg_is(name, "noise") || arg_is(name, "volume") ||
           arg_is(name, "stop-after") || arg_is(name, "render") ||
           arg_is(name, "seconds") || arg_is(name, "seed");
}

int wn_cmd_parse_line(char *line, wn_cmd *out, int cap, char *err,
                      size_t err_cap)
{
    const char *tok[WN_TOKEN_MAX];
    int count = 0;
    char *p;

    if (line == NULL) {
        set_error(err, err_cap, "empty command");
        return -1;
    }
    if (strlen(line) >= WN_LINE_MAX) {
        set_error(err, err_cap, "command line is too long");
        return -1;
    }

    p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (count >= WN_TOKEN_MAX) {
            set_error(err, err_cap, "too many arguments");
            return -1;
        }

        tok[count++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n') {
            p++;
        }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }

    if (count == 0) {
        set_error(err, err_cap, "empty command");
        return -1;
    }

    return parse_tokens(tok, count, out, cap, err, err_cap);
}

/* --- IPC line formatting ---------------------------------------------------
 *
 * The inverse of wn_cmd_parse_line, kept beside it so the CLI and the daemon
 * can never disagree about command text (ARCHITECTURE 2). `used` accumulates
 * the full length while only the bytes that fit are written, matching the
 * snprintf contract used by the config and status formatters. */

static size_t append_text(char *out, size_t cap, size_t used, const char *text)
{
    size_t length = strlen(text);

    if (out != NULL && used < cap) {
        size_t room = cap - used;
        size_t copy = length < room - 1 ? length : room - 1;

        memcpy(out + used, text, copy);
        out[used + copy] = '\0';
    }
    return used + length;
}

static size_t append_int(char *out, size_t cap, size_t used, const char *prefix,
                         int value)
{
    char number[24];

    (void)snprintf(number, sizeof(number), "%s%d", prefix, value);
    return append_text(out, cap, used, number);
}

static size_t format_one(const wn_cmd *cmd, char *out, size_t cap)
{
    size_t used = 0;

    switch (cmd->kind) {
        case WN_CMD_NOISE: {
            const char *name = cmd->svalue != NULL
                                   ? cmd->svalue
                                   : wn_noise_type_name(cmd->ivalue);

            used = append_text(out, cap, used, "noise ");
            used = append_text(out, cap, used, name != NULL ? name : "pink");
            break;
        }
        case WN_CMD_VOLUME:
            used = append_int(out, cap, used, "volume ", cmd->ivalue);
            break;
        case WN_CMD_STOP_AFTER:
            if (cmd->ivalue == WN_STOP_AFTER_OFF) {
                used = append_text(out, cap, used, "stop-after off");
            } else {
                used = append_int(out, cap, used, "stop-after ", cmd->ivalue);
            }
            break;
        case WN_CMD_PLAY:
            used = append_text(out, cap, used, "play");
            break;
        case WN_CMD_PAUSE:
            used = append_text(out, cap, used, "pause");
            break;
        case WN_CMD_TOGGLE:
            used = append_text(out, cap, used, "toggle");
            break;
        case WN_CMD_MONO:
            used = append_text(out, cap, used, "mono");
            break;
        case WN_CMD_STEREO:
            used = append_text(out, cap, used, "stereo");
            break;
        case WN_CMD_STATUS:
            used = append_text(out, cap, used, "status");
            break;
        case WN_CMD_QUIT:
            used = append_text(out, cap, used, "quit");
            break;
        default:
            /* Local-only and render commands are never forwarded. */
            break;
    }

    return used;
}

size_t wn_cmd_format_ipc_line(const wn_cmd *cmds, int count, char *out,
                              size_t cap)
{
    size_t used = 0;
    int i;

    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (cmds == NULL) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        const wn_cmd *cmd = &cmds[i];

        if (cmd->kind == WN_CMD_NONE || cmd->kind == WN_CMD_PORTABLE ||
            cmd->kind == WN_CMD_FOREGROUND || cmd->kind == WN_CMD_VERSION ||
            cmd->kind == WN_CMD_HELP || cmd->kind == WN_CMD_RENDER ||
            cmd->kind == WN_CMD_SECONDS || cmd->kind == WN_CMD_SEED) {
            continue;
        }
        if (used > 0) {
            used = append_text(out, cap, used, " ");
        }
        used += format_one(cmd, out != NULL && used < cap ? out + used : NULL,
                           out != NULL && used < cap ? cap - used : 0);
    }

    return used;
}
