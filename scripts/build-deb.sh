#!/usr/bin/env bash
# Build build/obs-screen-glide_<version>_amd64.deb for native (apt) OBS Studio.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION="${1:-0.1.0}"
make
root=build/deb
rm -rf "$root"
install -Dm755 build/obs-screen-glide.so "$root/usr/lib/x86_64-linux-gnu/obs-plugins/obs-screen-glide.so"
ext="$root/usr/share/gnome-shell/extensions/screen-glide@archidesk"
install -d "$ext"
install -m644 extension/screen-glide@archidesk/* "$ext/"
install -Dm644 LICENSE "$root/usr/share/doc/obs-screen-glide/copyright"
install -d "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<CTL
Package: obs-screen-glide
Version: $VERSION
Architecture: amd64
Maintainer: Cameron Weller <20343009+CameronWeller@users.noreply.github.com>
Depends: obs-studio, libc6 (>= 2.38)
Recommends: gnome-shell
Section: video
Priority: optional
Homepage: https://github.com/CameronWeller/obs-screen-glide
Description: OBS source that slides between monitors following the active one
 Screen Glide shows whichever monitor you are working on, sliding between
 monitor-capture inputs with a debounced, swooshy animation instead of
 switching scenes. Includes a GNOME Shell extension that reports the active monitor.
CTL
dpkg-deb --root-owner-group --build "$root" "build/obs-screen-glide_${VERSION}_amd64.deb"
