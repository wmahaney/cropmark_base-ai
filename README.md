# drawing_tool-claude

`pmark`: crop photos, draw on them with markers and shapes, and add text. C11.

- `pmark` is a command-line tool (no dependencies).
- `pmark-gui` is an interactive SDL2 window with a toolbar (first pass).

Saves are atomic, keep the photo's colour profile, and never silently replace
the original. Edits can be saved as `.pmark` sessions and reopened.
Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build

```sh
make            # build/pmark
make gui        # build/pmark-gui   (needs SDL2: apt install libsdl2-dev / brew install sdl2)
make test       # unit tests, CLI end-to-end, golden images
make test-gui   # headless GUI selftest
make asan       # everything under ASan + UBSan
make fuzz-smoke # mutation-fuzz the file parsers
```

## Command line

Edits run left to right; coordinates are pixels in the image as it is at
that point.

```sh
build/pmark photo.jpg \
    --crop 100,50,1600,1200 \
    --color ffeb3b80 --width 40 --stroke "200,900 1200,900" \
    --color e53935 --width 10 --arrow 300,200,520,400 --ellipse 500,350,300,200 \
    --color ffffff --size 64 --outline 4 --text 220,860 "Look here" \
    -o marked.jpg --save-session marked.pmark

# later: reopen the session, add to it, export again
build/pmark marked.pmark --rect 40,40,300,200 -o marked-v2.jpg
```

| Output flag | Meaning |
|---|---|
| `-o FILE` | `.png` / `.jpg` / `.jpeg`, written atomically |
| `--quality N` | JPEG quality 1-100 (default 92) |
| `--no-clobber` | fail rather than replace an existing file |
| `--force` | allow `-o` to be the input photo itself (refused by default) |
| `--strip-icc` | do not embed the colour profile |
| `--save-session FILE` | write a `.pmark` session |
| `--lenient` | open a session whose photo changed or whose font is missing |

`build/pmark --help` lists everything. Text needs a TrueType font: pass
`--font FILE.ttf`, set `PMARK_FONT`, or rely on the built-in search of
common system locations.

## GUI

```sh
build/pmark-gui photo.jpg [output.png] [--quality N] [--no-session]
build/pmark-gui photo-marked.pmark          # continue a saved edit
```

The output defaults to `photo-marked.png`. Saving writes the image and a
`photo-marked.pmark` session next to it. The first save over an existing
file asks first (Enter replaces it, Esc cancels).

| Key / mouse                    | Action                                   |
|--------------------------------|------------------------------------------|
| `M` `H` `A` `B` `E`            | marker, highlighter, arrow, box, ellipse |
| `C`                            | crop: drag a box, `Enter` to apply       |
| `T`                            | text: click, type, drag the box to move it, `Enter` (`Shift+Enter` = new line) |
| `O`                            | text outline on / off                    |
| `1`-`8`                        | red, orange, yellow, green, blue, purple, black, white |
| `[` / `]`                      | smaller / larger line or text            |
| wheel, middle-drag, space-drag | zoom, pan; `0` fits the window           |
| `Ctrl+Z`, `Ctrl+Y`             | undo, redo                               |
| `Ctrl+S`, `Ctrl+Shift+S`       | save, save as                            |
| `Esc`                          | cancel the edit or prompt in progress    |

Everything is also on the toolbar.
