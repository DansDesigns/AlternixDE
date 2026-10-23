#!/usr/bin/env bash
# AlternixOS / Devuan Excalibur audio repair and hardware diagnosis
# Runtime repair runs as the desktop user. Root/image builds defer hardware
# and user-session repair rather than failing live-build. Compatible with SysVinit.
set -u
set -o pipefail

MODE=repair
case "${1:-}" in
  ''|--repair) ;;
  --diagnose|--diagnostics) MODE=diagnose ;;
  --session-only) MODE=session ;;
  --image-prep) MODE=image ;;
  -h|--help)
    printf 'Usage: bash %s [--repair|--diagnose|--session-only|--image-prep]\n' "$0"
    exit 0 ;;
  *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
esac

# live-build runs the Alternix installer as root in a chroot, before a real
# logged-in desktop user, ALSA hardware, or a PulseAudio D-Bus session exists.
# The image should contain the packages and script; runtime configuration must
# happen on first user login, not during ISO construction.
if [ "$MODE" = image ]; then
  echo '[Alternix Audio] Image build: runtime audio repair deferred until first user login.'
  exit 0
fi
if [ "$(id -u)" -eq 0 ]; then
  echo '[Alternix Audio] Root/ISO installer detected: skipping user-session audio configuration.'
  echo '[Alternix Audio] Configure the audio script to run --session-only at the first desktop login.'
  exit 0
fi

STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/alternix"
mkdir -p "$STATE_DIR" || exit 1
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$STATE_DIR/audio-$STAMP.log"
exec > >(tee -a "$LOG") 2>&1

say()  { printf '\n[Alternix Audio] %s\n' "$*"; }
warn() { printf '[WARNING] %s\n' "$*"; }
has()  { command -v "$1" >/dev/null 2>&1; }
installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -qx 'install ok installed'; }
file_text() { if [ -r "$1" ]; then cat "$1"; else echo '(unavailable)'; fi; }

MODEL=$(cat /sys/class/dmi/id/product_name 2>/dev/null || echo unknown)
VENDOR=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || echo unknown)
SOUND_ID=$( (cat /proc/asound/card*/codec\#* 2>/dev/null || true) | grep -im1 '^Codec:' || true)
IS_ELITEPAD=0
IS_APPLE=0
IS_CS8409=0
case "$MODEL $VENDOR" in *ElitePad*|*elitepad*) IS_ELITEPAD=1;; esac
case "$MODEL $VENDOR" in *Apple*|*iMac*) IS_APPLE=1;; esac
if grep -q -i 'Codec:.*CS8409' /proc/asound/card*/codec\#* 2>/dev/null; then IS_CS8409=1; fi

say "Mode: $MODE | $(date -Is)"
printf 'Hardware: %s | %s\n' "$VENDOR" "$MODEL"
printf 'Kernel: %s\n' "$(uname -r)"
printf 'OS: %s\n' "$(grep '^PRETTY_NAME=' /etc/os-release 2>/dev/null | cut -d= -f2- | tr -d '"')"
printf 'User: %s | Groups: %s\n' "$(id -un)" "$(id -nG)"
printf 'Display: %s | XDG_RUNTIME_DIR: %s\n' "${DISPLAY:-unset}" "${XDG_RUNTIME_DIR:-unset}"
printf 'Codec hint: %s\n' "${SOUND_ID:-unknown}"

say 'Hardware detected by the kernel (before repair)'
file_text /proc/asound/cards
if has aplay; then aplay -l 2>&1 || true; fi
if has lspci; then lspci -nnk | grep -i -A4 -E 'Audio device|Multimedia audio controller' || true; fi
if has pactl; then
  echo '-- PulseAudio / PipeWire state --'
  pactl info 2>&1 || true
  pactl list short sinks 2>&1 || true
fi

