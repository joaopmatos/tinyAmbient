/* Parser tests. The parser is pure C and touches no I/O, so every rule in PRD
 * section 7 can be checked here. */

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

/* The line parser modifies its buffer, so the test keeps a copy. The returned
 * svalue may point into that copy and is only used while it is alive. */
static int parse_line(const char *text, wn_cmd *out, char *err, size_t err_cap)
{
    char buffer[WN_LINE_MAX];
    size_t length = strlen(text);

    if (length >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, text, length + 1);

    return wn_cmd_parse_line(buffer, out, WN_CMD_MAX, err, err_cap);
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
    static const char *const bad_word[] = {"t", "frobnicate", NULL};
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

static void test_bare_words(void)
{
    static const char *const noise[] = {"t", "noise", "pink", NULL};
    static const char *const volume[] = {"t", "volume", "25", NULL};
    static const char *const play[] = {"t", "play", NULL};
    static const char *const status[] = {"t", "status", NULL};
    static const char *const mono[] = {"t", "mono", NULL};
    static const char *const stopped[] = {"t", "stop-after", "off", NULL};
    static const char *const sequence[] = {"t",     "play",  "noise",
                                           "brown", "pause", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(noise, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_NOISE);
    CHECK(cmds[0].ivalue == 1);

    CHECK(parse(volume, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_VOLUME);
    CHECK(cmds[0].ivalue == 25);

    CHECK(parse(play, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_PLAY);

    CHECK(parse(status, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_STATUS);

    CHECK(parse(mono, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_MONO);

    CHECK(parse(stopped, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_STOP_AFTER);
    CHECK(cmds[0].ivalue == WN_STOP_AFTER_OFF);

    CHECK(parse(sequence, cmds, err, sizeof(err)) == 3);
    CHECK(cmds[0].kind == WN_CMD_PLAY);
    CHECK(cmds[1].kind == WN_CMD_NOISE);
    CHECK(cmds[2].kind == WN_CMD_PAUSE);
}

static void test_playback_and_modes(void)
{
    static const char *const toggle[] = {"t", "--toggle", NULL};
    static const char *const stereo[] = {"t", "--stereo", NULL};
    static const char *const portable[] = {"t", "--portable", NULL};
    static const char *const foreground[] = {"t", "--foreground", NULL};
    static const char *const quit[] = {"t", "--quit", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(toggle, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_TOGGLE);

    CHECK(parse(stereo, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_STEREO);

    CHECK(parse(portable, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_PORTABLE);

    CHECK(parse(foreground, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_FOREGROUND);

    CHECK(parse(quit, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_QUIT);
}

static void test_volume_bounds(void)
{
    static const char *const ok_zero[] = {"t", "--volume", "0", NULL};
    static const char *const ok_max[] = {"t", "--volume", "100", NULL};
    static const char *const bad_high[] = {"t", "--volume", "101", NULL};
    static const char *const bad_negative[] = {"t", "--volume", "-1", NULL};
    static const char *const bad_text[] = {"t", "--volume", "abc", NULL};
    static const char *const missing[] = {"t", "--volume", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(ok_zero, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].ivalue == 0);
    CHECK(parse(ok_max, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].ivalue == 100);
    CHECK(parse(bad_high, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_negative, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_text, cmds, err, sizeof(err)) == -1);
    CHECK(parse(missing, cmds, err, sizeof(err)) == -1);
}

static void test_stop_after_bounds(void)
{
    static const char *const ok_min[] = {"t", "stop-after", "1", NULL};
    static const char *const ok_max[] = {"t", "stop-after", "10080", NULL};
    static const char *const off[] = {"t", "stop-after", "off", NULL};
    static const char *const off_upper[] = {"t", "--stop-after", "OFF", NULL};
    static const char *const bad_zero[] = {"t", "stop-after", "0", NULL};
    static const char *const bad_high[] = {"t", "stop-after", "10081", NULL};
    static const char *const bad_text[] = {"t", "stop-after", "soon", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(ok_min, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].ivalue == 1);
    CHECK(parse(ok_max, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].ivalue == 10080);
    CHECK(parse(off, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].ivalue == WN_STOP_AFTER_OFF);
    CHECK(parse(off_upper, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].ivalue == WN_STOP_AFTER_OFF);
    CHECK(parse(bad_zero, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_high, cmds, err, sizeof(err)) == -1);
    CHECK(parse(bad_text, cmds, err, sizeof(err)) == -1);
}

static void test_combinations(void)
{
    static const char *const render_play[] = {"t", "--render", "a", "--play",
                                              NULL};
    static const char *const render_volume[] = {"t",        "--render", "a",
                                                "--volume", "10",       NULL};
    static const char *const render_status[] = {"t", "--render", "a",
                                                "--status", NULL};
    static const char *const render_mono[] = {"t", "--render", "a", "--mono",
                                              NULL};
    static const char *const render_full[] = {
        "t",      "--render", "a",        "--noise", "brown",  "--seconds", "2",
        "--seed", "7",        "--volume", "0",       "--mono", NULL};
    static const char *const seconds_no_render[] = {"t", "--seconds", "5",
                                                    NULL};
    static const char *const seed_no_render[] = {"t", "--seed", "5", NULL};
    static const char *const noise_no_render[] = {"t", "--noise", "brown",
                                                  NULL};
    static const char *const status_noise[] = {"t", "--status", "--noise",
                                               "pink", NULL};
    static const char *const status_play[] = {"t", "--status", "play", NULL};
    static const char *const status_portable[] = {"t", "--status", "--portable",
                                                  NULL};
    static const char *const play_pause[] = {"t", "play", "pause", NULL};
    static const char *const mono_stereo[] = {"t", "--mono", "--stereo", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(render_play, cmds, err, sizeof(err)) == -1);
    CHECK(parse(render_status, cmds, err, sizeof(err)) == -1);

    /* Volume and mono/stereo are offline parameter selectors when render is
     * present: FR-03 and FR-13 need a way to be checked without a device. */
    CHECK(parse(render_volume, cmds, err, sizeof(err)) == 2);
    CHECK(parse(render_mono, cmds, err, sizeof(err)) == 2);
    CHECK(parse(render_full, cmds, err, sizeof(err)) == 6);
    CHECK(parse(seconds_no_render, cmds, err, sizeof(err)) == -1);
    CHECK(parse(seed_no_render, cmds, err, sizeof(err)) == -1);
    CHECK(parse(noise_no_render, cmds, err, sizeof(err)) == 1);

    CHECK(parse(status_noise, cmds, err, sizeof(err)) == -1);
    CHECK(parse(status_play, cmds, err, sizeof(err)) == -1);
    CHECK(parse(status_portable, cmds, err, sizeof(err)) == 2);
    CHECK(cmds[0].kind == WN_CMD_STATUS);
    CHECK(cmds[1].kind == WN_CMD_PORTABLE);

    /* PRD section 7 rule 2: multiple content commands apply in written order. */
    CHECK(parse(play_pause, cmds, err, sizeof(err)) == 2);
    CHECK(parse(mono_stereo, cmds, err, sizeof(err)) == 2);
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
    static const char *const two_volume[] = {"t",        "--volume", "10",
                                             "--volume", "20",       NULL};
    static const char *const two_stop[] = {"t",          "stop-after", "10",
                                           "stop-after", "20",         NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    /* Silently keeping the last value would render the wrong file or the wrong
     * length without telling anyone. */
    CHECK(parse(two_render, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_seconds, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_seed, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_noise, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_volume, cmds, err, sizeof(err)) == -1);
    CHECK(parse(two_stop, cmds, err, sizeof(err)) == -1);
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
    static const char *const help_with_render[] = {"t", "--render", "a",
                                                   "--help", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(version, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_VERSION);

    CHECK(parse(help, cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_HELP);

    /* Rule 6 is applied by the CLI; the parser must not reject the combination
     * before the caller gets a chance to honour it. */
    CHECK(parse(help_with_render, cmds, err, sizeof(err)) == 2);
}

static void test_no_arguments(void)
{
    static const char *const argv[] = {"tinyambient", NULL};
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse(argv, cmds, err, sizeof(err)) == 0);
}

static void test_line_parser(void)
{
    wn_cmd cmds[WN_CMD_MAX];
    char err[160] = "";

    CHECK(parse_line("noise pink", cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_NOISE);
    CHECK(cmds[0].ivalue == 1);

    CHECK(parse_line("  volume   25  ", cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_VOLUME);
    CHECK(cmds[0].ivalue == 25);

    CHECK(parse_line("toggle", cmds, err, sizeof(err)) == 1);
    CHECK(cmds[0].kind == WN_CMD_TOGGLE);

    CHECK(parse_line("noise", cmds, err, sizeof(err)) == -1);
    CHECK(parse_line("bogus", cmds, err, sizeof(err)) == -1);
    CHECK(parse_line("", cmds, err, sizeof(err)) == -1);
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
    test_bare_words();
    test_playback_and_modes();
    test_volume_bounds();
    test_stop_after_bounds();
    test_combinations();
    test_duplicate_options();
    test_terminator();
    test_version_and_help();
    test_no_arguments();
    test_line_parser();

    if (failures != 0) {
        fprintf(stderr, "test_command: %d check(s) failed\n", failures);
        return 1;
    }

    printf("test_command: ok\n");
    return 0;
}
