#!/usr/bin/env bash
# AlternixOS X11 rotation service and tablet/touch coordinate mapping.
# Run by alternix-rotate-toggle.sh. --sync-input is safe for one-shot use.
# Monitor uses monitor-sensor (keeps the D-Bus accelerometer claim alive).

set -u
CONFIG_DIR="$HOME/.config/Alternix"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PIDFILE="$CONFIG_DIR/rotate-monitor.pid"
READYFILE="$CONFIG_DIR/rotate-monitor.ready"
LOCKFILE="$CONFIG_DIR/rotate-monitor.lock"
INVERT_FILE="$CONFIG_DIR/rotate-invert"
PEN_CONFIG="$CONFIG_DIR/stylus.conf"
LOGFILE="$CONFIG_DIR/logs/rotate-monitor.log"
mkdir -p "$CONFIG_DIR" "$CONFIG_DIR/logs"

log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOGFILE"; }

config_value() {
    local key="$1" value
    [ -r "$PEN_CONFIG" ] || return 0
    value=$(sed -n "s/^${key}=//p" "$PEN_CONFIG" | tail -n 1)
    printf '%s' "$value"
}

# Prefer the built-in panel over an external primary monitor.
select_output() {
    local configured name
    configured=$(config_value OUTPUT)
    if [ -n "$configured" ] && [ "$configured" != auto ]; then
        if xrandr --query | awk -v n="$configured" '$1==n && $2=="connected"{found=1} END{exit !found}'; then
            printf '%s\n' "$configured"
            return
        fi
    fi
    name=$(xrandr --query | awk '$2=="connected" && $1 ~ /^(eDP|DSI|LVDS)/{print $1; exit}')
    [ -n "$name" ] || name=$(xrandr --query | awk '$2=="connected" && $3=="primary"{print $1; exit}')
    [ -n "$name" ] || name=$(xrandr --query | awk '$2=="connected"{print $1; exit}')
    printf '%s\n' "$name"
}

output_geometry() {
    local output="$1"
    xrandr --query | awk -v target="$output" '
      $1==target && $2=="connected" {
        for (i=3;i<=NF;i++) {
          if ($i ~ /^[0-9]+x[0-9]+[+][-]?[0-9]+[+][-]?[0-9]+$/) {print $i; exit}
        }
      }'
}

output_rotation() {
    xrandr --query | awk -v target="$1" '
      $1==target && $2=="connected" {
        for (i=3;i<=NF;i++) if ($i ~ /^\((normal|left|right|inverted)$/) {
          gsub(/\(/,"",$i); print $i; exit
        }
      }'
}

# xinput's CTM transforms device coordinates into the full X11 desktop.
# The output location/size must be included for multi-monitor layouts.
make_matrix() {
    local rot="$1" geometry="$2" desktop="$3"
    local width height x y dw dh
    # XRandR geometry is WxH+X+Y (or WxH-X+Y with translated layouts).
    # Parse with bash rather than splitting on +: negative coordinates matter
    # when the user places a secondary monitor to the left of the tablet.
    if [[ ! "$geometry" =~ ^([0-9]+)x([0-9]+)([+-][0-9]+)([+-][0-9]+)$ ]]; then
        log "Unrecognized output geometry: $geometry"
        return 1
    fi
    width="${BASH_REMATCH[1]}"; height="${BASH_REMATCH[2]}"
    x="${BASH_REMATCH[3]}"; y="${BASH_REMATCH[4]}"
    if [[ ! "$desktop" =~ ^([0-9]+)x([0-9]+)$ ]]; then
        log "Unrecognized desktop geometry: $desktop"
        return 1
    fi
    dw="${BASH_REMATCH[1]}"; dh="${BASH_REMATCH[2]}"
    awk -v rot="$rot" -v w="$width" -v h="$height" -v x="$x" -v y="$y" \
        -v dw="$dw" -v dh="$dh" 'BEGIN {
        if (w<=0 || h<=0 || dw<=0 || dh<=0) exit 1
        sx=w/dw; sy=h/dh; ox=x/dw; oy=y/dh
        # CTM maps physical, unrotated absolute touch/pen coordinates into
        # the logical desktop. The inverted case is a FULL 180-degree turn.
        if (rot=="left")             { A=0; B=-1; C=1; D=1; E=0; F=0 }
        else if (rot=="right")       { A=0; B=1; C=0; D=-1; E=0; F=1 }
        else if (rot=="inverted")    { A=-1; B=0; C=1; D=0; E=-1; F=1 }
        else if (rot=="normal")      { A=1; B=0; C=0; D=0; E=1; F=0 }
        else exit 1
        printf "%.8f %.8f %.8f %.8f %.8f %.8f 0 0 1\n", sx*A,sx*B,sx*C+ox,sy*D,sy*E,sy*F+oy
    }'
}

# Device names alone are not reliable (the Miix 520 may show a vendor HID
# identifier rather than 'touchscreen' or 'pen'). Prefer udev classification;
# fall back to XInput properties and the device's name. Never remap relative
# mice or touchpads. Their on-screen pointer already follows display geometry.
input_kind() {
    local id="$1" name="$2" props node udev=''
    props=$(xinput list-props "$id" 2>/dev/null) || props=''
    node=$(printf '%s\n' "$props" | sed -n 's/.*Device Node.*"\([^"]*\)".*/\1/p' | head -n1)
    if [[ "$node" == /dev/input/event* ]] && command -v udevadm >/dev/null 2>&1; then
        udev=$(udevadm info --query=property --name="$node" 2>/dev/null) || :
    fi
    if grep -qx 'ID_INPUT_TOUCHSCREEN=1' <<< "$udev"; then printf 'touch'; return; fi
    if grep -qxE 'ID_INPUT_(TABLET|TABLET_TOOL|STYLUS|PEN)=1' <<< "$udev"; then printf 'pen'; return; fi
    if grep -qxE 'ID_INPUT_(TOUCHPAD|MOUSE|POINTINGSTICK|TABLET_PAD)=1' <<< "$udev"; then
        return
    fi
    case "${name,,}" in
        *touchpad*|*trackpoint*|*mouse*|*virtual\ core*|*xtest*|*keyboard*|*tablet\ pad*|*pad\ pad*) return ;;
    esac
    if [[ "$props" == *'libinput Tablet Tool Pressurecurve'* ]] ||
       [[ "$props" == *'Wacom Tool Type'* ]]; then
        printf 'pen'; return
    fi
    # Absolute-position libinput devices export a calibration matrix, even
    # when their vendor-specific XInput name omits 'touchscreen'.
    if [[ "$props" == *'libinput Calibration Matrix'* ]]; then
        printf 'touch'; return
    fi
    case "${name,,}" in
        *touchscreen*|*touch\ screen*|*finger*|*multitouch*|*multi-touch*|*touch\ input*) printf 'touch' ;;
        *stylus*|*eraser*|*digitizer*|*pen*|*tablet\ tool*) printf 'pen' ;;
    esac
}

