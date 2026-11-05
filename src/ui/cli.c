#include "ui/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "core/config.h"
#include "core/engine.h"
#include "core/noise.h"
#include "core/status.h"
#include "core/text.h"
#include "platform/config_file.h"
#include "platform/paths.h"
#include "platform/platform.h"

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
          "commands:\n"
          "  play | pause | toggle      control playback\n"
          "  noise <white|pink|brown>   select the noise type\n"
          "  volume <0-100>             set the volume\n"
          "  stop-after <1-10080|off>   end the session after N minutes\n"
          "  status                     print the current state\n"
          "  quit                       stop the daemon\n"
          "\n"
          "options:\n"
          "  --noise --volume --play --pause --toggle --stop-after\n"
          "  --mono --stereo --status --quit\n"
          "      the same commands in long-flag form\n"
          "  --portable                 config.ini next to the executable\n"
          "  --foreground               keep the daemon attached to the "
          "terminal\n"
          "\n"
          "notes:\n"
          "  a new session uses pink noise at volume 20, fading in over 1 s\n"
          "\n"
          "render:\n"
          "  --render <file>            offline 48 kHz stereo float32; a .wav\n"
          "                             path adds a WAV header\n"
          "  --noise <type>             white, pink, or brown; default white\n"
          "  --volume <0-100>           render volume, default 100\n"
          "  --mono | --stereo          duplicate one channel; default stereo\n"
          "  --seconds <1-3600>         render length, default 10\n"
          "  --seed <n>                 generator seed, default 1\n"
          "\n"
          "other:\n"
          "  --version                  print version and exit\n"
          "  --help                     print this text and exit\n",
          stdout);
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
    return wn_text_equal_ci(path + len - 4, ".wav");
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
                       int noise_type, int volume, int mono)
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

    /* Render defaults to full scale and stereo. `--volume` and
     * `--mono`/`--stereo` select the same parameters a daemon would use, which
     * is what makes FR-03 and FR-13 observable without a sound card. */
    cfg.noise = noise_type;
    cfg.volume = volume;
    cfg.mono = mono;
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

/* --- CLI modes ------------------------------------------------------------ */

/* PRD section 7 rule 6: --help and --version take precedence over every other
 * argument. The full parser runs afterwards; this pass only looks for the two
 * tokens, skipping values so they are not mistaken for commands.
 *
 * A value is skipped only for a command the parser itself recognizes
 * (wn_cmd_takes_value shares the parser's case-sensitive matching). After an
 * unrecognized token there is no value to skip, so a following help/version
 * still wins -- the same outcome cli_check asserts for `--bogus --help`: rule
 * 6 covers bad flags too. */
static void scan_precedence(int argc, const char *const *argv, int *want_help,
                            int *want_version)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *name;

        if (arg == NULL) {
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            break;
        }

        name = arg;
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            name = arg + 2;
        } else if (arg[0] == '-') {
            continue; /* a short option; the parser reports it */
        }

        if (strcmp(name, "help") == 0) {
            *want_help = 1;
        } else if (strcmp(name, "version") == 0) {
            *want_version = 1;
        }

        if (wn_cmd_takes_value(name) && i + 1 < argc) {
            i++;
        }
    }
}

static int resolve_config_path(char *path, size_t cap, int portable)
{
    if (wn_paths_config_file(path, cap, portable) != 0) {
        if (portable) {
            wn_cli_error("cannot locate config.ini next to the executable");
        } else {
            wn_cli_error("cannot resolve the configuration directory");
        }
        return WN_EXIT_RUNTIME;
    }
    return WN_EXIT_OK;
}

static int load_config_file(const char *path, wn_config *cfg, char *text,
                            size_t *text_len)
{
    char warn[160];
    int result;

    result = wn_config_file_load(path, cfg, text, WN_CONFIG_TEXT_MAX, text_len,
                                 warn, sizeof(warn));
    if (result != 0) {
        wn_cli_error(warn[0] != '\0' ? warn : "cannot read the configuration");
        return WN_EXIT_RUNTIME;
    }
    if (warn[0] != '\0') {
        /* Malformed values never abort startup, but they are not silent. */
        wn_cli_error(warn);
    }

    return WN_EXIT_OK;
}

