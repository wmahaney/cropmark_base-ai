# cropmark architecture

cropmark (formerly pmark) opens a photo (JPEG or PNG), lets you crop it, draw on it with markers
and shapes, and place text on it, then writes the result as PNG or JPEG. An
edit can also be saved as a session and reopened later. It is written in C11.

Status legend: **[done]** implemented and tested, **[first pass]** works but
is deliberately minimal, **[planned]** not started.

## 1. Goals and non-goals

Goals

- Load JPEG and PNG, save PNG and JPEG, keeping the colour profile. **[done]**
- Edits: crop, freehand marker and highlighter, arrow, box, ellipse, and
  text with an optional outline. **[done]**
- Undo and redo for every edit, fast on large photos. **[done]**
- Saving that cannot corrupt or silently destroy a file. **[done]**
- Sessions: reopen an edit and keep working on it. **[done]**
- Treat every input file as untrusted: bounded resources, fuzzed parsers. **[done]**
- One editing core shared by every frontend, so a scripted edit and an
  interactive edit produce the same pixels. **[done]**
- Build with a C compiler and `make`; the core has no dependencies outside
  the repository. **[done]**

Non-goals (for now): layers, selections, filters, colour *conversion*, RAW
formats, re-editing a committed edit, complex text shaping (right-to-left,
ligatures, emoji).

## 2. Layering

```
                    cli/main.c  (cropmark: chooses a frontend)
                   /                          \
  cli/batch.c  (command line)       gui/gui.c + gui/picker.c  (SDL2)
  argv -> ops -> save               picker, toolbar, input, view, previews
          \                        /
           v                      v
      include/pmark.h   -- the only header frontends include
  +---------------------------------------------------------------+
  | pm_doc      document = base + op log, checkpointed undo/redo  |
  | pm_session  .pmark files: save, parse, load                   |
  | pm_draw     stroke rasteriser (incremental), shapes           |
  | pm_text     text layout, outline                              |
  | pm_image    buffers, crop, decode/encode     pm_exif  orient  |
  | pm_icc      colour profiles                  pm_file  atomic  |
  +---------------------------------------------------------------+
  third_party/  stb_image, stb_image_write, stb_truetype
                (instantiated once, in src/pm_stb.c)
```

Rules that keep the layers honest:

1. The core never includes SDL, never prints, and holds no global state.
2. Frontends never write pixels into the canvas. Committed edits go through
   `pm_doc_*`. Previews use the same rasterisers: `pm_op_apply` on a scratch
   copy, or the incremental stroke builder (section 4), which is tested to be
   bit-identical to the committed result.
3. stb is an implementation detail. No stb type or function appears outside
   `pm_stb.c`, `pm_image.c` and `pm_text.c`, so the codecs or the font
   rasteriser can be swapped (libjpeg-turbo, libspng, FreeType) without
   touching a frontend.

## 3. Data model

**Image** (`pm_image`): `w`, `h`, and one owned buffer of RGBA8 pixels,
straight (non-premultiplied) alpha, row-major, stride `4*w`, origin top-left.

**Metadata** (`pm_meta`): the ICC profile of the input, carried to the output
(section 6).

**Coordinates**: continuous image coordinates, in which pixel `(i, j)` covers
`[i, i+1) x [j, j+1)` and has its centre at `(i+0.5, j+0.5)`. Strokes and
text positions are floats; crop rectangles are integer pixel rectangles.

**Operation** (`pm_op`): a tagged union, one of

| kind   | payload                                                 |
|--------|---------------------------------------------------------|
| crop   | integer rect                                            |
| stroke | point list, width, colour (alpha allowed)               |
| text   | UTF-8 string, position, style (size, colour, outline width and colour), font |

Arrows, boxes and ellipses are not op kinds: `pm_shape_*` turn them into
point lists that are drawn as one stroke. That keeps the op set, the session
format and the renderer small, and drawing a shape as a single stroke means a
translucent box does not darken at its corners.

**Document** (`pm_doc`):

```
base         the decoded file, never modified (plus its path, hash and ICC profile)
ops[]        the op log
             ops[0 .. n_applied)      applied
             ops[n_applied .. n_ops)  redo tail
canvas       cache: base with ops[0 .. n_applied) applied
checkpoints  canvas snapshots at some op counts (see below)
fonts        fonts the document owns (from sessions and the CLI)
```

An op's coordinates are relative to the canvas at the moment it was pushed,
that is, after all earlier crops. Replaying the log from `base` is therefore
deterministic and needs no coordinate fix-ups.

