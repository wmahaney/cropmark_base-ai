#!/bin/sh
# Golden-image tests: render fixed edits and compare with tests/golden/*.png.
#   tests/golden.sh BUILD_DIR          check
#   tests/golden.sh BUILD_DIR update   rewrite the references (review the diff!)
set -e
B=${1:-build}
MODE=${2:-check}
D=$(dirname "$0")
G=$D/golden
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PMARK=$B/pmark
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
fail=0

case_() { # name, then pmark args (output path is appended)
  name=$1; shift
  "$PMARK" "$@" -o "$T/$name.png"
  if [ "$MODE" = update ]; then
    cp "$T/$name.png" "$G/$name.png"; echo "updated $name"
  elif "$B/imgdiff" "$G/$name.png" "$T/$name.png"; then
    echo "golden $name ok"
  else
    echo "golden $name FAILED (output kept at $T/$name.png)"; fail=1; trap - EXIT
  fi
}

case_ markers "$D/data/sample.png" --crop 40,20,400,260 \
  --color ffeb3b80 --width 30 --stroke "20,200 380,200 200,120 20,200" \
  --color e53935 --width 6 --stroke "30,30 120,70 60,150 200,40"

case_ shapes "$D/data/sample.png" --width 5 \
  --color 1e88e5 --arrow 40,280,250,120 --color 43a047 --rect 280,30,170,120 \
  --color 8e24aa80 --width 14 --ellipse 300,170,160,120 --color e53935 --width 1 --stroke "5,5"

case_ exif "$D/data/sample_exif6.jpg" --crop 100,50,300,200 \
  --color 000000 --width 3 --rect 10,10,280,180

# the session must reproduce the markers case exactly
"$PMARK" "$D/data/sample.png" --crop 40,20,400,260 \
  --color ffeb3b80 --width 30 --stroke "20,200 380,200 200,120 20,200" \
  --save-session "$T/s.pmark"
case_ markers "$T/s.pmark" --color e53935 --width 6 --stroke "30,30 120,70 60,150 200,40"

if [ -f "$FONT" ]; then
  case_ text "$D/data/sample.png" --font "$FONT" --size 40 --color ffffff --outline 3 \
    --text 30,40 "Outlined text\nsecond line" --color 00000080 --outline 0 --size 64 --text 60,190 "AVAWfij"
else
  echo "golden text skipped (needs $FONT)"
fi
[ $fail = 0 ]
