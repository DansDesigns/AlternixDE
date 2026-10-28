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

# Do not accidentally reinstall an outdated Location module. The updater
# compiles the repository's location.cpp, not a copy bundled in this script.
# Older versions contained the misleading clock-permission error and would
# overwrite the corrected module even if the time helper is authorised.
ALT_LOCATION_SOURCE="$ALT_ROOT/Alternix/apps/osm-settings/location.cpp"
if [ ! -f "$ALT_LOCATION_SOURCE" ] || \
   ! grep -Fq 'Clock already synchronised by Chrony' "$ALT_LOCATION_SOURCE" || \
   ! grep -Fq 'System sudo handles authentication through its PAM service' "$ALT_LOCATION_SOURCE"; then
    echo "ERROR: The repository does not contain the corrected location.cpp." >&2
    echo "Install the corrected source at Alternix/apps/osm-settings/location.cpp" >&2
    echo "before updating, or the old clock-permission failure will return." >&2
    exit 1
fi

# The normal sudo PAM mechanism handles all privilege elevation.
# No direct -n probe and no bypassing credential requests.
if ! /usr/bin/sudo -v; then
    echo "ERROR: Alternix sudo authentication failed or access was denied." >&2
    exit 1
fi

sudo nala install -y libssl-dev fonts-urw-base35 fonts-freefont-ttf wmctrl cmake g++ bluez

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

visor update


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


cd "$ALT_ROOT/Alternix/apps/osm-settings"

echo "• Updating osm-settings..."
g++ osm-settings.cpp -o osm-settings -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-settings && sudo mv osm-settings /usr/local/bin/

echo "• Building wifi.so..."
g++ -fPIC -shared wifi.cpp -o wifi.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 wifi.so /usr/local/bin/

echo "• Building display.so..."
g++ display.cpp -o display.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 display.so /usr/local/bin/

echo "• Building bluetooth.so..."
g++ -fPIC -shared bluetooth.cpp -o bluetooth.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 bluetooth.so /usr/local/bin/

echo "• Updating reticulum.so..."
g++ -std=c++17 -Wall -Wextra -fPIC -shared -o reticulum.so reticulum.cpp $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 reticulum.so /usr/local/bin/

echo "• Building storage.so..."
g++ -fPIC -shared storage.cpp -o storage.so $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 storage.so /usr/local/bin/

echo "• Building system.so..."
g++ -fPIC -shared system.cpp -o system.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo install -m755 system.so /usr/local/bin/

# Update Location alongside the clock helper so existing installations use
# the same helper-based clock actions as fresh installations. Never deploy an
# old/stale location.so if this compilation fails.
echo "• Building location.so..."
if ! g++ location.cpp -o location.so -shared -fPIC -O2 $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core); then
    echo "ERROR: Could not compile location.cpp. Existing installed location.so was left unchanged." >&2
    exit 1
fi
if ! sudo install -m755 location.so /usr/local/bin/location.so; then
    echo "ERROR: Could not install the newly built location.so." >&2
    exit 1
fi


cd "$ALT_ROOT/Alternix"

# Keep the graphical authentication command consistent with the Location module.
echo "• Updating osm-sudo..."
sudo install -m 755 apps/osm-sudo /usr/local/bin/osm-sudo

echo "• Building osm-kernels..."
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


echo "• Building osm-notify..."
g++ -fPIC apps/osm-notify.cpp -o osm-notify $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core Qt5DBus) -lX11 -lXtst
chmod +x osm-notify && sudo mv osm-notify /usr/local/bin/


echo "• Building osm-clock..."
g++ apps/osm-clock.cpp -o osm-clock -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-clock && sudo mv osm-clock /usr/local/bin/

echo "• Updating osm-power..."
g++ -fPIC apps/osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/

echo "• Building osm-files..."
g++ -fPIC apps/osm-files.cpp -o osm-files $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-files && sudo mv osm-files /usr/local/bin/

echo "• Building osm-viewer..."
g++ -fPIC apps/osm-viewer.cpp -o osm-viewer $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core poppler-qt5) -Wno-deprecated-declarations
chmod +x osm-viewer && sudo mv osm-viewer /usr/local/bin/

echo "• Building osm-draw..."
g++ -fPIC apps/osm-draw.cpp -o osm-draw -std=c++17 $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-draw && sudo mv osm-draw /usr/local/bin/

echo "• Building osm-paper..."
g++ -fPIC apps/osm-paper.cpp -o osm-paper $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-paper && sudo mv osm-paper /usr/local/bin/

echo "• Building osm-launcher-lock-cleaner..."
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
"$HOME/.qtile_venv/bin/pip" install dbus-fast

#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~



echo "• Updating Icons..."
cd "$ALT_ROOT/Alternix"
sudo install -Dm644 icons/*.png /usr/share/icons/hicolor/64x64/apps/

echo "• Updating sounds..."
cp -r "$ALT_ROOT/Alternix/sounds" ~/.config/Alternix/

echo "• Updating scripts..."
cp -r "$ALT_ROOT/Alternix/scripts" ~/.config/Alternix/
cd ~/.config/Alternix/scripts
chmod +x alternix-rotate-monitor.sh
chmod +x alternix-rotate-setup.sh
chmod +x alternix-rotate-toggle.sh

chmod +x alternix-touchscroll.py
cp alternix-touchscroll.py ~/.local/bin/alternix-touchscroll.py

chmod +x alternix-waydroid-session
sudo cp alternix-waydroid-session /usr/local/bin

chmod +x alternix-exe
sudo cp alternix-exe /usr/local/bin





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
# Commit the sudo authentication policy as the LAST privileged operation.
# This also installs the protected verifier and new osm-lock executable.
echo "[Security] Activating Alternix sudo PAM authentication..."
sudo bash "$ALT_ROOT/Alternix/apps/alternix-auth-setup.sh" "$ALT_TIME_USER" "$ALT_ROOT/Alternix/apps"

echo "=============================================="
echo "         Alternix Update Complete!"
echo "=============================================="
echo " "
echo "=============================================="
echo "       THIS UPDATE REQUIRES A RESTART"
echo "=============================================="
