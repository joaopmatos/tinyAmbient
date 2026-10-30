#include "core/command.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/noise.h"

static int arg_is(const char *arg, const char *flag)
{
    return strcmp(arg, flag) == 0;
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

int wn_cmd_parse_argv(int argc, const char *const *argv, wn_cmd *out, int cap,
                      char *err, size_t err_cap)
{
    int count = 0;
    int seen_render = 0;
    int seen_noise = 0;
    int seen_seconds = 0;
    int seen_seed = 0;
    int i;

    if (out == NULL || cap <= 0) {
        set_error(err, err_cap, "internal: no command buffer");
        return -1;
    }

    /* argv[0] is the program name. */
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (arg == NULL) {
            continue;
        }

        if (arg[0] != '-') {
            set_error(err, err_cap, "unknown command '%s'", arg);
            return -1;
        }

        if (arg[1] != '-') {
            set_error(err, err_cap, "unknown option '%s'", arg);
            return -1;
        }

        if (arg[2] == '\0') {
            /* Bare "--" ends option parsing. v1 has no positional arguments, so
             * anything after it is an error rather than silently ignored. */
            if (i + 1 < argc) {
                set_error(err, err_cap, "unexpected argument '%s'",
                          argv[i + 1]);
                return -1;
            }
            break;
        }

        if (arg_is(arg, "--render")) {
            if (i + 1 >= argc) {
                set_error(err, err_cap, "option '--render' requires a path");
                return -1;
            }
            if (seen_render) {
                set_error(err, err_cap,
                          "option '--render' given more than once");
                return -1;
            }
            seen_render = 1;
            i++;
            if (emit(out, cap, &count, WN_CMD_RENDER, 0, argv[i], err,
                     err_cap) != 0) {
                return -1;
            }
        } else if (arg_is(arg, "--noise")) {
            int type;

            if (i + 1 >= argc) {
                set_error(err, err_cap, "option '--noise' requires a value");
                return -1;
            }
            if (seen_noise) {
                set_error(err, err_cap,
                          "option '--noise' given more than once");
                return -1;
            }
            i++;
            type = wn_noise_type_parse(argv[i]);
            if (type == WN_NOISE_NONE) {
                set_error(err, err_cap,
                          "'%s' is not a noise type (white, pink, brown)",
                          argv[i]);
                return -1;
            }
            seen_noise = 1;
            if (emit(out, cap, &count, WN_CMD_NOISE, type,
                     wn_noise_type_name(type), err, err_cap) != 0) {
                return -1;
            }
        } else if (arg_is(arg, "--seconds")) {
            int value;
            if (i + 1 >= argc) {
                set_error(err, err_cap, "option '--seconds' requires a value");
                return -1;
            }
            if (seen_seconds) {
                set_error(err, err_cap,
                          "option '--seconds' given more than once");
                return -1;
            }
            i++;
            if (parse_int(argv[i], WN_RENDER_SECONDS_MIN, WN_RENDER_SECONDS_MAX,
                          &value) != 0) {
                set_error(
                    err, err_cap, "'%s' is not a valid --seconds value (%d-%d)",
                    argv[i], WN_RENDER_SECONDS_MIN, WN_RENDER_SECONDS_MAX);
                return -1;
            }
            seen_seconds = 1;
            if (emit(out, cap, &count, WN_CMD_SECONDS, value, NULL, err,
                     err_cap) != 0) {
                return -1;
            }
        } else if (arg_is(arg, "--seed")) {
            int value;
            if (i + 1 >= argc) {
                set_error(err, err_cap, "option '--seed' requires a value");
                return -1;
            }
            if (seen_seed) {
                set_error(err, err_cap, "option '--seed' given more than once");
                return -1;
            }
            i++;
            if (parse_int(argv[i], 0, 2147483647L, &value) != 0) {
                set_error(err, err_cap,
                          "'%s' is not a valid --seed value (0-2147483647)",
                          argv[i]);
                return -1;
            }
            if (emit(out, cap, &count, WN_CMD_SEED, value, NULL, err,
                     err_cap) != 0) {
                return -1;
            }
            seen_seed = 1;
        } else if (arg_is(arg, "--version")) {
            if (emit(out, cap, &count, WN_CMD_VERSION, 0, NULL, err, err_cap) !=
                0) {
                return -1;
            }
        } else if (arg_is(arg, "--help")) {
            if (emit(out, cap, &count, WN_CMD_HELP, 0, NULL, err, err_cap) !=
                0) {
                return -1;
            }
        } else {
            set_error(err, err_cap, "unknown option '%s'", arg);
            return -1;
        }
    }

    return count;
}