static int save_config_file(const char *path, const wn_config *cfg,
                            const char *original, int portable)
{
    char err[160];

    if (wn_config_file_save(path, cfg, original, portable, err, sizeof(err)) !=
        0) {
        wn_cli_error(err[0] != '\0' ? err : "cannot write the configuration");
        return WN_EXIT_RUNTIME;
    }

    return WN_EXIT_OK;
}

/* --- Session setup --------------------------------------------------------
 *
 * Both session modes share one preparation step: read the configuration, apply
 * the persisted setting commands, save once, create the engine, then apply the
 * playback and timer commands. Preparing before the fork copies the resulting
 * state into the daemon, so the child starts exactly where a foreground run
 * would (PRD section 7 rule 7: the same commands produce the same result on
 * both paths). */

typedef struct {
    wn_config cfg;
    char path[WN_PATH_MAX];
    char text[WN_CONFIG_TEXT_MAX];
    size_t text_len;
    wn_engine *engine;
    long stop_after_ms;
    int quit_only;
} session_setup;

static void print_status(const wn_config *cfg)
{
    char text[WN_IPC_REPLY_MAX];

    /* The no-daemon form from FR-08: the persisted settings, playing=0, and no
     * timer. The formatter is shared with the daemon reply. */
    (void)wn_status_format(text, sizeof(text), 0, cfg->noise, cfg->volume,
                           cfg->mono, WN_STOP_AFTER_SECONDS_OFF);
    fputs(text, stdout);
}

static void apply_persisted(wn_config *cfg, const wn_cmd *cmd)
{
    switch (cmd->kind) {
        case WN_CMD_NOISE:
            cfg->noise = cmd->ivalue;
            break;
        case WN_CMD_VOLUME:
            cfg->volume = cmd->ivalue;
            break;
        case WN_CMD_MONO:
            cfg->mono = 1;
            break;
        case WN_CMD_STEREO:
            cfg->mono = 0;
            break;
        default:
            break;
    }
}

static int prepare_session(const wn_cmd *cmds, int n, int portable,
                           session_setup *setup)
{
    int have_persist = 0;
    int result;
    int i;

    memset(setup, 0, sizeof(*setup));
    setup->stop_after_ms = -1;

    for (i = 0; i < n; i++) {
        if (cmds[i].kind == WN_CMD_QUIT) {
            /* Nothing is running, so quitting is already satisfied. */
            setup->quit_only = 1;
            return WN_EXIT_OK;
        }
    }

    result = resolve_config_path(setup->path, sizeof(setup->path), portable);
    if (result != WN_EXIT_OK) {
        return result;
    }
    result = load_config_file(setup->path, &setup->cfg, setup->text,
                              &setup->text_len);
    if (result != WN_EXIT_OK) {
        return result;
    }

    for (i = 0; i < n; i++) {
        switch (cmds[i].kind) {
            case WN_CMD_NOISE:
            case WN_CMD_VOLUME:
            case WN_CMD_MONO:
            case WN_CMD_STEREO:
                apply_persisted(&setup->cfg, &cmds[i]);
                have_persist = 1;
                break;
            default:
                break;
        }
    }

    if (have_persist) {
        result = save_config_file(setup->path, &setup->cfg,
                                  setup->text_len > 0 ? setup->text : NULL,
                                  portable);
        if (result != WN_EXIT_OK) {
            return result;
        }
    }

    setup->engine = wn_engine_create(&setup->cfg);
    if (setup->engine == NULL) {
        wn_cli_error("out of memory");
        return WN_EXIT_RUNTIME;
    }

    /* A session opens over a second instead of the 20 ms used for playback
     * changes: starting a daemon is a surprise the user did not ask to hear at
     * full level (PRD FR-09). --render keeps the short fade. */
    wn_engine_set_opening_fade(setup->engine, WN_OPENING_FADE_MS);

    for (i = 0; i < n; i++) {
        switch (cmds[i].kind) {
            case WN_CMD_PLAY:
            case WN_CMD_PAUSE:
            case WN_CMD_TOGGLE:
                (void)wn_engine_apply_cmd(setup->engine, &cmds[i]);
                break;
            case WN_CMD_STOP_AFTER:
                setup->stop_after_ms = cmds[i].ivalue == WN_STOP_AFTER_OFF
                                           ? -1
                                           : (long)cmds[i].ivalue * 60000L;
                break;
            default:
                break;
        }
    }

    return WN_EXIT_OK;
}

