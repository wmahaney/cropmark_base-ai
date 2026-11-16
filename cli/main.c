/*
 * cropmark -- entry point. Decides between the window and batch mode:
 *
 *   cropmark                     picture picker in the current folder
 *   cropmark FOLDER              picture picker in FOLDER
 *   cropmark PHOTO [OUTPUT]      editor window on PHOTO
 *   cropmark PHOTO EDITS... -o OUTPUT
 *                                batch mode: apply EDITS, no window
 *
 * Any option other than the window's own (--quality, --no-session) means
 * batch mode, so every command line that worked with the old `pmark` CLI
 * behaves the same here.
 */
#include "cropmark.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
    fputs(
"usage: cropmark                       pick a picture in this folder, then edit it\n"
"       cropmark FOLDER                pick a picture in FOLDER\n"
"       cropmark PHOTO [OUTPUT]        edit PHOTO (.jpg/.png, or a .pmark session)\n"
"                [--quality N] [--no-session]\n"
"       cropmark PHOTO EDITS... -o OUTPUT\n"
"                                      apply EDITS without opening a window\n"
"\n"
"In the window, Ctrl+S saves OUTPUT (default PHOTO-marked.png) and, unless\n"
"--no-session, OUTPUT.pmark next to it so the edit can be reopened.\n"
"Ctrl+O goes back to the picker.\n"
"\n", f);
    cm_batch_usage(f);
}

/* Options the window understands; `takes_value` ones consume the next word. */
static bool gui_option(const char *a, bool *takes_value)
{
    *takes_value = !strcmp(a, "--quality") || !strcmp(a, "--selftest");
    return *takes_value || !strcmp(a, "--no-session") || !strcmp(a, "--selftest-picker");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
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
            fprintf(stderr, "cropmark: batch mode needs the photo first: "
                            "cropmark PHOTO EDITS... -o OUTPUT (see --help)\n");
            return 2;
        }
        return cm_batch_main(argc, argv);
    }
#ifdef CM_NO_GUI
    fprintf(stderr, "cropmark: this build has no window (it was built with `make headless`).\n"
                    "Give edits and -o OUTPUT for batch mode, or see --help.\n");
    return 2;
#else
    return cm_gui_main(argc, argv);
#endif
}