- **push**: apply the op to `canvas`; on success drop the redo tail (and any
  checkpoints inside it) and append. A failed push leaves the log and the
  canvas unchanged.
- **undo**: replay from the newest checkpoint at or before the target state
  (or from `base`), then swap the result in.
- **redo**: apply `ops[n_applied]` to `canvas`.

**Checkpoints.** After every `PM_CHECKPOINT_EVERY` (8) ops a snapshot of the
canvas is taken, provided it fits in `cp_budget` (256 MB by default, about 5
snapshots of a 12 MP photo). Over budget, the oldest snapshot is evicted
first, because recent undos are the common case and the base image is always
there as a fallback. Undo therefore replays at most 7 ops in the common case.
Measured on a 4000x3000 photo with 100 strokes: 401 ms per undo without
checkpoints, 39 ms with them.

Because `base` is kept, crop is non-destructive: undoing a crop brings the
cropped-away pixels back.

## 4. Rendering

**Strokes** (`pm_draw.c`). A stroke is a polyline with round caps and joins:
the union of capsules of radius `width/2` around each segment. Coverage of a
pixel by one capsule is `clamp(r + 0.5 - dist(pixel centre, segment), 0, 1)`.

The rasteriser is incremental (`pm_stroke_begin / add / composite / end`):

1. An 8-bit coverage mask covers an area (the stroke's bounds for a one-shot
   draw, the whole canvas for live drawing).
2. Each added point rasterises one new segment, visiting only that segment's
   bounding box, and stores `max(old, new)` coverage. It returns that box as
   the dirty rectangle.
3. Compositing blends the colour through the mask over a rectangle.

Taking the max and compositing once means a translucent stroke that crosses
itself does not darken at the crossing (a real highlighter), and the final
mask is independent of the order and grouping of segments. So the GUI's live
preview, which recomposites only each new segment's box, produces exactly
the pixels `pm_draw_stroke` produces at commit. A unit test checks this.
Measured on a 12 MP canvas: 0.006 ms per mouse move, against 6.1 ms when the
whole stroke was re-rasterised per move at 2,000 points.

**Text** (`pm_text.c`). UTF-8 is decoded to code points (malformed bytes
become U+FFFD). Each glyph is rasterised by stb_truetype at a sub-pixel x
offset, with pair kerning, and max-accumulated into one coverage mask for the
whole text, which is then composited once. This means translucent text does
not darken where glyph boxes overlap. With an outline, the mask is dilated by
an anti-aliased disk of the outline radius; the outline is composited first
and the fill on top. One layout routine serves drawing, measuring and bounds,
so they cannot disagree.

**Blending**: straight-alpha source-over in 8-bit integer arithmetic, on the
stored values (no linearisation; see limitations).

**EXIF orientation** (`pm_exif.c`). The loader reads tag 0x0112 and bakes the
rotation or flip into the pixels, so everything downstream sees upright
images.

## 5. Untrusted input

Every byte that enters cropmark from a file is treated as hostile.

| Limit | Value | Where it is enforced |
|---|---|---|
| Input file size | 1 GB | `pm_read_file`, while reading |
| Dimensions | 32768 per side | stb (`STBI_MAX_DIMENSIONS`) and `pm_image_decode` |
| Total pixels | 128 MP (512 MB per RGBA copy) | `pm_image_decode`, from the **header**, before any pixel is decoded |
| ICC profile | 4 MB | `pm_icc_extract` (larger profiles are dropped) |
| Stroke width | 4096 px | `pm_draw_stroke`, `pm_stroke_begin` |
| Text size / outline | 4096 px / 64 px | `pm_text_draw` |
| Session stroke points | 2^24 per stroke | `pm_session_parse` |

The pixel limit is checked with `stbi_info` on the header, so a 40000x40000
"image" a few hundred bytes long is rejected without allocating anything.
When stb itself refuses an oversized header, cropmark reads the PNG IHDR or JPEG
SOF dimensions to report `PM_ERR_TOO_BIG` rather than a generic format error.

**Fuzzing.** `tests/fuzz.c` is a libFuzzer target covering every parser that
sees file bytes: the image decoders, EXIF, ICC extraction, and the session
parser. `make fuzz-libfuzzer` runs it coverage-guided (needs clang).
`make fuzz-smoke` runs a built-in mutation driver under ASan and UBSan with
gcc; 30,000 mutated inputs from the fixture images, ICC-tagged images and a
session file run clean.

## 6. Persistence

### 6.1 Writing image files

