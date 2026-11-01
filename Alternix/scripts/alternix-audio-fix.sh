#!/usr/bin/env bash
# AlternixOS / Devuan Excalibur one-time audio hardware provisioning.
# SysVinit compatible. No systemd dependency; no scheduled login-time scanning.
#
# --image-prep         Root in ISO chroot: include broad audio/firmware support.
# --install            Root in the installed system/chroot: scan and provision ONCE.
# --install-target DIR Root in the LIVE installer: scan actual hardware, install
#                      packages into DIR (the mounted target system), ONCE.
# --repair             Explicit manual troubleshooting, not automatic at login.
# --session-only       Explicit manual sound-session troubleshooting, no APT.
# --diagnose           Read-only hardware/audio diagnostics (creates a user log).
# --linx-force-sst     Linx 810B ONLY: opt in to legacy SST rather than SOF.
# --linx-auto          Remove this script's Linx SST override.
# --apple-driver       Apple + CS8409 ONLY: explicitly opt in to external DKMS.
# --scan               Read-only cross-platform hardware inventory and package plan.
# Hardware-aware (PCI, USB, ACPI, Device Tree, ALSA) with no speculative modprobe overrides.
#
# Note: 2017/2019 Apple CS8409 speakers sometimes require an out-of-tree driver;
# silently installing arbitrary kernel code on all iMacs is unsafe.
set -u
set -o pipefail
export LC_ALL=C

MODE=repair
TARGET_ROOT=
case "${1:-}" in
  ''|--repair) ;;
  --diagnose|--diagnostics) MODE=diagnose ;;
  --scan) MODE=scan ;;
  --session-only) MODE=session ;;
  --image-prep) MODE=image ;;
  --install) MODE=install ;;
  --install-target)
    MODE=install
    if [ "$#" -ne 2 ] || [ -z "${2:-}" ]; then
      printf '%s\n' 'Usage: alternix-audio-fix.sh --install-target /path/to/mounted/system' >&2
      exit 2
    fi
    TARGET_ROOT=$2 ;;
  --linx-force-sst) MODE=linx-sst ;;
  --linx-auto) MODE=linx-auto ;;
  --apple-driver) MODE=apple-driver ;;
  -h|--help)
    cat <<'HELP'
Usage: bash alternix-audio-fix.sh [MODE]
  --image-prep      Run during ISO build (root, in image chroot): preinstall broad
                    ALSA, sound-server, firmware and device support for live media.
  --install         Run ONCE as root inside the installed OS or its installer chroot:
                    inspect actual hardware and provision matching packages.
  --install-target DIR  Run ONCE as root from the live installer, targeting the
                    mounted new OS at DIR (e.g., /target). Scans LIVE hardware,
                    but installs packages only inside the target OS.
  --repair          Optional manual audio troubleshooting after installation.
  --session-only    Optional manual desktop sound-server/routing troubleshooting.
  --diagnose        Do not change audio settings; write a detailed report.
  --scan            Read-only audio hardware inventory and firmware package plan.
  --linx-force-sst  On detected Linx 810B + RT5640, select SST on next boot.
  --linx-auto       Undo the script's Linx 810B override on next boot.
  --apple-driver    On detected Apple + CS8409, opt in to third-party DKMS.
                    This downloads/builds a kernel module; see its upstream code.
HELP
    exit 0 ;;
  *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
esac

has() { command -v "$1" >/dev/null 2>&1; }
say() { printf '\n[Alternix Audio] %s\n' "$*"; }
warn() { printf '[WARNING] %s\n' "$*"; }
installed() {
  has dpkg-query || return 1
  if [ -n "$TARGET_ROOT" ]; then
    chroot "$TARGET_ROOT" dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -qx 'install ok installed'
  else
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -qx 'install ok installed'
  fi
}
read_file() { if [ -r "$1" ]; then cat "$1"; else printf '(unavailable)\n'; fi; }

ROOT=0
[ "$(id -u)" -eq 0 ] && ROOT=1
if [ "$MODE" = install ] || [ "$MODE" = image ]; then
  if [ "$ROOT" -ne 1 ]; then
    printf '%s\n' '[Alternix Audio] Installer setup must run as root.' >&2
    exit 1
  fi
fi
if [ -n "$TARGET_ROOT" ]; then
  if ! has realpath || ! has chroot; then
    printf '%s\n' '[Alternix Audio] --install-target needs realpath and chroot.' >&2
    exit 1
  fi
  TARGET_ROOT=$(realpath -e -- "$TARGET_ROOT") || exit 1
  if [ "$TARGET_ROOT" = / ] || [ ! -s "$TARGET_ROOT/var/lib/dpkg/status" ] || \
     [ ! -f "$TARGET_ROOT/etc/os-release" ]; then
    printf '%s\n' '[Alternix Audio] Refusing unsafe target: not a mounted, installed Debian/Devuan root.' >&2
    exit 1
  fi
  if [ ! -e "$TARGET_ROOT/dev/null" ] || ! mountpoint -q "$TARGET_ROOT/proc" || \
     ! mountpoint -q "$TARGET_ROOT/sys" ]; then
    printf '%s\n' '[Alternix Audio] Mount/bind /dev, /proc and /sys under target before running installation.' >&2
    exit 1
  fi
fi
if [ "$ROOT" -eq 1 ]; then
  STATE_DIR="${TARGET_ROOT}/var/log/alternix"
else
  STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/alternix"
fi
mkdir -p "$STATE_DIR" 2>/dev/null || STATE_DIR="${TMPDIR:-/tmp}"
STAMP=$(date +%Y%m%d-%H%M%S)
LOG="$STATE_DIR/audio-$STAMP-$$.log"
# Avoid launching desktop/session state, and avoid collecting machine info from
# the build host, during ISO preparation. Logging works in a chroot.
exec > >(tee -a "$LOG") 2>&1

# Root may provision the current installed machine, but may NOT modify /root's
# desktop audio state. --image-prep is explicitly for a generic offline ISO.
if [ "$ROOT" -eq 1 ] && [ "$MODE" != image ] && [ "$MODE" != install ]; then
  case "$MODE" in
    repair)
      MODE=system
      say 'Root repair: hardware-aware package provisioning only.'
      say 'Audio session setup belongs to the logged-in desktop user.' ;;
    scan|diagnose) : ;; # Read-only hardware inspection is allowed as root.
    *)
      warn "Mode $MODE needs a logged-in user; use --image-prep for ISO provisioning."
      exit 1 ;;
  esac
