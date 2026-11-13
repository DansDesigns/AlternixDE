#!/bin/bash

# Root of the Alternix repo
ALT_ROOT="$(cd "$(dirname "$0")" && pwd)"

echo ""
echo "Current working directory:"
pwd
cd "$ALT_ROOT/Alternix"
echo ""
sleep 1
#if [ ! -d "$ALT_ROOT" ]; then
#    echo "ERROR: $ALT_ROOT not found. Please place install-update.sh inside ~/Alternix."
#    exit 1
#fi


#===========================================================
# Settings to be updated (configs, system icons & settings):
#===========================================================
echo ""
# echo "No Settings or Configs to update..."
echo ""
echo ""
echo "[Config] Updating Grub Entry.."

# Rename Grub entry from "Devuan GNU/Linux" to "Alternix"
#sudo sed -i 's/Devuan GNU\/Linux/Alternix/g' /boot/grub/grub.cfg
# update-grub
#sudo dpkg --add-architecture i386

#sudo nala install xserver-xlibre-input-libinput ntfs-3g exfatprogs exfat-fuse udisks2 pmount /
#   libmbim-utils libqmi-utils modemmanager mobile-broadband-provider-info x11-apps tlp fake-hwclock /
#   iio-sensor-proxy libxext-dev wine32 win64 -y

sudo apt install libxi-dev

# Check both independent Settings modules before making any system changes.
# The clock/PAM implementation now belongs to date-time.cpp, NOT location.cpp.
ALT_SETTINGS_SOURCE="$ALT_ROOT/Alternix/apps/osm-settings"
ALT_LOCATION_SOURCE="$ALT_SETTINGS_SOURCE/location.cpp"
ALT_DATE_TIME_SOURCE="$ALT_SETTINGS_SOURCE/date-time.cpp"
if [ ! -f "$ALT_LOCATION_SOURCE" ] || \
   ! grep -Fq 'class LocationPage' "$ALT_LOCATION_SOURCE" || \
   ! grep -Fq 'Location/network_enabled' "$ALT_LOCATION_SOURCE"; then
    echo "ERROR: The repository does not contain the corrected location.cpp." >&2
    exit 1
fi
if [ ! -f "$ALT_DATE_TIME_SOURCE" ] || \
   ! grep -Fq 'class DateTimePage' "$ALT_DATE_TIME_SOURCE" || \
   ! grep -Fq 'Clock already synchronised by Chrony' "$ALT_DATE_TIME_SOURCE" || \
   ! grep -Fq 'System sudo handles authentication through its PAM service' "$ALT_DATE_TIME_SOURCE"; then
    echo "ERROR: The repository does not contain the independent Date & Time module." >&2
    echo "Add Alternix/apps/osm-settings/date-time.cpp before updating." >&2
    exit 1
fi

# The updater installs audio support from the same AlternixDE source as
# new installations. Check before updating any system files.
ALT_AUDIO_SOURCE="$ALT_ROOT/Alternix/scripts/alternix-audio-fix.sh"
if [ ! -s "$ALT_AUDIO_SOURCE" ] || ! bash -n "$ALT_AUDIO_SOURCE"; then
    echo "ERROR: Missing or invalid audio setup source: $ALT_AUDIO_SOURCE" >&2
    echo "Place alternix-audio-fix.sh in Alternix/scripts/ in the repository first." >&2
    exit 1
fi

# Verify the complete X11 pen, touch and auto-rotation runtime before an
# ISO build/update begins. These files belong in AlternixDE, not in the
# installer directory. Refuse to ship a UI with missing backends.
# Wallpaper rotation must ship as ONE consistent set of sources. The Qtile
# event hook is in the repository config (not in Alternix/scripts/), while
# the existing wallpaper restorer is in Alternix/apps/.
ALT_PAPER_RESTORE_SOURCE="$ALT_ROOT/Alternix/apps/osm-paper-restore"
ALT_PAPER_QTILE_SOURCE="$ALT_ROOT/Alternix/configs/qtile/config.py"
if [ ! -s "$ALT_PAPER_RESTORE_SOURCE" ] || \
   [ ! -s "$ALT_PAPER_QTILE_SOURCE" ] || \
   ! grep -Fq -- '--clear' "$ALT_PAPER_RESTORE_SOURCE" || \
   ! grep -Fq 'Alternix wallpaper refresh after XRandR/Qtile reconfiguration' "$ALT_PAPER_QTILE_SOURCE"; then
    echo "ERROR: Wallpaper rotation sources are missing/outdated in Alternix/apps or Alternix/configs/qtile." >&2
    exit 1
fi
if ! python3 - "$ALT_PAPER_RESTORE_SOURCE" "$ALT_PAPER_QTILE_SOURCE" <<'ALT_PAPER_SYNTAX'
import ast
import pathlib
import sys
for filename in sys.argv[1:]:
    ast.parse(pathlib.Path(filename).read_text(encoding='utf-8'), filename=filename)
ALT_PAPER_SYNTAX
then
    echo "ERROR: Wallpaper restoration/Qtile Python source failed syntax validation." >&2
    exit 1
fi

ALT_INPUT_SOURCE_DIR="$ALT_ROOT/Alternix/scripts"
for alt_input_file in \
    alternix-rotate-monitor.sh \
    alternix-rotate-setup.sh \
    alternix-rotate-toggle.sh \
    alternix-stylus-apply.sh; do
    if [ ! -s "$ALT_INPUT_SOURCE_DIR/$alt_input_file" ] || \
       ! bash -n "$ALT_INPUT_SOURCE_DIR/$alt_input_file"; then
        echo "ERROR: Missing or invalid Alternix pen/rotation source: $ALT_INPUT_SOURCE_DIR/$alt_input_file" >&2
        echo "Add the complete updated scripts to Alternix/scripts before installing." >&2
        exit 1
    fi
done
# Faster sensor loop: prevent restoring the old 1-second debounce code.
if ! grep -Fq 'FAST_ROTATION_V2' "$ALT_INPUT_SOURCE_DIR/alternix-rotate-monitor.sh"; then
    echo 'ERROR: Add the updated fast auto-rotation backend to Alternix/scripts before installing.' >&2
    exit 1
fi
# Refuse outdated gesture and input sources: earlier versions silently read
# XRandR's SUPPORTED orientations as "normal" even while rotated.
if ! grep -Fq 'The active rotation follows the geometry' \
        "$ALT_INPUT_SOURCE_DIR/alternix-rotate-monitor.sh" || \
   ! grep -Fq '    restart)' "$ALT_INPUT_SOURCE_DIR/alternix-rotate-toggle.sh"; then
    echo 'ERROR: Install the latest complete rotation monitor and controller in Alternix/scripts.' >&2
    exit 1
fi
# These three launch helpers are stored in the repository's Alternix/scripts,
# not alongside this installer or in Alternix/apps. Check them before changing
# the installed system; a failed copy must never look like a completed update.
for alt_layer_script in alternix-touchscroll.py alternix-waydroid-session alternix-exe; do
    if [ ! -s "$ALT_INPUT_SOURCE_DIR/$alt_layer_script" ]; then
        echo "ERROR: Required Alternix script missing: $ALT_INPUT_SOURCE_DIR/$alt_layer_script" >&2
        exit 1
    fi
done

# Qt/Xlib compatibility: the overlay sources must import Xlib only AFTER
# Qt headers. Xlib's Status/None macros otherwise cause hundreds of Qt5
# compile errors on Devuan Excalibur. Detect stale source BEFORE building.
for alt_x11_overlay in osm-status.cpp osm-notify.cpp; do
    alt_x11_src="$ALT_ROOT/Alternix/apps/$alt_x11_overlay"
    if [ -s "$alt_x11_src" ] && ! grep -Fq '#  undef Status' "$alt_x11_src"; then
        echo "ERROR: $alt_x11_src is the old Xlib/Qt header-conflicting version." >&2
        echo "Replace it with the supplied rotation-aware source before building." >&2
        exit 1
    fi
done

for alt_input_ui in osm-running.cpp osm-status.cpp osm-notify.cpp; do
    if [ ! -s "$ALT_ROOT/Alternix/apps/$alt_input_ui" ] || \
       ! grep -Fq 'alternixActiveScreenGeometry' "$ALT_ROOT/Alternix/apps/$alt_input_ui"; then
        echo "ERROR: Missing rotation-aware gesture source: Alternix/apps/$alt_input_ui" >&2
        exit 1
    fi
done

if [ ! -s "$ALT_ROOT/Alternix/apps/osm-settings/display.cpp" ] || \
   ! grep -Fq 'alternix-stylus-apply.sh' "$ALT_ROOT/Alternix/apps/osm-settings/display.cpp"; then
    echo "ERROR: Updated Display settings source with stylus controls is missing." >&2
    exit 1
fi

# Alternix VLC: require the FULL native Qt interface theme before the install.
# No .vlt files or separate patch/repair commands are used.
ALT_VLC_THEME_SRC="$ALT_ROOT/Alternix/themes/vlc"
for vlc_asset in \
    "$ALT_VLC_THEME_SRC/Kvantum/AlternixVLC/AlternixVLC.svg" \
    "$ALT_VLC_THEME_SRC/Kvantum/AlternixVLC/AlternixVLC.kvconfig" \
    "$ALT_VLC_THEME_SRC/qt5-style/alternixvlcstyle.cpp" \
    "$ALT_VLC_THEME_SRC/qt5-style/alternixvlcstyle.json" \
    "$ALT_VLC_THEME_SRC/icons/play_up.png" \
    "$ALT_VLC_THEME_SRC/icons/pause_up.png"; do
    if [ ! -s "$vlc_asset" ]; then
        echo "ERROR: Required Alternix VLC theme asset not found: $vlc_asset" >&2
        echo "Commit Alternix/themes/vlc from the supplied repository assets first." >&2
        exit 1
    fi
done

# Welcome: ship the complete native Qt5 source, launcher icon and deck-image
# assets in the repository. Do not silently install a first-boot guide with
# missing PowerPoint-derived UI artwork.
ALT_WELCOME_SOURCE="$ALT_ROOT/Alternix/apps/welcome.cpp"
ALT_WELCOME_ICON="$ALT_ROOT/Alternix/icons/welcome.png"
ALT_WELCOME_ASSET_DIR="$ALT_ROOT/Alternix/assets/welcome"
ALT_WELCOME_TOP_BAR="$ALT_WELCOME_ASSET_DIR/top-bar.png"
ALT_WELCOME_BOTTOM_BAR="$ALT_WELCOME_ASSET_DIR/bottom-bar.png"
if [ ! -s "$ALT_WELCOME_SOURCE" ] || [ ! -s "$ALT_WELCOME_ICON" ] ||    [ ! -s "$ALT_WELCOME_TOP_BAR" ] || [ ! -s "$ALT_WELCOME_BOTTOM_BAR" ]; then
    echo "ERROR: Welcome requires Alternix/apps/welcome.cpp, Alternix/icons/welcome.png and Alternix/assets/welcome/{top-bar,bottom-bar}.png." >&2
    exit 1
fi

# The normal sudo PAM mechanism handles all privilege elevation.
# No direct -n probe and no bypassing credential requests.
if ! /usr/bin/sudo -v; then
    echo "ERROR: Alternix sudo authentication failed or access was denied." >&2
    exit 1
fi