static void fill_session(wn_session *session, const session_setup *setup,
                         int portable, int verbose)
{
    memset(session, 0, sizeof(*session));
    session->config = setup->cfg;
    (void)snprintf(session->config_path, sizeof(session->config_path), "%s",
                   setup->path);
    if (setup->text_len > 0 && setup->text_len < sizeof(session->config_text)) {
        memcpy(session->config_text, setup->text, setup->text_len);
        session->config_text[setup->text_len] = '\0';
        session->config_text_len = setup->text_len;
    }
    session->portable = portable;
    session->stop_after_ms = setup->stop_after_ms;
    session->stop_fade_ms = WN_STOP_FADE_MS;
    session->ready_fd = -1;
    session->verbose = verbose;
}

/* --- Forwarded replies ----------------------------------------------------- */

/* Declared here because the foreground path below forwards `quit` before those
 * helpers are defined. */
static int handle_reply(const char *reply);
static int has_kind(const wn_cmd *cmds, int n, wn_cmd_kind kind);

/* A competing start won the single-instance address after this client's probe
 * but before it bound. Forward the command to the winner instead of failing,
 * waiting a short window for the winner to finish starting. Returns WN_EXIT_OK
 * on delivery, WN_EXIT_USAGE if the line is too long, WN_EXIT_IPC otherwise. */
static int forward_competing_daemon(const wn_cmd *cmds, int n)
{
    char line[WN_LINE_MAX];
    char reply[WN_IPC_REPLY_MAX];
    size_t length = wn_cmd_format_ipc_line(cmds, n, line, sizeof(line));
    int result;

    if (length >= sizeof(line)) {
        wn_cli_error("the command line is too long");
        return WN_EXIT_USAGE;
    }

    if (length > 0) {
        result = wn_platform_ipc_send_wait(line, reply, sizeof(reply));
    } else {
        /* A bare start carries no command; success means a daemon answers. */
        result = wn_platform_ipc_send_wait("status", reply, sizeof(reply));
    }

    if (result == WN_IPC_OK) {
        return length > 0 ? handle_reply(reply) : WN_EXIT_OK;
    }
    if (result == WN_IPC_ERROR) {
        wn_cli_error("cannot reach the daemon");
        return WN_EXIT_IPC;
    }

    /* Still absent: the competing daemon vanished before answering. */
    wn_cli_error("another daemon is already running");
    return WN_EXIT_IPC;
}

/* --foreground: run the session attached to this terminal (PRD FR-12). It also
 * owns the IPC socket, so forwarded commands work against a foreground daemon
 * exactly as they do against a detached one.
 *
 * Ordering matters here, for two reasons the background path does not have:
 *
 * 1. quit is a command for a daemon, not a request to start one. With a live
 *    daemon it must reach that daemon; with none it is already satisfied.
 * 2. The single-instance address is taken before anything is persisted, so a
 *    second foreground start fails with the conflict instead of writing
 *    settings the running daemon never sees. */
static int run_foreground(const wn_cmd *cmds, int n, int portable)
{
    session_setup setup;
    wn_session session;
    wn_ipc_server *ipc;
    wn_engine *engine;
    char err[160];
    int result;

    if (has_kind(cmds, n, WN_CMD_QUIT)) {
        char line[WN_LINE_MAX];
        char reply[WN_IPC_REPLY_MAX];
        size_t length = wn_cmd_format_ipc_line(cmds, n, line, sizeof(line));

        if (length >= sizeof(line)) {
            wn_cli_error("the command line is too long");
            return WN_EXIT_USAGE;
        }
        if (length > 0) {
            result = wn_platform_ipc_send(line, reply, sizeof(reply));
            if (result == WN_IPC_OK) {
                return handle_reply(reply);
            }
            if (result == WN_IPC_ERROR) {
                wn_cli_error("cannot reach the daemon");
                return WN_EXIT_IPC;
            }
        }
        return WN_EXIT_OK;
    }

    ipc = wn_ipc_server_open(err, sizeof(err), NULL);
    if (ipc == NULL) {
        wn_cli_error(err);
        return WN_EXIT_IPC;
    }

    result = prepare_session(cmds, n, portable, &setup);
    if (result != WN_EXIT_OK) {
        wn_ipc_server_close(ipc);
        return result;
    }
    if (setup.quit_only) {
        /* Unreachable after the quit check above; kept as a guard. */
        wn_ipc_server_close(ipc);
        return WN_EXIT_OK;
    }
    engine = setup.engine;

    fill_session(&session, &setup, portable, 1);
    session.ipc = ipc;

    result = wn_platform_run(engine, &session);
    wn_engine_destroy(engine);

    if (result != 0) {
        wn_cli_error("cannot open an audio device");
        return WN_EXIT_RUNTIME;
    }
    return WN_EXIT_OK;
}

