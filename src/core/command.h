#ifndef WN_CORE_COMMAND_H
#define WN_CORE_COMMAND_H

#include <stddef.h>

/* Single parser for argv and IPC lines. Knows nothing about sockets, files, or
 * stdout. Bare words and long flags are equivalent, so `noise pink` and
 * `--noise pink` produce the same command. */
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

/* Stop-after is session state in minutes. `off` cancels it. One week is the
 * upper bound: the timer exists to bound a work session, not a machine uptime. */
#define WN_STOP_AFTER_OFF (-1)
#define WN_STOP_AFTER_MIN 1
#define WN_STOP_AFTER_MAX 10080

/* Parses one argv-style token list. Returns the number of commands written to
 * `out`, or -1 on a usage error with a message in `err`. */
int wn_cmd_parse_argv(int argc, const char *const *argv, wn_cmd *out, int cap,
                      char *err, size_t err_cap);

/* Parses one command line without the terminator. The buffer is modified in
 * place: tokens are separated with NUL bytes, and `svalue` may point into it.
 * The caller must keep `line` alive while it uses the results. Returns the
 * number of commands, or -1 with a message in `err`. */
int wn_cmd_parse_line(char *line, wn_cmd *out, int cap, char *err,
                      size_t err_cap);

/* True when the command name consumes the next token as its value. Matching is
 * case-sensitive, exactly like wn_cmd_parse_argv. The precedence scan in
 * ui/cli.c uses this so its value-skipping cannot drift from the parser. */
int wn_cmd_takes_value(const char *name);

/* Formats `count` commands as one IPC line. --portable, --foreground, --help,
 * and --version are local to the client and are skipped; the parser already
 * forbids render options in a forwarded line. Returns the byte count the line
 * needs, snprintf-style, so a result >= cap signals truncation. */
size_t wn_cmd_format_ipc_line(const wn_cmd *cmds, int count, char *out,
                              size_t cap);

#endif /* WN_CORE_COMMAND_H */