# ────────────────────────────────────────────────
# AlternixOS terminal policy: keep xterm/uxterm out of future APT operations.
# uxterm is shipped by the same Debian/Devuan package as xterm.
# This preference blocks xterm without blocking the x-terminal-emulator
# virtual package, which Alacritty provides.
# ────────────────────────────────────────────────
echo "[Terminal] Blocking xterm/uxterm from future APT installations..."
sudo install -d -m 0755 /etc/apt/preferences.d
sudo tee /etc/apt/preferences.d/alternix-no-xterm.pref >/dev/null <<'ALT_NO_XTERM'
Package: xterm
Pin: version *
Pin-Priority: -1
ALT_NO_XTERM

sudo nala install -y libssl-dev fonts-urw-base35 fonts-freefont-ttf wmctrl cmake g++ bluez vlc qt5-style-kvantum fonts-comfortaa

# XLibre-safe tablet/pen stack. xinput and xrandr must be present for
# display mapping; monitor-sensor comes from iio-sensor-proxy. Do not pull in
# the Xorg Wacom driver on an XLibre session (its server ABI may differ).
echo "[Input] Installing touchscreen, Wacom and rotation dependencies..."
sudo apt-get install -y xinput x11-xserver-utils iio-sensor-proxy dbus-x11
if apt-cache policy xserver-xlibre-input-wacom 2>/dev/null | \
   grep -Eq '^[[:space:]]*Candidate:[[:space:]]*[^ (]'; then
    if ! sudo apt-get install -y xserver-xlibre-input-wacom; then
        echo "WARNING: XLibre Wacom driver installation failed; libinput pens remain supported." >&2
    fi
else
    echo "[Input] No xserver-xlibre-input-wacom candidate in configured repositories; using XLibre libinput." 
fi


# Refresh the signed Devuan package indexes before resolving the newest
# supported ClamAV build for this machine's architecture (amd64/arm64/etc.).
echo "[Security] Updating package indexes for ClamAV..."
if ! sudo apt-get update; then
    echo "ERROR: APT indexes could not be refreshed; cannot verify the newest ClamAV packages." >&2
    exit 1
fi

ALT_CLAMAV_BUILD_CHROOT=0
if [ "${ALTERNIX_ISO_PREBUILD:-0}" = 1 ] || [ -e /etc/debian_chroot ]; then
    ALT_CLAMAV_BUILD_CHROOT=1
fi
# ────────────────────────────────────────────────
# AlternixOS Security: ClamAV on-demand malware scanner
# - APT selects its signed native package for the detected architecture.
# - No external .deb downloads, GitHub binaries or added Debian repositories.
# - clamscan is used by Settings -> Security; clamd is not required.
# - freshclam provides automatic malware-definition updates on SysVinit.
# ────────────────────────────────────────────────
echo "[Security] Installing/updating ClamAV for $(dpkg --print-architecture)..."
if ! sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clamav clamav-freshclam; then
    echo "ERROR: ClamAV packages could not be installed from the configured Devuan repositories." >&2
    apt-cache policy clamav clamav-freshclam || true
    exit 1
fi

if ! dpkg-query -W -f='${Status}' clamav 2>/dev/null | grep -qx 'install ok installed' || \
   ! dpkg-query -W -f='${Status}' clamav-freshclam 2>/dev/null | grep -qx 'install ok installed' || \
   ! command -v clamscan >/dev/null 2>&1 || ! command -v freshclam >/dev/null 2>&1; then
    echo "ERROR: ClamAV installation did not provide both clamscan and freshclam." >&2
    exit 1
fi

# Only the definition updater is a service. On-demand file scans don't run
# permanently, reducing resource use on older tablets and laptops.
if [ -x /etc/init.d/clamav-freshclam ]; then
    sudo update-rc.d clamav-freshclam defaults >/dev/null
    sudo update-rc.d clamav-freshclam enable >/dev/null
else
    echo "WARNING: ClamAV freshclam SysVinit script is missing; automatic signature updates may not run." >&2
fi

# Full signature databases can be large. Seed a new image/installation so the
# scanner can work offline after installation; don't redownload existing
# databases every time the updater is run. freshclam checks for newer releases
# automatically via its SysVinit service once the installed OS is running.
alt_clamav_database_ready() {
    local main_ok=0 daily_ok=0
    for db in /var/lib/clamav/main.cvd /var/lib/clamav/main.cld; do
        [ ! -s "$db" ] || main_ok=1
    done
    for db in /var/lib/clamav/daily.cvd /var/lib/clamav/daily.cld; do
        [ ! -s "$db" ] || daily_ok=1
    done
    [ "$main_ok" = 1 ] && [ "$daily_ok" = 1 ]
}

# Freshclam's manual and daemon modes must not share a live database lock.
# Never start/stop the *host's* services from an ISO-build chroot.
if ! alt_clamav_database_ready; then
    echo "[Security] Downloading initial ClamAV virus definitions..."
    if [ "${ALT_CLAMAV_BUILD_CHROOT:-0}" != 1 ] && [ -x /etc/init.d/clamav-freshclam ]; then
        sudo service clamav-freshclam stop >/dev/null 2>&1 || true
    fi
    if ! sudo timeout 900 freshclam --stdout; then
        echo "WARNING: Initial virus-definition download failed (network/CDN issue)." >&2
        echo "WARNING: freshclam will retry automatically when the installed OS is online." >&2
    fi
fi

if [ "${ALT_CLAMAV_BUILD_CHROOT:-0}" != 1 ] && [ -x /etc/init.d/clamav-freshclam ]; then
    if ! sudo service clamav-freshclam restart; then
        echo "WARNING: Unable to start the ClamAV definition-update service now; it is enabled at boot." >&2
    fi
else
    echo "[Security] ISO build: freshclam enabled for installed-system boot; no live services started."
fi

if alt_clamav_database_ready; then
    echo "[Security] ClamAV signature databases are ready."
else
    echo "WARNING: ClamAV is installed but its main/daily signatures are not yet ready." >&2
    echo "WARNING: Scans require freshclam to finish downloading signatures." >&2
fi

echo "[Security] Installed $(clamscan --version | head -n 1)"

# The updater may be started either as the desktop user or via sudo.
# The helper rule must belong to the actual desktop login, never root.
if [ "$(id -u)" -eq 0 ]; then
    ALT_TIME_USER="${SUDO_USER:-}"
else
    ALT_TIME_USER="$(id -un)"
fi
if [ -z "$ALT_TIME_USER" ] || [ "$ALT_TIME_USER" = root ] || \
   ! [[ "$ALT_TIME_USER" =~ ^[a-z_][a-z0-9_-]*$ ]] || \
   ! id "$ALT_TIME_USER" >/dev/null 2>&1; then
    echo "ERROR: Cannot identify the desktop user for clock-helper permissions." >&2
    echo "Run the updater as the normal AlternixOS user (using sudo only where prompted)." >&2
    exit 1
fi

ALT_VLC_USER="$ALT_TIME_USER"
ALT_VLC_HOME="$(getent passwd "$ALT_VLC_USER" | cut -d: -f6)"
if [ -z "$ALT_VLC_HOME" ] || [ "$ALT_VLC_HOME" = /root ] || [ ! -d "$ALT_VLC_HOME" ]; then
    echo "ERROR: Could not find the desktop user home for Alternix VLC." >&2
    exit 1
fi
# ────────────────────────────────────────────────
# Alternix VLC — compiled Qt5 theme + responsive player layout INSIDE VLC.
# VLC itself is still the standard Qt QMainWindow; Qtile owns tile geometry.
# Build this shipped, complete source directly here (no patch files/scripts).
# ────────────────────────────────────────────────
echo "[VLC] Building and activating Alternix native Qt player theme..."
sudo apt-get install -y qtbase5-dev qtbase5-dev-tools vlc vlc-plugin-qt qt5-style-kvantum

sudo install -d -m 0755 /usr/share/Kvantum/AlternixVLC
sudo install -m 0644 "$ALT_VLC_THEME_SRC/Kvantum/AlternixVLC/AlternixVLC.svg" \
    /usr/share/Kvantum/AlternixVLC/AlternixVLC.svg
sudo install -m 0644 "$ALT_VLC_THEME_SRC/Kvantum/AlternixVLC/AlternixVLC.kvconfig" \
    /usr/share/Kvantum/AlternixVLC/AlternixVLC.kvconfig

