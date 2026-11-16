# pmark

pmark marks up photos. It crops them, draws on them with markers,
highlighters, arrows, boxes and ellipses, and adds text. It is a small C11
program with two front ends, both built on the same editing core:

- **`pmark`**, a command-line tool for scripted and batch edits. It has no
  dependencies beyond a C compiler.
- **`pmark-gui`**, an interactive window with a toolbar. It needs SDL2.

![Example output: a cropped photo with a highlighter stroke, a circled sun, an arrow and outlined text](docs/images/example.png)

## Features

- **Input:** JPEG and PNG. Phone photos stored sideways are rotated upright
  automatically using their EXIF orientation.
- **Edits:** crop, freehand marker, highlighter, arrow, box, ellipse, and
  text with an optional outline. Any colour can be translucent, and a
  highlighter stroke that crosses itself does not darken where it overlaps.
- **Undo and redo** for every edit, including crops: undoing a crop brings
  the cropped-away pixels back.
- **Safe saving:** files are written atomically, so a failed save never
  leaves a half-written file. pmark never silently replaces the photo you
  opened.
- **Colour profiles are kept**, so photos from phones (often Display P3)
  don't come out washed out. Other EXIF data, including GPS location, is
  dropped.
- **Sessions:** a `.pmark` file records the photo and every edit, so you can
  reopen an edit later and keep working on it.
- **Hardened input:** oversized images are rejected from their header alone
  before any pixels are decoded, and every file parser is fuzz-tested.

## Building

You need a C11 compiler and `make`. The GUI additionally needs SDL2.

| Platform | Install the GUI dependency |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential libsdl2-dev` |
| Fedora | `sudo dnf install gcc make SDL2-devel` |
| Arch | `sudo pacman -S base-devel sdl2` |
| macOS (Homebrew) | `xcode-select --install && brew install sdl2 pkg-config` |

```sh
make            # builds build/pmark (command line)
make gui        # builds build/pmark-gui (needs SDL2)
```

The image codecs and font rasteriser (stb) are vendored in `third_party/`,
so nothing else needs installing. Windows is not yet supported: the code is
written with Windows in mind, but it has not been built or run there.

## Quick start

Annotate a photo from the command line:

```sh
build/pmark photo.jpg \
    --crop 100,50,1600,1200 \
    --color ffeb3b80 --width 40 --stroke "200,900 1200,900" \
    --color e53935 --width 10 --arrow 300,200,520,400 \
    --color ffffff --size 64 --outline 4 --text 220,860 "Look here" \
    -o marked.jpg
```

Or open it in the editor:

```sh
build/pmark-gui photo.jpg
```

Press `Ctrl+S` to save. The result goes to `photo-marked.png`, alongside a
`photo-marked.pmark` session you can reopen later.

## Command-line reference

```
pmark INPUT [edits and settings...] [-o OUTPUT] [--save-session FILE.pmark]
```

`INPUT` is a `.jpg`/`.jpeg`/`.png` photo or a `.pmark` session. Arguments
run **left to right**, like a small script. Settings apply to the edits that
come after them, and each edit's coordinates are pixels in the image *as it
is at that point*, so after a `--crop`, `0,0` is the crop's top-left corner.

**Edits**

| Flag | Draws |
|---|---|
| `--crop X,Y,W,H` | keeps only this rectangle |
| `--stroke "X,Y X,Y ..."` | a freehand line through the points (one point draws a dot) |
| `--arrow X0,Y0,X1,Y1` | an arrow pointing at `X1,Y1` |
| `--rect X,Y,W,H` | a rectangle outline |
| `--ellipse X,Y,W,H` | an ellipse outline inside the rectangle |
| `--text X,Y "STRING"` | text whose top-left corner is at `X,Y` (`\n` starts a new line) |

**Settings**

| Flag | Default | Meaning |
|---|---|---|
| `--color RRGGBB[AA]` | `e53935` (red) | colour for lines and text; an alpha below `ff` makes it translucent |
| `--width PX` | `8` | line width |
| `--size PX` | `48` | text height |
| `--outline PX` | `0` (off) | width of the outline around text |
| `--outline-color HEX` | `000000` | outline colour |
| `--font FILE.ttf` | see [Fonts](#fonts) | TrueType font for text |

**Output**

| Flag | Meaning |
|---|---|
| `-o FILE` | write `.png`, `.jpg` or `.jpeg` (format chosen by the extension) |
| `--quality N` | JPEG quality, 1 to 100 (default 92) |
| `--no-clobber` | fail rather than replace an existing file |
| `--force` | allow `-o` to replace the input photo (refused otherwise) |
| `--strip-icc` | don't embed the colour profile in the output |
| `--save-session FILE` | also write a `.pmark` session |
| `--lenient` | open a session even if its photo changed or its font is missing |
| `--info` | print the final image size, e.g. `1600x1200` |

Exit status is 0 on success, 1 if an edit or file operation failed, and 2
for a usage error. Run `build/pmark --help` for the same reference.

**More examples**

```sh
# A translucent yellow highlight and a green box, saved as PNG
build/pmark scan.png --color fdd83580 --width 30 --stroke "120,400 900,400" \
    --color 43a047 --width 6 --rect 100,360,820,80 -o scan-marked.png

