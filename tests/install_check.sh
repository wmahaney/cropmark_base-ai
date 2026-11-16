#!/bin/sh
# `make install` into a scratch DESTDIR: program, pmark alias, menu entry
# (pointing at the installed program and icon), then `make uninstall`.
set -e
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
P=/opt/cropmark-test
make -s install DESTDIR="$T" PREFIX=$P >/dev/null
make -s desktop-shortcut DESTDIR="$T" PREFIX=$P DESKTOP_DIR="$T/Desktop" >/dev/null
"$T$P/bin/pmark" --version | grep -q "^cropmark [0-9]"
E=$T$P/share/applications/cropmark.desktop
grep -qx "Exec=\"$P/bin/cropmark\" --pictures %f" "$E"
grep -qx "Icon=$P/share/icons/hicolor/scalable/apps/cropmark.svg" "$E"
[ -f "$T$P/share/icons/hicolor/scalable/apps/cropmark.svg" ]
[ -x "$T/Desktop/cropmark.desktop" ]
! grep -q @ "$E"                                       # every placeholder filled
make -s uninstall DESTDIR="$T" PREFIX=$P DESKTOP_DIR="$T/Desktop" >/dev/null
[ -z "$(find "$T" -type f -o -type l)" ]
echo "install check ok"