show_diagnostics() {
  say 'Final diagnostics'
  printf '%s\n' '-- ALSA cards --'
  file_text /proc/asound/cards
  if has aplay; then aplay -l 2>&1 || true; fi
  printf '%s\n' '-- HDA codec identification (if present) --'
  for codec in /proc/asound/card*/codec\#*; do
    [ -r "$codec" ] || continue
    printf '%s\n' "$codec"
    head -n 6 "$codec" 2>/dev/null || true
  done
  printf '%s\n' '-- Sound server --' 
  if has pactl; then
    pactl info 2>&1 || true
    pactl list short sinks 2>&1 || true
    pactl list sinks 2>&1 | grep -E 'Name: |Description: |Active Port: |analog-output|Mute: ' | head -n 70 || true
  fi
  printf '%s\n' '-- Kernel audio warnings (if readable) --'
  if has dmesg; then
    dmesg 2>/dev/null | grep -iE 'snd|audio|codec|sst|sof|firmware' | tail -n 40 || true
  fi
  printf '%s\n' '-- Installed sound packages --'
  dpkg-query -W 'pulseaudio*' 'pipewire*' 'wireplumber' 'alsa-ucm-conf' \
    'firmware-intel-sound' 'firmware-sof-signed' 2>/dev/null || true
  printf '\nReport saved to: %s\n' "$LOG"
}

if [ "$MODE" = diagnose ]; then
  show_diagnostics
  exit 0
fi

if ! has apt-get || ! has dpkg-query; then
  warn 'This repair is designed for Devuan/Debian systems with APT.'
  show_diagnostics
  exit 1
fi
if [ "$MODE" != session ] && ! has sudo; then
  warn 'sudo is missing; cannot install packages. Run with --diagnose to collect a report.'
  show_diagnostics
  exit 1
fi

if [ "$MODE" = session ]; then
  say 'Session-only mode: not running APT or sudo (safe for first desktop login)'
else
  say 'Checking required ALSA and audio-server packages'
# Alternix has traditionally used PulseAudio. Keep a working PipeWire-Pulse
# installation intact: installing both competing servers can break audio.
PACKAGES=(alsa-utils alsa-ucm-conf alsa-topology-conf pavucontrol)
if installed pipewire-pulse; then
  say 'PipeWire-Pulse found: preserving it (no PulseAudio replacement).'
  PACKAGES+=(pipewire wireplumber)
else
  say 'PulseAudio selected (matches the current Alternix installer).'
  PACKAGES+=(pulseaudio pulseaudio-utils)
fi

# Firmware only when the system advertises related Intel audio hardware.
if [ "$IS_ELITEPAD" -eq 1 ] || {
    has lspci && lspci -nn 2>/dev/null | grep -qiE '(Intel Corporation.*(Audio|SST|Smart Sound)|Audio.*Intel Corporation)';
  }; then
  PACKAGES+=(firmware-intel-sound)
fi
if [ "$IS_ELITEPAD" -eq 0 ] && has lspci && \
   lspci -nn 2>/dev/null | grep -qiE '(Intel Corporation.*(Audio|Smart Sound)|Audio.*Intel Corporation)'; then
  PACKAGES+=(firmware-sof-signed)
fi

MISSING=()
for pkg in "${PACKAGES[@]}"; do
  if ! installed "$pkg"; then MISSING+=("$pkg"); fi
done
if [ "${#MISSING[@]}" -gt 0 ]; then
  if ! sudo apt-get update; then
    warn 'APT index refresh failed. Will only install packages already available in existing lists.'
  fi
  TO_INSTALL=()
  for pkg in "${MISSING[@]}"; do
    candidate=$(apt-cache policy "$pkg" 2>/dev/null | awk '/Candidate:/ {print $2; exit}')
    if [ -n "$candidate" ] && [ "$candidate" != '(none)' ]; then
      TO_INSTALL+=("$pkg")
    else
      warn "$pkg is unavailable from your configured repositories."
      case "$pkg" in
        firmware-*) warn 'Firmware may require non-free-firmware enabled in the Devuan Excalibur sources.';;
      esac
    fi
  done
  if [ "${#TO_INSTALL[@]}" -gt 0 ]; then
    say "Installing: ${TO_INSTALL[*]}"
    if ! sudo apt-get install -y "${TO_INSTALL[@]}"; then
      warn 'APT could not install all components; see the output above.'
    fi
  fi
