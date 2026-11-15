#!/bin/sh
# End-to-end check of the CLI against the fixtures in tests/data.
set -e
PMARK=${1:-build/pmark}
D=$(dirname "$0")/data
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

[ "$("$PMARK" "$D/sample.png" --info)" = "480x320" ]
# stored sideways (320x480) with EXIF orientation 6: must come out upright
[ "$("$PMARK" "$D/sample_exif6.jpg" --info)" = "480x320" ]

# PNG in -> crop, highlighter, marker -> JPEG out -> reload
"$PMARK" "$D/sample.png" --crop 40,20,300,200 \
    --color ffeb3b80 --width 24 --stroke "20,100 280,100" \
    --color e53935 --width 6 --stroke "30,30 120,60 60,150" \
    -o "$T/out.jpg" --quality 85
[ "$("$PMARK" "$T/out.jpg" --info)" = "300x200" ]

# error paths must fail cleanly, not crash
! "$PMARK" "$D/sample.png" --crop 900,900,5,5 -o "$T/x.png" 2>/dev/null
! "$PMARK" "$T/missing.png" --info 2>/dev/null
! "$PMARK" "$D/sample.png" -o "$T/x.bmp" 2>/dev/null
! "$PMARK" "$D/sample.png" --stroke "1,2 3" -o "$T/x.png" 2>/dev/null
echo "cli smoke ok"
