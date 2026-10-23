# obs-screen-glide

OBS source "Screen Glide (active monitor)": shows whichever monitor you are working on and
slides between your monitor-capture inputs (no scene change). Wayland gives apps no pointer
or focus info, so `extension/screen-glide@archidesk` (GNOME Shell) publishes it to
`$XDG_RUNTIME_DIR/app/com.obsproject.Studio/screen-glide.state`, which the Flatpak OBS can read.

## Install
    scripts/install.sh
    # log out/in once (Wayland), then:
    gnome-extensions enable screen-glide@archidesk

## Use
1. Add one *Screen Capture (PipeWire)* source per monitor.
2. Add *Screen Glide* to your scene; pick the captures per slot.
3. Per slot set the **Desktop monitor** (control layout) and **Physical position** (left to right).
   Swipe direction uses physical position, so a middle monitor that is rightmost on the desktop
   still slides in between the other two.
4. Tune **Switch delay** (debounce) and **Swipe duration**.

State file lines: `pointer N`, `focus N`, `mon N x y w h`. Override the path with `SCREEN_GLIDE_STATE`.

## License
GPL-2.0-or-later, matching OBS Studio. See LICENSE.

Status: pre-release; built against OBS 32.2.2 headers, not yet field-tested.

## apt (native OBS on Ubuntu/Debian, amd64)
    sudo curl -fsSLo /etc/apt/keyrings/obs-screen-glide.gpg https://cameronweller.github.io/obs-screen-glide/obs-screen-glide.gpg
    echo "deb [signed-by=/etc/apt/keyrings/obs-screen-glide.gpg] https://cameronweller.github.io/obs-screen-glide stable main" | sudo tee /etc/apt/sources.list.d/obs-screen-glide.list
    sudo apt update && sudo apt install obs-screen-glide

Not for the Flatpak OBS (it cannot see `/usr/lib`); use `scripts/install.sh` there.
Maintainers: `scripts/publish-apt.sh <version>` builds, signs and pushes the repo to `gh-pages`.