fi

# Privileged operations are never performed by a guessed osm-sudo CLI: the
# graphical pattern is not a Unix sudo password. Root image/installer paths
# should install firmware; ordinary repair can use Unix sudo if configured.
priv() {
  if [ "$ROOT" -eq 1 ]; then
    if [ -n "$TARGET_ROOT" ]; then chroot "$TARGET_ROOT" "$@"; else "$@"; fi
    return
  fi
  if has sudo; then sudo "$@"; return; fi
  warn "Missing sudo; cannot run privileged command: $*"
  return 1
}

pkg_available() {
  local c
  if [ -n "$TARGET_ROOT" ]; then
    c=$(chroot "$TARGET_ROOT" apt-cache policy "$1" 2>/dev/null | awk '/Candidate:/ {print $2; exit}')
  else
    c=$(apt-cache policy "$1" 2>/dev/null | awk '/Candidate:/ {print $2; exit}')
  fi
  [ -n "$c" ] && [ "$c" != '(none)' ]
}

# Hardware-class package decisions are made after PCI, ACPI, USB, ALSA and
# Device Tree scanning on the installed system. --image-prep cannot inspect
# the eventual hardware, so ships a broad but bounded firmware selection.
# Do not try to install random vendor kernel modules or change DSP preferences.
select_audio_packages() {
  WANTED=(alsa-utils alsa-ucm-conf alsa-topology-conf pavucontrol
          libasound2-plugins pciutils usbutils kmod)
  FIRMWARE_WANTED=()
  if installed pipewire-pulse; then
    SERVER_SELECTED=pipewire
    WANTED+=(pipewire wireplumber libspa-0.2-modules)
  else
    SERVER_SELECTED=pulseaudio
    WANTED+=(pulseaudio pulseaudio-utils)
  fi

  if [ "$MODE" = image ]; then
    # Supports Intel Atom/SST, newer Intel SOF, Cirrus amps, Realtek, AMD
    # HDMI/ACP systems, assorted embedded codecs, and Qualcomm-based ARM.
    # The installer may choose a narrower list to save ISO size.
    FIRMWARE_WANTED+=(firmware-intel-sound firmware-sof-signed
      firmware-realtek firmware-cirrus firmware-misc-nonfree
      firmware-amd-graphics firmware-qcom-soc)
    WANTED+=(bluez)
  else
    [ "${IS_INTEL_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-intel-sound firmware-sof-signed)
    [ "${HAS_REALTEK_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-realtek)
    [ "${HAS_CIRRUS_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-cirrus)
    [ "${HAS_AMD_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-misc-nonfree)
    [ "${HAS_AMD_GPU_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-amd-graphics)
    [ "${HAS_NVIDIA_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-misc-nonfree)
    [ "${HAS_QCOM_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-qcom-soc)
    [ "${HAS_MEDIATEK_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-mediatek)
    [ "${HAS_UNRECOGNISED_SOC_AUDIO:-0}" -eq 1 ] &&
      FIRMWARE_WANTED+=(firmware-misc-nonfree)
    [ "${HAS_BLUETOOTH:-0}" -eq 1 ] && WANTED+=(bluez)
  fi

  if [ "$MODE" = image ] || [ "${HAS_BLUETOOTH:-0}" -eq 1 ]; then
    if [ "$SERVER_SELECTED" = pipewire ]; then
      WANTED+=(libspa-0.2-bluetooth)
    else
      WANTED+=(pulseaudio-module-bluetooth)
    fi
  fi
  WANTED+=("${FIRMWARE_WANTED[@]}")
  # De-duplicate without depending on newer versions of associative arrays
  # or external tools, and keep the order of dependencies predictable.
  local -a unique=()
  local package old found
  for package in "${WANTED[@]}"; do
    found=0
    for old in "${unique[@]}"; do
      if [ "$old" = "$package" ]; then found=1; break; fi
    done
    [ "$found" -eq 1 ] || unique+=("$package")
  done
  WANTED=("${unique[@]}")
}

# Install missing packages; maintain the currently chosen sound server.
# --no-remove prevents APT from silently replacing the audio stack.
install_audio_packages() {
  if ! has apt-get || ! has apt-cache || ! has dpkg-query; then
    warn 'APT/dpkg unavailable; skipping package provisioning.'
    return 1
  fi
  select_audio_packages
  local -a missing=() candidates=()
  local package failed=0
  for package in "${WANTED[@]}"; do
    installed "$package" || missing+=("$package")
  done
  if [ "${#missing[@]}" -eq 0 ]; then
    say 'All detected audio support packages are installed.'
    return 0
  fi
  say "Required/matching missing packages: ${missing[*]}"
  if ! priv apt-get update; then
    warn 'APT index refresh failed; trying existing package metadata.'
  fi
  for package in "${missing[@]}"; do
    if pkg_available "$package"; then
      candidates+=("$package")
    else
      warn "Package unavailable in the current Devuan sources: $package"
      case "$package" in
        firmware-*) warn 'Check the non-free-firmware component in the existing Devuan repository entries.' ;;
      esac
      failed=1
    fi
  done
  if [ "${#candidates[@]}" -eq 0 ]; then
    warn 'No packages could be installed. This does not imply hardware is configured.'
    return 1
  fi
  say "Installing hardware-matched audio packages: ${candidates[*]}"
  if priv apt-get install -y --no-remove "${candidates[@]}"; then
    return "$failed"
  fi
  # A missing dependency for one optional firmware package must not stop
  # all other matched drivers, UCM profiles and utilities being installed.
  warn 'Batch install failed; retrying packages individually without removals.'
  for package in "${candidates[@]}"; do
    if ! installed "$package" && ! priv apt-get install -y --no-remove "$package"; then
      warn "Could not install $package"
      failed=1
    fi
  done
  return "$failed"
}

# Keep one copy for optional manual diagnostics; never autostart it.
# A previous version installed an XDG autostart hook which ran hardware repair
# on *every* login. Delete only that exact, script-owned entry if present.
install_integrated_script() {
  [ "$ROOT" -eq 1 ] || return 0
  local root="${TARGET_ROOT:-/}" src dest dir hook
  src=$(readlink -f "${BASH_SOURCE[0]}" 2>/dev/null || true)
  dir="$root/usr/local/bin"
  dest="$dir/alternix-audio-fix.sh"
  hook="$root/etc/xdg/autostart/alternix-audio-setup.desktop"
  [ -r "$src" ] || { warn 'Could not locate audio script for target installation.'; return 1; }
  install -d -m 0755 "$dir" || return 1
  if [ "$src" != "$dest" ] && ! cmp -s "$src" "$dest"; then
    install -m 0755 "$src" "$dest" || return 1
  fi
  if [ -f "$hook" ] && grep -qx 'X-Alternix-Audio-Setup=true' "$hook" && \
     grep -q '^Exec=/usr/local/bin/alternix-audio-fix.sh --session-only$' "$hook"; then
    rm -f "$hook" || return 1
    say 'Removed obsolete recurring Alternix audio hardware-scan autostart.'
  fi
  say "Installed one-time audio setup utility at $dest (no login hook)."
}

# A successful installer run leaves a marker so a repeated installer hook is
# harmless. Manual --repair and --diagnose still work and do not need marker.
MARKER="${TARGET_ROOT}/var/lib/alternix/audio-install-complete"
if [ "$MODE" = install ] && [ -f "$MARKER" ]; then
  say "Audio hardware was already provisioned: $MARKER"
  say 'Skipping another installer-time setup. Manual --repair remains available.'
  exit 0
fi

if [ "$MODE" = image ]; then
  say 'ISO / root system provisioning (no hardware/session access expected)'
  IS_INTEL_AUDIO=1
  say 'Building a multi-hardware ISO (no attempt to probe the build host).'
  select_audio_packages
  say "Image audio package selection: ${WANTED[*]}"
  install_audio_packages || warn 'Some ISO firmware packages were not available; check package repositories.'
  install_integrated_script || warn 'Could not copy audio setup utility into the image.'
  say 'ISO preparation finished. Hardware-specific provisioning runs once in the installed OS.'
  say "Report: $LOG"
  exit 0
fi

MODEL=$(cat /sys/class/dmi/id/product_name 2>/dev/null || printf unknown)
VENDOR=$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null || printf unknown)
BOARD=$(cat /sys/class/dmi/id/board_name 2>/dev/null || printf unknown)
KERNEL=$(uname -r)
CODECS=$( (cat /proc/asound/card*/codec\#* 2>/dev/null || true) | grep -i -m4 '^Codec:' || true)
LSPCI=$(lspci -nnk 2>/dev/null || true)
IS_APPLE=0
IS_LINX810B=0
IS_CS8409=0
IS_RT5640=0
IS_INTEL_AUDIO=0
case "$MODEL $VENDOR" in *Apple*|*iMac*|*MacBook*) IS_APPLE=1;; esac
case "$MODEL" in *LINX810B*|*Linx810B*|*Linx\ 810B*|*linx810b*) IS_LINX810B=1;; esac
if printf '%s\n' "$CODECS" | grep -qi 'CS8409'; then IS_CS8409=1; fi
if compgen -G '/sys/bus/acpi/devices/10EC5640:*' >/dev/null || \
   grep -qiE '(bytcr[-_]?rt5640|rt5640)' /proc/asound/cards 2>/dev/null; then
  IS_RT5640=1
fi
if printf '%s\n' "$LSPCI" | grep -qiE 'Intel Corporation.*(Audio|SST|Smart Sound|cAVS)|Audio.*Intel Corporation' || \
   [[ "$MODEL $VENDOR" == *ElitePad* ]] || \
   compgen -G '/sys/bus/acpi/devices/80860F28:*' >/dev/null || \
   compgen -G '/sys/bus/acpi/devices/808622A8:*' >/dev/null || [ "$IS_LINX810B" -eq 1 ]; then
  IS_INTEL_AUDIO=1
fi

# Full cross-platform hardware scan. Inspect actual enumerated buses instead of
# guessing from model names; combine kernel sound cards with unbound devices.
# lspci/lsusb can be missing on minimal ISOs, so sysfs is authoritative.
HAS_REALTEK_AUDIO=0
HAS_CIRRUS_AUDIO=0
HAS_AMD_AUDIO=0
HAS_AMD_GPU_AUDIO=0
HAS_NVIDIA_AUDIO=0
HAS_QCOM_AUDIO=0
HAS_MEDIATEK_AUDIO=0
HAS_UNRECOGNISED_SOC_AUDIO=0
HAS_USB_AUDIO=0
HAS_BLUETOOTH=0
HAS_HDA_AUDIO=0
HAS_ACPI_AUDIO=0
HAS_HDMI_AUDIO=0
HAS_ANY_AUDIO=0
AUDIO_PCI_ROWS=()
AUDIO_USB_ROWS=()
AUDIO_ACPI_ROWS=()
AUDIO_PLATFORM_ROWS=()
AUDIO_KERNEL_ROWS=()

scan_audio_hardware() {
  local node class vendor device driver iface name id audio_hint
  local compat=''
  local all_codecs="${CODECS:-}"
  for node in /sys/bus/pci/devices/*; do
    [ -r "$node/class" ] || continue
    class=$(cat "$node/class" 2>/dev/null || true)
    # PCI multimedia controller subclasses: audio / HD audio / other multimedia.
    [[ "$class" == 0x04* ]] || continue
    vendor=$(cat "$node/vendor" 2>/dev/null || true)
    device=$(cat "$node/device" 2>/dev/null || true)
    driver=unbound
    if [ -L "$node/driver" ]; then
      driver=$(basename "$(readlink -f "$node/driver")")
    fi
    AUDIO_PCI_ROWS+=("${node##*/} $vendor:$device class=$class driver=$driver")
    HAS_ANY_AUDIO=1
    case "$class" in 0x0403*) HAS_HDA_AUDIO=1 ;; esac
    case "$vendor" in
      0x8086) IS_INTEL_AUDIO=1 ;;
      0x1022) HAS_AMD_AUDIO=1 ;;
      0x1002) HAS_AMD_AUDIO=1; HAS_AMD_GPU_AUDIO=1 ;;
      0x10de) HAS_NVIDIA_AUDIO=1 ;;
      0x1013) HAS_CIRRUS_AUDIO=1 ;;
      0x10ec) HAS_REALTEK_AUDIO=1 ;;
      0x14c3) HAS_MEDIATEK_AUDIO=1 ;;
      0x17cb) HAS_QCOM_AUDIO=1 ;;
    esac
  done

  for iface in /sys/bus/usb/devices/*:*; do
    [ -r "$iface/bInterfaceClass" ] || continue
    class=$(cat "$iface/bInterfaceClass" 2>/dev/null || true)
    [ "$class" = 01 ] || [ "$class" = 1 ] || continue
    node=${iface%:*}
    name=$(cat "$node/product" 2>/dev/null || true)
    vendor=$(cat "$node/idVendor" 2>/dev/null || true)
    device=$(cat "$node/idProduct" 2>/dev/null || true)
    AUDIO_USB_ROWS+=("${node##*/} ${vendor:-????}:${device:-????} ${name:-USB Audio Class}")
    HAS_USB_AUDIO=1
    HAS_ANY_AUDIO=1
  done

  for node in /sys/bus/acpi/devices/*; do
    [ -e "$node" ] || continue
    id=${node##*/}; id=${id%%:*}
    case "$id" in
      10EC*|ESSX*|CLSA*|CSC*|CS35*|MX983*|MAX98*|NVT*|NAU*|WM8*|80860F28|808622A8|INT34C2|INTC10*|AMDI*|AMD*|WCD*|QCOM*)
        AUDIO_ACPI_ROWS+=("${node##*/}")
        HAS_ACPI_AUDIO=1; HAS_ANY_AUDIO=1
        case "$id" in
          10EC*|RT*) HAS_REALTEK_AUDIO=1 ;;
          CLSA*|CSC*|CS35*) HAS_CIRRUS_AUDIO=1 ;;
          8086*|INT*) IS_INTEL_AUDIO=1 ;;
          AMDI*|AMD*) HAS_AMD_AUDIO=1 ;;
          WCD*|QCOM*) HAS_QCOM_AUDIO=1 ;;
        esac ;;
    esac
  done

  for node in /sys/bus/platform/devices/*; do
    [ -e "$node" ] || continue
    name=${node##*/}
    case "${name,,}" in
      *audio*|*sound*|*sof*|*sst*|*asoc*|*amd-acp*|acp3x*|acp5x*|acp6x*|acp7x*|*acp-pdm*|*rt5640*|*rt5682*|*es8316*|*cs35l*|*max983*|*wcd9*)
        AUDIO_PLATFORM_ROWS+=("$name")
        HAS_ANY_AUDIO=1
        case "${name,,}" in
          *sof*|*sst*|*intel*) IS_INTEL_AUDIO=1 ;;
          *amd-acp*|acp3x*|acp5x*|acp6x*|acp7x*|*acp-pdm*) HAS_AMD_AUDIO=1 ;;
          *rt5640*|*rt5682*) HAS_REALTEK_AUDIO=1 ;;
          *cs35l*) HAS_CIRRUS_AUDIO=1 ;;
          *wcd9*) HAS_QCOM_AUDIO=1 ;;
        esac ;;
    esac
  done

  # ARM / RISC-V boards may advertise audio only through Device Tree.
  if [ -r /sys/firmware/devicetree/base/compatible ]; then
    compat=$(tr '\0' '\n' < /sys/firmware/devicetree/base/compatible 2>/dev/null || true)
    if [ -n "$compat" ] && [ "${#AUDIO_PLATFORM_ROWS[@]}" -gt 0 ]; then
      case "${compat,,}" in
        *qcom*|*qualcomm*) HAS_QCOM_AUDIO=1 ;;
        *mediatek*|*mt[0-9]*) HAS_MEDIATEK_AUDIO=1 ;;
        *) HAS_UNRECOGNISED_SOC_AUDIO=1 ;;
      esac
    fi
  fi

  for node in /sys/class/sound/card[0-9]*; do
    [ -e "$node" ] || continue
    name=$(cat "$node/id" 2>/dev/null || echo "${node##*/}")
    driver='unknown'
    if [ -L "$node/device/driver" ]; then
      driver=$(basename "$(readlink -f "$node/device/driver")")
    fi
    AUDIO_KERNEL_ROWS+=("${node##*/} id=$name driver=$driver")
    HAS_ANY_AUDIO=1
  done

  if compgen -G '/sys/class/bluetooth/hci*' >/dev/null; then
    HAS_BLUETOOTH=1
  fi

  # Codec IDs are a stronger signal than computer marketing names.
  if printf '%s\n' "$all_codecs" | grep -qiE '(Realtek|ALC[0-9]|RT5640)'; then
    HAS_REALTEK_AUDIO=1
  fi
  if printf '%s\n' "$all_codecs" | grep -qiE '(Cirrus|CS8409|CS35L)'; then
    HAS_CIRRUS_AUDIO=1
  fi
  if printf '%s\n' "$LSPCI" | grep -qiE 'HDMI Audio|Display Audio|HDMI/DP'; then
    HAS_HDMI_AUDIO=1
  fi
  if [ "$IS_RT5640" -eq 1 ]; then HAS_REALTEK_AUDIO=1; fi
  if [ "$IS_CS8409" -eq 1 ]; then HAS_CIRRUS_AUDIO=1; fi
}