sudo install -d -m 0755 /usr/share/alternix/vlc/icons /usr/share/alternix/vlc/qt5-style
for vlc_icon in "$ALT_VLC_THEME_SRC"/icons/*.png; do
    [ -f "$vlc_icon" ] || { echo "ERROR: Alternix VLC icon set is missing." >&2; exit 1; }
    sudo install -m 0644 "$vlc_icon" /usr/share/alternix/vlc/icons/
done
for vlc_style_source in alternixvlcstyle.cpp alternixvlcstyle.json; do
    sudo install -m 0644 "$ALT_VLC_THEME_SRC/qt5-style/$vlc_style_source" \
        "/usr/share/alternix/vlc/qt5-style/$vlc_style_source"
done

ALT_VLC_MOC="$(command -v moc || true)"
[ -n "$ALT_VLC_MOC" ] || ALT_VLC_MOC=/usr/lib/qt5/bin/moc
if [ ! -x "$ALT_VLC_MOC" ] || ! command -v qmake >/dev/null 2>&1 || \
   ! pkg-config --exists Qt5Widgets Qt5Gui Qt5Core; then
    echo "ERROR: VLC Qt5 theme build tools are unavailable." >&2
    exit 1
fi
ALT_VLC_QT_VERSION="$(qmake -query QT_VERSION 2>/dev/null)"
case "$ALT_VLC_QT_VERSION" in
    5.*) ;;
    *) echo "ERROR: Expected Qt5 qmake, found Qt version: $ALT_VLC_QT_VERSION" >&2; exit 1 ;;
esac
ALT_VLC_PLUGIN_ROOT="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null)"
if [ -z "$ALT_VLC_PLUGIN_ROOT" ] || [ "$ALT_VLC_PLUGIN_ROOT" = / ]; then
    echo "ERROR: Cannot determine the Qt5 style plugin directory." >&2
    exit 1
fi
ALT_VLC_BUILD_DIR="$(mktemp -d /tmp/alternix-vlc-native.XXXXXXXX)"
if ! "$ALT_VLC_MOC" "$ALT_VLC_THEME_SRC/qt5-style/alternixvlcstyle.cpp" \
        -o "$ALT_VLC_BUILD_DIR/alternixvlcstyle.moc" || \
   ! g++ -std=c++17 -O2 -fPIC -shared \
        -I "$ALT_VLC_BUILD_DIR" \
        "$ALT_VLC_THEME_SRC/qt5-style/alternixvlcstyle.cpp" \
        -o "$ALT_VLC_BUILD_DIR/libalternixvlcstyle.so" \
        $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core); then
    echo "ERROR: Alternix native VLC Qt theme failed to compile. The previous plugin remains untouched." >&2
    rm -rf -- "$ALT_VLC_BUILD_DIR"
    exit 1
fi
sudo install -D -m 0644 "$ALT_VLC_BUILD_DIR/libalternixvlcstyle.so" \
    "$ALT_VLC_PLUGIN_ROOT/styles/libalternixvlcstyle.so"
rm -rf -- "$ALT_VLC_BUILD_DIR"
if [ ! -s "$ALT_VLC_PLUGIN_ROOT/styles/libalternixvlcstyle.so" ]; then
    echo "ERROR: Native Qt theme plugin installation failed." >&2
    exit 1
fi

# The preceding integration installed this exact fixed-size legacy skin.
# It must not remain enabled as a seemingly valid Alternix player option.
# Remove only our own named skin file; leave other user skins untouched.
sudo rm -f -- "$ALT_VLC_HOME/.local/share/vlc/skins2/Alternix-VLC.vlt" \
              "/etc/skel/.local/share/vlc/skins2/Alternix-VLC.vlt"

# Configure the logged-in desktop account and defaults for future users.
# Do not change the globally selected Kvantum theme or create new launchers.
sudo python3 - "$ALT_VLC_HOME" "$ALT_VLC_USER" /etc/skel <<'ALT_VLC_CONFIGURATION'
import os
import pwd
import re
import sys
from pathlib import Path

home = Path(sys.argv[1])
account = pwd.getpwnam(sys.argv[2])


def save(path, contents, owner):
    missing = []
    parent = path.parent
    while not parent.exists():
        missing.append(parent)
        parent = parent.parent
    path.parent.mkdir(parents=True, exist_ok=True)
    for directory in reversed(missing):
        if owner is not None:
            os.chown(directory, *owner)
    if not path.is_file() or path.read_text(encoding='utf-8') != contents:
        path.write_text(contents, encoding='utf-8')
    if owner is not None:
        os.chown(path, *owner)
    os.chmod(path, 0o644)


def modify_ini(path, section_name, changes, owner):
    """Preserve every unrelated preference and replace only requested keys."""
    lines = path.read_text(encoding='utf-8').splitlines(keepends=True) if path.is_file() else []
    output = []
    active = None
    found = False
    written = set()

    def finish():
        for key, value in changes.items():
            if key not in written:
                output.append(f'{key}={value}\n')
                written.add(key)

    for line in lines:
        section = re.match(r'^\s*\[([^]]+)\]', line)
        if section:
            if active == section_name:
                finish()
            active = section.group(1).strip()
            found |= active == section_name
            output.append(line)
            continue
        if active == section_name:
            key_match = re.match(r'^\s*#?\s*([\w.-]+)\s*=', line)
            if key_match and key_match.group(1) in changes:
                key = key_match.group(1)
                if key not in written:
                    output.append(f'{key}={changes[key]}\n')
                    written.add(key)
                continue
        output.append(line)
    if active == section_name:
        finish()
    if not found:
        if output and not output[-1].endswith('\n'):
            output[-1] += '\n'
        output.append(f'\n[{section_name}]\n')
        finish()
    save(path, ''.join(output), owner)


def map_application_kvantum(path, owner):
    """Assign just vlc to AlternixVLC; preserve all other app assignments."""
    lines = path.read_text(encoding='utf-8').splitlines(keepends=True) if path.is_file() else []
    result = []
    section = None
    seen_section = False
    wrote = False
    for line in lines:
        heading = re.match(r'^\s*\[([^]]+)\]', line)
        if heading:
            if section == 'Applications' and not wrote:
                result.append('AlternixVLC=vlc\n')
                wrote = True
            section = heading.group(1).strip()
            seen_section |= section == 'Applications'
            result.append(line)
            continue
        assignment = re.match(r'^(\s*)([A-Za-z0-9_.-]+)(\s*=\s*)([^\r\n]*)(\r?\n?)$', line)
        if section == 'Applications' and assignment:
            lead, theme, sep, apps, ending = assignment.groups()
            items = [app.strip() for app in apps.split(',') if app.strip() and app.strip() != 'vlc']
            if theme == 'AlternixVLC' and not wrote:
                items.append('vlc')
                wrote = True
            if items:
                result.append(lead + theme + sep + ', '.join(items) + (ending or '\n'))
            continue
        result.append(line)
    if section == 'Applications' and not wrote:
        result.append('AlternixVLC=vlc\n')
    elif not seen_section:
        if result and not result[-1].endswith('\n'):
            result[-1] += '\n'
        result.extend(['\n[Applications]\n', 'AlternixVLC=vlc\n'])
    save(path, ''.join(result), owner)


def remove_prior_duplicate_launcher(path):
    """Remove only the redundant user launcher generated by v1 integration."""
    if not path.is_file() or path.is_symlink():
        return
    content = path.read_text(encoding='utf-8')
    if ('QT_STYLE_OVERRIDE=kvantum' in content and
            '--intf=qt' in content and
            '--no-qt-video-autoresize' in content and
            re.search(r'(?m)^\s*\[Desktop Entry\]\s*$', content)):
        path.unlink()
        print('[VLC] Removed previous generated duplicate launcher:', path)
    else:
        print('[VLC] Kept independently customized user launcher:', path)


for target, owner in ((home, (account.pw_uid, account.pw_gid)), (Path(sys.argv[3]), None)):
    map_application_kvantum(target / '.config/Kvantum/kvantum.kvconfig', owner)
    modify_ini(target / '.config/vlc/vlcrc', 'core', {'intf': 'qt'}, owner)
    modify_ini(target / '.config/vlc/vlcrc', 'qt', {
        'qt-video-autoresize': '0',
        'qt-dark-palette': '0',
        'embedded-video': '1',
        'qt-minimal-view': '0',
    }, owner)
    modify_ini(target / '.config/vlc/vlc-qt-interface.conf', 'MainWindow', {
        'QtStyle': 'AlternixVLC',
        'MainToolbar1': '65;2;10;9;19;20;7;36;65',
        'MainToolbar2': '65;1;22;0;23;12;65',
        'ToolbarPos': 'false',
    }, owner)
    remove_prior_duplicate_launcher(target / '.local/share/applications/vlc.desktop')


# Modify the *existing* distribution desktop entry IN PLACE, so no extra VLC
# appears in osm-launcher. --intf=qt wins over a saved Skins2 preference;
# this is especially important when VLC previously ran a .vlt skin. Preserve
# the distro desktop name, menu categories, MIME types and %U arguments.
launcher = Path('/usr/share/applications/vlc.desktop')
if launcher.is_file() and not launcher.is_symlink():
    raw = launcher.read_text(encoding='utf-8')
    output = []
    changed_exec = 0
    for line in raw.splitlines(keepends=True):
        if not line.startswith('Exec='):
            output.append(line)
            continue
        command = line[len('Exec='):].rstrip('\r\n')
        # Only change commands which start the system VLC binary.
        match = re.search(r'(?<!\S)(?:/[^\s]*)?\bvlc(?=\s|$)', command)
        if match is None:
            output.append(line)
            continue
        command = re.sub(r'(?<!\S)--intf(?:=|\s+)skins2(?=\s|$)', '', command)
        command = re.sub(r'(?<!\S)--skins2-last(?:=\S+|\s+\S+)', '', command)
        command = re.sub(r'(?<!\S)QT_STYLE_OVERRIDE=\S+\s*', '', command).strip()
        match = re.search(r'(?<!\S)(?:/[^\s]*)?\bvlc(?=\s|$)', command)
        # The natural user-native interface is enforced for THIS launcher,
        # not the global /usr/bin/vlc binary or unrelated applications.
        if not re.search(r'(?<!\S)--intf(?:=|\s+)qt(?=\s|$)', command):
            command = command[:match.end()] + ' --intf=qt' + command[match.end():]
        if '--no-qt-video-autoresize' not in command:
            command = command.replace('--intf=qt', '--intf=qt --no-qt-video-autoresize', 1)
        # No extra desktop entry: this style override is specific to VLC only.
        if command.startswith('env '):
            command = 'env QT_STYLE_OVERRIDE=AlternixVLC ' + command[4:]
        else:
            command = 'env QT_STYLE_OVERRIDE=AlternixVLC ' + command
        output.append('Exec=' + command + '\n')
        changed_exec += 1
    if changed_exec:
        updated = ''.join(output)
        if updated != raw:
            backup = Path('/usr/share/alternix/vlc/vlc.desktop.original')
            backup.parent.mkdir(parents=True, exist_ok=True)
            if not backup.exists():
                backup.write_text(raw, encoding='utf-8')
                backup.chmod(0o644)
            launcher.write_text(updated, encoding='utf-8')
            launcher.chmod(0o644)
        print(f'[VLC] Updated {changed_exec} existing desktop Exec line(s) for native Qt.')
    else:
        print('[VLC] WARNING: /usr/share/applications/vlc.desktop has no recognized VLC command.')
else:
    print('[VLC] WARNING: distro VLC launcher not present: /usr/share/applications/vlc.desktop')

print('[VLC] Alternix Qt player layout and theme selected; standard VLC window remains under Qtile.')
ALT_VLC_CONFIGURATION
if command -v update-desktop-database >/dev/null 2>&1; then
    sudo update-desktop-database /usr/share/applications 2>/dev/null || true
    sudo update-desktop-database "$ALT_VLC_HOME/.local/share/applications" 2>/dev/null || true
fi
if [ ! -s /usr/share/Kvantum/AlternixVLC/AlternixVLC.svg ] || \
   [ ! -s "$ALT_VLC_HOME/.config/vlc/vlc-qt-interface.conf" ]; then
    echo "ERROR: AlternixVLC native Qt theme installation failed verification." >&2
    exit 1
fi
# Do not force-close VLC during an installation or updater (unsaved playback
# state could be lost). Close all running VLC instances after the update.
if pgrep -u "$ALT_VLC_USER" -x vlc >/dev/null 2>&1; then
    echo "[VLC] NOTE: VLC is running. Exit all VLC windows/tray instances before reopening it."
fi
echo "[VLC] Compiled Alternix Qt theme installed; VLC uses a single tiled native window."

# ------------------------------------------------
# AlternixOS clock actions: osm-sudo pattern authentication + a fixed helper.
# The helper cannot run arbitrary commands and requires no CLI sudo password.
# ------------------------------------------------
echo "[Time] Configuring osm-sudo clock permissions..."
sudo install -d -m 0755 /usr/local/sbin
sudo /bin/sh -c 'cat > /usr/local/sbin/alternix-time-helper' <<'ALT_TIME_HELPER_CONTENT'
#!/bin/sh
# Installed by AlternixOS. Root-owned and callable only for specific clock actions.
# There is intentionally NO arbitrary command, shell, or file argument interface.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
umask 022

if [ "$(id -u)" -ne 0 ]; then
    echo "TIME_HELPER_DENIED: elevated privileges were not granted"
    exit 1
fi

case "${1:-}" in
    status)
        [ "$#" -eq 1 ] || exit 2
        echo TIME_HELPER_READY
        exit 0
        ;;
    set|network-set)
        [ "$#" -eq 2 ] || { echo "Invalid clock request"; exit 2; }
        # Only accept a numeric Unix timestamp; never evaluate shell input.
        printf '%s\n' "$2" | LC_ALL=C grep -Eq '^[0-9]{9,11}(\.[0-9]{1,3})?$' || {
            echo "Invalid clock timestamp"; exit 2;
        }
        YEAR=$(date -u -d "@$2" +%Y 2>/dev/null) || {
            echo "Invalid clock timestamp"; exit 2;
        }
        [ "$YEAR" -ge 2000 ] && [ "$YEAR" -le 2099 ] || {
            echo "Timestamp outside supported range"; exit 2;
        }
RTCLOG=""
FAKELOG=""
RTC_PRESENT=0
HW=""
RTC_MODE=UTC
[ -f /etc/adjtime ] && [ "$(sed -n '3p' /etc/adjtime 2>/dev/null)" = LOCAL ] && RTC_MODE=LOCAL

rtc_detect() {
  for r in /sys/class/rtc/rtc*; do
    [ -d "$r" ] || continue
    RTC_PRESENT=1
    RTCLOG="$RTCLOG
Detected $r ($(cat "$r/name" 2>/dev/null))"
  done
  for r in /dev/rtc /dev/rtc[0-9]* /dev/misc/rtc; do
    [ -e "$r" ] && RTC_PRESENT=1
  done
  [ -r /proc/driver/rtc ] && RTC_PRESENT=1
  RTCLOG="$RTCLOG
RTC time scale from /etc/adjtime: $RTC_MODE"
}

rtc_readback_ok() {
  for r in /sys/class/rtc/rtc*; do
    [ -r "$r/date" ] && [ -r "$r/time" ] || continue
    RD=$(cat "$r/date" 2>/dev/null)
    RT=$(cat "$r/time" 2>/dev/null)
    [ -n "$RD" ] && [ -n "$RT" ] || continue
    if [ "$RTC_MODE" = LOCAL ]; then
      RE=$(date -d "$RD $RT" +%s 2>/dev/null) || continue
    else
      RE=$(date -u -d "$RD $RT" +%s 2>/dev/null) || continue
    fi
    NOW=$(date +%s)
    D=$((RE - NOW))
    [ "$D" -lt 0 ] && D=$((-D))
    [ "$D" -le 5 ] && return 0
    RTCLOG="$RTCLOG
$r reads $RD $RT ($RTC_MODE); ${D}s away from system time"
  done
  return 1
}

rtc_try() {
  O=$("$HW" "$@" 2>&1)
  RC=$?
  if [ "$RC" -eq 0 ]; then return 0; fi
  RTCLOG="$RTCLOG
$HW $*: $O"
  if rtc_readback_ok; then
    RTCLOG="$RTCLOG
RTC read-back agrees with the corrected system time"
    return 0
  fi
  return 1
}

rtc_write() {
  rtc_detect
  for c in /usr/sbin/hwclock /sbin/hwclock /usr/bin/hwclock /bin/hwclock; do
    if [ -x "$c" ]; then HW="$c"; break; fi
  done
  if [ -z "$HW" ]; then
    if command -v hwclock >/dev/null 2>&1; then
      HW=$(command -v hwclock)
    fi
  fi
  if [ -z "$HW" ]; then
    if command -v dpkg-query >/dev/null 2>&1 &&
       dpkg-query -W -f='${Status}' util-linux-extra 2>/dev/null | grep -q 'install ok installed'; then
      RTCLOG="$RTCLOG
The hwclock executable could not be located, but util-linux-extra is installed. This is not an RTC hardware failure."
    else
      RTCLOG="$RTCLOG
The hwclock executable could not be located. Check whether util-linux-extra is installed. This does not mean the RTC chip is missing."
    fi
    return 1
  fi

  # First use the machine's existing /etc/adjtime mode.
  rtc_try --systohc && return 0
  for d in /dev/rtc /dev/rtc[0-9]* /dev/misc/rtc; do
    [ -e "$d" ] || continue
    rtc_try --systohc --rtc "$d" && return 0
  done

  # Bypass a broken/missing adjtime file, but KEEP its LOCAL/UTC mode.
  rtc_try --systohc --noadjfile "--$( [ "$RTC_MODE" = LOCAL ] && echo localtime || echo utc )" && return 0
  for d in /dev/rtc /dev/rtc[0-9]* /dev/misc/rtc; do
    [ -e "$d" ] || continue
    rtc_try --systohc --noadjfile "--$( [ "$RTC_MODE" = LOCAL ] && echo localtime || echo utc )" --rtc "$d" && return 0
  done

  # Never use direct ISA writes on modern tablets as a blind fallback.
  RTCLOG="$RTCLOG
Linux could not WRITE the RTC. The hardware may still be present and readable.
$($HW --show --verbose 2>&1 | head -n 16)"
  return 1
}

fake_clock_save() {
  if command -v fake-hwclock >/dev/null 2>&1; then
    O=$(fake-hwclock save 2>&1)
    if [ $? -eq 0 ]; then
      FAKELOG="Saved an approximate boot timestamp to disk (not a real RTC)."
      return 0
    fi
    FAKELOG="fake-hwclock save failed: $O"
    return 1
  fi
  FAKELOG="No RTC is exposed to Linux and fake-hwclock is not installed."
  return 1
}

rtc_store() {
  RTC=ok
  if ! rtc_write; then
    if [ "$RTC_PRESENT" -eq 1 ]; then
      RTC=unwritable
    else
      RTC=unavailable
      fake_clock_save && RTC=fake
    fi
  fi
}
        if ! O=$(date -u -s "@$2" 2>&1); then
            if [ "$1" = set ]; then echo OSMSET_FAIL; else echo OSMSYNC_FAIL; fi
            echo "date -u -s: $O"
            exit 1
        fi
        rtc_store
        if [ "$1" = set ]; then
            echo "OSMSET_OK:$RTC"
        else
            echo "OSMSYNC_OK:$RTC:network NTP server"
        fi
        echo "$RTCLOG"
        echo "$FAKELOG"
        exit 0
        ;;
    chrony)
        [ "$#" -eq 1 ] || exit 2
        echo CHRONY_REACHED
        chronyc -a online 2>&1
        chronyc -a 'burst 4/4' 2>&1
        chronyc -n waitsync 10 1 100 2 2>&1
        RES=$?
        chronyc -n tracking 2>&1
        if [ "$RES" -eq 0 ]; then echo CHRONY_SYNC_OK
        else echo CHRONY_SYNC_WAIT; fi
        exit 0
        ;;
    http-sync)
        [ "$#" -eq 1 ] || exit 2
RTCLOG=""
FAKELOG=""
RTC_PRESENT=0
HW=""
RTC_MODE=UTC
[ -f /etc/adjtime ] && [ "$(sed -n '3p' /etc/adjtime 2>/dev/null)" = LOCAL ] && RTC_MODE=LOCAL

rtc_detect() {
  for r in /sys/class/rtc/rtc*; do
    [ -d "$r" ] || continue
    RTC_PRESENT=1
    RTCLOG="$RTCLOG
Detected $r ($(cat "$r/name" 2>/dev/null))"
  done
  for r in /dev/rtc /dev/rtc[0-9]* /dev/misc/rtc; do
    [ -e "$r" ] && RTC_PRESENT=1
  done
  [ -r /proc/driver/rtc ] && RTC_PRESENT=1
  RTCLOG="$RTCLOG
RTC time scale from /etc/adjtime: $RTC_MODE"
}

rtc_readback_ok() {
  for r in /sys/class/rtc/rtc*; do
    [ -r "$r/date" ] && [ -r "$r/time" ] || continue
    RD=$(cat "$r/date" 2>/dev/null)
    RT=$(cat "$r/time" 2>/dev/null)
    [ -n "$RD" ] && [ -n "$RT" ] || continue
    if [ "$RTC_MODE" = LOCAL ]; then
      RE=$(date -d "$RD $RT" +%s 2>/dev/null) || continue
    else
      RE=$(date -u -d "$RD $RT" +%s 2>/dev/null) || continue
    fi
    NOW=$(date +%s)
    D=$((RE - NOW))
    [ "$D" -lt 0 ] && D=$((-D))
    [ "$D" -le 5 ] && return 0
    RTCLOG="$RTCLOG
$r reads $RD $RT ($RTC_MODE); ${D}s away from system time"
  done
  return 1
}

rtc_try() {
  O=$("$HW" "$@" 2>&1)
  RC=$?
  if [ "$RC" -eq 0 ]; then return 0; fi
  RTCLOG="$RTCLOG
$HW $*: $O"
  if rtc_readback_ok; then
    RTCLOG="$RTCLOG
RTC read-back agrees with the corrected system time"
    return 0
  fi
  return 1
}

rtc_write() {
  rtc_detect
  for c in /usr/sbin/hwclock /sbin/hwclock /usr/bin/hwclock /bin/hwclock; do
    if [ -x "$c" ]; then HW="$c"; break; fi
  done
  if [ -z "$HW" ]; then
    if command -v hwclock >/dev/null 2>&1; then
      HW=$(command -v hwclock)
    fi
  fi
  if [ -z "$HW" ]; then
    if command -v dpkg-query >/dev/null 2>&1 &&
       dpkg-query -W -f='${Status}' util-linux-extra 2>/dev/null | grep -q 'install ok installed'; then
      RTCLOG="$RTCLOG
The hwclock executable could not be located, but util-linux-extra is installed. This is not an RTC hardware failure."
    else
      RTCLOG="$RTCLOG
The hwclock executable could not be located. Check whether util-linux-extra is installed. This does not mean the RTC chip is missing."
    fi
    return 1
  fi

  # First use the machine's existing /etc/adjtime mode.
  rtc_try --systohc && return 0
  for d in /dev/rtc /dev/rtc[0-9]* /dev/misc/rtc; do
    [ -e "$d" ] || continue
    rtc_try --systohc --rtc "$d" && return 0
  done

  # Bypass a broken/missing adjtime file, but KEEP its LOCAL/UTC mode.
  rtc_try --systohc --noadjfile "--$( [ "$RTC_MODE" = LOCAL ] && echo localtime || echo utc )" && return 0
  for d in /dev/rtc /dev/rtc[0-9]* /dev/misc/rtc; do
    [ -e "$d" ] || continue
    rtc_try --systohc --noadjfile "--$( [ "$RTC_MODE" = LOCAL ] && echo localtime || echo utc )" --rtc "$d" && return 0
  done

  # Never use direct ISA writes on modern tablets as a blind fallback.
  RTCLOG="$RTCLOG
Linux could not WRITE the RTC. The hardware may still be present and readable.
$($HW --show --verbose 2>&1 | head -n 16)"
  return 1
}

fake_clock_save() {
  if command -v fake-hwclock >/dev/null 2>&1; then
    O=$(fake-hwclock save 2>&1)
    if [ $? -eq 0 ]; then
      FAKELOG="Saved an approximate boot timestamp to disk (not a real RTC)."
      return 0
    fi
    FAKELOG="fake-hwclock save failed: $O"
    return 1
  fi
  FAKELOG="No RTC is exposed to Linux and fake-hwclock is not installed."
  return 1
}

rtc_store() {
  RTC=ok
  if ! rtc_write; then
    if [ "$RTC_PRESENT" -eq 1 ]; then
      RTC=unwritable
    else
      RTC=unavailable
      fake_clock_save && RTC=fake
    fi
  fi
}
LOG=""
SRC=""
HAVE_CURL=0; command -v curl >/dev/null 2>&1 && HAVE_CURL=1
HAVE_WGET=0; command -v wget >/dev/null 2>&1 && HAVE_WGET=1
if [ "$HAVE_CURL" = 0 ] && [ "$HAVE_WGET" = 0 ]; then
  LOG="$LOG
neither curl nor wget is installed, so there is no way to fetch the time"
fi
for url in http://deb.devuan.org/ https://deb.devuan.org/ http://www.google.com/; do
  D=""
  if [ "$HAVE_CURL" = 1 ]; then
    D=$(curl -sI -m 8 "$url" 2>/dev/null | grep -i '^date:' | head -n1 | sed 's/^[Dd][Aa][Tt][Ee]:[ ]*//' | tr -d '\r')
  fi
  if [ -z "$D" ] && [ "$HAVE_WGET" = 1 ]; then
    D=$(wget -S --spider -T 8 -q -O /dev/null "$url" 2>&1 | grep -i '^ *date:' | head -n1 | sed 's/^ *[Dd][Aa][Tt][Ee]:[ ]*//' | tr -d '\r')
  fi
  if [ -n "$D" ]; then
    if date -u -s "$D" >/dev/null 2>&1; then SRC="the web server at $url"; break; fi
    LOG="$LOG
