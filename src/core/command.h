#ifndef WN_CORE_COMMAND_H
#define WN_CORE_COMMAND_H

#include <stddef.h>

/* Single parser for argv and IPC lines. Knows nothing about sockets, files, or
 * stdout. Bare words and long flags are equivalent.
 *
 * M1 recognizes --render, --seconds, --seed, --version, --help. The remaining
 * kinds are declared now because the enum is the parser's contract, but they
 * are rejected as unknown until their flag lands. */
typedef enum {
    WN_CMD_NONE = 0,
    WN_CMD_PLAY,
    WN_CMD_PAUSE,
    WN_CMD_TOGGLE,
    WN_CMD_NOISE,
    WN_CMD_VOLUME,
    WN_CMD_STOP_AFTER,
    WN_CMD_MONO,
    WN_CMD_STEREO,
    WN_CMD_STATUS,
    WN_CMD_RENDER,
    WN_CMD_SECONDS,
    WN_CMD_SEED,
    WN_CMD_PORTABLE,
    WN_CMD_FOREGROUND,
    WN_CMD_VERSION,
    WN_CMD_HELP,
    WN_CMD_QUIT
} wn_cmd_kind;

typedef struct {
    wn_cmd_kind kind;
    int ivalue;         /* volume, minutes, seconds, seed */
    const char *svalue; /* noise type, render path */
} wn_cmd;

/* Fixed capacity for one invocation's command list. */
#define WN_CMD_MAX 16
/* Maximum length of one IPC line, including the newline. */
#define WN_LINE_MAX 256

/* Defaults for the render options, applied by the caller when the flag is
 * absent (PRD FR-10). */
#define WN_RENDER_SECONDS_DEFAULT 10
#define WN_RENDER_SEED_DEFAULT 1
#define WN_RENDER_SECONDS_MIN 1
#define WN_RENDER_SECONDS_MAX 3600

/* Parses one argv-style token list. Returns the number of commands written to
 * `out`, or -1 on a usage error with a message in `err`. */
int wn_cmd_parse_argv(int argc, const char *const *argv, wn_cmd *out, int cap,
                      char *err, size_t err_cap);

/* Parses one newline-terminated IPC line without the terminator. Lands in M7. */
int wn_cmd_parse_line(const char *line, wn_cmd *out, int cap, char *err,
                      size_t err_cap);

#endif /* WN_CORE_COMMAND_H */