show_hardware_inventory() {
  local row
  say 'Automatic audio hardware inventory (no driver changes)'
  printf 'Apple=%s Linx810B=%s Intel=%s AMD=%s AMD-GPU=%s NVIDIA=%s Realtek=%s Cirrus=%s Qualcomm=%s MediaTek=%s\n' \
    "$IS_APPLE" "$IS_LINX810B" "$IS_INTEL_AUDIO" "$HAS_AMD_AUDIO" \
    "$HAS_AMD_GPU_AUDIO" "$HAS_NVIDIA_AUDIO" "$HAS_REALTEK_AUDIO" \
    "$HAS_CIRRUS_AUDIO" "$HAS_QCOM_AUDIO" "$HAS_MEDIATEK_AUDIO"
  printf 'PCI=%s USB-audio=%s ACPI-audio=%s SoC-platform=%s ALSA-cards=%s Bluetooth-adapter=%s\n' \
    "${#AUDIO_PCI_ROWS[@]}" "${#AUDIO_USB_ROWS[@]}" "${#AUDIO_ACPI_ROWS[@]}" \
    "${#AUDIO_PLATFORM_ROWS[@]}" "${#AUDIO_KERNEL_ROWS[@]}" "$HAS_BLUETOOTH"
  for row in "${AUDIO_PCI_ROWS[@]}"; do printf '  PCI: %s\n' "$row"; done
  for row in "${AUDIO_USB_ROWS[@]}"; do printf '  USB: %s\n' "$row"; done
  for row in "${AUDIO_ACPI_ROWS[@]}"; do printf '  ACPI: %s\n' "$row"; done
  for row in "${AUDIO_PLATFORM_ROWS[@]}"; do printf '  SoC: %s\n' "$row"; done
  for row in "${AUDIO_KERNEL_ROWS[@]}"; do printf '  ALSA: %s\n' "$row"; done
  if [ "$HAS_ANY_AUDIO" -eq 0 ]; then
    warn 'No supported bus reported sound hardware; inspect BIOS/ACPI/kernel and the detailed log.'
  elif [ "${#AUDIO_KERNEL_ROWS[@]}" -eq 0 ]; then
    warn 'Hardware is enumerated but no ALSA sound card exists: likely kernel driver, codec, or firmware issue.'
  fi
}