could not apply the date from $url: $D"
  else
    LOG="$LOG
no date sent by $url"
  fi
done
if [ -z "$SRC" ]; then
  echo OSMSYNC_FAIL
  echo "$LOG"
  exit 1
fi
rtc_store
echo "OSMSYNC_OK:$RTC:$SRC"
echo "$LOG"
echo "$RTCLOG"
echo "$FAKELOG"
exit 0
        ;;
    *)
        echo "Unsupported time operation"
        exit 2
        ;;
esac
ALT_TIME_HELPER_CONTENT
sudo chown root:root /usr/local/sbin/alternix-time-helper
sudo chmod 0755 /usr/local/sbin/alternix-time-helper
# No NOPASSWD clock exception: all administrative actions now use
# sudo's standard PAM pattern/PIN prompt. Existing exceptions are removed
# at the end of installation by alternix-auth-setup.sh.
if ! sudo test -x /usr/local/sbin/alternix-time-helper; then
    echo "ERROR: Alternix clock helper is missing or not executable." >&2
    exit 1
fi
echo "[Time] Clock helper installed (PAM authentication applies)."


#echo "[Config] Installing updated configs..."
#cp -r "$ALT_ROOT/Alternix/configs/." "$HOME/.config/"

# ────────────────────────────────────────────────
# Devuan / elogind: stop unexpected tablet sleep
# Applied on the reboot already required by this updater.
# ────────────────────────────────────────────────
echo "[Config] Updating elogind power management..."
sudo mkdir -p /etc/elogind/logind.conf.d /etc/elogind/sleep.conf.d

