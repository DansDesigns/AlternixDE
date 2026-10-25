#!/bin/bash
# alternix-rotate-setup.sh
#
# Cross-device accelerometer/rotation-sensor detector and fixer.
#
# Laptops and tablets expose their accelerometer to Linux through several
# different transport paths depending on vendor/generation:
#   - Intel Sensor Hub (ISH): intel_ishtp -> intel_ishtp_hid ->
#     hid-sensor-hub -> hid-sensor-accel-3d -> IIO   (Miix 520 and most
#     recent Intel-based 2-in-1s)
#   - ACPI HID-over-I2C sensors that show up as generic HID sensor hubs
#     without needing ISH at all (some non-Intel-hub laptops)
#   - Direct I2C/SPI accelerometer chips wired straight to IIO
#     (kxcjk1013, bmc150, bmi160, mxc4005, fxos8700, mma8452, adxl345,
#     various st_accel parts)
#   - ChromeOS-derived embedded controllers (cros_ec_accel_legacy /
#     cros_ec_sensors) on Chromebook-lineage hardware
#
# Rather than guess which one applies, this tries all of them (loading an
# irrelevant/unavailable module is a harmless no-op) and, if nothing is
# found — or the sensor's there but iio-sensor-proxy still won't stay up —
# captures *why*, including whether the failure looks like a permissions
# problem, which is common: owning the net.hadess.SensorProxy D-Bus
# system-bus name is usually gated to root by policy.
#
# Output: KEY=VALUE lines, parsed by the Display settings page. Safe to
# re-run any time — nothing here is destructive.

set -u

LOGDIR="$HOME/.config/Alternix/logs"
mkdir -p "$LOGDIR"
PROXY_LOG="$LOGDIR/iio-sensor-proxy.log"
DMESG_LOG="$LOGDIR/rotate-dmesg.log"
ACTIVATE_ERR_LOG="$LOGDIR/rotate-dbus-activate.log"

SENSOR_FOUND="no"
SENSOR_PATH=""
SENSOR_NAME=""
DRIVER_LOADED="none"
PROXY_INSTALLED="no"
PROXY_RUNNING="no"
ORIENTATION="unknown"
REASON=""
HINT=""
RAN_AS_ROOT="no"
[ "$(id -u)" = "0" ] && RAN_AS_ROOT="yes"

# ---------------------------------------------------------------------
# Scans /sys/bus/iio/devices for anything that looks like an
# accelerometer, by sysfs attribute or by name. Sets the SENSOR_* vars.
# ---------------------------------------------------------------------
scan_iio() {
    SENSOR_FOUND="no"
    SENSOR_PATH=""
    SENSOR_NAME=""
    DRIVER_LOADED="none"

    for dev in /sys/bus/iio/devices/iio:device*; do
        [ -d "$dev" ] || continue
        [ -f "$dev/name" ] || continue
        name=$(cat "$dev/name" 2>/dev/null)

        if [ -f "$dev/in_accel_x_raw" ] || [ -f "$dev/in_accel_x_input" ] || \
           echo "$name" | grep -qiE 'accel|kxcj|kxsd|bmc150|bmi160|mxc4005|fxos|mma8|adxl|kionix|kiox|st_accel'; then
            SENSOR_FOUND="yes"
            SENSOR_PATH="$dev"
            SENSOR_NAME="$name"
            real=$(readlink -f "$dev/device/driver" 2>/dev/null)
            [ -n "$real" ] && DRIVER_LOADED=$(basename "$real")
            return 0
        fi
    done
    return 1
}

# ---------------------------------------------------------------------
# Pass 1 — maybe it's already there, no driver work needed.
# ---------------------------------------------------------------------
scan_iio

