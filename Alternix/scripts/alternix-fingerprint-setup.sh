#!/usr/bin/env bash
# AlternixOS: device-agnostic fingerprint discovery and setup for Devuan/Debian.
# Designed for SysVinit (no systemd dependency). Run on the installed OS.
# Uses repository packages only; it does not download untrusted vendor drivers.
set -Eeuo pipefail

SCRIPT_VERSION='1.0.1'
MODE='install'
ENROLL='ask'
ENABLE_PAM=0
TARGET_USER=''
APT_UPDATE=1
TTY=0
[[ -t 0 && -t 1 ]] && TTY=1

say()  { printf '%s\n' "$*"; }
info() { printf '\n\033[1;36m[Alternix fingerprint]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[WARNING]\033[0m %s\n' "$*" >&2; }
fail() { printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'HELP'
Usage:
  bash alternix-fingerprint-setup.sh --scan
  sudo bash alternix-fingerprint-setup.sh [options]

Default: scan hardware; install the official fingerprint stack; test whether
fprintd recognises the reader; optionally enroll a fingerprint.

Options:
  --scan          Probe hardware and existing fprintd; install nothing.
  --no-enroll     Install and test only; do not offer fingerprint enrollment.
  --enroll        Attempt interactive enrollment when a reader is found.
  --user NAME     User to enroll (default: original sudo user).
  --enable-pam    After a successful live fingerprint verification, enable
                  Debian's standard fingerprint PAM profile (opt-in).
  --no-update     Skip apt-get update (use existing package indexes).
  -h, --help      Show this help.

Note: --enable-pam may affect sudo, display managers, and other PAM clients.
It does NOT add fingerprint support to Alternix's custom osm-lock pattern UI.
The existing password/pattern mechanism is not overwritten by this script.
HELP
}

while (($#)); do
  case "$1" in
    --scan) MODE='scan'; shift ;;
    --no-enroll) ENROLL='no'; shift ;;
    --enroll) ENROLL='yes'; shift ;;
    --enable-pam) ENABLE_PAM=1; shift ;;
    --no-update) APT_UPDATE=0; shift ;;
    --user)
      (($# >= 2)) || fail '--user requires a username'
      TARGET_USER="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) fail "Unrecognised option: $1 (see --help)" ;;
  esac
done

# Choose a real local user; do not accidentally enroll a fingerprint for root.
if [[ -z "$TARGET_USER" ]]; then
  if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != root ]]; then
    TARGET_USER="$SUDO_USER"
  elif [[ -n "${PKEXEC_UID:-}" ]] && command -v getent >/dev/null 2>&1; then
    TARGET_USER="$(getent passwd "$PKEXEC_UID" | cut -d: -f1 || true)"
  elif [[ "$(id -u)" -ne 0 ]]; then
    TARGET_USER="$(id -un)"
  fi
fi
if [[ -n "$TARGET_USER" ]] && ! id "$TARGET_USER" >/dev/null 2>&1; then
  fail "User '$TARGET_USER' does not exist. Use --user <existing-user>."
fi
if [[ "$TARGET_USER" == root ]]; then
  warn 'Root fingerprint enrollment is disabled. Use --user <normal-user>.'
  TARGET_USER=''
fi

if [[ -r /etc/os-release ]]; then
  # shellcheck source=/dev/null
  . /etc/os-release
  say "OS: ${PRETTY_NAME:-unknown}"
fi
if command -v dpkg >/dev/null 2>&1; then
  say "Architecture: $(dpkg --print-architecture)"
else
  say "Architecture: $(uname -m)"
fi
say "Target user: ${TARGET_USER:-not selected}"