sudo tee /etc/elogind/logind.conf.d/alternix-power.conf >/dev/null <<'ELOGINDFILE'
[Login]
HandlePowerKey=ignore
HandleSuspendKey=ignore
HandleHibernateKey=ignore
HandleLidSwitch=ignore
HandleLidSwitchExternalPower=ignore
HandleLidSwitchDocked=ignore
IdleAction=ignore
ELOGINDFILE

sudo tee /etc/elogind/sleep.conf.d/alternix-no-sleep.conf >/dev/null <<'SLEEPFILE'
[Sleep]
AllowSuspend=no
AllowHibernation=no
AllowHybridSleep=no
AllowSuspendThenHibernate=no
SLEEPFILE

# Keep the power-button fallback for older elogind versions without drop-ins.
sudo sed -i 's/^#*HandlePowerKey=.*/HandlePowerKey=ignore/' /etc/elogind/logind.conf 2>/dev/null || true

echo "[✓] elogind sleep prevention configured (effective after restart)."

if command -v visor >/dev/null 2>&1; then
    visor update
else
    echo "• Visor command not found; skipping Visor update."
fi


echo ""
#===========================================================
# Apps to be updated (compilation commands & icons):
#===========================================================
echo ""
# echo "No Apps need updating..."


echo "• Creating fetch.desktop launcher..."
sudo rm /usr/share/applications/fetch.desktop
sudo tee /usr/share/applications/fetch.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=About
Comment=System monitor
Exec=alacritty -e fetch
Terminal=false
Icon=fetch
Categories=System;
EOF

echo ""

# ────────────────────────────────────────────────
# Install all .deb packages in ~/Alternix/installers/
# ────────────────────────────────────────────────
echo " "
#echo "[System] Installing local .deb packages..."
#
#INSTALLER_DIR="$ALT_ROOT/Alternix/installers"
#
#if [ -d "$INSTALLER_DIR" ]; then
#    DEB_COUNT=$(ls -1 "$INSTALLER_DIR"/*.deb 2>/dev/null | wc -l)
#
#    if [ "$DEB_COUNT" -gt 0 ]; then
#        echo "• Found $DEB_COUNT installer package(s). Installing..."
#
#        # Install each .deb file
#        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true
#
#        # Fix missing dependencies automatically
#        #sudo apt-get update -y
#        sudo nala install -f -y
#
#        # try to Install each .deb file AGAIN
#        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true
#
#        echo "• Local installer packages installed."
#    else
#        echo "• No .deb files in installers folder, skipping."
#    fi
#else
#    echo "• No installers folder found, skipping."
#fi

echo "• Updating rounded-corners..."
# Download a clean copy on each update; no undefined helper or directory variables.
if ! command -v git >/dev/null 2>&1; then
    echo "ERROR: git is required to update Rounded Corners." >&2
    exit 1
fi
ROUNDED_CORNERS_TMP="$(mktemp -d)" || {
    echo "ERROR: Could not create a temporary Rounded Corners download directory." >&2
    exit 1
}
if git clone --depth 1 https://github.com/DansDesigns/rounded_corners.git \
        "$ROUNDED_CORNERS_TMP/rounded_corners" &&
   (cd "$ROUNDED_CORNERS_TMP/rounded_corners" && bash ./install_corners.sh); then
    echo "[✓] Rounded Corners updated."
else
    echo "ERROR: Rounded Corners download or installation failed." >&2
    rm -rf -- "$ROUNDED_CORNERS_TMP"
    exit 1
fi
rm -rf -- "$ROUNDED_CORNERS_TMP"

#echo " "
#echo "[Config] Installing Extras..."
#
#if [ -d "$ALT_ROOT/Alternix/extras" ]; then
#    mkdir "$HOME/extras"
#    sudo cp -r "$ALT_ROOT/Alternix/extras/"* "$HOME/extras/"
#
#    echo "• Extras installed successfully."
#else
#    echo "--------------------------------"
#    echo "• [ERROR] Installing Extras..."
#    echo "--------------------------------"
#fi

# ~~~~~~~~~~~~~~~~~~~~~~~~
#    Settings Pages
# ~~~~~~~~~~~~~~~~~~~~~~~~

cd "$ALT_ROOT/Alternix/apps/osm-settings"

echo "• Updating osm-settings..."
g++ osm-settings.cpp -o osm-settings -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-settings && sudo mv osm-settings /usr/local/bin/


echo "• Updating wifi.so..."
g++ -fPIC -shared wifi.cpp -o wifi.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 wifi.so /usr/local/bin/

echo "• Updating bluetooth.so..."
g++ -fPIC -shared bluetooth.cpp -o bluetooth.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 bluetooth.so /usr/local/bin/

echo "• Updating apps.so..."
g++ -fPIC -shared apps.cpp -o apps.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 apps.so /usr/local/bin/

echo "• Updating mobile.so..."
g++ -fPIC -shared mobile.cpp -o mobile.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 mobile.so /usr/local/bin/

echo "• Updating location.so..."
g++ location.cpp -o location.so -shared -fPIC -O2 $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 location.so /usr/local/bin/

echo "• Updating date-time.so..."
g++ date-time.cpp -o date-time.so -shared -fPIC -O2 $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 date-time.so /usr/local/bin/

echo "• Updating battery.so..."
g++ -fPIC -shared battery.cpp -o battery.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 battery.so /usr/local/bin/

echo "• Updating emulation.so..."
g++ emulation.cpp -o emulation.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 emulation.so /usr/local/bin/

echo "• Updating security.so..."
g++ -shared -fPIC security.cpp -o security.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo install -m755 security.so /usr/local/bin/

echo "• Updating ethernet.so..."
g++ -fPIC -shared ethernet.cpp -o ethernet.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 ethernet.so /usr/local/bin/

echo "• Updating display.so..."
g++ display.cpp -o display.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 display.so /usr/local/bin/

echo "• Updating sound.so..."
g++ -std=c++17 -fPIC -shared sound.cpp -o sound.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo install -m755 sound.so /usr/local/bin/

echo "• Updating kernel.so..."
g++ -std=c++17 -fPIC -shared kernel.cpp -o kernel.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo install -m755 kernel.so /usr/local/bin/

echo "• Updating storage.so..."
g++ -fPIC -shared storage.cpp -o storage.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 storage.so /usr/local/bin/

echo "• Updating accounts.so..."
g++ -std=c++11 accounts.cpp -o accounts.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 accounts.so /usr/local/bin/

echo "• Updating system.so..."
g++ -fPIC -shared system.cpp -o system.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 system.so /usr/local/bin/

echo "• Updating ui.so..."
g++ -fPIC -shared ui.cpp -o ui.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 ui.so /usr/local/bin/

echo "• Updating reticulum.so..."
g++ -std=c++17 -Wall -Wextra -fPIC -shared -o reticulum.so reticulum.cpp $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 reticulum.so /usr/local/bin/reticulum.so


# ~~~~~~~~~~~~~~~~~~~~~~~~
#          APPS
# ~~~~~~~~~~~~~~~~~~~~~~~~

cd "$ALT_ROOT/Alternix"


# Keep the graphical authentication command consistent with the Location module.
echo "• Updating osm-sudo..."
sudo install -m 755 apps/osm-sudo /usr/local/bin/osm-sudo

echo "• Updating osm-kernels..."
g++ -fPIC apps/osm-kernels.cpp -o osm-kernels $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m 755 osm-kernels /usr/local/bin/

if [ -f "icons/osm-kernels.png" ]; then
    sudo cp icons/osm-kernels.png /usr/share/icons/hicolor/64x64/apps/osm-kernels.png
fi

# The .desktop file is generated here; make sure its destination exists.
mkdir -p "$HOME/.local/share/applications"
cat <<EOF > "$HOME/.local/share/applications/osm-kernels.desktop"
[Desktop Entry]
Type=Application
Name=Kernels
Comment=Install, remove and choose Linux kernels
Exec=osm-kernels
Icon=osm-kernels
Terminal=false
Categories=System;Settings;
Keywords=kernel;linux;mainline;liquorix;xanmod;grub;
EOF
chmod +x "$HOME/.local/share/applications/osm-kernels.desktop"

echo "• Updating osm-lockscreen..."
g++ -fPIC apps/osm-lockscreen.cpp -o osm-lockscreen $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m 755 osm-lockscreen /usr/local/bin/

echo "• Updating osm-launcher..."
g++ -O3 -fPIC apps/osm-launcher.cpp -o osm-launcher $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m 755 osm-launcher /usr/local/bin/

echo "• Building welcome (responsive fullscreen onboarding)..."
g++ -std=c++17 -O3 -fPIC apps/welcome.cpp -o welcome $(pkg-config --cflags --libs Qt5Widgets) || exit 1
sudo install -m 0755 welcome /usr/local/bin/welcome || exit 1
sudo install -D -m 0644 icons/welcome.png /usr/share/icons/hicolor/256x256/apps/welcome.png || exit 1
sudo install -d -m 0755 /usr/share/alternix/welcome || exit 1
sudo install -m 0644 assets/welcome/top-bar.png /usr/share/alternix/welcome/top-bar.png || exit 1
sudo install -m 0644 assets/welcome/bottom-bar.png /usr/share/alternix/welcome/bottom-bar.png || exit 1
# One global launcher: osm-launcher scans /usr/share/applications by default.
sudo tee /usr/share/applications/welcome.desktop >/dev/null <<'ALT_WELCOME_DESKTOP'
[Desktop Entry]
Version=1.0
Type=Application
Name=Welcome
GenericName=AlternixOS Welcome
Comment=Touch-friendly introduction to AlternixOS
Exec=/usr/local/bin/welcome
Icon=welcome
Terminal=false
StartupNotify=false
Categories=Utility;Education;
ALT_WELCOME_DESKTOP
if command -v update-desktop-database >/dev/null 2>&1; then
    sudo update-desktop-database /usr/share/applications 2>/dev/null || true
fi