else
  say 'All selected audio packages are already installed.'
fi
fi  # end of package installation; --session-only skips all APT operations

say 'Checking user audio-server state'
if has pactl && pactl info >/dev/null 2>&1; then
  say 'A compatible PulseAudio protocol server is already running.'
elif installed pipewire-pulse; then
  # Devuan with SysVinit/Qtile does not necessarily have a user service manager.
  # Start these in the existing desktop login session only, never as root.
  if [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -d "/run/user/$(id -u)" ]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
  fi
  if has pipewire && ! pgrep -u "$(id -u)" -x pipewire >/dev/null 2>&1; then
    (pipewire >>"$STATE_DIR/pipewire.log" 2>&1 &) || true
  fi
  if has wireplumber && ! pgrep -u "$(id -u)" -x wireplumber >/dev/null 2>&1; then
    (wireplumber >>"$STATE_DIR/wireplumber.log" 2>&1 &) || true
  fi
  if has pipewire-pulse && ! pgrep -u "$(id -u)" -f '^pipewire-pulse($| )' >/dev/null 2>&1; then
    (pipewire-pulse >>"$STATE_DIR/pipewire-pulse.log" 2>&1 &) || true
  fi
  sleep 2
else
  if has pulseaudio; then
    pulseaudio --start 2>&1 || warn 'PulseAudio did not start; inspect the session and report.'
    sleep 1
  fi
fi

say 'Unmuting existing ALSA playback controls (no kernel/module changes)'
if has amixer; then
  for cardpath in /proc/asound/card[0-9]*; do
    [ -d "$cardpath" ] || continue
    card="${cardpath##*card}"
    for control in Master Speaker PCM; do
      if amixer -c "$card" sget "$control" >/dev/null 2>&1; then
        amixer -c "$card" sset "$control" unmute >/dev/null 2>&1 || true
      fi
    done
  done
fi
if has pactl && pactl info >/dev/null 2>&1; then
  DEFAULT_SINK=$(pactl get-default-sink 2>/dev/null || \
    pactl info | sed -n 's/^Default Sink: //p' | head -n 1)
  if [ "$DEFAULT_SINK" = auto_null ]; then
    ANALOG_SINK=$(pactl list short sinks | awk '$2 ~ /analog|speaker/ {print $2; exit}')
    if [ -n "$ANALOG_SINK" ]; then
      say "Selecting detected analogue sink: $ANALOG_SINK"
      pactl set-default-sink "$ANALOG_SINK" || true
    else
      warn 'Only a dummy audio output exists; this suggests a driver/firmware issue.'
    fi
  fi
  pactl set-sink-mute @DEFAULT_SINK@ 0 >/dev/null 2>&1 || true
else
  warn 'No responding audio server. This may be a desktop-session startup issue.'
fi

say 'Adding reliable audio autostart to the current Alternix startx/Qtile session'
XINIT="$HOME/.xinitrc"
WRAPPER="$HOME/.local/bin/alternix-audio-session"
mkdir -p "$HOME/.local/bin"
cat > "$WRAPPER" <<'WRAPPER_EOF'
#!/bin/sh
# Start sound as the logged-in user *inside* the session D-Bus environment.
# Qtile/startx does not process freedesktop XDG autostart entries by default.
if [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -d "/run/user/$(id -u)" ]; then
  export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi
if command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1; then
  : # Audio is already running.