# Several lines of text with a contrasting outline
build/pmark photo.jpg --color ffffff --size 56 --outline 3 \
    --text 40,40 "Before the repair\nNorth wall, 3 May" -o labelled.jpg

# Just crop
build/pmark photo.jpg --crop 0,0,1200,800 -o cropped.jpg
```

## GUI guide

```sh
build/pmark-gui INPUT [OUTPUT] [--quality N] [--no-session]
```

![The pmark editor: toolbar along the top, the photo with a marker, highlighter, arrow, circle and text, and the status bar along the bottom](docs/images/gui.png)

The toolbar along the top holds the tools, eight colours, size controls, an
outline toggle, undo, redo, Save and Save as. Everything also has a
keyboard shortcut:

| Key / mouse | Action |
|---|---|
| `M` | marker: drag to draw |
| `H` | highlighter: a wide translucent marker |
| `A` / `B` / `E` | arrow / box / ellipse: drag from corner to corner (or tail to tip) |
| `C` | crop: drag a box, then `Enter` to apply or `Esc` to cancel |
| `T` | text: click to place, type, drag the box to move it, `Enter` to finish (`Shift+Enter` for a new line) |
| `O` | text outline on or off |
| `1` to `8` | red, orange, yellow, green, blue, purple, black, white |
| `[` / `]` | smaller / larger line or text |
| mouse wheel | zoom around the cursor |
| middle-drag, or `Space` + drag | pan |
| `0` | fit the photo to the window |
| `Ctrl+Z` / `Ctrl+Y` (or `Ctrl+Shift+Z`) | undo / redo |
| `Ctrl+S` | save |
| `Ctrl+Shift+S` | save as |
| `Esc` | cancel whatever is in progress |

The status bar along the bottom shows the file, image size, zoom and where
the output will be saved, along with messages and prompts.

### Saving in the GUI

- **Where it saves.** `OUTPUT` if you gave one, otherwise `photo-marked.png`
  next to the photo. For a `.pmark` session, the default is the session's
  name with `.png`.
- **Asking first.** The first time you save over a file that already exists,
  the status bar asks for confirmation: `Enter` replaces the file, `Esc`
  cancels. Saving over the original photo gets a stronger warning. Once the
  editor has written a file, later saves to it don't ask again.
- **Save as** changes the file name only. The prompt shows the current name
  without its extension, and the extension (`.png` or `.jpg`) stays the
  same. If you type an extension anyway, pmark replaces it rather than
  doubling it. To save in a different format, start the editor with an
  `OUTPUT` of that type, e.g. `pmark-gui photo.jpg out.jpg`.
- **Sessions.** Every save also writes `NAME.pmark` next to the image
  (unless you pass `--no-session`).
- **Closing.** If you close the window with unsaved changes, the status bar
  warns you; close again to discard them.

## Sessions

A session is a small text file listing the photo and every edit:

```
pmark-session 1
base "photo.jpg" 9b2928b9eaa7cf92
crop 30 20 420 280
stroke ffeb3b80 26 2 40 215 250 215
text ffffffff 34 40 196 3 000000ff "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" "Look\nhere"
```

Open one with either front end to continue where you left off. Undo keeps
working through the reopened edits.

```sh
build/pmark photo.jpg --crop 0,0,800,600 -o out.png --save-session edit.pmark
build/pmark edit.pmark --arrow 50,50,200,200 -o out-v2.png    # continue
build/pmark-gui edit.pmark                                    # or interactively
```

- The photo is stored **relative to the session file** when it's in the same
  folder (or below), so you can move the folder as a unit.
- The session remembers a fingerprint of the photo. If the photo has
  changed since, pmark refuses to open the session rather than silently
  drawing on the wrong image. `--lenient` (or the GUI, which warns) opens it
  anyway.
- Fonts are recorded by path. On another computer, use `--lenient` to fall
  back to the default font; text may then be laid out slightly differently.

## How files are handled

- **Atomic saves.** The output is written to a temporary file in the same
  folder, synced to disk, then renamed into place. If anything goes wrong,
  you keep the previous file, and the temporary file is removed.
- **The original is protected.** The command line refuses `-o` pointing at
  the input photo (even through a hard link) unless you pass `--force`. The
  GUI asks first.
- **Metadata:**

  | | What pmark does |
  |---|---|
  | Colour profile (ICC) | kept, unless `--strip-icc` |
  | Orientation | applied to the pixels, then removed |
  | Other EXIF (camera, date, GPS) | removed |

- **Formats.** PNG output from an opaque photo is saved without an alpha
  channel, which makes it smaller. JPEG has no transparency, so transparent
  areas become white.
- **Limits.** Images up to 32768 pixels per side and 128 megapixels. Bigger
  or malformed files are refused with an error rather than exhausting
  memory.

## Fonts

Text needs a TrueType font. pmark looks, in order, at `--font FILE.ttf` on
the command line, the `PMARK_FONT` environment variable, and common system
locations (DejaVu Sans, Liberation Sans or Noto Sans on Linux, Arial on
macOS and Windows). If none is found, text is unavailable but everything
else works.

```sh
export PMARK_FONT=/path/to/MyFont.ttf
```

## Testing

```sh
make test         # unit tests, command-line end-to-end tests, golden images
make test-gui     # scripted GUI session, headless (no display needed)
make asan         # all of the above under AddressSanitizer + UBSan
make fuzz-smoke   # mutation-fuzz the image, EXIF, ICC and session parsers
```

`make golden-update` regenerates the reference images in `tests/golden/`
after an intended change to rendering; review the image diff before
committing. With clang, `make fuzz-libfuzzer` runs coverage-guided fuzzing.

## Project layout

```
include/pmark.h     public API of the editing core
src/                the core: images, drawing, text, documents, sessions, files
cli/main.c          pmark (command line)
gui/main.c          pmark-gui (SDL2)
tests/              unit, end-to-end, golden-image and fuzz tests, fixtures
third_party/        vendored stb headers
docs/               ARCHITECTURE.md and README images
```

The core is a library (`build/libpmark.a`) with no dependency on SDL, so
another front end can be built on it. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
covers the design: data model, rendering, the session format, input limits,
the persistence rules and the roadmap.

## Known limitations

- Committed edits can't be selected or moved; use undo instead.
- The GUI's Save As takes a typed name, not a file dialog, and keeps the
  output format.
- Very large images must fit in one GPU texture to be shown in the GUI
  (commonly 8192 or 16384 pixels per side). The command line has no such
  limit.
- Text is laid out left to right with simple kerning: no right-to-left
  scripts, ligatures or colour emoji.

## License

pmark's own license has not been chosen yet. The vendored stb libraries in
`third_party/` are public domain or MIT, at your option.