scan_audio_hardware

# A usable server may already be operating; never replace it during repair.
server_ok() { has pactl && pactl info >/dev/null 2>&1; }
server_name() { pactl info 2>/dev/null | sed -n 's/^Server Name: //p' | head -n1; }
alsa_cards_count() {
  if [ -r /proc/asound/cards ]; then
    awk 'BEGIN{n=0} /^[[:space:]]*[0-9]+[[:space:]]+\[/{n++} END{print n}' /proc/asound/cards 2>/dev/null
  else
    printf '0\n'
  fi
}

show_diagnostics() {
  say 'Hardware and session diagnosis'
  printf 'OS: %s\n' "$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | head -n1)"
  printf 'Model: %s | Vendor: %s | Board: %s\n' "$MODEL" "$VENDOR" "$BOARD"
  printf 'Kernel: %s | User: %s | Groups: %s\n' "$KERNEL" "$(id -un)" "$(id -nG)"
  printf 'DISPLAY=%s | XDG_RUNTIME_DIR=%s | DBUS_SESSION_BUS_ADDRESS=%s\n' \
    "${DISPLAY:-unset}" "${XDG_RUNTIME_DIR:-unset}" "${DBUS_SESSION_BUS_ADDRESS:+set}"
  show_hardware_inventory
  printf '%s\n' '-- Kernel ALSA cards --'
  read_file /proc/asound/cards
  if has aplay; then printf '%s\n' '-- ALSA playback --'; aplay -l 2>&1 || true; fi
  if has arecord; then printf '%s\n' '-- ALSA capture --'; arecord -l 2>&1 || true; fi
  printf '%s\n' '-- HDA codecs --'
  for codec in /proc/asound/card*/codec\#*; do
    [ -r "$codec" ] || continue
    printf '%s\n' "$codec"
    head -n 6 "$codec" || true
  done
  printf '%s\n' '-- ACPI audio hardware --'
  for node in /sys/bus/acpi/devices/10EC5640:* /sys/bus/acpi/devices/80860F28:* \
              /sys/bus/acpi/devices/808622A8:* /sys/bus/acpi/devices/ESSX8316:*; do
    [ -e "$node" ] && printf '%s\n' "$node"
  done
  if [ -n "$LSPCI" ]; then
    printf '%s\n' '-- PCI drivers --'
    printf '%s\n' "$LSPCI" | grep -iE -A4 '(Audio device|Multimedia audio controller|Multimedia controller)' || true
  fi
  printf '%s\n' '-- Intel audio driver modules / configuration --'
  if has lsmod; then lsmod | grep -iE 'snd_(intel|soc|sof|hda)|soundwire' | head -n 45 || true; fi
  if [ -r /sys/module/snd_intel_dspcfg/parameters/dsp_driver ]; then
    read_file /sys/module/snd_intel_dspcfg/parameters/dsp_driver
  fi
  if has modinfo && [ "$IS_LINX810B" -eq 1 ]; then
    modinfo snd_soc_sst_bytcr_rt5640 2>&1 | head -n 5 || true
  fi
  if has alsaucm; then
    printf '%s\n' '-- ALSA UCM profiles --'
    alsaucm listcards 2>&1 | head -n 65 || true
  fi
  printf '%s\n' '-- PulseAudio-compatible server --'
  if has pactl; then
    pactl info 2>&1 || true
    pactl list short cards 2>&1 || true
    pactl list short sinks 2>&1 || true
    pactl list cards 2>&1 | grep -E 'Name: |Active Profile:|output:analog|HiFi|Profile:' | head -n 90 || true
    pactl list sinks 2>&1 | grep -E 'Name: |Description: |State: |Mute: |Active Port:' | head -n 70 || true
  fi
  printf '%s\n' '-- Missing/failed firmware messages --'
  if has dmesg; then
    dmesg 2>/dev/null | grep -iE '(firmware|sof|sst|acp|codec).*(failed|missing|not found)|Direct firmware load.*failed' | tail -n 45 || true
  fi
  printf '%s\n' '-- Relevant kernel log entries --' 
  if has dmesg; then
    dmesg 2>&1 | grep -iE 'snd|audio|codec|rt5640|sst|sof|firmware|cs8409|soundwire' | tail -n 75 || true
  fi
  printf '%s\n' '-- Firmware / UCM / sound packages --'
  if has dpkg-query; then
    dpkg-query -W 'alsa-ucm-conf' 'alsa-topology-conf' 'pulseaudio*' \
      'pipewire*' 'wireplumber' 'firmware-intel-sound' 'firmware-sof-signed' \
      'firmware-realtek' 'firmware-cirrus' 'firmware-misc-nonfree' \
      'firmware-qcom-soc' 'firmware-amd-graphics' 2>/dev/null || true
  fi
  if [ -d /etc/modprobe.d ]; then
    grep -RhsE '^options[[:space:]]+snd[-_]intel[-_]dspcfg|^options[[:space:]]+snd[-_]hda[-_]intel' \
      /etc/modprobe.d 2>/dev/null | head -n 20 || true
  fi
  printf 'Report: %s\n' "$LOG"
}

