# osm-widgets — integration notes

Round 1: overlay daemon, settings app, plugin ABI, one reference widget.
Plasmoid host is round 2 (see the bottom of this file).

---

## 1. Source layout

Put the files here in the repo:

```
apps/osm-widgets/osm-widget-plugin.h
apps/osm-widgets/osm-widgets.cpp
apps/osm-widgets/osm-widgets-settings.cpp
apps/osm-widgets/widgets/clock.cpp
```

Runtime layout:

```
/usr/local/bin/osm-widgets                    daemon
/usr/local/bin/osm-widgets-settings           launcher app
/usr/local/lib/alternix/widgets/*.so          widget plugins
~/.config/Alternix/osm-widgets.conf           all state
```

Widget plugins live in their own directory rather than `/usr/local/bin`
so the picker can list them without also finding the osm-settings
plugins, which export a different ABI.

---

## 2. `install-alternix_devuan.sh` — one word, line 147

Line 147 currently reads:

```
    xprintidle libx11-dev libxtst-dev ntfs-3g aria2 ranger x11-apps zip \
```

`osm-widgets` links `-lXext` for the XShape click-through mask. `libxext-dev`
arrives transitively via `libxtst-dev` today, but that is a dependency
accident, not a guarantee. Add it explicitly:

```
    xprintidle libx11-dev libxtst-dev libxext-dev ntfs-3g aria2 ranger x11-apps zip \
```

---

## 3. `install-alternix_devuan.sh` — new build block after line 649

Line 649 is `sudo mv ui.so /usr/local/bin/`, the last of the osm-settings
plugins. Line 650 is blank and 651 begins the
`# Custom App Shortcuts & Icons` divider. Paste this between them:

```sh
# ────────────────────────────────────────────────
# 6b. Build OSM Widgets (desktop widget overlay)
# ────────────────────────────────────────────────
echo " "
echo "[8/10] Building osm-widgets..."

cd "$ALT_ROOT/Alternix/apps/osm-widgets" || { echo "ERROR: apps/osm-widgets folder missing"; exit 1; }

sudo mkdir -p /usr/local/lib/alternix/widgets

g++ -fPIC osm-widgets.cpp -o osm-widgets -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11 -lXext
chmod +x osm-widgets && sudo mv osm-widgets /usr/local/bin/

echo "• Building osm-widgets-settings..."
g++ osm-widgets-settings.cpp -o osm-widgets-settings -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-widgets-settings && sudo mv osm-widgets-settings /usr/local/bin/

echo "• Building clock.so widget..."
g++ -I. -fPIC -shared widgets/clock.cpp -o clock.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv clock.so /usr/local/lib/alternix/widgets/

if [ -f "$ALT_ROOT/Alternix/icons/osm-widgets.png" ]; then
    sudo cp "$ALT_ROOT/Alternix/icons/osm-widgets.png" /usr/share/icons/hicolor/64x64/apps/osm-widgets.png
fi

cat <<EOF > "$HOME/.local/share/applications/osm-widgets.desktop"
[Desktop Entry]
Type=Application
Name=Desktop Widgets
Comment=Add and arrange widgets on the Alternix desktop
Exec=/usr/local/bin/osm-widgets-settings
Icon=osm-widgets
Terminal=false
Categories=Utility;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-widgets.desktop"
```

The `-I.` on the clock build is required — `widgets/clock.cpp` includes
`osm-widget-plugin.h` from the directory above it.

An `icons/osm-widgets.png` is needed for the launcher tile. Until one
exists the block skips the copy and the launcher falls back to no icon.

---

## 4. `config.py` — one line after line 70

Line 70 is `subprocess.Popen(['osm-running'])`, line 71 is
`subprocess.Popen(['touchegg'])`. Insert between them:

```python
    subprocess.Popen(['osm-widgets'])
```