# Scan across exposed buses. Descriptor matching finds *candidates*, not proof
# of a working fingerprint sensor. fprintd's live D-Bus enumeration is decisive.
CANDIDATES=0
HINTS=0
is_sensor_text() {
  local value="${1,,}"
  [[ "$value" =~ fingerprint|finger.print|biometric|authentec|u\.are\.u|touchchip|touchstrip|fprint|vfs[0-9]|fpc102|egistec|elan70[0-9a-f]|goodix.*finger|validity.*sensor ]]
}
show_candidate() {
  say "  $1"
  CANDIDATES=$((CANDIDATES + 1))
}
scan_hardware() {
  info 'Scanning USB, HID, SPI, ACPI and PCI for fingerprint hardware'
  local d vendor product label manufacturer description modalias item text entry

  shopt -s nullglob
  for d in /sys/bus/usb/devices/*; do
    [[ -r "$d/idVendor" && -r "$d/idProduct" ]] || continue
    vendor="$(cat "$d/idVendor")"; product="$(cat "$d/idProduct")"
    label=''; manufacturer=''
    [[ ! -r "$d/product" ]] || label="$(cat "$d/product")"
    [[ ! -r "$d/manufacturer" ]] || manufacturer="$(cat "$d/manufacturer")"
    text="$manufacturer $label"
    if is_sensor_text "$text"; then
      show_candidate "USB ${vendor}:${product} - ${text:-unknown}"
    fi
  done

  for d in /sys/bus/hid/devices/*; do
    [[ -r "$d/uevent" ]] || continue
    text="$(grep -E '^(HID_NAME|HID_ID|HID_UNIQ)=' "$d/uevent" | sed '/HID_UNIQ=/d' || true)"
    if is_sensor_text "$text"; then
      show_candidate "HID $(basename "$d") - $text"
    fi
  done

  for d in /sys/bus/spi/devices/*; do
    modalias=''
    [[ ! -r "$d/modalias" ]] || modalias="$(cat "$d/modalias")"
    if is_sensor_text "$(basename "$d") $modalias"; then
      show_candidate "SPI $(basename "$d") - $modalias"
    fi
  done

  for d in /sys/bus/acpi/devices/*; do
    description=''; modalias=''
    [[ ! -r "$d/hid" ]] || description="$(cat "$d/hid")"
    [[ ! -r "$d/modalias" ]] || modalias="$(cat "$d/modalias")"
    text="$(basename "$d") $description $modalias"
    if is_sensor_text "$text"; then
      show_candidate "ACPI ${description:-$(basename "$d")} - $modalias"
    fi
  done
  shopt -u nullglob

  if command -v lspci >/dev/null 2>&1; then
    while IFS= read -r entry; do
      [[ -n "$entry" ]] || continue
      if is_sensor_text "$entry"; then
        show_candidate "PCI $entry"
      fi
    done < <(lspci -nn 2>/dev/null || true)
  fi

  if command -v lsusb >/dev/null 2>&1; then
    while IFS= read -r entry; do
      [[ -n "$entry" ]] || continue
      if is_sensor_text "$entry"; then
        # Some devices have empty sysfs names but a descriptive USB ID database.
        show_candidate "USB database: $entry"
      fi
    done < <(lsusb 2>/dev/null || true)
  fi

  if (( CANDIDATES == 0 )); then
    warn 'No recognisable fingerprint descriptor found. Some readers identify generically, so this is not conclusive.'
  else
    say "Found $CANDIDATES possible device descriptor(s) (duplicates across buses are expected)."
  fi

  if command -v lsusb >/dev/null 2>&1; then
    say ''
    say 'USB inventory (use device IDs to research unsupported readers):'
    lsusb || true
  fi
}

# D-Bus speaks directly to the fprintd Manager; it works with SysVinit and does
# not require systemctl. Fall back to fprintd-list output for older versions.
FPRINT_READER=0
FPRINT_CHECK_LOG=''
check_fprintd() {
  FPRINT_READER=0
  info 'Checking the actual libfprint/fprintd driver stack'
  if ! command -v fprintd-list >/dev/null 2>&1; then
    warn 'fprintd is not installed (or its utilities are not on PATH).'
    return 1
  fi
  local reply='' status=0
  if command -v dbus-send >/dev/null 2>&1; then
    reply="$(dbus-send --system --print-reply --reply-timeout=8000 \
      --dest=net.reactivated.Fprint /net/reactivated/Fprint/Manager \
      net.reactivated.Fprint.Manager.GetDevices 2>&1)" || status=$?
    if (( status == 0 )); then
      if grep -Eq 'object path "/net/reactivated/Fprint/Device/' <<< "$reply"; then
        FPRINT_READER=1
      else
        warn 'The fprintd service responded, but listed no supported reader.'
      fi
    else
      warn 'D-Bus enumeration failed; will check using fprintd-list instead.'
      say "$reply"
    fi
  fi
  if [[ -n "$TARGET_USER" ]]; then
    FPRINT_CHECK_LOG="$(fprintd-list "$TARGET_USER" 2>&1)" || true
    say "$FPRINT_CHECK_LOG"
    if grep -Eqi '^found [1-9][0-9]* devices|Device at /net/reactivated/Fprint/Device/|Using device /net/reactivated/Fprint/Device/' <<< "$FPRINT_CHECK_LOG"; then
      FPRINT_READER=1
    fi
  fi
  if (( FPRINT_READER )); then
    say 'RESULT: a reader is recognised by the installed fingerprint driver.'
    return 0
  fi
  warn 'RESULT: no working fingerprint reader confirmed by fprintd.'
  return 1
}

start_dbus_if_needed() {
  local ready=0
  if command -v dbus-send >/dev/null 2>&1 && \
     dbus-send --system --print-reply --reply-timeout=4000 \
       --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
    ready=1
  fi
  if (( ready )); then return 0; fi
  warn 'The system D-Bus is not currently responding.'
  if [[ -x /etc/init.d/dbus ]]; then
    info 'Starting D-Bus using the SysVinit service'
    /etc/init.d/dbus start || warn 'Could not start dbus; fprintd may not be usable yet.'
  elif command -v service >/dev/null 2>&1; then
    service dbus start || warn 'Could not start dbus.'
  else
    warn 'No D-Bus init script was found; start the system bus before enrolling.'
  fi
}

backup_pam() {
  local dest
  install -d -m 0700 /var/backups/alternix-fingerprint
  dest="$(mktemp -d /var/backups/alternix-fingerprint/$(date +%Y%m%d-%H%M%S)-XXXXXXXX)"
  chmod 0700 "$dest"
  local f
  for f in /etc/pam.d/common-auth /etc/pam.d/common-account \
           /etc/pam.d/common-password /etc/pam.d/common-session; do
    [[ ! -f "$f" ]] || cp -a -- "$f" "$dest/"
  done
  say "PAM backup saved to: $dest"
}

apt_install_stack() {
  command -v apt-get >/dev/null 2>&1 || fail 'apt-get is required (Devuan/Debian).'
  command -v dpkg >/dev/null 2>&1 || fail 'dpkg is required (Devuan/Debian).'
  if (( APT_UPDATE )); then
    info 'Refreshing configured Devuan/Debian package indexes'
    DEBIAN_FRONTEND=noninteractive apt-get update || fail 'apt-get update failed. Check your APT repositories/network.'
  fi
  local pkg
  for pkg in fprintd libfprint-2-2 libpam-fprintd dbus usbutils pciutils; do
    if ! apt-cache show "$pkg" >/dev/null 2>&1; then
      fail "No candidate package for '$pkg' in the configured repositories. No third-party sources were added."
    fi
  done
  info 'Installing fingerprint drivers, daemon, PAM module, D-Bus and scan utilities'
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    fprintd libfprint-2-2 libpam-fprintd dbus usbutils pciutils \
    || fail 'Fingerprint package installation failed; see APT output above.'
  if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload >/dev/null 2>&1 || true
  fi
}

ask_yes() {
  local question="$1" response=''
  (( TTY )) || return 1
  read -r -p "$question [y/N]: " response || return 1
  [[ "${response,,}" == y || "${response,,}" == yes ]]
}

VERIFIED=0
verify_fingerprint() {
  [[ -n "$TARGET_USER" ]] || { warn 'No user selected for verification.'; return 1; }
  (( TTY )) || { warn 'Live verification needs an interactive terminal.'; return 1; }
  info "Verifying the fingerprint for $TARGET_USER"
  local result=0 output=''
  output="$(fprintd-verify "$TARGET_USER" 2>&1)" || result=$?
  say "$output"
  # Some fprintd versions can finish the CLI interaction even on a mismatch.
  # Require an actual match, not just a successful process exit.
  if (( result == 0 )) && grep -qi 'verify-match' <<< "$output"; then
    VERIFIED=1
    say 'Live fingerprint match confirmed.'
    return 0
  fi
  warn 'Fingerprint verification did not confirm a match; PAM will not be enabled.'
  return 1
}

enroll_and_verify() {
  [[ -n "$TARGET_USER" ]] || { warn 'No normal user selected; pass --user NAME to enroll.'; return 1; }
  (( TTY )) || { warn 'Enrollment requires an interactive terminal. Run: fprintd-enroll (as your normal user).'; return 1; }
  info "Enroll a fingerprint for $TARGET_USER"
  say 'Follow the prompts to touch/swipe the sensor repeatedly.'
  if ! fprintd-enroll "$TARGET_USER"; then
    warn 'Enrollment failed. Check whether the sensor is supported or already in use.'
    return 1
  fi
  verify_fingerprint
}

enable_pam_safely() {
  (( VERIFIED )) || { warn 'PAM was NOT changed: a live fingerprint verification did not succeed in this run.'; return 1; }
  [[ -f /usr/share/pam-configs/fprintd ]] || { warn 'PAM fingerprint profile is missing.'; return 1; }
  command -v pam-auth-update >/dev/null 2>&1 || { warn 'pam-auth-update is missing.'; return 1; }
  [[ -f /etc/pam.d/common-auth ]] || { warn 'PAM common-auth is missing.'; return 1; }
  if ! grep -q 'pam_unix\.so' /etc/pam.d/common-auth; then
    warn 'No pam_unix password fallback was detected in common-auth. Refusing to change PAM.'
    return 1
  fi
  if grep -q 'pam_fprintd\.so' /etc/pam.d/common-auth; then
    say 'Fingerprint PAM already appears enabled. Leaving existing configuration unchanged.'
    return 0
  fi
  if ! grep -q 'pam-auth-update' /etc/pam.d/common-auth; then
    warn 'common-auth appears custom/nonstandard. Refusing to rewrite Alternix authentication.'
    return 1
  fi
  backup_pam
  info 'Enabling standard fingerprint authentication (explicit opt-in)'
  # Do not use --force, nor edit individual /etc/pam.d services directly.
  if ! DEBIAN_FRONTEND=noninteractive pam-auth-update --enable fprintd; then
    warn 'pam-auth-update failed. PAM backup is available; no forced modification was attempted.'
    return 1
  fi
  if grep -q 'pam_fprintd\.so' /etc/pam.d/common-auth && \
     grep -q 'pam_unix\.so' /etc/pam.d/common-auth; then
    say 'Fingerprint PAM enabled. Password authentication remains configured.'
    warn 'Keep an existing logged-in session open until you test authentication in a second terminal.'
    return 0
  fi
  warn 'pam-auth-update did not produce a confirmed fingerprint + password configuration.'
  warn 'No forced changes were attempted. Inspect the PAM configuration and backup.'
  return 1
}

info "AlternixOS fingerprint setup $SCRIPT_VERSION"
scan_hardware
if [[ "$MODE" == scan ]]; then
  if command -v fprintd-list >/dev/null 2>&1; then check_fprintd || true; fi
  info 'Scan finished — no packages, fingerprints or PAM configuration changed'
  exit 0
fi
(( EUID == 0 )) || fail 'Installation needs root. Run: sudo bash alternix-fingerprint-setup.sh'

# Back up current authentication files *before* libpam-fprintd's package
# maintainer scripts run. The installed default profile is normally off.
backup_pam
apt_install_stack
start_dbus_if_needed

if ! check_fprintd; then
  info 'Packages installed, but no supported reader is currently usable'
  say 'Possible causes: absent/disabled sensor, unsupported USB/SPI hardware, or firmware/driver limitations.'
  say 'Check the hardware IDs shown above against https://fprint.freedesktop.org/supported-devices.html'
  say 'That development list can be newer than the driver version provided by Devuan.'
  say 'No fingerprint enrollment or PAM changes have been attempted.'
  exit 0
fi

if [[ "$ENROLL" == yes ]] || { [[ "$ENROLL" == ask ]] && ask_yes 'Enroll a fingerprint now?'; }; then
  enroll_and_verify || true
elif [[ "$ENROLL" != no ]]; then
  say "Enrollment skipped. Later, as your normal user, run: fprintd-enroll"
fi

if (( ENABLE_PAM )); then
  if (( ! VERIFIED )); then
    # Handles a rerun with an already-enrolled fingerprint, without requiring
    # users to re-enroll each time PAM activation is requested.
    verify_fingerprint || true
  fi
  enable_pam_safely || true
else
  say 'System-wide fingerprint login remains disabled unless already configured.'
  say 'To enable standard PAM authentication, rerun with --enable-pam and verify a live scan.'
fi
info 'Fingerprint setup finished'
say 'Note: osm-lock custom pattern/PIN authentication is separate from PAM.'
say 'Fingerprint login in that custom UI requires a future osm-lock integration.'