# ---------------------------------------------------------------------
# Pass 2 — load every plausible driver stack and rescan. (modprobe on
# something already loaded, built-in, or unavailable for this kernel is
# a harmless no-op — safe to always run this even when Pass 1 succeeded,
# since it costs nothing and covers hardware that needs a nudge.)
# ---------------------------------------------------------------------
if [ "$SENSOR_FOUND" = "no" ]; then
    MODULES="
        industrialio industrialio-triggered-buffer kfifo_buf
        intel_ishtp intel_ishtp_hid intel_ishtp_loader
        hid_sensor_hub hid_sensor_accel_3d hid_sensor_trigger hid_sensor_iio_common
        cros_ec_accel_legacy cros_ec_sensors cros_ec_sensors_core cros_ec_sensorhub
        kxcjk1013 kxsd9 kxsd9_i2c
        bmc150_accel_i2c bmc150_accel_spi
        bmi160_i2c bmi160_spi bmi160_core
        mxc4005 fxos8700_i2c mma8452 adxl345_i2c st_accel_i2c st_accel_spi
    "
    for m in $MODULES; do
        modprobe "$m" >/dev/null 2>&1
    done
    sleep 2
    scan_iio
fi

# ---------------------------------------------------------------------
# Pass 3 — some ISH stacks fail silently for want of firmware blobs
# rather than a missing module.
# ---------------------------------------------------------------------
if [ "$SENSOR_FOUND" = "no" ]; then
    # Avoid a silent, guaranteed-to-fail APT attempt from osm-settings.
    # Firmware installation requires administrative privileges and should
    # not be treated as successful merely because APT was invoked.
    if [ "$RAN_AS_ROOT" = "yes" ] && command -v apt-get >/dev/null 2>&1; then
        apt-get install -y linux-firmware firmware-misc-nonfree >/dev/null 2>&1 || true
    fi
    modprobe -r intel_ishtp_hid intel_ishtp >/dev/null 2>&1
    modprobe intel_ishtp intel_ishtp_hid >/dev/null 2>&1
    sleep 2
    scan_iio
fi

# ---------------------------------------------------------------------
# Kernel log capture, with fallbacks for when the ring buffer itself is
# restricted (kernel.dmesg_restrict=1 is common and blocks non-root
# reads regardless of root status of THIS script if it's not actually
# root — dmesg failing with "Operation not permitted" is a strong sign
# we're not running with enough privilege here).
# ---------------------------------------------------------------------
dmesg_out=""
if dmesg_out=$(dmesg 2>/dev/null) && [ -n "$dmesg_out" ]; then
    :
elif [ -r /var/log/kern.log ]; then
    dmesg_out=$(tail -n 400 /var/log/kern.log 2>/dev/null)
elif [ -r /var/log/syslog ]; then
    dmesg_out=$(tail -n 400 /var/log/syslog 2>/dev/null)
fi
echo "$dmesg_out" | grep -iE 'ish|sensor|accel|iio' | tail -60 > "$DMESG_LOG" 2>/dev/null || true

# ---------------------------------------------------------------------
# Diagnosis if the sensor still wasn't found.
# ---------------------------------------------------------------------
if [ "$SENSOR_FOUND" = "no" ]; then
    if grep -qiE 'ish.*(fail|error)|ishtp.*(fail|error)' "$DMESG_LOG" 2>/dev/null; then
        REASON="Sensor hub driver loaded but reported an error"
        HINT="Check $DMESG_LOG for ish/ishtp error lines — often a missing firmware blob, or the sensor hub disabled in BIOS/UEFI firmware settings."
    elif grep -qi 'firmware' "$DMESG_LOG" 2>/dev/null; then
        REASON="A firmware file the sensor driver needs may be missing"
        HINT="Check which firmware package supplies this device on AlternixOS/Devuan. If firmware was installed by an administrator, reboot and scan again."
    elif [ ! -s "$DMESG_LOG" ]; then
        REASON="No sensor-related kernel messages found"
        HINT="If this is unexpected, the kernel ring buffer may be restricted (dmesg needs root) — re-run this scan as root/sudo to be sure. Otherwise this device may not have a rotation sensor fitted, or it's disabled in BIOS/UEFI settings."
    else
        REASON="No accelerometer detected after trying known driver stacks"
        HINT="See $DMESG_LOG for the sensor-related kernel log lines captured during this scan."
    fi
fi

# ---------------------------------------------------------------------
# iio-sensor-proxy: package installation, executable location, and D-Bus
# availability are distinct checks. On Debian/Devuan the executable is
# typically /usr/libexec/iio-sensor-proxy (NOT on an ordinary user's PATH).
# Do not treat "command not found" or an absent process as "not installed".
# ---------------------------------------------------------------------
PROXY_BINARY=""
PROXY_ACCESSIBLE="no"

