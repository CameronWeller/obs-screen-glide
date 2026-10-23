// SPDX-License-Identifier: GPL-2.0-or-later
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

const APP_DIR = 'app/com.obsproject.Studio';
const FILE = 'screen-glide.state';
const POLL_MS = 100;

export default class ScreenGlideExtension extends Extension {
    enable() {
        const runtime = GLib.get_user_runtime_dir();
        GLib.mkdir_with_parents(`${runtime}/${APP_DIR}`, 0o700);
        this._paths = [`${runtime}/${APP_DIR}/${FILE}`, `${runtime}/${FILE}`];
        this._last = '';
        this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, POLL_MS, () => {
            this._publish();
            return GLib.SOURCE_CONTINUE;
        });
        this._publish();
    }

    disable() {
        if (this._timer)
            GLib.source_remove(this._timer);
        this._timer = null;
        for (const path of this._paths ?? []) {
            try {
                Gio.File.new_for_path(path).delete(null);
            } catch (_e) {
                // already gone
            }
        }
    }

    _monitorAt(monitors, x, y) {
        return monitors.findIndex(m =>
            x >= m.x && x < m.x + m.width && y >= m.y && y < m.y + m.height);
    }

    _publish() {
        const monitors = Main.layoutManager.monitors;
        const [px, py] = global.get_pointer();
        const focusWindow = global.display.focus_window;
        const focus = focusWindow && focusWindow.get_window_type() === 0
            ? focusWindow.get_monitor() : -1;
        const lines = [`pointer ${this._monitorAt(monitors, px, py)}`, `focus ${focus}`];
        monitors.forEach((m, i) => lines.push(`mon ${i} ${m.x} ${m.y} ${m.width} ${m.height}`));
        const text = `${lines.join('\n')}\n`;
        if (text === this._last)
            return;
        this._last = text;
        const bytes = new TextEncoder().encode(text);
        for (const path of this._paths) {
            try {
                Gio.File.new_for_path(path).replace_contents(
                    bytes, null, false, Gio.FileCreateFlags.REPLACE_DESTINATION, null);
            } catch (e) {
                console.warn(`screen-glide: cannot write ${path}: ${e.message}`);
            }
        }
    }
}
