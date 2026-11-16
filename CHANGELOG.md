# Changelog

cropmark is **pre-1.0**. Until 1.0, any release may change command-line
options, the `.pmark` session format and the C API without a migration
path, so a session saved by one 0.x version may not open in the next. The
images cropmark exports (PNG and JPEG) are the durable output.

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
