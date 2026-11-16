#!/bin/sh
# End-to-end check of batch mode against the fixtures in tests/data.
set -e
CROPMARK=${1:-build/cropmark}
D=$(dirname "$0")/data
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

[ "$("$CROPMARK" "$D/sample.png" --info)" = "480x320" ]
# stored sideways (320x480) with EXIF orientation 6: must come out upright
[ "$("$CROPMARK" "$D/sample_exif6.jpg" --info)" = "480x320" ]

"$CROPMARK" "$D/sample.png" --crop 40,20,300,200 \
    --color ffeb3b80 --width 24 --stroke "20,100 280,100" \
    --arrow 10,10,100,100 --rect 5,5,50,50 --ellipse 100,20,80,60 \
    -o "$T/out.jpg" --quality 85 --save-session "$T/out.pmark"
[ "$("$CROPMARK" "$T/out.jpg" --info)" = "300x200" ]
# a session reopens, continues, and saves again
[ "$("$CROPMARK" "$T/out.pmark" --crop 0,0,100,50 --info)" = "100x50" ]

# overwrite rules
cp "$D/sample.png" "$T/in.png"
! "$CROPMARK" "$T/in.png" --crop 0,0,10,10 -o "$T/in.png" 2>/dev/null        # input protected
[ "$("$CROPMARK" "$T/in.png" --info)" = "480x320" ]
ln "$T/in.png" "$T/alias.png"
! "$CROPMARK" "$T/in.png" --crop 0,0,10,10 -o "$T/alias.png" 2>/dev/null     # even via a hard link
"$CROPMARK" "$T/in.png" --crop 0,0,10,10 -o "$T/in.png" --force
[ "$("$CROPMARK" "$T/in.png" --info)" = "10x10" ]
! "$CROPMARK" "$D/sample.png" -o "$T/out.jpg" --no-clobber 2>/dev/null       # exists
"$CROPMARK" "$D/sample.png" -o "$T/out.jpg"                                   # replaced
[ "$("$CROPMARK" "$T/out.jpg" --info)" = "480x320" ]

# stale sessions are refused unless --lenient
cp "$D/sample.png" "$T/base.png"
"$CROPMARK" "$T/base.png" --crop 0,0,50,50 --save-session "$T/b.pmark"
"$CROPMARK" "$D/sample_exif6.jpg" -o "$T/base.png"
! "$CROPMARK" "$T/b.pmark" --info 2>/dev/null
[ "$("$CROPMARK" "$T/b.pmark" --lenient --info)" = "50x50" ]

# error paths must fail cleanly, not crash
! "$CROPMARK" "$D/sample.png" --crop 900,900,5,5 -o "$T/x.png" 2>/dev/null
! "$CROPMARK" "$T/missing.png" --info 2>/dev/null
! "$CROPMARK" "$D/sample.png" -o "$T/x.bmp" 2>/dev/null
! "$CROPMARK" "$D/sample.png" --stroke "1,2 3" -o "$T/x.png" 2>/dev/null
! "$CROPMARK" "$D/sample.png" --width 1e9 --stroke "1,2" -o "$T/x.png" 2>/dev/null
[ -z "$(ls "$T" | grep pmtmp || true)" ]   # no temp files left behind

# choosing between the window and batch mode
"$CROPMARK" --help | grep -q "^usage: cropmark"
"$CROPMARK" "$D/sample.png" --help | grep -q "Batch mode"          # --help anywhere
! "$CROPMARK" --crop 0,0,5,5 "$D/sample.png" 2>/dev/null           # photo must come first
"$CROPMARK" "$D/sample.png" --quality 80 -o "$T/q.jpg"              # -o makes it batch
[ "$("$CROPMARK" "$T/q.jpg" --info)" = "480x320" ]
echo "cli smoke ok"