/* Background daemon: bind the single-instance address, detach, and let the
 * daemon run the loop. The parent returns as soon as the daemon reports that
 * the audio device is up, so a first start still fails with exit 1 and a clear
 * message when no device exists (PRD FR-14, FR-09). */
static int run_background(const wn_cmd *cmds, int n, int portable, int argc,
                          const char *const *argv)
{
    session_setup setup;
    wn_session session;
    wn_ipc_server *ipc = NULL;
    wn_engine *engine;
    char err[160];
    intptr_t ready_fd = -1;
    int result;

#if !defined(_WIN32)
    /* The forking parent takes the socket and the lock before the fork, so the
     * address is owned from the moment the client gives up, and before anything
     * is persisted: a race with another start must fail on the lock instead of
     * saving settings no session applied. Windows cannot inherit this process's
     * handles into a detached copy that way, so its child creates the pipe
     * itself. */
    {
        int conflict = 0;

        ipc = wn_ipc_server_open(err, sizeof(err), &conflict);
        if (ipc == NULL) {
            if (conflict) {
                /* Lost a simultaneous-start race: a daemon now owns the
                 * address. Forward to it instead of failing (PRD FR-07). */
                return forward_competing_daemon(cmds, n);
            }
            wn_cli_error(err);
            return WN_EXIT_IPC;
        }
    }
#endif

    result = prepare_session(cmds, n, portable, &setup);
    if (result != WN_EXIT_OK) {
        wn_ipc_server_close(ipc);
        return result;
    }
    if (setup.quit_only) {
        wn_ipc_server_close(ipc);
        return WN_EXIT_OK;
    }
    engine = setup.engine;

    fill_session(&session, &setup, portable, 0);
    session.ipc = ipc;

    result = wn_platform_detach(argc, argv, &ready_fd, err, sizeof(err));
    if (result == WN_DETACH_ERROR) {
        wn_cli_error(err);
        wn_ipc_server_close(session.ipc);
        wn_engine_destroy(engine);
        return WN_EXIT_IPC;
    }
    if (result == WN_DETACH_PARENT) {
        /* The daemon owns the socket now. Release this process's descriptors
         * and memory without unlinking the path, then let the client exit. */
        wn_ipc_server_abandon(session.ipc);
        wn_engine_destroy(engine);
        return WN_EXIT_OK;
    }
    if (result == WN_DETACH_PARENT_FAIL) {
        /* The child either cleaned up its own handle or was killed, leaving a
         * stale socket the next start removes under the lock. This process only
         * reports the failure; unlinking here could delete a socket a newer
         * daemon has just bound. */
        wn_cli_error(err);
        wn_ipc_server_abandon(session.ipc);
        wn_engine_destroy(engine);
        return WN_EXIT_RUNTIME;
    }

    /* Child (POSIX only; Windows never returns WN_DETACH_CHILD). */
    session.ready_fd = ready_fd;
    result = wn_platform_run(engine, &session);
    wn_engine_destroy(engine);

    if (result != 0) {
        return WN_EXIT_RUNTIME;
    }
    return WN_EXIT_OK;
}

#if defined(_WIN32)
/* The detached child: the parent cannot pass memory across CreateProcess, so
 * the child re-runs the same arguments. The persisted settings were already
 * saved by the parent and every remaining step is deterministic, so the session
 * is the one a forked child would have inherited. */