`pm_image_save_ex(img, path, opts)` with `opts = {jpeg_quality, meta, overwrite}`:

- **Format by extension**: `.png`, `.jpg`, `.jpeg` (case-insensitive).
  Anything else is `PM_ERR_FORMAT`, checked before any work is done.
- **Atomic replace.** The encoded file is written to a temporary file in the
  same directory (`mkstemp`), `fsync`ed, then `rename`d over the target, and
  the directory is `fsync`ed. A crash, full disk or encoder failure leaves
  either the old file or the complete new one, never a truncated file. The
  temporary file is removed on every failure path, and it gets normal
  `0666 & ~umask` permissions rather than `mkstemp`'s 0600.
- **No-clobber.** With `overwrite = false`, the final step is `link()`
  instead of `rename()`, which fails atomically with `EEXIST` if the target
  appeared in the meantime. On file systems without hard links it falls back
  to check-then-rename (a small race window, documented).
- **PNG**: fully opaque images (every photo) are written as RGB rather than
  RGBA, which is about 25% smaller. Images with transparency stay RGBA.
- **JPEG**: alpha is flattened onto white; quality defaults to 92 (stb uses
  4:4:4 chroma above quality 90, 4:2:0 at or below).
- **Windows**: the same contract via `MoveFileExA` with
  `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. This path compiles
  but is untested.

### 6.2 Metadata policy

| Metadata | Policy | Why |
|---|---|---|
| ICC colour profile | **Kept** (JPEG APP2 chunks, PNG `iCCP`) | Phones tag photos Display P3. Without the profile, output looks washed out. |
| EXIF orientation | **Applied, then dropped** | Pixels are already upright; keeping the tag would rotate them twice. |
| Other EXIF (time, camera, GPS) | **Dropped** | Privacy (GPS location), and stale fields (thumbnail, dimensions) would contradict the edited image. A future opt-in could copy the date. |
| Bit depth | 16-bit PNG becomes 8-bit | The core is RGBA8. |

The CLI's `--strip-icc` drops the profile too, for maximum privacy or when a
consumer mishandles profiles.

### 6.3 Overwrite rules for frontends

- **CLI**: `-o` replaces an existing output (normal CLI behaviour), except
  that it refuses to replace the input photo itself, detected by device and
  inode so hard links and `./` paths are caught, unless `--force` is given.
  `--no-clobber` refuses any existing file.
- **GUI**: the first save to a path that exists asks for confirmation in the
  status bar (Enter confirms, Esc cancels). Saving over the original photo
  gets a stronger message. Once the window has written a path, later saves
  to it don't ask again. Save As (Ctrl+Shift+S) changes the file *name*
  only: the prompt shows the current path without its extension, the
  output's extension is fixed and shown after it, and a typed image
  extension is replaced rather than doubled (`notes.jpg` saves as
  `notes.png` when the output is PNG). The output format is chosen once, by
  the OUTPUT argument. A new name resets the ask-before-replacing
  permission.

### 6.4 Session files

A session (`.pmark`) records the base image and the applied ops, so an edit
can be reopened, continued and re-exported. It is line-based UTF-8 text:

```
pmark-session 1
base "photo.jpg" 9b2928b9eaa7cf92
crop 30 20 420 280
stroke ffeb3b80 26 2 40 215 250 215
text ffffffff 34 40 196 3 000000ff "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" "Look\nhere"
```

- Line 1 is the magic and version. Blank lines and `#` comments are allowed.
- `base PATH HASH`: PATH is relative to the session file's directory when
  the photo is at or below it (so a folder can be moved as a unit), otherwise
  absolute. HASH is FNV-1a 64 of the photo file's bytes, used to detect that
  the photo changed (it is change detection, not security).
- `crop X Y W H`, `stroke RRGGBBAA WIDTH N x1 y1 ... xN yN`,
  `text RRGGBBAA SIZE X Y OUTLINE_PX OUTLINE_RRGGBBAA "FONT" "TEXT"`.
- Floats are written with `%.9g`, which round-trips IEEE single precision
  exactly, so a reopened session reproduces the canvas bit for bit (tested).
- Strings are double-quoted with `\" \\ \n \t \r \xHH` escapes; NUL is
  rejected.
- Only applied ops are saved; the redo tail is not.

On load, a changed photo gives `PM_ERR_STALE`, and a missing font gives
`PM_ERR_FONT`. With `PM_SESSION_LENIENT` (CLI `--lenient`; the GUI retries
leniently and shows a warning) the session opens anyway, substituting the
default font.