# Keep the long-running edge-gesture services in sync with rotated screens.
# The previous updater omitted osm-running and osm-status entirely, leaving
# their old fixed-geometry swipe bars active even after a successful update.
echo "• Updating osm-running (rotation-aware left edge)..."
g++ -fPIC apps/osm-running.cpp -o osm-running -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11 || exit 1
sudo install -m 755 osm-running /usr/local/bin/ || exit 1

echo "• Updating osm-status (rotation-aware right edge)..."
g++ -fPIC apps/osm-status.cpp -o osm-status -ldl $(pkg-config --cflags --libs Qt5Widgets Qt5DBus) -lX11 || exit 1
sudo install -m 755 osm-status /usr/local/bin/ || exit 1

echo "• Updating osm-notify (rotation-aware top swipe)..."
g++ -fPIC apps/osm-notify.cpp -o osm-notify $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core Qt5DBus) -lX11 -lXtst || exit 1
sudo install -m 755 osm-notify /usr/local/bin/ || exit 1

echo "• Updating osm-clock..."
g++ apps/osm-clock.cpp -o osm-clock -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-clock && sudo mv osm-clock /usr/local/bin/

echo "• Updating osm-power..."
g++ -fPIC apps/osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/

echo "• Updating osm-files..."
g++ -fPIC apps/osm-files.cpp -o osm-files $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-files && sudo mv osm-files /usr/local/bin/

echo "• Updating osm-viewer..."
g++ -fPIC apps/osm-viewer.cpp -o osm-viewer $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core poppler-qt5) -Wno-deprecated-declarations
chmod +x osm-viewer && sudo mv osm-viewer /usr/local/bin/

echo "• Updating osm-draw..."
g++ -fPIC apps/osm-draw.cpp -o osm-draw -std=c++17 $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-draw && sudo mv osm-draw /usr/local/bin/

echo "• Updating osm-paper..."
g++ -fPIC apps/osm-paper.cpp -o osm-paper $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-paper && sudo mv osm-paper /usr/local/bin/

echo "• Installing osm-paper-restore (rotation-safe full redraw)..."
sudo install -m 755 "$ALT_PAPER_RESTORE_SOURCE" /usr/local/bin/ || exit 1

# Only update the wallpaper hook inside the installed Qtile config. The
# updater intentionally does NOT overwrite personal keybindings, layouts,
# groups, widgets, or user customization with the full repository config.
echo "• Enabling wallpaper redraw after Qtile screen rotation..."
if ! sudo python3 - "$ALT_PAPER_QTILE_SOURCE" "$ALT_VLC_HOME/.config/qtile/config.py" <<'ALT_PAPER_QTILE_INSTALL'
from pathlib import Path
import ast
import os
import shutil
import sys

source = Path(sys.argv[1]); target = Path(sys.argv[2])
start_marker = '# --- Alternix wallpaper refresh after XRandR/Qtile reconfiguration ---'
end_marker = '# --- End Alternix wallpaper refresh ---'
s = source.read_text(encoding='utf-8')
assert s.count(start_marker) == 1 and s.count(end_marker) == 1
hook = s[s.index(start_marker):s.index(end_marker) + len(end_marker)]
if target.exists():
    current = target.read_text(encoding='utf-8')
    if start_marker in current:
        if end_marker not in current:
            raise ValueError('Existing wallpaper refresh section has no end marker')
        prior = current[current.index(start_marker):current.index(end_marker) + len(end_marker)]
        updated = current.replace(prior, hook, 1)
    else:
        updated = current.rstrip('\n') + '\n\n' + hook + '\n'
    ast.parse(updated, filename=str(target))
    if updated != current:
        target.write_text(updated, encoding='utf-8')
        print('Qtile: updated wallpaper hook, retained existing configuration')
    else:
        print('Qtile: wallpaper hook already current, no changes necessary')
else:
    ast.parse(s, filename=str(source))
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    print('Qtile: installed complete configuration for missing user config')
ALT_PAPER_QTILE_INSTALL
then
    echo "ERROR: Could not update live Qtile wallpaper rotation hook." >&2
    exit 1
fi
sudo chown "$ALT_VLC_USER:$(id -gn "$ALT_VLC_USER")" \
    "$ALT_VLC_HOME/.config/qtile/config.py" || exit 1
sudo install -Dm644 "$ALT_PAPER_QTILE_SOURCE" /etc/skel/.config/qtile/config.py || exit 1


echo "• Updating osm-lockscreen..."
g++ -fPIC apps/osm-lockscreen.cpp -o osm-lockscreen $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m 755 osm-lockscreen /usr/local/bin/

echo "• Updating osm-launcher-lock-cleaner..."
# Resolve the source in the repository instead of assuming it is in the working directory.
LOCKCLEAN_SRC="$(find "$ALT_ROOT" -type f -name 'osm-launcher-lockclean' -print -quit)"
if [ -n "$LOCKCLEAN_SRC" ]; then
    sudo install -m755 "$LOCKCLEAN_SRC" /usr/local/bin/osm-launcher-lockclean
elif [ -f /usr/local/bin/osm-launcher-lockclean ]; then
    # Previous installers moved the original source into /usr/local/bin.
    sudo chmod 755 /usr/local/bin/osm-launcher-lockclean
    echo "• osm-launcher-lockclean is already installed."
else
    echo "[ERROR] osm-launcher-lockclean was not found in the repo or /usr/local/bin."
fi

echo "• Updating Pong2 Game..."

# Find the source directory, using the same repository search as Breakout.
PONG_SRC="$ALT_ROOT/Alternix/extras/games"
if [ ! -f "$PONG_SRC/Pong2.html" ]; then
    PONG_SRC="$ALT_ROOT/extras/games"
fi

# Resolve the desktop user's home before creating the launcher.
if [ -z "${TARGET_HOME:-}" ]; then
    TARGET_USER="${SUDO_USER:-$(id -un)}"
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
fi

if [ ! -f "$PONG_SRC/Pong2.html" ]; then
    echo "✗ Pong2 source not found: $PONG_SRC/Pong2.html"

elif [ ! -d "$TARGET_HOME" ] || [ "$TARGET_HOME" = "/root" ]; then
    echo "✗ Invalid target home: $TARGET_HOME"

else
    TARGET_USER="$(stat -c '%U' "$TARGET_HOME")"
    TARGET_GROUP="$(stat -c '%G' "$TARGET_HOME")"

    PONG_DEST="$TARGET_HOME/extras/games"
    PONG_DESKTOP_DIR="$TARGET_HOME/.local/share/applications"

    # Create the target directories with the correct desktop-user ownership.
    sudo install -d -o "$TARGET_USER" -g "$TARGET_GROUP" \
        "$TARGET_HOME/extras" \
        "$PONG_DEST" \
        "$TARGET_HOME/.local" \
        "$TARGET_HOME/.local/share" \
        "$PONG_DESKTOP_DIR"

    # Update the actual game, not only its launcher.
    echo "• Installing Pong2 HTML..."
    sudo install -m 644 \
        -o "$TARGET_USER" -g "$TARGET_GROUP" \
        "$PONG_SRC/Pong2.html" \
        "$PONG_DEST/Pong2.html"

    # Install/update the icon if present.
    if [ -f "$PONG_SRC/Pong2.png" ]; then
        echo "• Installing Pong2 icon..."
        sudo install -D -m 644 \
            "$PONG_SRC/Pong2.png" \
            /usr/share/icons/hicolor/64x64/apps/Pong2.png
    else
        echo "⚠ Pong2 icon not found; skipping."
    fi

    # Create the user-owned launcher, matching Breakout's Chromium app mode.
    echo "• Creating Pong2 launcher..."
    sudo tee "$PONG_DESKTOP_DIR/Pong2.desktop" > /dev/null <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Pong2
Comment=Two-player touch Pong
Exec=brave-browser --app=file://$PONG_DEST/Pong2.html --new-window --force-device-scale-factor=1.3
Icon=Pong2
Terminal=false
Categories=Game;
StartupNotify=true
EOF

    sudo chown "$TARGET_USER:$TARGET_GROUP" \
        "$PONG_DESKTOP_DIR/Pong2.desktop"
    sudo chmod 755 "$PONG_DESKTOP_DIR/Pong2.desktop"

    # Verify the files needed to launch the game are installed.
    if [ -f "$PONG_DEST/Pong2.html" ] && \
       [ -f "$PONG_DESKTOP_DIR/Pong2.desktop" ]; then
        echo "✓ Pong2 Game updated successfully."
        echo "  HTML: $PONG_DEST/Pong2.html"
        echo "  Launcher: $PONG_DESKTOP_DIR/Pong2.desktop"
    else
        echo "✗ Pong2 installation failed."
    fi
fi


echo "• Updating Breakout Game..."

# Find the source directory.
GAME_SRC="$ALT_ROOT/Alternix/extras/games"
if [ ! -f "$GAME_SRC/breakout.html" ]; then
    GAME_SRC="$ALT_ROOT/extras/games"
fi

# Find the actual user's home directory if not already set.
if [ -z "${TARGET_HOME:-}" ]; then
    TARGET_USER="${SUDO_USER:-$(id -un)}"
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
fi

if [ ! -f "$GAME_SRC/breakout.html" ]; then
    echo "✗ Breakout source not found: $GAME_SRC/breakout.html"

elif [ ! -d "$TARGET_HOME" ] || [ "$TARGET_HOME" = "/root" ]; then
    echo "✗ Invalid target home: $TARGET_HOME"

else
    TARGET_USER="$(stat -c '%U' "$TARGET_HOME")"
    TARGET_GROUP="$(stat -c '%G' "$TARGET_HOME")"

    GAME_DEST="$TARGET_HOME/extras/games"
    DESKTOP_DIR="$TARGET_HOME/.local/share/applications"

    # Create directories with correct user ownership.
    sudo install -d -o "$TARGET_USER" -g "$TARGET_GROUP" \
        "$TARGET_HOME/extras" \
        "$GAME_DEST" \
        "$TARGET_HOME/.local" \
        "$TARGET_HOME/.local/share" \
        "$DESKTOP_DIR"

    # Update the HTML game.
    echo "• Installing Breakout HTML..."
    sudo install -m 644 \
        -o "$TARGET_USER" -g "$TARGET_GROUP" \
        "$GAME_SRC/breakout.html" \
        "$GAME_DEST/breakout.html"

    # Update the game icon.
    if [ -f "$GAME_SRC/breakout.png" ]; then
        echo "• Installing Breakout icon..."
        sudo install -D -m 644 \
            "$GAME_SRC/breakout.png" \
            /usr/share/icons/hicolor/64x64/apps/breakout.png
    else
        echo "⚠ Breakout icon not found; skipping."
    fi

    # Create the desktop launcher.
    echo "• Creating Breakout launcher..."

    sudo tee "$DESKTOP_DIR/breakout.desktop" > /dev/null <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Breakout
Comment=Break the block game
Exec=brave-browser --app=file://$GAME_DEST/breakout.html --new-window --force-device-scale-factor=1.3
Icon=breakout
Terminal=false
Categories=Game;
StartupNotify=true
EOF

    sudo chown "$TARGET_USER:$TARGET_GROUP" \
        "$DESKTOP_DIR/breakout.desktop"
    sudo chmod 755 "$DESKTOP_DIR/breakout.desktop"

    # Verify installation.
    if [ -f "$GAME_DEST/breakout.html" ] && \
       [ -f "$DESKTOP_DIR/breakout.desktop" ]; then
        echo "✓ Breakout Game updated successfully."
        echo "  HTML: $GAME_DEST/breakout.html"
        echo "  Launcher: $DESKTOP_DIR/breakout.desktop"
    else
        echo "✗ Breakout installation failed."
    fi
