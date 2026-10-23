#!/usr/bin/env bash
# Build and install the OBS plugin (Flatpak OBS) and the GNOME Shell tracker extension.
set -euo pipefail
cd "$(dirname "$0")/.."
scripts/fetch-obs-headers.sh
[ -d .deps/simde ] || git clone -q --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde .deps/simde
make
plugin="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-screen-glide/bin/64bit"
mkdir -p "$plugin"
install -m 755 build/obs-screen-glide.so "$plugin/"
ext="$HOME/.local/share/gnome-shell/extensions/screen-glide@archidesk"
mkdir -p "$ext"
install -m 644 extension/screen-glide@archidesk/* "$ext/"
echo "Installed. Enable the extension after the next GNOME login: gnome-extensions enable screen-glide@archidesk"