find_proxy_binary() {
    local candidate
    candidate=$(command -v iio-sensor-proxy 2>/dev/null || true)
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        PROXY_BINARY="$candidate"
        return 0
    fi

    for candidate in /usr/libexec/iio-sensor-proxy \
                     /usr/lib/iio-sensor-proxy \
                     /usr/bin/iio-sensor-proxy \
                     /usr/sbin/iio-sensor-proxy; do
        if [ -x "$candidate" ]; then
            PROXY_BINARY="$candidate"
            return 0
        fi
    done

    # Covers distributions that install their daemon in an alternate path.
    if command -v dpkg-query >/dev/null 2>&1; then
        while IFS= read -r candidate; do
            case "$candidate" in
                */iio-sensor-proxy)
                    if [ -f "$candidate" ] && [ -x "$candidate" ]; then
                        PROXY_BINARY="$candidate"
                        return 0
                    fi
                    ;;
            esac
        done < <(dpkg-query -L iio-sensor-proxy 2>/dev/null)
    fi
    return 1
}

check_proxy_installed() {
    # Query the Debian package database first; the daemon is not generally
    # located on PATH. Status is e.g. "install ok installed".
    if command -v dpkg-query >/dev/null 2>&1 &&
       [ "$(dpkg-query -W -f='${Status}' iio-sensor-proxy 2>/dev/null)" = "install ok installed" ]; then
        return 0
    fi
    [ -n "$PROXY_BINARY" ] || command -v monitor-sensor >/dev/null 2>&1
}

# Peer.Ping verifies that the proxy actually owns its well-known name on
# the system bus. D-Bus may activate it when a service registration exists.
# "pgrep -x" is not suitable: Linux comm names are limited to 15 chars,
# whereas "iio-sensor-proxy" is 16 chars long.
probe_proxy_bus() {
    command -v dbus-send >/dev/null 2>&1 || return 1
    dbus-send --system --print-reply --reply-timeout=2500 \
        --dest=net.hadess.SensorProxy /net/hadess/SensorProxy \
        org.freedesktop.DBus.Peer.Ping >"$ACTIVATE_ERR_LOG" 2>&1
}

find_proxy_binary || true
if check_proxy_installed; then
    PROXY_INSTALLED="yes"
elif [ "$RAN_AS_ROOT" = "yes" ] && command -v apt-get >/dev/null 2>&1; then
    # Only attempt an installation when the scan was explicitly run as root.
    # A normal osm-settings session must not silently attempt privileged APT.
    if apt-get install -y iio-sensor-proxy >"$PROXY_LOG" 2>&1; then
        find_proxy_binary || true
        check_proxy_installed && PROXY_INSTALLED="yes"
    fi
fi

# The Debian/Devuan package typically includes a systemd unit, but
# AlternixOS uses SysVinit. A daemon installed in /usr/libexec does not
# automatically become a runnable SysV service. Keep this fix inside the
# normal rotation setup script (no secondary repair script required).
PROXY_SERVICE="none"
SERVICE_ERROR=""