Why a line format rather than JSON: it needs no parser dependency, it diffs
well, and a hand-written strict parser for it is small enough to fuzz
thoroughly. The version number in line 1 leaves room to move.

The editor writes `OUTPUT.pmark` next to every saved image (unless
`--no-session`), and can open a `.pmark` directly. It does not write a
session when the output replaced its own base photo, since that session
would describe edits on top of an image that no longer exists.

### 6.5 Status codes

`PM_ERR_IO`, `PM_ERR_FORMAT`, `PM_ERR_NOMEM`, `PM_ERR_ARG`, `PM_ERR_FONT`,
`PM_ERR_TOO_BIG` (limits in section 5), `PM_ERR_EXISTS` (no-clobber
refused), `PM_ERR_STALE` (session base changed). `pm_strerror` maps each to
a message.

## 7. Errors and ownership

- Every fallible function returns `pm_status` (`PM_OK == 0`). The core does
  not print or abort.
- Out parameters are written only on success.
- Whoever creates an object frees it: `pm_image_free`, `pm_meta_free`,
  `pm_font_free`, `pm_stroke_end`, `pm_doc_close`.
- `pm_doc_*` push functions copy their inputs. `pm_doc_from_image` takes
  ownership of the image. `pm_doc_own_font` transfers a font to the document
  (freed on close, and freed immediately if the call fails). Fonts not handed
  over are borrowed and must outlive the document.

## 8. Frontends

**One command.** Both frontends are linked into one program, `cropmark`.
`cli/main.c` picks one from the arguments: any option other than the
window's own (`--quality`, `--no-session`) means the command line, so every
command that worked with the old `pmark` CLI behaves the same. Otherwise
the window opens: on the picker with no argument or a folder, on the editor
with a photo or session. `make headless` builds the same program without
the window (`-DCM_NO_GUI`) for machines without SDL2.

**Command line (`cli/batch.c`)** **[done]**. Arguments are a small left-to-right program:
setting flags (`--color`, `--width`, `--size`, `--outline`,
`--outline-color`, `--font`) change the current state; edit flags (`--crop`,
`--stroke`, `--arrow`, `--rect`, `--ellipse`, `--text`) push ops. INPUT may
be a photo or a `.pmark` session. Output flags are covered in section 6.

**Window (`gui/`)** **[first pass]**. SDL2, one window shared by the picker
and the editor.

- *Picker* (`gui/picker.c`): lists a folder's subfolders and `.jpg`,
  `.jpeg`, `.png` and `.pmark` files (hidden files skipped, folders first,
  case-insensitive order) as a grid of tiles. It owns no window: the app
  feeds it events and asks it to draw, and it reports `PICK_OPEN` or
  `PICK_CANCEL`. Typing filters by name; a typed path opens on Enter.
- *Thumbnails*: one worker thread decodes and area-averages them, newest
  folder only (a generation counter discards results from a folder left
  behind). The main thread marks the tiles on screen, plus one row each
  way, as wanted each frame, so the worker never decodes pictures nobody is
  looking at; it uploads finished thumbnails as textures and drops
  off-screen ones beyond 240. A session's thumbnail is its rendered canvas.
  All shared state is behind one mutex; drawing reads only main-thread
  fields (checked with ThreadSanitizer).
- *Switching*: Ctrl+O or the Open button shows the picker in the photo's
  folder. With unsaved edits the first request only warns, and the old
  document is kept until a new one opens successfully, so Esc from the
  picker returns to the edit intact and an unreadable file changes nothing.
  The UI font belongs to the app, not the document, since documents come
  and go.
- *Toolbar*: Open, tools, eight colour swatches, size −/+, outline toggle,
  undo, redo, save and save as, all drawn with the core's own text renderer
  into a texture. Every button has a keyboard shortcut.
- *Status bar*: file, size, zoom, output path, transient messages, and the
  Save As and confirmation prompts.
- *View*: `screen = image * zoom + pan`, in renderer pixels. The window is
  created with `SDL_WINDOW_ALLOW_HIGHDPI`, and mouse coordinates are scaled
  by `renderer pixels / window coordinates`, so drawing is sharp and accurate
  on Retina and other HiDPI displays.
- *Previews*: marker and highlighter strokes use the incremental builder
  (section 4); shapes and text use `pm_op_apply` on a scratch copy of the
  affected region. The crop rectangle and text-box outline are drawn by SDL
  and never reach the canvas.
- *Text*: click to place, type, then drag the box to move it before
  committing with Enter.
