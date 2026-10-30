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
          "  --render <file>      offline 48 kHz stereo float32; a .wav\n"
          "                       path adds a WAV header\n"
          "  --noise <type>       white, pink, or brown; default white\n"
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

/* --- WAV container --------------------------------------------------------
 *
 * 44-byte canonical header: RIFF/WAVE, one `fmt ` chunk and one `data` chunk.
 * WAVE_FORMAT_IEEE_FLOAT (3) with 32-bit samples, so the payload is byte-for-byte
 * the same as a raw render and no conversion happens.
 *
 * The data size is known before writing starts (seconds * 384000), so the header
 * needs no seek-back. At the 3600 s limit the data chunk is 1,382,400,000 bytes,
 * which still fits the 32-bit RIFF size field. */

#define WN_WAV_HEADER_BYTES 44
#define WN_WAV_FORMAT_IEEE_FLOAT 3
#define WN_WAV_BITS_PER_SAMPLE 32

static void put_u16le(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void put_u32le(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

static void build_wav_header(unsigned char *header, uint32_t data_bytes)
{
    const uint16_t channels = WN_CHANNELS;
    const uint32_t rate = WN_SAMPLE_RATE;
    const uint16_t block_align =
        (uint16_t)(channels * (WN_WAV_BITS_PER_SAMPLE / 8));

    memcpy(header + 0, "RIFF", 4);
    put_u32le(header + 4, 36u + data_bytes); /* 44-byte header minus 8 */
    memcpy(header + 8, "WAVE", 4);

    memcpy(header + 12, "fmt ", 4);
    put_u32le(header + 16, 16u); /* PCM fmt chunk size */
    put_u16le(header + 20, WN_WAV_FORMAT_IEEE_FLOAT);
    put_u16le(header + 22, channels);
    put_u32le(header + 24, rate);
    put_u32le(header + 28, rate * (uint32_t)block_align); /* byte rate */
    put_u16le(header + 32, block_align);
    put_u16le(header + 34, WN_WAV_BITS_PER_SAMPLE);

    memcpy(header + 36, "data", 4);
    put_u32le(header + 40, data_bytes);
}

static int render_file(const char *path, int seconds, uint64_t seed,
                       int noise_type)
{
    wn_config cfg;
    wn_engine *engine;
    FILE *out;
    float block[WN_BLOCK_FRAMES * WN_CHANNELS];
    unsigned total;
    unsigned done = 0;
    int want_wav = has_wav_extension(path);

    total = (unsigned)WN_SAMPLE_RATE * (unsigned)seconds;

    out = fopen(path, "wb");
    if (out == NULL) {
        wn_cli_error("cannot open the render output for writing");
        return WN_EXIT_RUNTIME;
    }

    if (want_wav) {
        unsigned char header[WN_WAV_HEADER_BYTES];
        uint32_t data_bytes = (uint32_t)total * WN_CHANNELS * 4u;

        build_wav_header(header, data_bytes);
        if (fwrite(header, sizeof(header), 1, out) != 1) {
            wn_cli_error("cannot write the WAV header");
            (void)fclose(out);
            return WN_EXIT_RUNTIME;
        }
    }

    /* Render uses full scale. There is no --volume at M2, and the calibrated
     * type gain already targets -18 dBFS. */
    cfg.noise = noise_type;
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
    int noise_type = WN_NOISE_WHITE;
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
            case WN_CMD_NOISE:
                noise_type = cmds[i].ivalue;
                have_render_option = 1;
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
                kind != WN_CMD_SEED && kind != WN_CMD_NOISE) {
                wn_cli_error("--render cannot be combined with other commands");
                return WN_EXIT_USAGE;
            }
        }

        return render_file(render_path, seconds, (uint64_t)seed, noise_type);
    }

    if (have_render_option) {
        wn_cli_error("--noise, --seconds and --seed require --render");
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