say "Mode: $MODE | $(date -Is) | $VENDOR $MODEL"
if [ "$IS_LINX810B" -eq 1 ]; then
  say "Linx 810B detected. RT5640 codec found: $IS_RT5640"
fi
if [ "$IS_APPLE" -eq 1 ]; then
  say "Apple hardware detected. CS8409 codec found: $IS_CS8409"
fi

if [ "$MODE" = diagnose ]; then
  show_hardware_inventory
  select_audio_packages
  say "Recommended hardware-matched packages: ${WANTED[*]}"
  show_diagnostics
  exit 0
fi
if [ "$MODE" = scan ]; then
  show_hardware_inventory
  select_audio_packages
  say "Detected audio stack: $SERVER_SELECTED"
  say "Hardware-matched package plan: ${WANTED[*]}"
  say "Already installed: $(for package in "${WANTED[@]}"; do installed "$package" && printf '%s ' "$package"; done)"
  say 'No audio configuration or packages were changed.'
  exit 0
fi

# Narrow, reversible Linx 810B driver-choice option. Bay Trail ACPI Intel
# audio can work under either SST or SOF; do not force SST by default.
LINX_CONF=/etc/modprobe.d/90-alternix-linx810b-audio.conf
if [ "$MODE" = linx-sst ] || [ "$MODE" = linx-auto ]; then
  if [ "$IS_LINX810B" -ne 1 ]; then
    warn 'Refusing driver override: this is not a detected Linx 810B.'
    exit 1
  fi
  if [ "$MODE" = linx-sst ] && [ "$IS_RT5640" -ne 1 ]; then
    warn 'Refusing to force SST without an enumerated RT5640 codec.'
    exit 1
  fi
  if [ "$MODE" = linx-sst ]; then
    if [ -e "$LINX_CONF" ] && ! grep -q '^# AlternixOS Linx810B audio' "$LINX_CONF"; then
      warn "Unrecognised existing config at $LINX_CONF; leaving it unchanged."
      exit 1
    fi
    temp=$(mktemp) || exit 1
    printf '%s\n' '# AlternixOS Linx810B audio (reversible; --linx-auto)' \
      '# Intel Bay Trail SST selection; do not apply to arbitrary Intel laptops.' \
      'options snd-intel-dspcfg dsp_driver=2' > "$temp"
    if priv install -m 0644 "$temp" "$LINX_CONF"; then
      say 'Selected Intel SST for the next boot (no live module unloading).'
    else
      warn 'Could not install the driver selection.'
      rm -f "$temp"
      exit 1
    fi
    rm -f "$temp"
  elif [ -f "$LINX_CONF" ] && grep -q '^# AlternixOS Linx810B audio' "$LINX_CONF"; then
    if priv rm -f "$LINX_CONF"; then say 'Restored automatic Intel DSP selection.'; else exit 1; fi
  else
    say 'No Alternix Linx SST override is installed.'
  fi
  if has update-initramfs; then
    priv update-initramfs -u -k "$KERNEL" || warn 'initramfs update failed; see log.'
  fi
  say 'Reboot to apply the change; no audio processes were killed.'
  exit 0