sync_input() {
    command -v xrandr >/dev/null 2>&1 || { log 'xrandr unavailable'; return 1; }
    command -v xinput >/dev/null 2>&1 || { log 'xinput unavailable'; return 1; }
    local output rot geometry desktop matrix wrot line id name kind type
    local -A wacom_processed=()
    local total=0 failures=0
    output=$(select_output)
    [ -n "$output" ] || { log 'No connected X11 display output'; return 1; }
    geometry=$(output_geometry "$output")
    [ -n "$geometry" ] || { log "No XRandR geometry for $output"; return 1; }
    rot=$(output_rotation "$output")
    [ -n "$rot" ] || rot=normal
    desktop=$(xrandr --query | sed -n 's/.*current \([0-9]\+\) x \([0-9]\+\).*/\1x\2/p' | head -n1)
    matrix=$(make_matrix "$rot" "$geometry" "$desktop") || return 1
    [[ -n "$matrix" ]] || return 1

    case "$rot" in
        left) wrot=ccw ;; right) wrot=cw ;;
        inverted) wrot=half ;; *) wrot=none ;;
    esac
    if command -v xsetwacom >/dev/null 2>&1; then
        while IFS= read -r line; do
            [[ "$line" =~ id:[[:space:]]*([0-9]+) ]] || continue
            id="${BASH_REMATCH[1]}"
            type="${line##*type:}"
            type="${type,,}"
            case "$type" in
                *stylus*|*eraser*|*touch*|*cursor*) ;;
                *) continue ;;    # Wacom pad buttons are not screen coordinates.
            esac
            # For the xf86-input-wacom driver, use its native Rotate plus
            # MapToOutput. Do NOT add a second XInput rotation CTM on top.
            if xsetwacom set "$id" Rotate "$wrot" >/dev/null 2>&1 && \
               xsetwacom set "$id" MapToOutput "$output" >/dev/null 2>&1; then
                wacom_processed[$id]=1
                ((total+=1))
            else
                # If either native operation fails, reset native rotation
                # BEFORE applying the generic CTM (avoids double-rotation).
                xsetwacom set "$id" Rotate none >/dev/null 2>&1 || :
                log "Wacom native orientation unavailable for ID $id; using CTM"
            fi
        done < <(xsetwacom --list devices 2>/dev/null)
    fi

    while IFS= read -r line; do
        [[ "$line" =~ id=([0-9]+) ]] || continue
        id="${BASH_REMATCH[1]}"
        [ -z "${wacom_processed[$id]+set}" ] || continue
        name="${line%%id=*}"
        kind=$(input_kind "$id" "$name")
        [ -n "$kind" ] || continue
        # All libinput/XInput touchscreen and tablet tools use the complete
        # 3x3 matrix (including 180 degrees and multi-monitor offsets).
        # An explicit float type avoids XInput interpreting 0/1 as integers.
        # shellcheck disable=SC2086
        if xinput set-prop --type=float "$id" 'Coordinate Transformation Matrix' $matrix 2>/dev/null; then
            ((total+=1))
        else
            log "Input CTM failed for $kind $id: $name"
            ((failures+=1))
        fi
    done < <(xinput --list --short 2>/dev/null)

    # Explicit commands can be run in the user's own X11 session to inspect
    # which devices actually got mapped without needing root permissions.
    if [ "${1:-}" = verbose ]; then
        printf 'OUTPUT=%s\nROTATION=%s\nGEOMETRY=%s\nDESKTOP=%s\nMATRIX=%s\nMAPPED=%s\nFAILED=%s\n' \
            "$output" "$rot" "$geometry" "$desktop" "$matrix" "$total" "$failures"
    fi
    if (( total == 0 )); then
        log "No touch/pen absolute devices mapped (output $output, rotation $rot); check xinput --list"
    fi
    (( failures == 0 ))
}

