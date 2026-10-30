#include <stddef.h>

#include "ui/cli.h"

/* Wiring only: argv -> command_parse -> engine/IPC. No DSP, no UI logic.
 *
 * M0 scaffolding: the real dispatch table lands in M5, together with
 * ui/cli.c. Until then the binary builds, links, and reports its size. */
int main(int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;
    return WN_EXIT_OK;
}