fi

# Explicit-only external module installer; does NOT run in an ISO chroot.
# Keep source under the user's home so it is inspectable and removable.
if [ "$MODE" = apple-driver ]; then
  if [ "$IS_APPLE" -ne 1 ] || [ "$IS_CS8409" -ne 1 ]; then
    warn 'Not an Apple computer with an enumerated CS8409 codec. Refusing driver install.'
    exit 1
  fi
  say 'Optional third-party Apple CS8409 driver requested.'
  warn 'This compiles/install kernel code from github.com/davidjo/snd_hda_macbookpro.'
  warn 'It is NOT part of the kernel or Devuan. Kernel version compatibility is not guaranteed.'
  warn 'It can break sound or require recovery; take a backup before proceeding.'
  if ! has apt-get || ! has apt-cache; then warn 'APT is required.'; exit 1; fi
  deps=(git dkms build-essential make gcc patch wget xz-utils)
  if pkg_available "linux-headers-$KERNEL"; then
    deps+=("linux-headers-$KERNEL")
  elif [ ! -d "/lib/modules/$KERNEL/build" ]; then
    warn "Matching linux-headers-$KERNEL are not available; cannot build safely."
    exit 1
  fi
  missing_deps=()
  for pkg in "${deps[@]}"; do installed "$pkg" || missing_deps+=("$pkg"); done
  if [ "${#missing_deps[@]}" -gt 0 ]; then
    priv apt-get update || true
    priv apt-get install -y "${missing_deps[@]}" || { warn 'Build dependencies could not be installed.'; exit 1; }
  fi
  srcdir="${XDG_CACHE_HOME:-$HOME/.cache}/alternix/snd_hda_macbookpro"
  mkdir -p "$(dirname "$srcdir")"
  if [ -e "$srcdir" ] && [ ! -d "$srcdir/.git" ]; then
    warn "Existing non-git path $srcdir; cannot overwrite it."
    exit 1
  fi
  if [ -d "$srcdir/.git" ]; then
    say 'Using the already cloned driver source (no silent git pull).'
  else
    if ! git clone --depth 1 https://github.com/davidjo/snd_hda_macbookpro.git "$srcdir"; then
      warn 'Could not download driver source.'
      exit 1
    fi
  fi
  say "External source directory: $srcdir"
  say "Source commit: $(git -C "$srcdir" rev-parse HEAD 2>/dev/null || echo unknown)"
  if [ ! -f "$srcdir/install.cirrus.driver.sh" ]; then
    warn 'Missing upstream DKMS installer.'
    exit 1
  fi
  # No unbounded automatic root execution without a second explicit consent.
  # The flag itself is an explicit opt-in to driver installation.
  if (cd "$srcdir" && priv bash ./install.cirrus.driver.sh -i); then
    say 'Upstream DKMS installer returned success; reboot before testing audio.'
  else
    warn 'Upstream DKMS installer failed. Check exact kernel headers/source and DKMS log.'
    exit 1
  fi
  case "$MODEL" in
    iMac19,2)
      warn 'iMac19,2 may ALSO need its GPIO4 speaker-amplifier patch after reboot:'
      warn 'https://github.com/a000a999a/imac2019audiofix'
      warn 'The amp patch has NOT been installed automatically.' ;;
    iMac19,1)
      warn 'iMac19,1 four-speaker tuning is model-specific. See:'
      warn 'https://github.com/MrHogun/imac-a2115-linux-audio' ;;
  esac
  say "Report: $LOG"
  exit 0
fi

if [ "$MODE" = repair ] || [ "$MODE" = system ] || [ "$MODE" = install ]; then
  show_hardware_inventory
  select_audio_packages
  say "Selected hardware support: ${WANTED[*]}"
  if ! install_audio_packages; then
    if [ "$MODE" = install ]; then
      warn 'One-time hardware provisioning is INCOMPLETE; no completion marker will be created.'
      warn 'Check the log for missing firmware, package sources or failed dependencies.'
      install_integrated_script || true
      exit 1
    fi
    warn 'Package preparation incomplete; continuing with installed components.'
  fi
