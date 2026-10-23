#!/usr/bin/env bash
# Build the .deb, regenerate a signed apt repo, and push it to the gh-pages branch.
# Signing key lives outside the repo: $SIGN_HOME (default ~/.config/obs-screen-glide/gnupg).
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION="${1:-0.1.0}"
SIGN_HOME="${SIGN_HOME:-$HOME/.config/obs-screen-glide/gnupg}"
KEY="obs-screen-glide apt signing"
scripts/build-deb.sh "$VERSION"
deb="build/obs-screen-glide_${VERSION}_amd64.deb"

site=build/site
rm -rf "$site"
if git ls-remote --exit-code origin gh-pages >/dev/null 2>&1; then
  git fetch -q origin gh-pages && git worktree add -q -f "$site" FETCH_HEAD
else
  mkdir -p "$site"
fi
pool="$site/pool/main/o/obs-screen-glide"
bin="$site/dists/stable/main/binary-amd64"
mkdir -p "$pool" "$bin"
cp "$deb" "$pool/"

python3 - "$site" <<'PY'
import gzip, hashlib, os, subprocess, sys
site = sys.argv[1]
entries = []
pool = os.path.join(site, "pool/main/o/obs-screen-glide")
for name in sorted(os.listdir(pool)):
    path = os.path.join(pool, name)
    data = open(path, "rb").read()
    ctl = subprocess.check_output(["dpkg-deb", "-f", path]).decode().rstrip("\n")
    rel = os.path.relpath(path, site)
    entries.append(f"{ctl}\nFilename: {rel}\nSize: {len(data)}\nSHA256: {hashlib.sha256(data).hexdigest()}\n")
packages = "\n".join(entries).encode()
bdir = os.path.join(site, "dists/stable/main/binary-amd64")
open(os.path.join(bdir, "Packages"), "wb").write(packages)
open(os.path.join(bdir, "Packages.gz"), "wb").write(gzip.compress(packages, mtime=0))
lines = ["Origin: obs-screen-glide", "Label: obs-screen-glide", "Suite: stable", "Codename: stable",
         "Architectures: amd64", "Components: main", "SHA256:"]
for f in ("Packages", "Packages.gz"):
    d = open(os.path.join(bdir, f), "rb").read()
    lines.append(f" {hashlib.sha256(d).hexdigest()} {len(d)} main/binary-amd64/{f}")
open(os.path.join(site, "dists/stable/Release"), "w").write("\n".join(lines) + "\n")
PY
rel="$site/dists/stable"
gpg --homedir "$SIGN_HOME" --batch --yes --armor --detach-sign -o "$rel/Release.gpg" "$rel/Release"
gpg --homedir "$SIGN_HOME" --batch --yes --clearsign -o "$rel/InRelease" "$rel/Release"
gpg --homedir "$SIGN_HOME" --export "$KEY" > "$site/obs-screen-glide.gpg"
touch "$site/.nojekyll"
cat > "$site/index.html" <<'HTML'
<!doctype html><meta charset="utf-8"><title>obs-screen-glide apt repository</title>
<pre>sudo curl -fsSLo /etc/apt/keyrings/obs-screen-glide.gpg https://cameronweller.github.io/obs-screen-glide/obs-screen-glide.gpg
echo "deb [signed-by=/etc/apt/keyrings/obs-screen-glide.gpg] https://cameronweller.github.io/obs-screen-glide stable main" | sudo tee /etc/apt/sources.list.d/obs-screen-glide.list
sudo apt update &amp;&amp; sudo apt install obs-screen-glide</pre>
HTML

if [ -e "$site/.git" ]; then
  git -C "$site" add -A
  git -C "$site" commit -q -m "apt repo $VERSION"
  git -C "$site" push -q origin HEAD:gh-pages
else
  git -C "$site" init -q -b gh-pages
  git -C "$site" add -A
  git -C "$site" -c user.name=CameronWeller -c user.email=20343009+CameronWeller@users.noreply.github.com commit -q -m "apt repo $VERSION"
  git -C "$site" push -q "$(git config remote.origin.url)" HEAD:gh-pages
fi
