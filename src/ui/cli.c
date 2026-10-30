#include "ui/cli.h"

#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "core/config.h"
#include "core/engine.h"
#include "core/noise.h"

/* Provided by CMake from the project version, so there is one source of truth
 * (PRD FR-15). */
#ifndef TA_VERSION
#define TA_VERSION "0.0.0"
#endif

/* Matches the audio period planned in M4, so the render loop and the callback
 * have the same shape. */
#define WN_BLOCK_FRAMES 512

void wn_cli_error(const char *message)
{
    fprintf(stderr, "tinyambient: %s\n", message);
}

void wn_cli_usage(void)
{
    fputs("usage: tinyambient [command] [options]\n"
          "\n"
          "render:\n"
          "  --render <file>      offline 48 kHz stereo float32\n"
          "  --seconds <1-3600>   render length, default "
          "10\n"
          "  --seed <n>           generator seed, default 1\n"
          "\n"
          "other:\n"
          "  --version            print version and exit\n"
          "  --help               print this text and exit\n",
          stdout);
}

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

static int has_wav_extension(const char *path)
{
    size_t len;

    if (path == NULL) {
        return 0;
    }
    len = strlen(path);
    if (len < 4) {
        return 0;
    }
    return ascii_equal_ci(path + len - 4, ".wav");
}

static int render_file(const char *path, int seconds, uint64_t seed)
{
    wn_config cfg;
    wn_engine *engine;
    FILE *out;
    float block[WN_BLOCK_FRAMES * WN_CHANNELS];
    unsigned total;
    unsigned done = 0;

    out = fopen(path, "wb");
    if (out == NULL) {
        wn_cli_error("cannot open the render output for writing");
        return WN_EXIT_RUNTIME;
    }

    /* Render uses full scale. There is no --volume at M1, and the calibrated
     * type gain already targets -18 dBFS. */
    cfg.noise = WN_NOISE_WHITE;
    cfg.volume = 100;
    cfg.mono = 0;
    cfg.fade_ms = 0;

    engine = wn_engine_create(&cfg);
    if (engine == NULL) {
        wn_cli_error("out of memory");
        (void)fclose(out);
        return WN_EXIT_RUNTIME;
    }
    wn_engine_set_seed(engine, seed);

    total = (unsigned)WN_SAMPLE_RATE * (unsigned)seconds;

    while (done < total) {
        unsigned frames = total - done;

        if (frames > WN_BLOCK_FRAMES) {
            frames = WN_BLOCK_FRAMES;
        }

        wn_engine_render(engine, block, frames);

        if (fwrite(block, sizeof(float) * WN_CHANNELS * frames, 1, out) != 1) {
            wn_cli_error("cannot write the render output");
            wn_engine_destroy(engine);
            (void)fclose(out);
            return WN_EXIT_RUNTIME;
        }
        done += frames;
    }

    wn_engine_destroy(engine);

    if (fclose(out) != 0) {
        wn_cli_error("cannot close the render output");
        return WN_EXIT_RUNTIME;
    }

    return WN_EXIT_OK;
}

int wn_cli_main(int argc, const char *const *argv)
{
    wn_cmd cmds[WN_CMD_MAX];
    char err[160];
    const char *render_path = NULL;
    int seconds = WN_RENDER_SECONDS_DEFAULT;
    int seed = WN_RENDER_SEED_DEFAULT;
    int have_render = 0;
    int have_render_option = 0;
    int want_help = 0;
    int want_version = 0;
    int n;
    int i;

    n = wn_cmd_parse_argv(argc, argv, cmds, WN_CMD_MAX, err, sizeof(err));
    if (n < 0) {
        wn_cli_error(err);
        return WN_EXIT_USAGE;
    }

    for (i = 0; i < n; i++) {
        switch (cmds[i].kind) {
            case WN_CMD_RENDER:
                render_path = cmds[i].svalue;
                have_render = 1;
                break;
            case WN_CMD_SECONDS:
                seconds = cmds[i].ivalue;
                have_render_option = 1;
                break;
            case WN_CMD_SEED:
                seed = cmds[i].ivalue;
                have_render_option = 1;
                break;
            case WN_CMD_VERSION:
                want_version = 1;
                break;
            case WN_CMD_HELP:
                want_help = 1;
                break;
            default:
                break;
        }
    }

    /* PRD section 7 rule 6: help and version win over every other argument. */
    if (want_help) {
        wn_cli_usage();
        return WN_EXIT_OK;
    }
    if (want_version) {
        printf("tinyambient %s\n", TA_VERSION);
        return WN_EXIT_OK;
    }

    if (have_render) {
        /* PRD section 7 rule 3: render is offline-only. */
        for (i = 0; i < n; i++) {
            wn_cmd_kind kind = cmds[i].kind;

            if (kind != WN_CMD_RENDER && kind != WN_CMD_SECONDS &&
                kind != WN_CMD_SEED) {
                wn_cli_error("--render cannot be combined with other commands");
                return WN_EXIT_USAGE;
            }
        }

        if (has_wav_extension(render_path)) {
            wn_cli_error(
                "WAV output is not implemented yet; use any other extension "
                "for raw float32");
            return WN_EXIT_RUNTIME;
        }

        return render_file(render_path, seconds, (uint64_t)seed);
    }

    if (have_render_option) {
        wn_cli_error("--seconds and --seed require --render");
        return WN_EXIT_USAGE;
    }

    if (n > 0) {
        wn_cli_error("this command is not implemented yet");
        return WN_EXIT_RUNTIME;
    }

    /* PRD FR-09: no arguments starts the daemon. That lands with IPC in M7. */
    wn_cli_error("daemon mode is not implemented yet");
    return WN_EXIT_RUNTIME;
}
