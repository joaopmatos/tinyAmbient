/* INI parser and formatter tests. core/config.c is I/O-free, so every case here
 * is a string in and a string out. */

#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/noise.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void parse(const char *text, wn_config *out, char *warn, size_t cap)
{
    wn_config_parse(text, strlen(text), out, warn, cap);
}

static void test_defaults(void)
{
    wn_config cfg;

    wn_config_defaults(&cfg);
    CHECK(cfg.noise == WN_NOISE_PINK);
    CHECK(cfg.volume == 40);
    CHECK(cfg.mono == 0);
    CHECK(cfg.fade_ms == 250);
}

static void test_valid_file(void)
{
    static const char text[] = "[general]\n"
                               "noise=brown\n"
                               "volume=25\n"
                               "mono=true\n"
                               "fade_ms=100\n";
    wn_config cfg;
    char warn[160] = "x";

    parse(text, &cfg, warn, sizeof(warn));

    CHECK(cfg.noise == WN_NOISE_BROWN);
    CHECK(cfg.volume == 25);
    CHECK(cfg.mono == 1);
    CHECK(cfg.fade_ms == 100);
    CHECK(warn[0] == '\0');
}

static void test_missing_keys_use_defaults(void)
{
    static const char text[] = "[general]\nvolume=10\n";
    wn_config cfg;
    char warn[160];

    parse(text, &cfg, warn, sizeof(warn));

    CHECK(cfg.noise == WN_NOISE_PINK);
    CHECK(cfg.volume == 10);
    CHECK(cfg.mono == 0);
    CHECK(cfg.fade_ms == 250);
    CHECK(warn[0] == '\0');
}

static void test_malformed_values_fall_back(void)
{
    static const char bad_noise[] = "[general]\nnoise=gray\n";
    static const char bad_volume[] = "[general]\nvolume=101\n";
    static const char bad_mono[] = "[general]\nmono=maybe\n";
    static const char bad_fade[] = "[general]\nfade_ms=abc\n";
    wn_config cfg;
    char warn[160];

    parse(bad_noise, &cfg, warn, sizeof(warn));
    CHECK(cfg.noise == WN_NOISE_PINK);
    CHECK(warn[0] != '\0');

    parse(bad_volume, &cfg, warn, sizeof(warn));
    CHECK(cfg.volume == 40);
    CHECK(warn[0] != '\0');

    parse(bad_mono, &cfg, warn, sizeof(warn));
    CHECK(cfg.mono == 0);
    CHECK(warn[0] != '\0');

    parse(bad_fade, &cfg, warn, sizeof(warn));
    CHECK(cfg.fade_ms == 250);
    CHECK(warn[0] != '\0');
}

static void test_fade_is_clamped(void)
{
    static const char high[] = "[general]\nfade_ms=9000\n";
    static const char low[] = "[general]\nfade_ms=-5\n";
    wn_config cfg;
    char warn[160];

    parse(high, &cfg, warn, sizeof(warn));
    CHECK(cfg.fade_ms == 2000);
    CHECK(warn[0] != '\0');

    parse(low, &cfg, warn, sizeof(warn));
    CHECK(cfg.fade_ms == 0);
    CHECK(warn[0] != '\0');
}

static void test_syntax_tolerances(void)
{
    static const char text[] = "  # a comment\n"
                               "; another\n"
                               "\n"
                               " [General] \n"
                               " NOISE = Pink \n"
                               " volume = 7 \n"
                               " mono = FALSE \n";
    wn_config cfg;
    char warn[160];

    parse(text, &cfg, warn, sizeof(warn));

    CHECK(cfg.noise == WN_NOISE_PINK);
    CHECK(cfg.volume == 7);
    CHECK(cfg.mono == 0);
    CHECK(cfg.fade_ms == 250);
    CHECK(warn[0] == '\0');
}

static void test_unknown_keys_are_ignored(void)
{
    static const char text[] = "[general]\n"
                               "noise=white\n"
                               "future=42\n"
                               "[other]\n"
                               "noise=brown\n";
    wn_config cfg;
    char warn[160];

    parse(text, &cfg, warn, sizeof(warn));

    CHECK(cfg.noise == WN_NOISE_WHITE);
    CHECK(warn[0] == '\0');
}

