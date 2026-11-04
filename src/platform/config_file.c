#if !defined(_WIN32)
/* Must precede every system header: mkstemp, fdopen, fsync, and fileno are
 * POSIX, and -std=c11 hides them without the feature macro. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform/config_file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.h"
#include "core/text.h"
#include "platform/paths.h"

int wn_config_file_load(const char *path, wn_config *cfg, char *text,
                        size_t text_cap, size_t *text_len, char *warn,
                        size_t warn_cap)
{
    FILE *file;
    size_t used = 0;
    int too_long = 0;

    if (path == NULL || cfg == NULL || text == NULL || text_cap == 0) {
        return -1;
    }

    wn_config_defaults(cfg);
    text[0] = '\0';
    if (text_len != NULL) {
        *text_len = 0;
    }
    if (warn != NULL && warn_cap > 0) {
        warn[0] = '\0';
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            /* First run: defaults are the configuration. */
            return 0;
        }
        wn_text_set(warn, warn_cap, "cannot read '%s'", path);
        return -1;
    }

    while (used + 1 < text_cap) {
        size_t got = fread(text + used, 1, text_cap - 1 - used, file);

        used += got;
        if (got == 0) {
            break;
        }
    }

    if (ferror(file) != 0) {
        (void)fclose(file);
        wn_text_set(warn, warn_cap, "cannot read '%s'", path);
        return -1;
    }

    if (feof(file) == 0) {
        int extra = fgetc(file);

        if (extra != EOF) {
            too_long = 1;
        }
        if (ferror(file) != 0) {
            (void)fclose(file);
            wn_text_set(warn, warn_cap, "cannot read '%s'", path);
            return -1;
        }
    }

    (void)fclose(file);
    text[used] = '\0';
    if (text_len != NULL) {
        *text_len = used;
    }

    if (too_long) {
        wn_text_set(warn, warn_cap,
                    "configuration file is too large; extra bytes ignored");
    }
    wn_config_parse(text, used, cfg, warn, warn_cap);

    return 0;
}

#if defined(_WIN32)
static int atomic_write(const char *path, const char *text, size_t length)
{
    char temporary[WN_PATH_MAX];
    FILE *file;
    int written;

    written = snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", path,
                       (unsigned long)GetCurrentProcessId());
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        return -1;
    }

    file = fopen(temporary, "wb");
    if (file == NULL) {
        return -1;
    }

    if (fwrite(text, 1, length, file) != length) {
        (void)fclose(file);
        (void)remove(temporary);
        return -1;
    }
    if (fflush(file) != 0 || _commit(_fileno(file)) != 0) {
        (void)fclose(file);
        (void)remove(temporary);
        return -1;
    }
    if (fclose(file) != 0) {
        (void)remove(temporary);
        return -1;
    }

    if (MoveFileExA(temporary, path,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        (void)remove(temporary);
        return -1;
    }

    return 0;
}
#else
static int atomic_write(const char *path, const char *text, size_t length)
{
    char temporary[WN_PATH_MAX];
    FILE *file;
    int descriptor;
    int written;

    written = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        return -1;
    }

    descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        return -1;
    }

    file = fdopen(descriptor, "wb");
    if (file == NULL) {
        (void)close(descriptor);
        (void)unlink(temporary);
        return -1;
    }

    if (fwrite(text, 1, length, file) != length) {
        (void)fclose(file);
        (void)unlink(temporary);
        return -1;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        (void)fclose(file);
        (void)unlink(temporary);
        return -1;
    }
    if (fclose(file) != 0) {
        (void)unlink(temporary);
        return -1;
    }

    if (rename(temporary, path) != 0) {
        (void)unlink(temporary);
        return -1;
    }

    return 0;
}
#endif

int wn_config_file_save(const char *path, const wn_config *cfg,
                        const char *original, int portable, char *err,
                        size_t err_cap)
{
    char text[WN_CONFIG_TEXT_MAX];
    size_t needed;

    if (path == NULL || cfg == NULL) {
        wn_text_set(err, err_cap, "internal: no configuration to write");
        return -1;
    }

    needed = wn_config_format(cfg, original, NULL, 0);
    if (needed >= sizeof(text)) {
        wn_text_set(err, err_cap, "configuration is too large to write");
        return -1;
    }
    (void)wn_config_format(cfg, original, text, sizeof(text));

    if (wn_paths_make_config_dir(path, portable, err, err_cap) != 0) {
        return -1;
    }

    if (atomic_write(path, text, needed) != 0) {
        if (portable) {
            wn_text_set(err, err_cap,
                        "cannot write config.ini next to the executable");
        } else {
            wn_text_set(err, err_cap, "cannot write the configuration file");
        }
        return -1;
    }

    return 0;
}