else
  say 'Session-only: no APT, sudo or kernel configuration changes.'
fi
if [ "$MODE" = install ]; then
  # Installation occurs before a real user's desktop/PulseAudio session exists.
  # Configure packages and kernel-visible devices ONLY; leave per-user
  # playback routing to the normal sound-server defaults after first login.
  if ! install_integrated_script; then
    warn 'Could not install the integrated sound diagnostic utility.'
    exit 1
  fi
  install -d -m 0755 "$(dirname "$MARKER")" || exit 1
  marker_temp=$(mktemp "${MARKER}.XXXXXX") || exit 1
  if ! {
    printf 'installed_at=%s\n' "$(date -Is)"
    printf 'hardware_vendor=%s\n' "$VENDOR"
    printf 'hardware_model=%s\n' "$MODEL"
    printf 'kernel_during_install=%s\n' "$KERNEL"
    printf 'scan_log=%s\n' "${LOG#${TARGET_ROOT}}"
    printf 'audio_server=%s\n' "$SERVER_SELECTED"
    printf 'firmware_selection=%s\n' "${FIRMWARE_WANTED[*]}"
    printf 'hardware_detected=%s\n' "$HAS_ANY_AUDIO"
  } > "$marker_temp"; then
    rm -f "$marker_temp"
    exit 1
  fi
  chmod 0644 "$marker_temp" || true
  mv -f "$marker_temp" "$MARKER" || { rm -f "$marker_temp"; exit 1; }
  say 'One-time installed-system audio provisioning completed. No login scans scheduled.'
  show_diagnostics
  exit 0
fi
if [ "$MODE" = system ]; then
  say 'Hardware-aware system package provisioning completed.'
  say 'No login hook installed. The audio server starts normally in the user session.'
  show_diagnostics
  exit 0
fi

# elogind/PAM normally provides XDG_RUNTIME_DIR under Devuan SysVinit.
# Do not create /run/user/$UID without authority; protect any fallback path.
ensure_runtime() {
  if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -d "$XDG_RUNTIME_DIR" ]; then return; fi
  if [ -d "/run/user/$(id -u)" ] && [ -O "/run/user/$(id -u)" ]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
    return
  fi
  # PipeWire needs a private runtime directory even without elogind. A home
  # fallback is deliberate and used only when the real one is absent.
  local fallback="${XDG_CACHE_HOME:-$HOME/.cache}/alternix-audio-runtime"
  if [ -L "$fallback" ]; then warn 'Runtime path is a symlink; refusing insecure fallback.'; return 1; fi
  mkdir -p "$fallback" || return 1
  chmod 700 "$fallback" || return 1
  if [ -O "$fallback" ]; then
    export XDG_RUNTIME_DIR="$fallback"
    warn 'No elogind /run/user runtime directory; using private home fallback for this login.'
  else
    warn 'Runtime directory is not user-owned.'
    return 1
  fi
}

# Avoid mixing up which sound server is active. Never kill a working server.
start_audio_server() {
  if server_ok; then say "Sound server already responding: $(server_name)"; return 0; fi
  ensure_runtime || warn 'Could not establish XDG_RUNTIME_DIR.'
  if installed pipewire-pulse; then
    say 'Starting PipeWire / WirePlumber / PipeWire-Pulse without systemd.'
    if has pipewire && ! pgrep -u "$(id -u)" -x pipewire >/dev/null 2>&1; then
      (pipewire >>"$STATE_DIR/pipewire.log" 2>&1 &)
    fi
    sleep 1
    if has wireplumber && ! pgrep -u "$(id -u)" -x wireplumber >/dev/null 2>&1; then
      (wireplumber >>"$STATE_DIR/wireplumber.log" 2>&1 &)
    fi
    if has pipewire-pulse && ! pgrep -u "$(id -u)" -x pipewire-pulse >/dev/null 2>&1; then
      (pipewire-pulse >>"$STATE_DIR/pipewire-pulse.log" 2>&1 &)
    fi
  elif has pulseaudio; then
    say 'Starting PulseAudio as the desktop user.'
    pulseaudio --start 2>&1 || warn 'PulseAudio failed to start.'
  else
    warn 'No PulseAudio-compatible sound server is installed.'
  fi
  local i
  for i in 1 2 3 4; do
    server_ok && break
    sleep 1
  done
  if server_ok; then say "Sound server online: $(server_name)"; else warn 'No responding sound server.'; return 1; fi
}