- *Selftest*: `--selftest OUT` drives the real event handlers through a
  scripted session (toolbar clicks, every tool, undo and redo of a crop, text
  move, save with confirmation, session round trip, Save As (empty name
  refused, typed extension replaced), refusal to replace the original) and checks the end state.
  `--selftest-picker`, run as a naked call in a prepared folder, checks the
  listing, thumbnails, filtering, folder navigation, opening, the unsaved
  edits guard, an unreadable file and a typed path. `tests/gui_selftest.sh`
  runs both headless with `SDL_VIDEODRIVER=dummy`, as part of `make test`.
  `CROPMARK_SELFTEST_SHOT=file.png` captures the final frame;
  `CROPMARK_PICKER_SHOT=file.png` with `--selftest-picker` only screenshots
  the picker of the current folder (used for `docs/images/picker.png`).

**Why still SDL2.** SDL3 (stable since 2025) offers native file dialogs
(`SDL_ShowSaveFileDialog`), which would replace the typed Save As prompt.
Migrating is mechanical but touches every event handler, and SDL3 is not yet
packaged on all current LTS distributions (Ubuntu 24.04 ships only SDL2), so
it is roadmap item 1 rather than part of this change.

## 9. Testing

| Target | What it covers |
|---|---|
| `make test` | 142 unit checks (`tests/test_core.c`); command-line end-to-end, including overwrite rules, stale sessions, cleanup of temporary files and the choice between window and command line (`tests/cli_smoke.sh`); golden images (`tests/golden.sh`); headless editor and picker selftests (`tests/gui_selftest.sh`) |
| `make test-headless` | the tests that need no SDL2, against `build/cropmark-headless` |
| `make asan` | `make test` under AddressSanitizer (with leak detection) and UBSan |
| `make fuzz-smoke` | mutation fuzzing of all parsers under ASan and UBSan |
| `make golden-update` | regenerate references after an intended rendering change (review the diff) |

Golden images compare per channel with a tolerance of 2, since the
rasterisers use floats and compilers may differ in the last bit. The text
case uses an explicit DejaVu Sans path and is skipped when that font is
absent, because font versions change glyph shapes.

## 10. Build and layout

```
include/pmark.h      public API
src/                 core
cli/main.c           cropmark: chooses the window or the command line
cli/batch.c          the command line
gui/gui.c            the window and the editor
gui/picker.c         the picture picker
tests/               test_core.c, cli_smoke.sh, golden.sh, gui_selftest.sh, imgdiff.c, fuzz.c,
                     data/ (fixtures), golden/ (references), fuzz_seeds/
third_party/         vendored stb headers (public domain / MIT)
docs/                this document
```

`make` builds `build/libpmark.a` and `build/cropmark` (needs SDL2 via
`pkg-config`); `make install` copies it to `$(PREFIX)/bin`, `~/.local/bin`
by default. `make headless` builds `build/cropmark-headless` without SDL2.
The core keeps its `pm_` prefix and the `.pmark` session format.

## 11. Known limitations

- Memory is about three copies of the image (base, canvas, texture) plus
  checkpoints up to the 256 MB budget: under 500 MB for a 12 MP photo.
- The picker lists folders with POSIX `opendir`/`realpath`, so the window
  builds on Linux and macOS only; the command line has no such limit.
- The editor needs the image to fit in one GPU texture (commonly 8192 or 16384
  pixels per side) and holds a canvas-sized 8-bit mask while a stroke is
  being drawn.
- Blending is done on stored values without linearising, which slightly
  darkens the edges of some colour pairs. Colour profiles are carried, not
  applied: drawing colours are interpreted in the image's own colour space.
- Session files depend on the font path. On another machine, `--lenient`
  substitutes the default font, which changes text metrics.
- Session base paths outside the session's directory are absolute, so moving
  only the session breaks the link.
- GUI: Save As is a typed name rather than a native dialog and cannot
  change the format (by design for now); the caret is
  always at the end of the text; committed edits cannot be selected or moved.
- Windows file-system code compiles but has not been run.

## 12. Roadmap

1. SDL3 migration: native open and save dialogs, and better HiDPI handling.
2. Editable objects: select, move and restyle committed shapes and text.
   This means replacing ops in the log, not just appending to it.
3. Stroke smoothing (Catmull-Rom or a one-euro filter) and tablet pressure.
4. Rotate, straighten and aspect-locked crop.
5. Autosave of the session file and crash recovery.
6. Opt-in EXIF date copying.
7. Linear-light blending, and colour conversion when exporting to sRGB.
8. Tiled textures for images larger than the GPU texture limit.