static int run_daemon_child(const wn_cmd *cmds, int n, int portable,
                            intptr_t ready_fd)
{
    session_setup setup;
    wn_session session;
    wn_engine *engine;
    char err[160];
    int result;

    result = prepare_session(cmds, n, portable, &setup);
    if (result != WN_EXIT_OK) {
        return result;
    }
    if (setup.quit_only) {
        return WN_EXIT_OK;
    }
    engine = setup.engine;

    fill_session(&session, &setup, portable, 0);
    session.ready_fd = ready_fd;
    {
        int conflict = 0;

        session.ipc = wn_ipc_server_open(err, sizeof(err), &conflict);
        if (session.ipc == NULL) {
            if (conflict) {
                /* Lost the pipe to a competing start. The command is applied by
                 * the winner, so tell the waiting parent it succeeded instead of
                 * timing out as if the device never came up. */
                result = forward_competing_daemon(cmds, n);
                if (result == WN_EXIT_OK) {
                    wn_platform_notify_ready(&session, 1);
                }
                wn_engine_destroy(engine);
                return result;
            }
            wn_cli_error(err);
            wn_engine_destroy(engine);
            return WN_EXIT_IPC;
        }
    }

    result = wn_platform_run(engine, &session);
    wn_engine_destroy(engine);

    return result != 0 ? WN_EXIT_RUNTIME : WN_EXIT_OK;
}
#endif

/* Prints a daemon reply and maps it to an exit code. `ok` is silent: setting
 * and playback commands produce no output (PRD FR-01, FR-06). An error reply
 * carries the exit code the daemon could not produce itself, so a failed
 * config write still exits 1 (FR-14) and an invalid command exits 2. */
static int handle_reply(const char *reply)
{
    char message[WN_IPC_REPLY_MAX];
    char *end = NULL;
    long code = WN_EXIT_USAGE;
    const char *text = reply;
    size_t length;

    if (strncmp(reply, "error ", 6) == 0) {
        text = reply + 6;
        code = strtol(text, &end, 10);
        if (end != text && *end == ' ') {
            text = end + 1;
        } else {
            code = WN_EXIT_USAGE;
        }

        (void)snprintf(message, sizeof(message), "%s", text);
        length = strlen(message);
        while (length > 0 &&
               (message[length - 1] == '\n' || message[length - 1] == '\r')) {
            message[--length] = '\0';
        }
        wn_cli_error(message);

        if (code == WN_EXIT_RUNTIME || code == WN_EXIT_IPC) {
            return (int)code;
        }
        return WN_EXIT_USAGE;
    }

    if (strncmp(reply, "ok", 2) != 0) {
        fputs(reply, stdout);
    }
    return WN_EXIT_OK;
}

static int has_kind(const wn_cmd *cmds, int n, wn_cmd_kind kind)
{
    int i;

    for (i = 0; i < n; i++) {
        if (cmds[i].kind == kind) {
            return 1;
        }
    }
    return 0;
}

