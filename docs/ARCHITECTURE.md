# pmark architecture

pmark opens a photo (JPEG or PNG), lets you crop it, draw on it with markers,
and place text on it, then writes the result back out as PNG or JPEG. It is
written in C11.

Status legend: **[done]** implemented and tested, **[first pass]** works but
is deliberately minimal, **[planned]** not started.

## 1. Goals and non-goals

Goals

- Load JPEG and PNG, save PNG and JPEG. **[done]**
- Three edits: crop, freehand marker strokes (opaque or highlighter), text. **[done]**
- Undo and redo for every edit. **[done]**
- One editing core shared by every frontend, so a scripted edit and an
  interactive edit produce the same pixels. **[done]**
- Build with a C compiler and `make`; the core has no dependencies outside
  the repository. **[done]**

Non-goals (for now): layers, selections, filters, colour management, RAW
formats, re-editing a committed stroke or text box, complex text shaping
(right-to-left, ligatures, emoji).

## 2. Layering

```
  cli/main.c  (pmark)          gui/main.c  (pmark-gui, SDL2)
  argv -> ops -> save          input, view transform, previews
          \                        /
           v                      v
      include/pmark.h   -- the only header frontends include
  +------------------------------------------------------+
  | pm_doc    document = base image + op log, undo/redo  |
  | pm_draw   stroke rasteriser     pm_text  text layout |
  | pm_image  buffer, crop, load/save    pm_exif  orient |
  +------------------------------------------------------+
  third_party/  stb_image, stb_image_write, stb_truetype
                (instantiated once, in src/pm_stb.c)
```

Rules that keep the layers honest:

1. The core never includes SDL, never prints, and holds no global state.
2. Frontends never write pixels into the canvas. Committed edits go through
   `pm_doc_*`; live previews call `pm_op_apply`, the same function a commit
   uses, on a scratch copy.
3. stb is an implementation detail. No stb type appears in `pmark.h`, so the
   codecs or the font rasteriser can be swapped (libjpeg-turbo, FreeType)
   without touching a frontend.

## 3. Data model

**Image** (`pm_image`): `w`, `h`, and one owned buffer of RGBA8 pixels,
straight (non-premultiplied) alpha, row-major, stride `4*w`, origin top-left.
Everything decodes to this one format. Dimensions are capped at 32768.

**Coordinates**: continuous image coordinates, in which pixel `(i, j)` covers
`[i, i+1) x [j, j+1)` and has its centre at `(i+0.5, j+0.5)`. Strokes and
text positions are floats in this space; crop rectangles are integer pixel
rectangles.

**Operation** (`pm_op`): a tagged union, one of

| kind   | payload                                   |
|--------|-------------------------------------------|
| crop   | integer rect                              |
| stroke | point list, width, colour (alpha allowed) |
| text   | UTF-8 string, position, pixel size, font, colour |

**Document** (`pm_doc`):

```
base      the decoded file, never modified
ops[]     the op log
          ops[0 .. n_applied)      applied
          ops[n_applied .. n_ops)  redo tail
canvas    cache: base with ops[0 .. n_applied) applied
```

An op's coordinates are relative to the canvas at the moment it was pushed,
that is, after all earlier crops. Replaying the log from `base` is therefore
deterministic and needs no coordinate fix-ups.

- **push**: apply the op to `canvas`; on success drop the redo tail and
  append. A failed push leaves both the log and the canvas unchanged.
- **undo**: replay `ops[0 .. n_applied-1)` onto a fresh copy of `base`, then
  swap it in. Simple and always correct; cost is one full replay.
- **redo**: apply `ops[n_applied]` to `canvas`.

Because `base` is kept, crop is non-destructive: undoing a crop brings the
cropped-away pixels back.

## 4. Algorithms

**Stroke** (`pm_draw.c`). A stroke is a polyline with round caps and joins,
i.e. the union of capsules of radius `width/2` around each segment.

1. Allocate an 8-bit coverage mask over the stroke's bounding box.
2. For each segment, visit only that segment's bounding box and set
   `mask = max(mask, clamp(r + 0.5 - dist(pixel centre, segment), 0, 1))`.
3. Composite the colour through the mask once.

Taking the max and compositing once is what makes translucent strokes behave
like a real highlighter: a stroke that crosses itself does not get darker at
the crossing. (Two separate strokes do stack, as they should.) Cost is the
sum of the segment boxes, not stroke box times segment count.