# Install a small, root-owned SysV init script if the package provides no
# native service. This only runs when the user explicitly invokes setup as
# root; a normal osm-settings scan never silently elevates privileges.
install_sysv_proxy_service() {
    [ "$RAN_AS_ROOT" = "yes" ] || return 1
    [ -n "$PROXY_BINARY" ] && [ -x "$PROXY_BINARY" ] || return 1

    if [ -f /etc/init.d/iio-sensor-proxy ]; then
        PROXY_SERVICE="iio-sensor-proxy"
        return 0
    fi

    PROXY_SERVICE="alternix-iio-sensor-proxy"
    local init_file="/etc/init.d/$PROXY_SERVICE"
    if [ ! -f "$init_file" ]; then
        # Single-quoted heredoc: no untrusted path/commands are expanded.
        cat > "$init_file" <<'ALT_SYSV_SERVICE'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          alternix-iio-sensor-proxy
# Required-Start:    $remote_fs $syslog dbus
# Required-Stop:     $remote_fs $syslog dbus
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: IIO sensor proxy for AlternixOS SysVinit
### END INIT INFO

PATH=/usr/sbin:/usr/bin:/sbin:/bin
PIDFILE=/run/alternix-iio-sensor-proxy.pid
find_daemon() {
    for candidate in /usr/libexec/iio-sensor-proxy /usr/lib/iio-sensor-proxy \
                     /usr/sbin/iio-sensor-proxy /usr/bin/iio-sensor-proxy; do
        if [ -x "$candidate" ]; then
            DAEMON="$candidate"
            return 0
        fi
    done
    return 1
}
find_daemon || { echo "iio-sensor-proxy executable missing" >&2; exit 5; }

case "${1:-}" in
    start)
        # --exec checks /proc/PID/exe; it does not rely on the truncated
        # process name. --background handles this foreground GLib daemon.
        start-stop-daemon --start --quiet --oknodo \
            --background --make-pidfile --pidfile "$PIDFILE" \
            --exec "$DAEMON" --startas "$DAEMON"
        ;;
    stop)
        start-stop-daemon --stop --quiet --oknodo --retry=TERM/5/KILL/5 \
            --pidfile "$PIDFILE" --exec "$DAEMON"
        rc=$?
        rm -f "$PIDFILE"
        exit "$rc"
        ;;
    restart|force-reload)
        "$0" stop && "$0" start
        ;;
    status)
        if start-stop-daemon --status --exec "$DAEMON"; then
            echo "iio-sensor-proxy is running"
            exit 0
        fi
        echo "iio-sensor-proxy is not running"
        exit 3
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|force-reload|status}" >&2
        exit 2
        ;;
esac
ALT_SYSV_SERVICE
        if ! chmod 0755 "$init_file"; then
            SERVICE_ERROR="Could not set SysV init script permissions"
            rm -f "$init_file"
            return 1
        fi
        chown root:root "$init_file" 2>/dev/null || true
    fi

    # Register the service for future boots with SysVinit, not systemd.
    if command -v update-rc.d >/dev/null 2>&1; then
        if ! update-rc.d "$PROXY_SERVICE" defaults >/dev/null 2>&1; then
            SERVICE_ERROR="update-rc.d failed to register $PROXY_SERVICE"
            return 1
        fi
    else
        SERVICE_ERROR="update-rc.d not found; automatic boot startup not enabled"
        return 1
    fi
    return 0
}

start_sysv_proxy_service() {
    local name=""
    if [ -f /etc/init.d/iio-sensor-proxy ]; then
        name="iio-sensor-proxy"
    elif [ -f /etc/init.d/alternix-iio-sensor-proxy ]; then
        name="alternix-iio-sensor-proxy"
    else
        return 1
    fi
    PROXY_SERVICE="$name"
    if command -v invoke-rc.d >/dev/null 2>&1; then
        # invoke-rc.d respects local runlevel policy; direct invocation is
        # used if a policy prevents setup from starting the service now.
        invoke-rc.d "$name" start >"$PROXY_LOG" 2>&1 || \
            "/etc/init.d/$name" start >>"$PROXY_LOG" 2>&1
    else
        "/etc/init.d/$name" start >"$PROXY_LOG" 2>&1
    fi
}

if [ "$PROXY_INSTALLED" = "yes" ]; then
    if probe_proxy_bus; then
        PROXY_RUNNING="yes"
        PROXY_ACCESSIBLE="yes"
    else
        # First check for an existing service. Only root can start/init it.
        if [ -f /etc/init.d/iio-sensor-proxy ]; then
            PROXY_SERVICE="iio-sensor-proxy"
        elif [ -f /etc/init.d/alternix-iio-sensor-proxy ]; then
            PROXY_SERVICE="alternix-iio-sensor-proxy"
        fi

        if [ "$RAN_AS_ROOT" = "yes" ]; then
            install_sysv_proxy_service || true
            start_sysv_proxy_service || true
            for i in 1 2 3 4 5; do
                if probe_proxy_bus; then
                    PROXY_RUNNING="yes"
                    PROXY_ACCESSIBLE="yes"
                    break
                fi
                sleep 1
            done
        fi
    fi