int wn_cli_main(int argc, const char *const *argv)
{
    wn_cmd cmds[WN_CMD_MAX];
    char err[160];
    char path[WN_PATH_MAX];
    char text[WN_CONFIG_TEXT_MAX];
    size_t text_len = 0;
    const char *render_path = NULL;
    int seconds = WN_RENDER_SECONDS_DEFAULT;
    int seed = WN_RENDER_SEED_DEFAULT;
    int noise_type = WN_NOISE_WHITE;
    int volume = 100;
    int mono = 0;
    int have_render = 0;
    int have_status = 0;
    int have_foreground = 0;
    int portable = 0;
    int want_help = 0;
    int want_version = 0;
    int result;
    int n;
    int i;

#if defined(_WIN32)
    int child_mode = 0;
    intptr_t child_ready = -1;

    /* Internal: the detached daemon is this same program with a readiness event
     * and the original arguments behind it. Stripping the two tokens leaves
     * argv[0] in place, so everything below is the normal path. */
    if (argc >= 3 && strcmp(argv[1], "--daemon-child") == 0) {
        child_ready = (intptr_t)strtoull(argv[2], NULL, 10);
        child_mode = 1;
        argv += 2;
        argc -= 2;
    }
#endif

    scan_precedence(argc, argv, &want_help, &want_version);
    if (want_help) {
        wn_cli_usage();
        return WN_EXIT_OK;
    }
    if (want_version) {
        printf("tinyambient %s\n", TA_VERSION);
        return WN_EXIT_OK;
    }

    n = wn_cmd_parse_argv(argc, argv, cmds, WN_CMD_MAX, err, sizeof(err));
    if (n < 0) {
        wn_cli_error(err);
        return WN_EXIT_USAGE;
    }

    for (i = 0; i < n; i++) {
        if (cmds[i].kind == WN_CMD_RENDER) {
            have_render = 1;
            render_path = cmds[i].svalue;
        }
    }

    if (have_render) {
        /* The parser has already rejected daemon commands next to --render. */
        for (i = 0; i < n; i++) {
            switch (cmds[i].kind) {
                case WN_CMD_NOISE:
                    noise_type = cmds[i].ivalue;
                    break;
                case WN_CMD_SECONDS:
                    seconds = cmds[i].ivalue;
                    break;
                case WN_CMD_SEED:
                    seed = cmds[i].ivalue;
                    break;
                case WN_CMD_VOLUME:
                    volume = cmds[i].ivalue;
                    break;
                case WN_CMD_MONO:
                    mono = 1;
                    break;
                case WN_CMD_STEREO:
                    mono = 0;
                    break;
                default:
                    break;
            }
        }
        return render_file(render_path, seconds, (uint64_t)seed, noise_type,
                           volume, mono);
    }

    for (i = 0; i < n; i++) {
        switch (cmds[i].kind) {
            case WN_CMD_STATUS:
                have_status = 1;
                break;
            case WN_CMD_PORTABLE:
                portable = 1;
                break;
            case WN_CMD_FOREGROUND:
                have_foreground = 1;
                break;
            default:
                break;
        }
    }

#if defined(_WIN32)
    if (child_mode) {
        return run_daemon_child(cmds, n, portable, child_ready);
    }
#endif

    if (have_status) {
        char reply[WN_IPC_REPLY_MAX];

        /* A running daemon answers with its live session state (FR-08). With no
         * daemon, the persisted settings stand in and nothing is started. */
        result = wn_platform_ipc_send("status", reply, sizeof(reply));
        if (result == WN_IPC_OK) {
            return handle_reply(reply);
        }
        if (result == WN_IPC_ERROR) {
            wn_cli_error("cannot query the daemon");
            return WN_EXIT_IPC;
        }

        {
            wn_config cfg;

            result = resolve_config_path(path, sizeof(path), portable);
            if (result != WN_EXIT_OK) {
                return result;
            }
            result = load_config_file(path, &cfg, text, &text_len);
            if (result != WN_EXIT_OK) {
                return result;
            }
            print_status(&cfg);
        }
        return WN_EXIT_OK;
    }

    if (have_foreground) {
        return run_foreground(cmds, n, portable);
    }

    {
        char line[WN_LINE_MAX];
        char reply[WN_IPC_REPLY_MAX];
        size_t length = wn_cmd_format_ipc_line(cmds, n, line, sizeof(line));

        if (length >= sizeof(line)) {
            wn_cli_error("the command line is too long");
            return WN_EXIT_USAGE;
        }

        /* Content commands go to the running daemon (PRD section 7 rule 7).
         * With no daemon they are applied by the one this invocation starts. */
        if (length > 0) {
            result = wn_platform_ipc_send(line, reply, sizeof(reply));
            if (result == WN_IPC_OK) {
                return handle_reply(reply);
            }
            if (result == WN_IPC_ERROR) {
                wn_cli_error("cannot reach the daemon");
                return WN_EXIT_IPC;
            }
        } else {
            /* No forwardable command: a bare start (or --portable alone). An
             * existing daemon makes this a no-op, exactly like a second bare
             * invocation of a running session (FR-09). */
            result = wn_platform_ipc_send("status", reply, sizeof(reply));
            if (result == WN_IPC_OK) {
                return WN_EXIT_OK;
            }
            if (result == WN_IPC_ERROR) {
                wn_cli_error("cannot query the daemon");
                return WN_EXIT_IPC;
            }
        }

        /* `quit` with nothing running is already satisfied; starting a daemon
         * only to stop it would be pointless. */
        if (has_kind(cmds, n, WN_CMD_QUIT)) {
            return WN_EXIT_OK;
        }

        return run_background(cmds, n, portable, argc, argv);
    }
}