elif command -v pipewire-pulse >/dev/null 2>&1 && \
    dpkg-query -W -f='${Status}' pipewire-pulse 2>/dev/null | grep -qx 'install ok installed'; then
  uid=$(id -u)
  if command -v pipewire >/dev/null 2>&1 && ! pgrep -u "$uid" -x pipewire >/dev/null 2>&1; then
    pipewire >/dev/null 2>&1 &
  fi
  sleep 1
  if command -v wireplumber >/dev/null 2>&1 && ! pgrep -u "$uid" -x wireplumber >/dev/null 2>&1; then
    wireplumber >/dev/null 2>&1 &
  fi
  if ! pgrep -u "$uid" -f '^pipewire-pulse($| )' >/dev/null 2>&1; then
    pipewire-pulse >/dev/null 2>&1 &
  fi
elif command -v pulseaudio >/dev/null 2>&1; then
  pulseaudio --start >/dev/null 2>&1 || true
fi
exec "$@"
WRAPPER_EOF
chmod 755 "$WRAPPER"

# Preserve the user's existing layout and every other custom xinitrc line.
# Modify only the exact launch line used by the current Alternix installer.
ORIGINAL='exec dbus-launch --exit-with-session "$HOME/.qtile_venv/bin/qtile" start'
REPLACEMENT='exec dbus-launch --exit-with-session "$HOME/.local/bin/alternix-audio-session" "$HOME/.qtile_venv/bin/qtile" start'
if [ ! -e "$XINIT" ]; then
  warn 'No ~/.xinitrc found; session wrapper is ready but not connected automatically.'
elif grep -Fqx "$REPLACEMENT" "$XINIT"; then
  say '~/.xinitrc is already configured to start audio before Qtile.'
elif grep -Fqx "$ORIGINAL" "$XINIT"; then
  cp -p "$XINIT" "$XINIT.audio-backup-$STAMP"
  TEMP=$(mktemp "${XINIT}.XXXXXX")
  while IFS= read -r line || [ -n "$line" ]; do
    if [ "$line" = "$ORIGINAL" ]; then
      printf '%s\n' "$REPLACEMENT"
    else
      printf '%s\n' "$line"
    fi
  done < "$XINIT" > "$TEMP"
  chmod --reference="$XINIT" "$TEMP" 2>/dev/null || chmod 755 "$TEMP"
  mv "$TEMP" "$XINIT"
  say "Audio startup enabled. Backup: $XINIT.audio-backup-$STAMP"
else
  warn '~/.xinitrc uses a custom launch line; left it unchanged to avoid breaking the desktop.'
  warn "To opt in, run Qtile via: $WRAPPER <the original Qtile command>"
fi

if [ "$IS_ELITEPAD" -eq 1 ]; then
  say 'HP ElitePad: Intel SST firmware may only take effect after restarting the system.'
  if ! installed firmware-intel-sound; then
    warn 'Intel SST firmware is still missing. Check Devuan non-free-firmware repository access.'
  fi
fi
if [ "$IS_APPLE" -eq 1 ] && [ "$IS_CS8409" -eq 1 ]; then
  warn 'APPLE CS8409 DETECTED: Core audio installation alone may not enable iMac speakers.'
  warn 'Your iMac may need a model-specific Cirrus/Apple codec driver or amp configuration.'
  warn 'Model references: https://github.com/davidjo/snd_hda_macbookpro'
  warn 'For iMac19,2: https://github.com/a000a999a/imac2019audiofix'
  warn 'For iMac19,1: https://github.com/MrHogun/imac-a2115-linux-audio'
  warn 'No third-party kernel driver has been applied automatically.'
fi

show_diagnostics
say 'Repair complete. Test at a low speaker volume: speaker-test -c 2 -t sine -l 1'
echo 'If sound still fails, reboot once and run: bash alternix-audio-fix.sh --diagnose'
echo 'Share the new report from ~/.local/state/alternix/ to identify any remaining hardware driver issue.'