fi

if [ "$PROXY_INSTALLED" != "yes" ]; then
    if [ -z "$REASON" ]; then
        REASON="iio-sensor-proxy package is not installed"
        HINT="Install it with administrative privileges: apt-get install iio-sensor-proxy."
    fi
elif [ "$PROXY_RUNNING" != "yes" ]; then
    # Service activation is a separate issue from package installation.
    # The Debian package may ship ONLY a systemd unit; Devuan doesn't use it.
    if [ -z "$PROXY_BINARY" ]; then
        PROXY_ISSUE="iio-sensor-proxy is installed, but its daemon executable was not found"
        PROXY_FIX="Check 'dpkg -L iio-sensor-proxy' and reinstall the package if necessary."
    elif grep -qiE 'ServiceUnknown|was not provided by any|not.*activat|Unit.*not found|org.freedesktop.systemd1' "$ACTIVATE_ERR_LOG" 2>/dev/null; then
        PROXY_ISSUE="iio-sensor-proxy is installed, but D-Bus could not activate it"
        PROXY_FIX="On AlternixOS/SysVinit run: sudo bash ~/.config/Alternix/scripts/alternix-rotate-setup.sh. This configures an /etc/init.d/ service. See $ACTIVATE_ERR_LOG."
    elif grep -qiE 'AccessDenied|not authorized|denied' "$ACTIVATE_ERR_LOG" "$PROXY_LOG" 2>/dev/null; then
        PROXY_ISSUE="iio-sensor-proxy is installed, but D-Bus denied its startup"
        PROXY_FIX="Check the D-Bus policy/service configuration. Do not grant ordinary users ownership of net.hadess.SensorProxy. See $ACTIVATE_ERR_LOG."
    else
        PROXY_ISSUE="iio-sensor-proxy is installed, but its D-Bus service is not running"
        PROXY_FIX="AlternixOS uses SysVinit. The proxy is at $PROXY_BINARY. Run the setup script once with administrator privileges to install/start the SysV service: sudo bash ~/.config/Alternix/scripts/alternix-rotate-setup.sh. See $ACTIVATE_ERR_LOG and $PROXY_LOG."
    fi
    if [ -z "$REASON" ]; then
        REASON="$PROXY_ISSUE"
        HINT="$PROXY_FIX"
    else
        HINT="${HINT:+$HINT }$PROXY_ISSUE. $PROXY_FIX"
    fi
fi

if [ -n "$SERVICE_ERROR" ]; then
    HINT="${HINT:+$HINT }$SERVICE_ERROR"
fi

if [ "$PROXY_RUNNING" = "yes" ]; then
    ORIENTATION=$(dbus-send --system --print-reply \
        --dest=net.hadess.SensorProxy \
        /net/hadess/SensorProxy \
        org.freedesktop.DBus.Properties.Get \
        string:net.hadess.SensorProxy string:AccelerometerOrientation \
        2>/dev/null | awk -F'"' '/string/{print $2}')
    [ -z "$ORIENTATION" ] && ORIENTATION="unknown"
fi

echo "SENSOR_FOUND=$SENSOR_FOUND"
echo "SENSOR_PATH=$SENSOR_PATH"
echo "SENSOR_NAME=$SENSOR_NAME"
echo "DRIVER_LOADED=$DRIVER_LOADED"
echo "PROXY_INSTALLED=$PROXY_INSTALLED"
echo "PROXY_RUNNING=$PROXY_RUNNING"
echo "PROXY_BINARY=$PROXY_BINARY"
echo "PROXY_ACCESSIBLE=$PROXY_ACCESSIBLE"
echo "PROXY_SERVICE=$PROXY_SERVICE"
echo "SERVICE_ERROR=$SERVICE_ERROR"
echo "ORIENTATION=$ORIENTATION"
echo "RAN_AS_ROOT=$RAN_AS_ROOT"
echo "REASON=$REASON"
echo "HINT=$HINT"
echo "LOG_PATH=$PROXY_LOG"
echo "DMESG_LOG=$DMESG_LOG"
echo "ACTIVATE_LOG=$ACTIVATE_ERR_LOG"