if [ "${1:-}" = '--sync-input' ]; then
    sync_input verbose
    exit $?
fi

if [ "${1:-}" != '' ]; then
    echo 'Usage: alternix-rotate-monitor.sh [--sync-input]' >&2
    exit 2
fi

command -v xrandr >/dev/null 2>&1 || { log 'xrandr missing'; exit 1; }
command -v monitor-sensor >/dev/null 2>&1 || { log 'monitor-sensor missing: install iio-sensor-proxy'; exit 1; }
command -v flock >/dev/null 2>&1 || { log 'flock missing'; exit 1; }

exec 9>"$LOCKFILE"
flock -n 9 || exit 0
printf '%s\n' "$$" > "$PIDFILE"
rm -f "$READYFILE"

sensor_pid=''
pipe_dir=''
cleanup() {
    trap - EXIT INT TERM
    [ -z "$sensor_pid" ] || { kill "$sensor_pid" 2>/dev/null || :; wait "$sensor_pid" 2>/dev/null || :; }
    [ -z "$pipe_dir" ] || rm -rf -- "$pipe_dir"
    if [ "$(cat "$PIDFILE" 2>/dev/null)" = "$$" ]; then
        rm -f "$PIDFILE" "$READYFILE"
    fi
}
trap 'exit 0' INT TERM
trap cleanup EXIT

pipe_dir=$(mktemp -d "${TMPDIR:-/tmp}/alternix-rotate.XXXXXXXX") || exit 1
mkfifo "$pipe_dir/orient" || exit 1
# This process holds its D-Bus connection open, so the accelerometer remains
# claimed. A one-shot dbus-send ClaimAccelerometer does NOT do that.
if command -v stdbuf >/dev/null 2>&1; then
    stdbuf -oL -eL monitor-sensor --accel > "$pipe_dir/orient" 2>&1 &
else
    monitor-sensor --accel > "$pipe_dir/orient" 2>&1 &
fi
sensor_pid=$!
exec 3< "$pipe_dir/orient"

last_orientation=''
pending=''
pending_since=0
started_at=$SECONDS
last_sync=$SECONDS
trap 'pending="$last_orientation"; last_orientation=""; pending_since=0' USR1
while :; do
    if IFS= read -r -t 1 line <&3; then
        if [[ "$line" =~ orientation:[[:space:]]*([a-z-]+) ]] || \
           [[ "$line" =~ orientation[[:space:]]changed:[[:space:]]*([a-z-]+) ]]; then
            value="${BASH_REMATCH[1]}"
            case "$value" in
                normal|bottom-up|left-up|right-up)
                    if [ "$value" != "$last_orientation" ]; then
                        pending="$value"
                        pending_since=$SECONDS
                    fi
                    ;;
            esac
        elif [[ "$line" == *'No accelerometer'* ]]; then
            log "Sensor unavailable: $line"
        elif [[ "$line" == *'failed'* || "$line" == *'denied'* ]]; then
            log "Sensor monitor: $line"
        fi
    fi
    if ! kill -0 "$sensor_pid" 2>/dev/null; then
        log 'monitor-sensor exited; see service/accelerometer setup'
        exit 1
    fi
    if [ -n "$pending" ]; then
        # Apply initial orientation immediately; debounce later changes so
        # a sensor crossing two axes does not rapidly flip the interface.
        if [ -z "$last_orientation" ] || (( SECONDS - pending_since >= 1 )); then
            target=normal
            case "$pending" in
                normal) target=normal ;; bottom-up) target=inverted ;;
                left-up) target=right ;; right-up) target=left ;;
            esac
            if [ "$(cat "$INVERT_FILE" 2>/dev/null)" = 'yes' ]; then
                case "$target" in left) target=right ;; right) target=left ;; esac
            fi
            output=$(select_output)
            if [ -n "$output" ] && xrandr --output "$output" --rotate "$target" 2>>"$LOGFILE"; then
                sync_input || :
                last_orientation="$pending"
                printf '%s\n' "$$" > "$READYFILE"
                last_sync=$SECONDS
            else
                log "Could not rotate display output: $output"
            fi
            pending=''
        fi
    fi
    # Remap after monitor hotplug or output geometry changes even if the
    # accelerometer has not changed orientation.
    if (( SECONDS - last_sync >= 8 )); then
        sync_input || :
        last_sync=$SECONDS
    fi
    if [ -z "$last_orientation" ] && (( SECONDS - started_at >= 25 )); then
        log 'No usable orientation from monitor-sensor after 25 seconds'
        exit 1
    fi
done
