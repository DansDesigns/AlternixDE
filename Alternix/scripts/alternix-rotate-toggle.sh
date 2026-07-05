#!/bin/bash
# alternix-rotate-toggle.sh
# Starts or stops the auto-rotate monitor.
# Called by osm-notify's Auto-Rotate card and the Display settings page.
#
# Usage: alternix-rotate-toggle.sh on|off

MODE="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIDFILE="$HOME/.config/Alternix/rotate-monitor.pid"

stop_monitor() {
    if [ -f "$PIDFILE" ]; then
        pid=$(cat "$PIDFILE")
        kill "$pid" >/dev/null 2>&1
        rm -f "$PIDFILE"
    fi
}

reset_orientation() {
    local o
    o=$(xrandr | awk '/ primary/{print $1; exit}')
    [ -z "$o" ] && o=$(xrandr | awk '/ connected/{print $1; exit}')
    [ -n "$o" ] && xrandr --output "$o" --rotate normal >/dev/null 2>&1
}

case "$MODE" in
    on)
        stop_monitor
        nohup "$SCRIPT_DIR/alternix-rotate-monitor.sh" >/dev/null 2>&1 &
        disown
        ;;
    off)
        stop_monitor
        reset_orientation
        ;;
    *)
        echo "Usage: $0 on|off" >&2
        exit 1
        ;;
esac
