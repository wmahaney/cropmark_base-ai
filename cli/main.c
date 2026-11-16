/*
 * cropmark (pmark for short) -- entry point:
 *
 *   cropmark                     picture picker
 *   cropmark PHOTO               editor on PHOTO
 *   cropmark PHOTO EDITS... -o OUTPUT
 *                                command-line mode: apply EDITS, no window
 *
 * Any option other than the window's own (--pictures, --quality,
 * --no-session) selects command-line mode.
 */
#include "cropmark.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
    fputs(
"cropmark " CROPMARK_VERSION " -- crop photos, draw on them, add text\n"
"\n"
"usage: cropmark              pick a picture (in this folder), then edit it\n"
"       cropmark PHOTO        edit PHOTO (.jpg, .png, or a .pmark session)\n"
"\n"
"Window options:\n"
"  cropmark FOLDER            start the picker in FOLDER\n"
"  --pictures                 start the picker in your Pictures folder\n"
"  cropmark PHOTO OUTPUT      save to OUTPUT (default PHOTO-marked.png)\n"
"  --quality N                JPEG quality of OUTPUT, 1-100 (default 92)\n"
"  --no-session               don't write OUTPUT.pmark next to OUTPUT\n"
"  --version                  print the version\n"
"\n"
"In the window: Ctrl+S saves, Ctrl+O goes back to the picker.\n"
"\n", f);
    cm_batch_usage(f);
}

/* Options the window understands; `takes_value` ones consume the next word. */
static bool gui_option(const char *a, bool *takes_value)
{
    *takes_value = !strcmp(a, "--quality") || !strcmp(a, "--selftest");
    return *takes_value || !strcmp(a, "--no-session") || !strcmp(a, "--pictures") ||
           !strcmp(a, "--selftest-picker");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            puts("cropmark " CROPMARK_VERSION);
            return 0;
        }
    }

    bool batch = false;
    for (int i = 1; i < argc && !batch; i++) {
        const char *a = argv[i];
        bool takes_value;
        if (a[0] != '-' || !a[1]) continue; /* positional ("-" is not an option) */
        if (gui_option(a, &takes_value)) i += takes_value;
        else batch = true;
    }
    if (batch) {
        if (argc < 2 || argv[1][0] == '-') {
            fprintf(stderr, "cropmark: command-line mode needs the photo first: "
                            "cropmark PHOTO EDITS... -o OUTPUT (see --help)\n");
            return 2;
        }
        return cm_batch_main(argc, argv);
    }
#ifdef CM_NO_GUI
    fprintf(stderr, "cropmark: this build has no window (it was built with `make headless`).\n"
                    "Give edits and -o OUTPUT for command-line mode, or see --help.\n");
    return 2;
#else
    return cm_gui_main(argc, argv);
#endif
}
