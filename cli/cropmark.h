/* The two halves of the cropmark program; cli/main.c chooses between them. */
#ifndef CROPMARK_H
#define CROPMARK_H

#include <stdio.h>

/* Batch mode (cli/batch.c): argv[1] is INPUT, the rest are edits and
 * output options. Never opens a window. */
int  cm_batch_main(int argc, char **argv);
void cm_batch_usage(FILE *f);

/* The window (gui/gui.c): no positional argument opens the picture picker
 * in the current folder; a folder opens the picker there; a photo or
 * .pmark session opens the editor on it. Not built with CM_NO_GUI. */
int  cm_gui_main(int argc, char **argv);

#endif
