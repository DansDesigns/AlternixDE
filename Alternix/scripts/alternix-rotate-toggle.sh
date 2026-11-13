#!/usr/bin/env bash
# AlternixOS user-session auto-rotation controller. X11 only, no root needed.
# on/off saves the selection; on --startup starts without altering preferences.
set -u
MODE="${1:-}"
STARTUP="${2:-}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$HOME/.config/Alternix"
PIDFILE="$CONFIG_DIR/rotate-monitor.pid"
READYFILE="$CONFIG_DIR/rotate-monitor.ready"
AUTOSTART="$HOME/.config/autostart/alternix-auto-rotate.desktop"
MONITOR="$SCRIPT_DIR/alternix-rotate-monitor.sh"
mkdir -p "$CONFIG_DIR"

# Refuse to kill an unrelated PID if a stale pidfile points to a reused PID.
monitor_pid() {
    local pid cmdline
    [ -r "$PIDFILE" ] || return 1
    pid=$(cat "$PIDFILE" 2>/dev/null)
    [[ "$pid" =~ ^[0-9]+$ ]] && (( pid > 1 )) || return 1
    [ -r "/proc/$pid/cmdline" ] || return 1
    cmdline=$(tr '\0' ' ' < "/proc/$pid/cmdline")
    [[ "$cmdline" == *alternix-rotate-monitor.sh* ]] || return 1
    printf '%s\n' "$pid"
}

stop_monitor() {
    local pid i
    pid=$(monitor_pid) || { rm -f "$PIDFILE" "$READYFILE"; return 0; }
    kill -TERM "$pid" 2>/dev/null || :
    for i in 1 2 3 4 5 6; do
        sleep 0.25
        monitor_pid >/dev/null || break
    done
    if ! monitor_pid >/dev/null; then
        rm -f "$PIDFILE" "$READYFILE"
    fi
}

reset_orientation() {
    local o configured
    command -v xrandr >/dev/null 2>&1 || return 0
    configured=$(sed -n 's/^OUTPUT=//p' "$CONFIG_DIR/stylus.conf" 2>/dev/null | tail -n 1)
    if [ -n "$configured" ] && [ "$configured" != auto ] && \
       xrandr --query | awk -v n="$configured" '$1==n && $2=="connected"{ok=1} END{exit !ok}'; then
        o="$configured"
    else
        o=''
    fi
    [ -n "$o" ] || o=$(xrandr --query | awk '$2=="connected" && $1 ~ /^(eDP|DSI|LVDS)/{print $1; exit}')
    [ -n "$o" ] || o=$(xrandr --query | awk '$2=="connected" && $3=="primary"{print $1; exit}')
    [ -n "$o" ] || o=$(xrandr --query | awk '$2=="connected"{print $1; exit}')
    if [ -n "$o" ]; then
        xrandr --output "$o" --rotate normal || return 1
        "$MONITOR" --sync-input || :
    fi
}

install_autostart() {
    # Desktop files quote paths, not shell command strings. No root needed.
    local escaped
    escaped=${MONITOR%/*}/alternix-rotate-toggle.sh
    escaped=${escaped//\\/\\\\}
    escaped=${escaped//\"/\\\"}
    escaped=${escaped//\$/\\\$}
    escaped=${escaped//\`/\\\`}
    mkdir -p "$(dirname "$AUTOSTART")"
    cat > "$AUTOSTART" <<ALT_DESKTOP
[Desktop Entry]
Type=Application
Name=Alternix Auto-Rotate
Comment=Start screen rotation in the X11 user session
Exec=/bin/bash "$escaped" on --startup
X-GNOME-Autostart-enabled=true
ALT_DESKTOP
}

case "$MODE" in
    on)
        if [ -z "${DISPLAY:-}" ]; then
            echo 'Auto-Rotate requires an X11 desktop session (DISPLAY is unset).' >&2
            exit 1
        fi
        for cmd in xrandr monitor-sensor xinput; do
            command -v "$cmd" >/dev/null 2>&1 || {
                echo "Missing command: $cmd (install X11 input tools / iio-sensor-proxy)." >&2
                exit 1
            }
        done
        [ -x "$MONITOR" ] || { echo "Monitor missing/not executable: $MONITOR" >&2; exit 1; }
        if [ "$STARTUP" != '--startup' ]; then install_autostart; fi
        if monitor_pid >/dev/null; then exit 0; fi
        rm -f "$PIDFILE" "$READYFILE"
        nohup /bin/bash "$MONITOR" >>"$CONFIG_DIR/rotate-launch.log" 2>&1 </dev/null &
        disown || :
        ;;
    refresh)
        pid=$(monitor_pid) || exit 0
        kill -USR1 "$pid" 2>/dev/null || :
        ;;
    off)
        rm -f "$AUTOSTART"
        stop_monitor
        reset_orientation
        ;;
    status)
        pid=$(monitor_pid) || exit 1
        [ "$(cat "$READYFILE" 2>/dev/null)" = "$pid" ] || exit 1
        echo "Auto-Rotate active (PID $pid)"
        ;;
    *)
        echo 'Usage: alternix-rotate-toggle.sh on|off|status|refresh [--startup]' >&2
        exit 2
        ;;
esac