# When desktop hardware controls are at 0% (or muted), a successfully started
# server still produces silence. Do not touch capture, headphone or HDMI paths.
fix_alsa_playback() {
  if ! has amixer; then warn 'amixer missing; cannot inspect hardware playback controls.'; return; fi
  local cardpath card control state volume
  # Respect the user's intentionally muted devices on subsequent logins.
  [ "$MODE" = repair ] || return 0
  for cardpath in /proc/asound/card[0-9]*; do
    [ -d "$cardpath" ] || continue
    card=${cardpath##*card}
    for control in Master Speaker PCM Front; do
      state=$(amixer -c "$card" sget "$control" 2>/dev/null) || continue
      # Only touch controls ALSA reports as playback controls.
      if ! printf '%s\n' "$state" | grep -q 'Playback'; then continue; fi
      if printf '%s\n' "$state" | grep -q '\[off\]'; then
        amixer -c "$card" sset "$control" unmute >/dev/null 2>&1 || true
        say "Unmuted ALSA $card/$control"
      fi
      volume=$(printf '%s\n' "$state" | grep -oE '\[[0-9]+%\]' | head -n1 | tr -d '[]%')
      if [ "$volume" = 0 ]; then
        amixer -c "$card" sset "$control" 45% >/dev/null 2>&1 || true
        say "Set silent ALSA $card/$control to a safe 45%"
      fi
    done
  done
}

# Detect disabled card profiles. UCM device names vary (analog-stereo / HiFi).
# Select ONLY if the card is off; don't override a chosen Bluetooth/HDMI setup.
fix_disabled_profiles() {
  server_ok || return
  local card profile details candidate
  # A disabled card may be intentional when another output already works.
  # Only recover off profiles when there are no real playback sinks at all.
  if pactl list short sinks 2>/dev/null | awk '$2 != "auto_null" && $2 != "" {found=1} END {exit !found}'; then
    return 0
  fi
  while read -r _ card _; do
    [ -n "${card:-}" ] || continue
    details=$(pactl list cards 2>/dev/null | awk -v wanted="$card" '
      /^Card #/ {matched=0}
      /^[[:space:]]*Name: / {matched=($2 == wanted)}
      matched {print}
    ')
    [ -n "$details" ] || continue
    profile=$(printf '%s\n' "$details" | sed -n 's/^[[:space:]]*Active Profile: //p' | head -n1)
    [ "$profile" = off ] || continue
    candidate=$(printf '%s\n' "$details" | awk '
      /^[[:space:]]*Profiles:/ {profiles=1;next}
      profiles && /^[[:space:]]*Active Profile:/ {exit}
      profiles && /\(sinks:[[:space:]]*[1-9]/ && !/available: no/ {
        p=$1; sub(/:$/, "", p)
        if (p ~ /output:analog-stereo|HiFi|Speaker/) {selected=1; print p; exit}
        if (fallback == "") fallback=p
      }
      END {if (fallback != "" && !selected) print fallback}
    ' | head -n1)
    if [ -n "$candidate" ] && [ "$candidate" != off ]; then
      say "Enabling available playback profile on $card: $candidate"
      pactl set-card-profile "$card" "$candidate" 2>&1 || true
    fi
  done < <(pactl list short cards 2>/dev/null)
}

fix_default_output() {
  server_ok || return
  local default selected v info
  default=$(pactl get-default-sink 2>/dev/null || true)
  [ -n "$default" ] || default=$(pactl info 2>/dev/null | sed -n 's/^Default Sink: //p' | head -n1)
  if [ -z "$default" ] || [ "$default" = auto_null ] || \
     ! pactl list short sinks 2>/dev/null | awk '{print $2}' | grep -Fqx "$default"; then
    # Prefer internal analogue only when the previous default disappeared.
    # USB, HDMI/DP, Bluetooth, and virtual playback are all valid fallbacks.
    # A user-chosen, still-present default is never replaced.
    selected=$(pactl list short sinks 2>/dev/null | awk '
      $2 == "auto_null" || $2 == "" {next}
      $2 ~ /analog|speaker|rt5640|HiFi/ {print $2; exit}
      $2 ~ /usb|USB/ && usb == "" {usb=$2}
      $2 ~ /hdmi|displayport|iec958/ && hdmi == "" {hdmi=$2}
      $2 ~ /bluez|bluetooth/ && bluetooth == "" {bluetooth=$2}
      other == "" {other=$2}
      END {if (usb != "") print usb; else if (hdmi != "") print hdmi;
           else if (bluetooth != "") print bluetooth; else if (other != "") print other}
    ' | head -n1)
    if [ -n "$selected" ]; then
      say "Selecting available audio output: $selected"
      pactl set-default-sink "$selected" 2>&1 || true
    else
      warn 'No usable playback sink detected; routing alone cannot create a missing kernel sound card.'
    fi
  fi
  if pactl list short sinks 2>/dev/null | awk '$2 != "auto_null" && $2 != "" {found=1} END {exit !found}'; then
    # --repair explicitly requests unmuting; do NOT re-unmute at every login.
    if [ "$MODE" = repair ]; then
      pactl set-sink-mute @DEFAULT_SINK@ 0 >/dev/null 2>&1 || true
    fi
    info=$(pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null || true)
    v=$(printf '%s\n' "$info" | grep -oE '[0-9]+%' | head -n1 | tr -d '%')
    if [ "$v" = 0 ] && [ "$MODE" = repair ]; then
      pactl set-sink-volume @DEFAULT_SINK@ 35% >/dev/null 2>&1 || true
      say 'Raised muted-by-volume default output to a safe 35%.'
    fi
  fi
}

# Installer configuration is deliberately one-shot. No .xinitrc mutations and
# no recurring audio-repair helper; explicit --repair is available if needed.

start_audio_server || true
fix_alsa_playback
if server_ok; then
  # If PulseAudio has no ALSA cards yet, try loading the standard udev scanner.
  if server_name | grep -qi 'pulseaudio' && \
     [ -z "$(pactl list short cards 2>/dev/null)" ] && \
     [ "$(alsa_cards_count)" -gt 0 ] && \
     ! pactl list short modules 2>/dev/null | grep -q 'module-udev-detect'; then
    say 'Loading PulseAudio udev ALSA card discovery.'
    pactl load-module module-udev-detect 2>&1 || true
  fi
  fix_disabled_profiles
  fix_default_output
fi

if [ "$IS_LINX810B" -eq 1 ]; then
  if [ "$IS_RT5640" -eq 1 ]; then
    say 'Linx 810B RT5640 detected: checked UCM/Intel SST firmware and playback profiles.'
    if [ "$(alsa_cards_count)" -eq 0 ]; then
      warn 'No ALSA card: check firmware load, kernel SST/SOF modules, and ACPI device binding.'
      warn 'If SOF currently fails on this tablet, opt in to --linx-force-sst then reboot.'
      warn 'Undo with --linx-auto; do NOT use this option on other laptops.'
    fi
  else
    warn 'Linx 810B RT5640 is not enumerating. ALSA user-space changes cannot repair an absent codec.'
  fi
fi
if [ "$IS_APPLE" -eq 1 ] && [ "$IS_CS8409" -eq 1 ]; then
  warn 'Apple Cirrus CS8409 found: standard Linux audio packages may not initialise speaker amps.'
  warn 'Driver option: --apple-driver (explicit third-party DKMS install; review risks first).'
  case "$MODEL" in
    iMac19,2) warn 'iMac19,2 may also need https://github.com/a000a999a/imac2019audiofix' ;;
    iMac19,1) warn 'iMac19,1 reference: https://github.com/MrHogun/imac-a2115-linux-audio' ;;
  esac
fi

show_diagnostics
if ! server_ok; then
  warn 'Sound server unavailable; see report and check DBus/elogind session environment.'
elif [ "$(alsa_cards_count)" -eq 0 ]; then
  warn 'No kernel ALSA hardware: check dmesg/firmware and installed kernel driver.'
elif pactl list short sinks 2>/dev/null | grep -q 'auto_null'; then
  warn 'Only dummy output: kernel driver or card-profile issue remains.'
else
  say 'Audio devices are enumerated; actual speaker playback has NOT been verified.'
fi
say "Report: $LOG"
say 'Low-volume playback test (manual): speaker-test -D default -c 2 -t sine -l 1'
say 'If firmware was newly installed, reboot once before testing.'
