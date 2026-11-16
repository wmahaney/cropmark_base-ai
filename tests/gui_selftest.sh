#!/bin/sh
# Drives the window with synthetic input; no display needed (SDL's dummy
# video driver). Covers the editor and a naked `cropmark` (the picker).
set -e
CROPMARK=${1:-build/cropmark}
case $CROPMARK in /*) ;; *) CROPMARK=$PWD/$CROPMARK ;; esac
D=$(cd "$(dirname "$0")/data" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export SDL_VIDEODRIVER=dummy

# editor: the output already exists, so the first save must ask
cp "$D/sample.png" "$T/photo.png"
cp "$D/sample.png" "$T/photo-marked.png"
"$CROPMARK" "$T/photo.png" --selftest "$T/renamed"

# naked call: the picker lists the current folder
P=$T/pick
mkdir "$P" "$P/sub"
cp "$D/sample.png" "$P/photo.png"
cp "$D/sample_exif6.jpg" "$P/other.jpg"
cp "$D/sample.png" "$P/.hidden.png"
cp "$D/sample.png" "$P/sub/inner.png"
echo "not a picture" > "$P/broken.png"
echo "notes" > "$P/notes.txt"
(cd "$P" && "$CROPMARK" --selftest-picker)

# --pictures (what the desktop entry runs): the picker starts in the
# Pictures folder from user-dirs.dirs, else ~/Pictures, whatever the cwd
H=$T/home
mkdir -p "$H/.config"
ln -s "$P" "$H/pick"
printf '%s\n' 'XDG_PICTURES_DIR="$HOME/pick"' > "$H/.config/user-dirs.dirs"
(cd / && HOME=$H XDG_CONFIG_HOME= "$CROPMARK" --pictures --selftest-picker)
rm "$H/.config/user-dirs.dirs"
ln -s "$P" "$H/Pictures"
(cd / && HOME=$H XDG_CONFIG_HOME= "$CROPMARK" --pictures --selftest-picker)
# ... and a file opened with the desktop entry goes straight to the editor
mkdir "$T/e2"
cp "$D/sample.png" "$T/e2/photo.png"
cp "$D/sample.png" "$T/e2/photo-marked.png"
"$CROPMARK" --pictures "$T/e2/photo.png" --selftest "$T/e2/renamed"

# a folder argument opens the picker there; a missing file fails before any window
"$CROPMARK" "$T/missing.png" 2>/dev/null && exit 1
"$CROPMARK" "$P" "$T/out.png" 2>/dev/null && exit 1   # OUTPUT needs a photo
echo "gui selftest ok"