No `Match` rule is needed in `floating_layout`. The overlay is
override-redirect (`Qt::X11BypassWindowManagerHint`), so Qtile never sees
the window at all and cannot tile it, focus it, or give it a bar slot.

---

## 5. How the pieces talk to each other

There is no IPC. `osm-widgets-settings` writes
`~/.config/Alternix/osm-widgets.conf`; the daemon has a
`QFileSystemWatcher` on it and rebuilds every widget on change. QSettings
saves atomically via rename, which fires the watcher two or three times
per save, so the daemon debounces for 350 ms and re-adds the watch path
afterwards.

Config shape:

```ini
[General]
Enabled=true
EditMode=false

[Instance-clock-1]
plugin=clock
x=40
y=40
w=300
h=140
screen=0

[Widget-clock-1]
TimeFormat=HH:mm
ShowDate=true
Colour=#ffffff
```

Instances are discovered by scanning for `Instance-*` groups, so there is
no separate index list that can drift out of sync with reality.

The host owns `[Instance-<id>]`. A plugin only ever touches
`[Widget-<id>]`, via the `osmWidgetGet` / `osmWidgetSet` helpers in the
header.

---

## 6. Stacking and input

Three things keep the overlay at the bottom:

1. Override-redirect, so the WM never manages or raises it.
2. `_NET_WM_WINDOW_TYPE_DESKTOP` plus `_NET_WM_STATE_BELOW`,
   `SKIP_TASKBAR`, `SKIP_PAGER`, `STICKY`, and
   `_NET_WM_DESKTOP = 0xFFFFFFFF` — ignored by Qtile but respected by
   picom and anything else that inspects the properties.
3. An `XLowerWindow` every 2 seconds, mirroring the raise timer in
   `osm-running`'s `ActivationEdgeBar` but in the opposite direction.

Geometry comes from `QScreen::availableGeometry()`, which honours the
struts of both Qtile bars, so a widget can never be dragged underneath
the top or bottom bar.

Clicks: outside arrange mode the overlay sets an XShape **input** mask
covering only the widget rectangles, so taps on bare wallpaper fall
straight through to the root window. In arrange mode the mask is cleared
so the whole overlay takes drags.

---

## 7. Writing another widget

Copy `widgets/clock.cpp`. Fill in the `OsmWidgetInfo` struct, implement
`osm_widget_create` and `osm_widget_config`, add a build line to the
installer block. Nothing else registers the widget — the picker finds it
by scanning the directory and reading `osm_widget_info()`.

The host refuses to load a `.so` whose `abi` field is not
`OSM_WIDGET_ABI`, so a stale plugin left over from an older install is
skipped with a warning rather than crashing the overlay.

---

## 8. Round 2 — Plasma widget compatibility

Not in this drop. The plan, so the ABI above does not have to change:

A `plasmoid.so` widget plugin that is a QML host. `qtdeclarative5-dev` is
already in the installer's package list at line 140, so `QQuickWidget` is
available with no new dependency.

- Scan `~/.local/share/plasma/plasmoids/` and
  `/usr/share/plasma/plasmoids/`, read each `metadata.json`.
- Ship a shim import path containing `qmldir` files for
  `org.kde.plasma.components`, `org.kde.plasma.core`, and
  `org.kde.plasma.plasmoid` that re-export the nearest
  `QtQuick.Controls` equivalents.
- Load `contents/ui/main.qml` into a `QQuickWidget`.

What this gets: self-contained plasmoids — analog clock, sticky notes,
colour picker, simple monitors.

What it will not get, and should say so loudly in the picker rather than
half-loading: anything using `Plasma5Support.DataSource`, which expects
plasmashell's DataEngines to be running, and anything using containment
APIs, which expects to be inside a Plasma containment. That covers most
of the network, weather, and media plasmoids.

The honest version of this feature is "loads simple plasmoids, refuses
the rest with a clear reason". Full compatibility means running
plasmashell, which is not a trade worth making on a Bay Trail tablet.