static void test_format_defaults(void)
{
    static const char expected[] = "[general]\n"
                                   "noise=pink\n"
                                   "volume=40\n"
                                   "mono=false\n"
                                   "fade_ms=250\n";
    wn_config cfg;
    char out[256];
    size_t written;

    wn_config_defaults(&cfg);
    written = wn_config_format(&cfg, NULL, out, sizeof(out));

    CHECK(written == strlen(expected));
    CHECK(strcmp(out, expected) == 0);
}

static void test_format_round_trip(void)
{
    wn_config cfg;
    wn_config parsed;
    char text[256];
    char warn[160];

    wn_config_defaults(&cfg);
    cfg.noise = WN_NOISE_BROWN;
    cfg.volume = 73;
    cfg.mono = 1;
    cfg.fade_ms = 900;

    (void)wn_config_format(&cfg, NULL, text, sizeof(text));
    parse(text, &parsed, warn, sizeof(warn));

    CHECK(parsed.noise == cfg.noise);
    CHECK(parsed.volume == cfg.volume);
    CHECK(parsed.mono == cfg.mono);
    CHECK(parsed.fade_ms == cfg.fade_ms);
    CHECK(warn[0] == '\0');
}

static void test_unknown_keys_survive_rewrite(void)
{
    static const char original[] = "[general]\n"
                                   "noise=white\n"
                                   "future=42\n"
                                   "[other]\n"
                                   "x=1\n"
                                   "noise=brown\n";
    static const char expected[] = "[general]\n"
                                   "noise=white\n"
                                   "volume=40\n"
                                   "mono=false\n"
                                   "fade_ms=250\n"
                                   "future=42\n"
                                   "[other]\n"
                                   "x=1\n"
                                   "noise=brown\n";
    wn_config cfg;
    char out[512];

    wn_config_defaults(&cfg);
    cfg.noise = WN_NOISE_WHITE;
    (void)wn_config_format(&cfg, original, out, sizeof(out));

    CHECK(strcmp(out, expected) == 0);
}

static void test_comments_survive_rewrite(void)
{
    static const char original[] = "# top comment\n"
                                   "[general]\n"
                                   "noise=white\n"
                                   "# keep me\n"
                                   "future=42\n"
                                   "; also keep me\n"
                                   "[other]\n"
                                   "; section comment\n"
                                   "x=1\n";
    wn_config cfg;
    char out[512];
    const char *general_extras;
    const char *other_section;

    wn_config_defaults(&cfg);
    cfg.noise = WN_NOISE_WHITE;
    (void)wn_config_format(&cfg, original, out, sizeof(out));

    /* Comments are user content: deleting them on a rewrite is data loss. */
    CHECK(strstr(out, "# top comment") != NULL);
    CHECK(strstr(out, "# keep me") != NULL);
    CHECK(strstr(out, "; also keep me") != NULL);
    CHECK(strstr(out, "; section comment") != NULL);
    CHECK(strstr(out, "future=42") != NULL);
    CHECK(strstr(out, "x=1") != NULL);

    /* Position: the preamble stays above [general], and general-section
     * extras stay above any other section so they cannot drift into it. */
    CHECK(strstr(out, "# top comment") < strstr(out, "[general]"));
    general_extras = strstr(out, "future=42");
    other_section = strstr(out, "[other]");
    CHECK(general_extras != NULL && other_section != NULL &&
          general_extras < other_section);
}

static void test_format_reports_truncation(void)
{
    wn_config cfg;
    char small[8];
    size_t needed;
    size_t written;

    wn_config_defaults(&cfg);

    needed = wn_config_format(&cfg, NULL, NULL, 0);
    written = wn_config_format(&cfg, NULL, small, sizeof(small));

    CHECK(needed > 0);
    CHECK(written == needed);
    CHECK(needed >= sizeof(small));
    CHECK(strncmp(small, "[genera", 7) == 0);
}

int main(void)
{
    test_defaults();
    test_valid_file();
    test_missing_keys_use_defaults();
    test_malformed_values_fall_back();
    test_fade_is_clamped();
    test_syntax_tolerances();
    test_unknown_keys_are_ignored();
    test_format_defaults();
    test_format_round_trip();
    test_unknown_keys_survive_rewrite();
    test_comments_survive_rewrite();
    test_format_reports_truncation();

    if (failures != 0) {
        fprintf(stderr, "test_config: %d check(s) failed\n", failures);
        return 1;
    }

    printf("test_config: ok\n");
    return 0;
}
