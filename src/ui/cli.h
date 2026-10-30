#ifndef WN_UI_CLI_H
#define WN_UI_CLI_H

#include <stddef.h>

/* Exit codes from PRD FR-14. */
#define WN_EXIT_OK 0
#define WN_EXIT_RUNTIME 1
#define WN_EXIT_USAGE 2
#define WN_EXIT_IPC 3

/* Writes one diagnostic line to stderr, prefixed with "tinyambient: ". */
void wn_cli_error(const char *message);

/* Prints usage text to stdout. */
void wn_cli_usage(void);

/* Parses argv and returns an exit code. */
int wn_cli_main(int argc, const char *const *argv);

#endif /* WN_UI_CLI_H */