fi

# Keep the user's existing wallpapers, updating files from the repository.
# Run the copy as the desktop user, even if this updater was started as root.
echo "[Config] Installing Wallpapers..."
WALL_SRC="$ALT_ROOT/Alternix/wallpapers"
if [ ! -d "$WALL_SRC" ]; then
    echo "---------------------------------------"
    echo "• No wallpapers folder found, skipping."
    echo "---------------------------------------"
elif [ -z "${TARGET_HOME:-}" ] || [ ! -d "$TARGET_HOME" ] || \
     [ "$TARGET_HOME" = /root ] || [ -z "${TARGET_USER:-}" ] || \
     [ "$TARGET_USER" = root ]; then
    echo "WARNING: Could not identify a valid desktop user/home for wallpapers; skipping." >&2
else
    WALL_DST="$TARGET_HOME/Pictures/wallpapers"
    if [ "$(id -u)" -eq 0 ]; then
        # Avoid root-owned files in the user's Pictures directory.
        if runuser -u "$TARGET_USER" -- mkdir -p "$WALL_DST" &&
           runuser -u "$TARGET_USER" -- cp -R "$WALL_SRC/." "$WALL_DST/"; then
            echo "• Wallpapers installed to $WALL_DST"
        else
            echo "WARNING: Failed to copy wallpapers to $WALL_DST" >&2
        fi
    elif [ "$(id -un)" = "$TARGET_USER" ]; then
        if mkdir -p "$WALL_DST" && cp -R "$WALL_SRC/." "$WALL_DST/"; then
            echo "• Wallpapers installed to $WALL_DST"
        else
            echo "WARNING: Failed to copy wallpapers to $WALL_DST" >&2
        fi
    else
        echo "WARNING: Wallpaper target user differs from current user; skipping." >&2
    fi
fi


echo "• Compiling osm-powerd..."
sudo g++ -O2 apps/osm-powerd.cpp -o osm-powerd
sudo chmod +x osm-powerd && sudo mv osm-powerd /usr/local/bin/
sudo chown root:root /usr/local/bin/osm-powerd
sudo chmod 4755 /usr/local/bin/osm-powerd

#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo "• Removing old lockscreen service..."
# The old service ran the lockscreen as a separate user with no access
# to the display, so it crashed and restarted endlessly. The lockscreen
# now runs in the user's own session instead.
if [ -f /etc/init.d/osm-lockscreen ]; then
    sudo service osm-lockscreen stop
    sudo update-rc.d -f osm-lockscreen remove
    sudo rm -f /etc/init.d/osm-lockscreen
fi
if id lockscreen >/dev/null 2>&1; then
    sudo pkill -u lockscreen
fi

echo "• Updating osm-lockd..."
sudo tee /usr/local/bin/osm-lockd >/dev/null <<'LOCKD'
#!/bin/bash

# Only one lockscreen at a time. Waking and the Lock button can both
# start this, and a second copy would stack another lockscreen on top.
exec 9>"/tmp/osm-lockd-$(id -u).lock"
flock -n 9 || exit 0

FLAG="/tmp/osm_unlock_success"

while true; do
    rm -f "$FLAG"
    /usr/local/bin/osm-lockscreen 9>&-

    if [ -f "$FLAG" ]; then
        rm -f "$FLAG"
        # signal the user session (osm-status plays the boot sound once per boot)
        touch /tmp/osm_boot_unlocked 2>/dev/null
        chmod 666 /tmp/osm_boot_unlocked 2>/dev/null
        exit 0
    fi

    sleep 0.05
done
LOCKD
sudo chmod +x /usr/local/bin/osm-lockd

echo "• Installing dbus-fast for Qtile..."
if [ -x "$HOME/.qtile_venv/bin/pip" ]; then
    "$HOME/.qtile_venv/bin/pip" install dbus-fast
elif [ -x "$HOME/.qtile_venv/bin/python3" ] && \
     "$HOME/.qtile_venv/bin/python3" -m pip --version >/dev/null 2>&1; then
    "$HOME/.qtile_venv/bin/python3" -m pip install dbus-fast
else
    echo "• Qtile virtual environment pip not found; skipping optional dbus-fast installation."
fi

#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
echo "• Updating desktop widgets..."
cd "$ALT_ROOT/Alternix/apps/osm-widgets" || { echo "ERROR: apps/osm-widgets folder missing"; exit 1; }

echo "• Updating osm-widgets..."
g++ -fPIC osm-widgets.cpp -o osm-widgets -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11 -lXext -lXi
sudo install -m 755 osm-widgets /usr/local/bin/

echo "• Updating calendar widget..."
g++ -I. -std=c++11 -fPIC -shared widgets/calendar.cpp -o calendar.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -Dm755 calendar.so /usr/local/lib/alternix/widgets/calendar.so

echo "• Updating 13-moon-calendar widget..."
g++ -I. -std=c++11 -fPIC -shared widgets/moon-calendar.cpp -o moon-calendar.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -Dm755 moon-calendar.so /usr/local/lib/alternix/widgets/moon-calendar.so

echo "• Updating tzolkin widget..."
g++ -I. -std=c++11 -fPIC -shared widgets/tzolkin.cpp -o tzolkin.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -Dm755 tzolkin.so /usr/local/lib/alternix/widgets/tzolkin.so