**Text** (`pm_text.c`). UTF-8 is decoded to code points (malformed bytes
become U+FFFD); each glyph is rasterised by stb_truetype at a sub-pixel x
offset and blended with its coverage as alpha. Pair kerning from the `kern`
table is applied; `\n` starts a new line. One layout routine serves both
drawing and measuring, so the two cannot disagree.

**Blending**: straight-alpha source-over in 8-bit integer arithmetic,
performed directly on the stored sRGB values (see limitations).

**EXIF orientation** (`pm_exif.c`). Phone JPEGs are commonly stored sideways
with an EXIF tag saying how to turn them. The loader finds tag 0x0112 in the
APP1 segment and bakes the rotation or flip into the pixels, so the rest of
the program only ever sees upright images.

## 5. Errors and ownership

- Every fallible function returns `pm_status` (`PM_OK == 0`);
  `pm_strerror` gives a message. The core does not print or abort.
- Out parameters are written only on success.
- Whoever creates an object frees it: `pm_image_free`, `pm_font_free`,
  `pm_doc_close`.
- `pm_doc_*` push functions copy their inputs (points, strings).
  `pm_doc_from_image` takes ownership of the image passed in.
- Fonts are borrowed by text ops and must outlive the document.
- Geometry is validated at the boundary: non-finite coordinates are rejected,
  off-canvas drawing is clipped, an empty crop is an error.

## 6. Frontends

**CLI (`pmark`)** **[done]**. Arguments are a small left-to-right program:
setting flags (`--color`, `--width`, `--size`, `--font`) change the current
state, edit flags (`--crop`, `--stroke`, `--text`) push ops. Useful for batch
work and it is what the tests drive.

**GUI (`pmark-gui`)** **[first pass]**. SDL2, one window, keyboard-selected
tools, state shown in the title bar.

- View: `screen = image * zoom + pan`; wheel zooms about the cursor.
- The canvas is mirrored in one streaming texture. While a stroke or text
  box is in progress, only the union of the old and new preview rectangles is
  recomputed: that region of the canvas is copied to a scratch image, the
  pending op is applied to it with `pm_op_apply`, and the region is uploaded.
  The canvas itself changes only on commit.
- The crop rectangle and text-box outline are drawn by the SDL renderer and
  never reach the canvas.
- `--selftest` feeds synthetic mouse and key events through the real event
  handlers and saves the result. With `SDL_VIDEODRIVER=dummy` this runs
  without a display.

## 7. Build and layout

```
include/pmark.h      public API
src/                 core (pm_image, pm_exif, pm_draw, pm_text, pm_doc, pm_stb)
cli/main.c           pmark
gui/main.c           pmark-gui
tests/               test_core.c, cli_smoke.sh, data/ fixtures
third_party/         vendored stb headers (public domain / MIT)
```

`make` builds `build/libpmark.a` and `build/pmark`. `make gui` adds
`build/pmark-gui` and needs SDL2 through `pkg-config`. `make test` runs the
unit tests and the CLI smoke test; `make asan` does the same under
AddressSanitizer and UBSan.

## 8. Known limitations

- Undo replays the whole log, and a live stroke preview re-rasterises the
  whole stroke on every mouse move. Both are fine for ordinary photos and
  short logs and will need work for very long sessions (see roadmap).
- Memory is about three copies of the image (base, canvas, texture): roughly
  150 MB for a 12-megapixel photo.
- The GUI needs the image to fit in one GPU texture (commonly 8192 or 16384
  pixels per side).
- Blending is done on sRGB values without linearising, which slightly darkens
  the edges of some colour pairs. Glyphs of translucent text can double-blend
  where they overlap.
- EXIF metadata is not carried over to the output; only its orientation is
  honoured. 16-bit PNGs are reduced to 8 bits.
- GUI: no on-screen toolbar or colour picker, no high-DPI scaling, no file
  dialog, and the caret is always at the end of the text.

## 9. Roadmap

1. Toolbar and colour/size controls drawn in the window; file open/save
   dialogs.
2. Incremental stroke preview (rasterise only the newest segment into a
   persistent mask) and undo checkpoints every N ops.
3. Stroke smoothing (Catmull-Rom or one-euro filter) and pressure from tablet
   events.
4. Text boxes that can be moved and re-edited before commit; font and style
   selection; an outline or background option for legibility on busy photos.
5. More shapes built on the stroke mask: arrow, rectangle, ellipse.
6. Rotate/straighten and aspect-locked crop.
7. A sidecar file that serialises the op log, so an edit session can be
   reopened.
8. Linear-light blending; tiled textures for very large images.
