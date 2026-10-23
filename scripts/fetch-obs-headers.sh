#!/usr/bin/env bash
# Fetch libobs headers matching the Flatpak OBS (no link-time libobs needed).
set -euo pipefail
cd "$(dirname "$0")/.."
TAG="${OBS_TAG:-32.2.2}"
[ -d .deps/obs-studio/libobs ] && exit 0
mkdir -p .deps
git clone --depth 1 --branch "$TAG" --filter=blob:none --sparse https://github.com/obsproject/obs-studio .deps/obs-studio
git -C .deps/obs-studio sparse-checkout set libobs
mkdir -p .deps/include
echo '/* stub: header-only build against libobs */' > .deps/include/obsconfig.h