#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo "• Updating Icons..."
cd "$ALT_ROOT/Alternix"
sudo install -Dm644 icons/*.png /usr/share/icons/hicolor/64x64/apps/

echo "• Updating sounds..."
cp -r "$ALT_ROOT/Alternix/sounds" ~/.config/Alternix/

echo "• Updating scripts (including touchscreen/stylus/rotation support)..."
ALT_INPUT_USER="$ALT_VLC_USER"
ALT_INPUT_HOME="$ALT_VLC_HOME"
ALT_INPUT_GROUP="$(id -gn "$ALT_INPUT_USER")"
sudo install -d -o "$ALT_INPUT_USER" -g "$ALT_INPUT_GROUP" -m 0755 \
    "$ALT_INPUT_HOME/.config/Alternix/scripts"
# Retain the normal update of all scripts but deploy them to the actual
# desktop account, rather than $HOME (which may be /root under sudo).
sudo cp -R "$ALT_ROOT/Alternix/scripts/." "$ALT_INPUT_HOME/.config/Alternix/scripts/"
for alt_input_file in \
    alternix-rotate-monitor.sh \
    alternix-rotate-setup.sh \
    alternix-rotate-toggle.sh \
    alternix-stylus-apply.sh; do
    sudo install -o "$ALT_INPUT_USER" -g "$ALT_INPUT_GROUP" -m 0755 \
        "$ALT_INPUT_SOURCE_DIR/$alt_input_file" \
        "$ALT_INPUT_HOME/.config/Alternix/scripts/$alt_input_file"
done
sudo chmod +x "$ALT_INPUT_HOME/.config/Alternix/scripts/alternix-audio-fix.sh"
sudo chown -R "$ALT_INPUT_USER:$ALT_INPUT_GROUP" "$ALT_INPUT_HOME/.config/Alternix/scripts"

# /etc/skel serves future accounts as well as the already-installed one.
sudo install -d -m 0755 /etc/skel/.config/Alternix/scripts
for alt_input_file in \
    alternix-rotate-monitor.sh \
    alternix-rotate-setup.sh \
    alternix-rotate-toggle.sh \
    alternix-stylus-apply.sh; do
    sudo install -m 0755 "$ALT_INPUT_SOURCE_DIR/$alt_input_file" \
        "/etc/skel/.config/Alternix/scripts/$alt_input_file"
done

# Preserve other startup commands. Add only this guarded block to existing
# Qtile startx sessions; don't overwrite an unrelated/custom .xinitrc.
sudo python3 - "$ALT_INPUT_HOME/.xinitrc" <<'ALT_INPUT_XINITRC'
from pathlib import Path
import sys
path = Path(sys.argv[1])
if not path.is_file():
    print('[Input] No .xinitrc to update; existing session launcher left untouched.')
else:
    raw = path.read_text(encoding='utf-8')
    if '# AlternixOS X11 pen/touch + auto-rotation login restoration.' in raw:
        print('[Input] Qtile login restoration already installed.')
    else:
        lines = raw.splitlines(keepends=True)
        pos = next((i for i, line in enumerate(lines)
                    if line.lstrip().startswith('exec ') and 'qtile' in line), None)
        if pos is None:
            print('[Input] No Qtile exec line in .xinitrc; leaving user startup unchanged.')
        else:
            hook = r"""
# AlternixOS X11 pen/touch + auto-rotation login restoration.
# Qtile does not automatically run ~/.config/autostart/*.desktop entries.
# These settings only run after the user deliberately saves/enables them.
ALT_INPUT_LOGIN_DIR="$HOME/.config/Alternix/scripts"
if [ -s "$HOME/.config/Alternix/stylus.conf" ] && \
   [ -x "$ALT_INPUT_LOGIN_DIR/alternix-stylus-apply.sh" ]; then
    (sleep 1; /bin/bash "$ALT_INPUT_LOGIN_DIR/alternix-stylus-apply.sh" apply) >/dev/null 2>&1 &
fi
if [ -x "$ALT_INPUT_LOGIN_DIR/alternix-rotate-toggle.sh" ]; then
    if [ -f "$HOME/.config/autostart/alternix-auto-rotate.desktop" ]; then
        touch "$HOME/.config/Alternix/rotate-autostart-migrated"
        (sleep 1; /bin/bash "$ALT_INPUT_LOGIN_DIR/alternix-rotate-toggle.sh" on --startup) >/dev/null 2>&1 &
    elif [ ! -e "$HOME/.config/Alternix/rotate-autostart-migrated" ] && \
         grep -Eq '^display_auto_rotate=true[[:space:]]*$' \
            "$HOME/.config/Alternix/osm-settings.conf" 2>/dev/null; then
        # Migrate the old preference once. If later disabled from osm-notify,
        # a stale Settings preference must NOT re-enable it at next login.
        touch "$HOME/.config/Alternix/rotate-autostart-migrated"
        (sleep 1; /bin/bash "$ALT_INPUT_LOGIN_DIR/alternix-rotate-toggle.sh" on) >/dev/null 2>&1 &
    fi
fi
# End AlternixOS input login restoration.
"""
            lines.insert(pos, hook)
            path.write_text(''.join(lines), encoding='utf-8')
            print('[Input] Installed pen/touch and auto-rotate startup in Qtile .xinitrc.')
ALT_INPUT_XINITRC

# Ensure Welcome starts after Qtile begins on the next installed-system login.
# The existing .xinitrc was amended above for pen/rotation; keep those hooks.
# We do not add Welcome to .config/autostart because Qtile ignores XDG entries.
sudo python3 - "$ALT_INPUT_HOME/.xinitrc" /etc/skel/.xinitrc <<'ALT_WELCOME_XINITRC'
from pathlib import Path
import sys

hook = r'''# AlternixOS Welcome first-login guide. The --once mode checks the current
# live/installed boot and leaves a completion marker only at the last page.
# The short delay lets Qtile register as the window manager before fullscreen.
if [ -x /usr/local/bin/welcome ]; then
    /usr/local/bin/welcome --once --startup-delay >/dev/null 2>&1 &
fi
# End AlternixOS Welcome first-login guide.
'''

for name in sys.argv[1:]:
    file = Path(name)
    if not file.is_file() or file.is_symlink():
        print('[Welcome] Existing Qtile startup not found (or is a symlink):', file)
        continue
    contents = file.read_text(encoding='utf-8')
    if '# AlternixOS Welcome first-login guide.' in contents:
        print('[Welcome] Existing first-login hook retained:', file)
        continue
    lines = contents.splitlines(keepends=True)
    idx = next((n for n, line in enumerate(lines)
                if line.lstrip().startswith('exec ') and 'qtile' in line), None)
    if idx is None:
        print('[Welcome] Unrecognized/custom .xinitrc, left unchanged:', file)
        continue
    lines.insert(idx, hook)
    file.write_text(''.join(lines), encoding='utf-8')
    print('[Welcome] Added one-time installed-system Qtile startup:', file)
ALT_WELCOME_XINITRC

# ────────────────────────────────────────────────
# Install/activate the SAME one-time hardware-aware audio setup used by
# fresh AlternixOS installations. --install checks its completion marker:
# systems provisioned already are left alone, older systems are configured.
# No login-time recurring scanner is added and no sound server is restarted.
# Perform this before the final PAM activation/verification transaction.
# ────────────────────────────────────────────────
echo "[Audio] Updating the one-time audio hardware setup utility..."
ALT_AUDIO_SETUP_FAILED=0
if ! sudo install -m 0755 "$ALT_AUDIO_SOURCE" /usr/local/bin/alternix-audio-fix.sh; then
    echo "WARNING: Could not install alternix-audio-fix.sh into /usr/local/bin." >&2
    ALT_AUDIO_SETUP_FAILED=1
elif ! sudo bash /usr/local/bin/alternix-audio-fix.sh --install; then
    echo "WARNING: One-time audio setup is incomplete. Review /var/log/alternix/audio-*.log; it will retry on a future updater run." >&2
    ALT_AUDIO_SETUP_FAILED=1
fi

echo "• Updating touchscroll from Alternix/scripts..."
sudo install -d -o "$ALT_INPUT_USER" -g "$ALT_INPUT_GROUP" -m 0755 \
    "$ALT_INPUT_HOME/.local/bin" || exit 1
sudo install -o "$ALT_INPUT_USER" -g "$ALT_INPUT_GROUP" -m 0755 \
    "$ALT_INPUT_SOURCE_DIR/alternix-touchscroll.py" \
    "$ALT_INPUT_HOME/.local/bin/alternix-touchscroll.py" || exit 1

echo "• Updating Android session from Alternix/scripts..."
sudo install -m 0755 "$ALT_INPUT_SOURCE_DIR/alternix-waydroid-session" \
    /usr/local/bin/alternix-waydroid-session || exit 1

echo "• Updating Windows launcher from Alternix/scripts..."
sudo install -m 0755 "$ALT_INPUT_SOURCE_DIR/alternix-exe" \
    /usr/local/bin/alternix-exe || exit 1





echo "• App Update & Install Complete."
echo ""
#===========================================================
# Updater:
#===========================================================
echo "• Updating Alternix Updater..."
sudo install -m 755 "$ALT_ROOT/update/os-check-update" /usr/bin/

# The desktop shortcut starts Alacritty itself, so Terminal must be false.
# Keep the system entry consistent with the fresh-install installer.
echo "• Updating Alternix Updater shortcut..."
sudo tee /usr/share/applications/os-check-update.desktop >/dev/null <<'UPDATER_DESKTOP'
[Desktop Entry]
Name=System Update
Exec=alacritty -e /usr/bin/os-check-update
Icon=os-check-update
Type=Application
Terminal=false
Categories=System;
UPDATER_DESKTOP

# A user-level entry overrides the system entry. Keep any existing copy
# consistent rather than leaving the old broken launcher active.
if [ -f "$HOME/.local/share/applications/os-check-update.desktop" ]; then
    install -m 644 /usr/share/applications/os-check-update.desktop \
        "$HOME/.local/share/applications/os-check-update.desktop"
fi

#=========================================
# Create Folder if not already existing:
#sudo mkdir /usr/share/alternix
#=========================================

# Update Version Number:
echo "• Updating Version Number..."
sudo cp "$ALT_ROOT/update/version.txt" /usr/share/alternix/version.txt


echo "• Updating udev rules..."
# Symlink qtile binary to where udev rules expect it
sudo mkdir -p /usr/lib/udev
sudo ln -sf "$HOME/.qtile_venv/bin/qtile" /usr/lib/udev/qtile
sudo udevadm control --reload-rules

echo "• Updating usermod access..."
sudo usermod -aG video,input "$USER"
#=========================================
# Un-Comment to Update the App Launcher:

#echo "• Updating Launcher..."
#sudo tee /usr/share/applications/os-check-update.desktop >/dev/null <<EOF
#[Desktop Entry]
#Name=System Update
#Exec= alacritty -e /usr/bin/os-check-update
#Icon=os-check-update
#Type=Application
#Terminal=true
#Categories=System;
#EOF

echo " "
echo " "
echo " "
echo " "
# Keep the authentication sources in a protected system-owned location for
# future updates and for the installed system to finish enrolment if needed.
sudo install -d -o root -g root -m 0755 /usr/share/alternix/auth
for src in alternix-auth-setup.sh alternix-auth.c osm-lock.cpp; do
    sudo install -o root -g root -m 0644 \
        "$ALT_ROOT/Alternix/apps/$src" "/usr/share/alternix/auth/$src"
done
sudo chmod 0755 /usr/share/alternix/auth/alternix-auth-setup.sh

# ────────────────────────────────────────────────
# AlternixOS terminal policy: remove xterm + uxterm and use Alacritty.
# Run after all package operations, so dependencies cannot pull xterm back
# into this installation/update. Do not autoremove unrelated X11 packages.
# ────────────────────────────────────────────────
echo "[Terminal] Enforcing Alacritty as the default terminal..."
if ! command -v alacritty >/dev/null 2>&1; then
    sudo apt-get install -y alacritty
fi
ALTERNIX_ALACRITTY="$(command -v alacritty || true)"
if [ -z "$ALTERNIX_ALACRITTY" ] || [ ! -x "$ALTERNIX_ALACRITTY" ]; then
    echo "ERROR: Alacritty is unavailable; refusing to remove the existing terminal." >&2
    exit 1
fi

if dpkg-query -W -f='${Status}' xterm 2>/dev/null | grep -qx 'install ok installed'; then
    # Make sure the purge would NOT remove any other packages (e.g. an X server).
    if ! ALT_XTERM_SIMULATION="$(sudo apt-get -s purge xterm)"; then
        echo "ERROR: Could not safely simulate removing xterm." >&2
        exit 1
    fi
    ALT_XTERM_EXTRA_REMOVALS="$(printf '%s\n' "$ALT_XTERM_SIMULATION" | awk '$1 == "Remv" && $2 != "xterm" {print $2}')"
    if [ -n "$ALT_XTERM_EXTRA_REMOVALS" ]; then
        echo "ERROR: Purging xterm would also remove: $ALT_XTERM_EXTRA_REMOVALS" >&2
        echo "Refusing to remove other packages. Check the x-terminal-emulator dependencies." >&2
        exit 1
    fi
    sudo env DEBIAN_FRONTEND=noninteractive apt-get purge -y xterm
fi

# Register Alacritty even if this package version did not register itself.
sudo update-alternatives --install /usr/bin/x-terminal-emulator \
    x-terminal-emulator "$ALTERNIX_ALACRITTY" 80
sudo update-alternatives --set x-terminal-emulator "$ALTERNIX_ALACRITTY"

if dpkg-query -W -f='${Status}' xterm 2>/dev/null | grep -qx 'install ok installed'; then
    echo "ERROR: The xterm package is still installed." >&2
    exit 1
fi
if [ "$(readlink -f /usr/bin/x-terminal-emulator)" != "$(readlink -f "$ALTERNIX_ALACRITTY")" ]; then
    echo "ERROR: x-terminal-emulator does not resolve to Alacritty." >&2
    exit 1
fi
echo "[Terminal] xterm/uxterm removed; x-terminal-emulator -> $ALTERNIX_ALACRITTY"

# Commit the sudo authentication policy as the LAST privileged operation.
# This also installs the protected verifier and new osm-lock executable.
echo "[Security] Testing and activating Alternix sudo PAM authentication..."
# Setup is a root-owned transaction: the old PAM/sudoers files are restored
# automatically unless a fresh, real sudo/PAM challenge succeeds.
if ! sudo bash "$ALT_ROOT/Alternix/apps/alternix-auth-setup.sh" \
        "$ALT_TIME_USER" "$ALT_ROOT/Alternix/apps"; then
    echo "ERROR: Pattern authentication did not pass its live test; activation was rolled back." >&2
    exit 1
fi

# All system modifications and PAM tests are complete. Restart only the
# existing, same-X-session gesture apps so the newly installed binaries are
# actually active without requiring a reboot. Never spawn apps that the user
# did not already have running, and never terminate another X11 session.
if [ "$(id -un)" = "$ALT_INPUT_USER" ] && [ -n "${DISPLAY:-}" ]; then
    echo "[Input] Activating updated touch mapping and gesture binaries for $DISPLAY..."
    ALT_INPUT_RUNTIME="$ALT_INPUT_HOME/.config/Alternix/scripts"
    # The monitor's old shell process keeps old code until it is restarted.
    if [ -x "$ALT_INPUT_RUNTIME/alternix-rotate-toggle.sh" ]; then
        /bin/bash "$ALT_INPUT_RUNTIME/alternix-rotate-toggle.sh" restart || \
            echo 'WARNING: Rotation monitor restart was not confirmed.' >&2
        /bin/bash "$ALT_INPUT_RUNTIME/alternix-rotate-monitor.sh" --sync-input || \
            echo 'WARNING: Input mapping could not be confirmed; inspect the rotation log.' >&2
    fi
    for alt_overlay in osm-running osm-status osm-notify; do
        overlay_pids=()
        while IFS= read -r overlay_pid; do
            [ -n "$overlay_pid" ] || continue
            if [ -r "/proc/$overlay_pid/environ" ] && \
               tr '\0' '\n' < "/proc/$overlay_pid/environ" | \
                   grep -Fxq "DISPLAY=$DISPLAY"; then
                overlay_pids+=("$overlay_pid")
            fi
        done < <(pgrep -u "$ALT_INPUT_USER" -x "$alt_overlay" 2>/dev/null || true)
        if (( ${#overlay_pids[@]} == 0 )); then continue; fi
        echo "[Input] Restarting $alt_overlay on $DISPLAY..."
        kill -TERM "${overlay_pids[@]}" 2>/dev/null || :
        # Wait for process exit / Qt lock and notification socket release.
        for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
            running=0
            for overlay_pid in "${overlay_pids[@]}"; do
                if kill -0 "$overlay_pid" 2>/dev/null; then running=1; fi
            done
            [ "$running" = 0 ] && break
            sleep 0.25
        done
        if [ "$running" = 0 ]; then
            nohup "/usr/local/bin/$alt_overlay" >/dev/null 2>&1 </dev/null &
        else
            echo "WARNING: $alt_overlay was not restarted; it will refresh after logout/reboot." >&2
        fi
    done
else
    echo '[Input] Session not directly accessible; new gesture binaries activate at next desktop login.'
fi

echo "=============================================="
echo "         Alternix Update Complete!"
echo "=============================================="
if [ "${ALT_AUDIO_SETUP_FAILED:-0}" = 1 ]; then
    echo "WARNING: Audio provisioning was incomplete; check /var/log/alternix/."
fi
echo " "
echo "=============================================="
echo "       THIS UPDATE REQUIRES A RESTART"
echo "=============================================="
