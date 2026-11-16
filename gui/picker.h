/*
 * The picture picker: a folder browser that shows the pictures (and .pmark
 * sessions) in a folder as a grid of thumbnails. Thumbnails are decoded on
 * a worker thread, only for the tiles on screen, so large folders of big
 * photos stay responsive.
 *
 * The picker owns no window: the caller passes its renderer, feeds it
 * events and asks it to draw, so the editor and the picker share one
 * window and the user can go back and forth (Ctrl+O in the editor).
 */
#ifndef CM_PICKER_H
#define CM_PICKER_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "pmark.h"

typedef struct picker picker;

typedef enum {
    PICK_NONE,   /* keep going                                     */
    PICK_OPEN,   /* the user chose a file: see picker_choice()     */
    PICK_CANCEL, /* Esc with nothing to clear                      */
} pick_result;

/* dir NULL or "" is the current folder. font is borrowed (NULL: no labels).
 * Returns NULL if dir cannot be listed or memory runs out. */
picker     *picker_new(SDL_Renderer *ren, const pm_font *font, const char *dir);
void        picker_free(picker *p);
pick_result picker_event(picker *p, const SDL_Event *e);
/* Draws the whole window and presents it. dpi = renderer pixels per
 * window coordinate. */
void        picker_render(picker *p, float dpi);
const char *picker_choice(const picker *p); /* full path, after PICK_OPEN */
const char *picker_dir(const picker *p);    /* absolute path being shown */
void        picker_set_status(picker *p, const char *fmt, ...);
bool        picker_is_dir(const char *path);

/* For the selftest. */
size_t      picker_shown(const picker *p);    /* tiles passing the filter */
const char *picker_selected(const picker *p); /* selected name, or NULL */
size_t      picker_thumbs_done(const picker *p);

#endif
