# drawing_tool-claude

`pmark`: crop photos, draw on them with markers, and add text. C11.

- `pmark` is a command-line tool (no dependencies).
- `pmark-gui` is an interactive SDL2 window (first pass).

Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build

```sh
make            # build/pmark
make gui        # build/pmark-gui   (needs SDL2: apt install libsdl2-dev / brew install sdl2)
make test       # unit tests + CLI smoke test
make asan       # the same under ASan + UBSan
```

## Command line

Edits run left to right; coordinates are pixels in the image as it is at
that point.

```sh
build/pmark photo.jpg \
    --crop 100,50,1600,1200 \
    --color ffeb3b80 --width 40 --stroke "200,900 1200,900" \
    --color e53935 --width 10 --stroke "300,200 500,180 520,400" \
    --color ffffff --size 64 --text 220,860 "Look here" \
    -o marked.png
```

`build/pmark --help` lists everything. Text needs a TrueType font: pass
`--font FILE.ttf`, set `PMARK_FONT`, or rely on the built-in search of
common system locations.

## GUI

```sh
build/pmark-gui photo.jpg [output.png]    # output defaults to photo-marked.png
```

| Key / mouse                    | Action                                   |
|--------------------------------|------------------------------------------|
| `M` / `H`                      | marker / highlighter; drag to draw       |
| `C`                            | crop: drag a box, `Enter` to apply       |
| `T`                            | text: click, type, `Enter` (`Shift+Enter` = new line) |
| `1`-`8`                        | red, orange, yellow, green, blue, purple, black, white |
| `[` / `]`                      | smaller / larger marker or text          |
| wheel, middle-drag, space-drag | zoom, pan; `0` fits the window           |
| `Ctrl+Z`, `Ctrl+Y`             | undo, redo                               |
| `Ctrl+S`                       | save                                     |
| `Esc`                          | cancel the edit in progress              |

The current tool, colour, size and zoom are shown in the title bar.
