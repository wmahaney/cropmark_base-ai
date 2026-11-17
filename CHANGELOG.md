# Changelog

cropmark is **pre-1.0**. Until 1.0, any release may change command-line
options, the `.pmark` session format and the C API without a migration
path, so a session saved by one 0.x version may not open in the next. The
images cropmark exports (PNG and JPEG) are the durable output.

## Unreleased

No change to the desktop program or its output.

- Web version: `cm_web_copy(path)` writes the picture as a PNG for the page to put on
  the clipboard, without counting as a save; `cm_web_status(msg)` shows a message from
  the page in the status bar. Both are inside `#ifdef __EMSCRIPTEN__`.

## Support for the web version (2026-11-16)

No change to the desktop program or its output.

- The editor can be compiled with Emscripten: a `web` section at the end of
  `gui/gui.c` (inside `#ifdef __EMSCRIPTEN__`) opens a picture handed over by the page,
  reports a save to the page so it becomes a download, and runs a loop that pauses
  when there is nothing to do.
- The toolbar's colours are one table, so the web version can match its page. The
  desktop colours are the same as before.
- `PM_MAX_PIXELS` can be lowered at build time (the web version uses 32 megapixels).
  The default stays 128.

## Repository renamed (2026-11-16)

- The repository `drawing_tool-claude` is now `cropmark_base-ai`: the desktop
  tool and the core library. The `-ai` suffix marks that the tool was built
  with Claude. Nothing in the program changed: the `cropmark` and `pmark`
  commands, `libpmark`, `pmark.h`, the `pm_` prefix, the `.pmark` session
  format and `PMARK_FONT` keep their names. Update a clone with
  `git remote set-url origin https://github.com/wmahaney/cropmark_base-ai.git`.

## 0.3.0 (2026-11-16)

- One command, `cropmark` (`pmark` for short), replaces the separate `pmark`
  and `pmark-gui` programs. On its own it opens a picture picker; with a
  photo it opens the editor; with edit options it works without a window.
- Picture picker: a folder's pictures and `.pmark` sessions as thumbnails
  (decoded in the background), folder navigation, type-to-filter, typed
  paths. Open button and `Ctrl+O` in the editor go back to it.
- Desktop launcher: `make install` adds cropmark to the applications menu
  (with an icon, and as an "Open With" choice for PNG and JPEG files);
  `make desktop-shortcut` also puts it on the desktop. It opens the picker
  in your Pictures folder (`--pictures`).
- `make` builds the window by default; `make headless` builds the
  command-line mode alone. `--version`.
- Fix: the core library now builds with clang.

## 0.2.0 (2026-11-15)

- Sessions (`.pmark`): reopen an edit and keep working on it.
- Atomic saves, protection of the original photo, colour profiles kept,
  other EXIF dropped; input size limits checked before decoding.
- Editor: toolbar, save and save-as flow, box and ellipse tools, HiDPI,
  incremental stroke preview.
- Golden-image tests and fuzzing of every parser.

## 0.1.0 (2026-11-15)

- Editing core: JPEG/PNG with EXIF orientation, anti-aliased strokes,
  arrows, text with outline, crop, undo and redo.
- Command-line frontend and a first SDL2 editor window.
