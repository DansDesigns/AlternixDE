#!/bin/bash
# alternix-rotate-monitor.sh
#
# Background loop: claims the accelerometer from iio-sensor-proxy and
# rotates the display with xrandr whenever AccelerometerOrientation
# changes. Started/stopped by alternix-rotate-toggle.sh — don't run this
# by hand except to test it (Ctrl+C to stop; it cleans up after itself).

PIDFILE="$HOME/.config/Alternix/rotate-monitor.pid"
INVERT_FILE="$HOME/.config/Alternix/rotate-invert"

mkdir -p "$HOME/.config/Alternix"
echo $$ > "$PIDFILE"

cleanup() {
    dbus-send --system --dest=net.hadess.SensorProxy \
        /net/hadess/SensorProxy net.hadess.SensorProxy.ReleaseAccelerometer \
        >/dev/null 2>&1
    rm -f "$PIDFILE"
    exit 0
}
trap cleanup TERM INT

detect_output() {
    local o
    o=$(xrandr | awk '/ primary/{print $1; exit}')
    [ -z "$o" ] && o=$(xrandr | awk '/ connected/{print $1; exit}')
    echo "$o"
}

OUTPUT=$(detect_output)

# Maps iio-sensor-proxy's AccelerometerOrientation values to xrandr
# --rotate arguments. This is the mapping used by most community
# rotate-helper scripts, but which physical edge counts as "left" vs
# "right" depends on how the chip is mounted on the board — if rotation
# comes out backwards on your panel, flip the "Invert rotation direction"
# toggle in Display settings rather than editing this.
apply_rotation() {
    local orientation="$1"
    local rot="normal"

    case "$orientation" in
        normal)    rot="normal" ;;
        bottom-up) rot="inverted" ;;
        left-up)   rot="right" ;;
        right-up)  rot="left" ;;
        *)         return ;;
    esac

    if [ -f "$INVERT_FILE" ] && [ "$(cat "$INVERT_FILE")" = "yes" ]; then
        case "$rot" in
            left)  rot="right" ;;
            right) rot="left" ;;
        esac
    fi

    [ -n "$OUTPUT" ] && xrandr --output "$OUTPUT" --rotate "$rot" >/dev/null 2>&1
}

read_orientation() {
    dbus-send --system --print-reply \
        --dest=net.hadess.SensorProxy /net/hadess/SensorProxy \
        org.freedesktop.DBus.Properties.Get \
        string:net.hadess.SensorProxy string:AccelerometerOrientation \
        2>/dev/null | awk -F'"' '/string/{print $2}'
}

# Tell iio-sensor-proxy someone wants readings.
dbus-send --system --dest=net.hadess.SensorProxy \
    /net/hadess/SensorProxy net.hadess.SensorProxy.ClaimAccelerometer \
    >/dev/null 2>&1

# Apply whatever the orientation already is before waiting for a change.
current=$(read_orientation)
[ -n "$current" ] && apply_rotation "$current"

# Watch for further orientation changes. dbus-monitor's exact output
# formatting can vary slightly between dbus versions; this expects the
# "AccelerometerOrientation" property name and its value to appear on
# two consecutive lines, which is the standard layout.
dbus-monitor --system \
  "type='signal',interface='org.freedesktop.DBus.Properties',path='/net/hadess/SensorProxy'" \
  2>/dev/null | \
while read -r line; do
    case "$line" in
        *AccelerometerOrientation*)
            read -r valueline
            orientation=$(echo "$valueline" | sed -n 's/.*string *"\([^"]*\)".*/\1/p')
            [ -n "$orientation" ] && apply_rotation "$orientation"
            ;;
    esac
done

cleanup
