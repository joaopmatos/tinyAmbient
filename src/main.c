#include "ui/cli.h"

/* Wiring only: argv -> command_parse -> engine/IPC. No DSP, no UI logic. */
int main(int argc, char **argv)
{
    return wn_cli_main(argc, (const char *const *)argv);
}
