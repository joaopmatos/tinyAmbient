/* Parser tests. The parser is pure C and touches no I/O, so every rule in PRD
 * section 7 that M1 supports can be checked here. */

#include <stdio.h>
#include <string.h>

#include "core/command.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* argv arrays are NULL-terminated so the length is derived, not miscounted. */
static int parse(const char *const *argv, wn_cmd *out, char *err,
                 size_t err_cap)
{
    int argc = 0;

    while (argv[argc] != NULL) {
        argc++;
    }
    return wn_cmd_parse_argv(argc, argv, out, WN_CMD_MAX, err, err_cap);
}

static void test_render_defaults(void)
{
    static const char *const argv[] = {"tinyambient", "--render", "out.raw",
                                       NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";
    int n = parse(argv, cmds, err, sizeof(err));

    CHECK(n == 1);
    if (n == 1) {
        CHECK(cmds[0].kind == WN_CMD_RENDER);
        CHECK(strcmp(cmds[0].svalue, "out.raw") == 0);
    }
    CHECK(err[0] == '\0');
}

static void test_render_options_in_order(void)
{
    static const char *const argv[] = {"tinyambient", "--render", "out.raw",
                                       "--seconds",   "5",        "--seed",
                                       "42",          NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";
    int n = parse(argv, cmds, err, sizeof(err));

    CHECK(n == 3);
    if (n == 3) {
        CHECK(cmds[0].kind == WN_CMD_RENDER);
        CHECK(cmds[1].kind == WN_CMD_SECONDS);
        CHECK(cmds[1].ivalue == 5);
        CHECK(cmds[2].kind == WN_CMD_SEED);
        CHECK(cmds[2].ivalue == 42);
    }
}

static void test_render_requires_path(void)
{
    static const char *const argv[] = {"tinyambient", "--render", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(argv, cmds, err, sizeof(err)) == -1);
    CHECK(err[0] != '\0');
}

static void test_seconds_bounds(void)
{
    static const char *const ok_min[] = {"t",         "--render", "a",
                                         "--seconds", "1",        NULL};
    static const char *const ok_max[] = {"t",         "--render", "a",
                                         "--seconds", "3600",     NULL};
    static const char *const bad_zero[] = {"t",         "--render", "a",
                                           "--seconds", "0",        NULL};
    static const char *const bad_high[] = {"t",         "--render", "a",
                                           "--seconds", "3601",     NULL};
    static const char *const bad_text[] = {"t",         "--render", "a",
                                           "--seconds", "abc",      NULL};
    static const char *const bad_suffix[] = {"t",         "--render", "a",
                                             "--seconds", "5x",       NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(ok_min, cmds, err, sizeof(err)) == 2);
    CHECK(parse(ok_max, cmds, err, sizeof(err)) == 2);
    CHECK(parse(bad_zero, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_high, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_text, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_suffix, cmds, err, sizeof(err)) == -1);
}

static void test_seed_bounds(void)
{
    static const char *const ok_zero[] = {"t",      "--render", "a",
                                          "--seed", "0",        NULL};
    static const char *const bad_negative[] = {"t",      "--render", "a",
                                               "--seed", "-1",       NULL};
    static const char *const bad_text[] = {"t",      "--render", "a",
                                           "--seed", "x",        NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(ok_zero, cmds, err, sizeof(err)) == 2);
    if (cmds[1].kind == WN_CMD_SEED) {
        CHECK(cmds[1].ivalue == 0);
    }
    CHECK(parse(bad_negative, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_text, cmds, err, sizeof(err)) == -1);
}

static void test_unknown_arguments(void)
{
    static const char *const bad_flag[] = {"t", "--nope", NULL};
    static const char *const bad_short[] = {"t", "-v", NULL};
    static const char *const bad_word[] = {"t", "noise", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(bad_flag, cmds, err, sizeof(err)) == -1);
    CHECK(err[0] != '\0');
    CHECK(parse(bad_short, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_word, cmds, err, sizeof(err)) == -1);
}

static void test_noise_option(void)
{
    static const char *const ok_pink[] = {"t",       "--render", "a",
                                          "--noise", "pink",     NULL};
    static const char *const ok_upper[] = {"t",       "--render", "a",
                                           "--noise", "BROWN",    NULL};
    static const char *const bad_type[] = {"t",       "--render", "a",
                                           "--noise", "gray",     NULL};
    static const char *const missing[] = {"t", "--render", "a", "--noise",
                                          NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(ok_pink, cmds, err, sizeof(err)) == 2);
    CHECK(cmds[1].kind == WN_CMD_NOISE);
    CHECK(cmds[1].ivalue == 1);
    CHECK(strcmp(cmds[1].svalue, "pink") == 0);

    CHECK(parse(ok_upper, cmds, err, sizeof(err)) == 2);
    CHECK(cmds[1].ivalue == 2);

    CHECK(parse(bad_type, cmds, err, sizeof(err)) == -1);
    CHECK(err[0] != '\0');
    CHECK(parse(missing, cmds, err, sizeof(err)) == -1);
}

static void test_duplicate_options(void)
{
    static const char *const two_render[] = {"t",        "--render", "a",
                                             "--render", "b",        NULL};
    static const char *const two_seconds[] = {
        "t", "--render", "a", "--seconds", "1", "--seconds", "2", NULL};
    static const char *const two_seed[] = {"t", "--render", "a", "--seed",
                                           "1", "--seed",   "2", NULL};
    static const char *const two_noise[] = {
        "t", "--render", "a", "--noise", "pink", "--noise", "brown", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    /* Silently keeping the last value would render the wrong file or the wrong
     * length without telling anyone. */
    CHECK(parse(two_render, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_seconds, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_seed, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_noise, cmds, err, sizeof(err)) == -1);
}

static void test_terminator(void)
{
    static const char *const trailing[] = {"t", "--", "--bogus", NULL};
    static const char *const bare_end[] = {"t", "--", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(trailing, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bare_end, cmds, err, sizeof(err)) == 0);
}

static void test_version_and_help(void)
{
    static const char *const version[] = {"t", "--version", NULL};
    static const char *const help[] = {"t", "--help", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(version, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_VERSION);

    CHECK(parse(help, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_HELP);
}

static void test_no_arguments(void)
{
    static const char *const argv[] = {"tinyambient", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(argv, cmds, err, sizeof(err)) == 0);
}

int main(void)
{
    test_render_defaults();
    test_render_options_in_order();
    test_render_requires_path();
    test_seconds_bounds();
    test_seed_bounds();
    test_unknown_arguments();
    test_noise_option();
    test_duplicate_options();
    test_terminator();
    test_version_and_help();
    test_no_arguments();

    if (failures != 0) {
        fprintf(stderr, "test_command: %d check(s) failed\n", failures);
        return 1;
    }

    printf("test_command: ok\n");
    return 0;
}
