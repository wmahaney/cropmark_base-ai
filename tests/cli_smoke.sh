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

"$PMARK" "$D/sample.png" --crop 40,20,300,200 \
    --color ffeb3b80 --width 24 --stroke "20,100 280,100" \
    --arrow 10,10,100,100 --rect 5,5,50,50 --ellipse 100,20,80,60 \
    -o "$T/out.jpg" --quality 85 --save-session "$T/out.pmark"
[ "$("$PMARK" "$T/out.jpg" --info)" = "300x200" ]
# a session reopens, continues, and saves again
[ "$("$PMARK" "$T/out.pmark" --crop 0,0,100,50 --info)" = "100x50" ]

# overwrite rules
cp "$D/sample.png" "$T/in.png"
! "$PMARK" "$T/in.png" --crop 0,0,10,10 -o "$T/in.png" 2>/dev/null        # input protected
[ "$("$PMARK" "$T/in.png" --info)" = "480x320" ]
ln "$T/in.png" "$T/alias.png"
! "$PMARK" "$T/in.png" --crop 0,0,10,10 -o "$T/alias.png" 2>/dev/null     # even via a hard link
"$PMARK" "$T/in.png" --crop 0,0,10,10 -o "$T/in.png" --force
[ "$("$PMARK" "$T/in.png" --info)" = "10x10" ]
! "$PMARK" "$D/sample.png" -o "$T/out.jpg" --no-clobber 2>/dev/null       # exists
"$PMARK" "$D/sample.png" -o "$T/out.jpg"                                   # replaced
[ "$("$PMARK" "$T/out.jpg" --info)" = "480x320" ]

# stale sessions are refused unless --lenient
cp "$D/sample.png" "$T/base.png"
"$PMARK" "$T/base.png" --crop 0,0,50,50 --save-session "$T/b.pmark"
"$PMARK" "$D/sample_exif6.jpg" -o "$T/base.png"
! "$PMARK" "$T/b.pmark" --info 2>/dev/null
[ "$("$PMARK" "$T/b.pmark" --lenient --info)" = "50x50" ]

# error paths must fail cleanly, not crash
! "$PMARK" "$D/sample.png" --crop 900,900,5,5 -o "$T/x.png" 2>/dev/null
! "$PMARK" "$T/missing.png" --info 2>/dev/null
! "$PMARK" "$D/sample.png" -o "$T/x.bmp" 2>/dev/null
! "$PMARK" "$D/sample.png" --stroke "1,2 3" -o "$T/x.png" 2>/dev/null
! "$PMARK" "$D/sample.png" --width 1e9 --stroke "1,2" -o "$T/x.png" 2>/dev/null
[ -z "$(ls "$T" | grep pmtmp || true)" ]   # no temp files left behind
echo "cli smoke ok"
