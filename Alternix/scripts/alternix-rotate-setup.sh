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
    if command -v apt-get >/dev/null 2>&1; then
        apt-get install -y linux-firmware firmware-misc-nonfree >/dev/null 2>&1
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
        HINT="linux-firmware / firmware-misc-nonfree were installed automatically — reboot and scan again."
    elif [ ! -s "$DMESG_LOG" ]; then
        REASON="No sensor-related kernel messages found"
        HINT="If this is unexpected, the kernel ring buffer may be restricted (dmesg needs root) — re-run this scan as root/sudo to be sure. Otherwise this device may not have a rotation sensor fitted, or it's disabled in BIOS/UEFI settings."
    else
        REASON="No accelerometer detected after trying known driver stacks"
        HINT="See $DMESG_LOG for the sensor-related kernel log lines captured during this scan."
    fi
fi

# ---------------------------------------------------------------------
# iio-sensor-proxy.
#
# Rather than forking the binary ourselves (which fails silently if this
# process doesn't have permission to own net.hadess.SensorProxy on the
# system bus), first just talk to the bus name — dbus-daemon itself can
# auto-activate a registered D-Bus system service on demand, independent
# of systemd, as long as the package installed a .service file under
# /usr/share/dbus-1/system-services/. That activation runs with whatever
# privilege the .service file's Exec= + dbus policy grants (usually
# root), so it works even when THIS script isn't root.
# ---------------------------------------------------------------------
if command -v iio-sensor-proxy >/dev/null 2>&1; then
    PROXY_INSTALLED="yes"
elif command -v apt-get >/dev/null 2>&1; then
    apt-get install -y iio-sensor-proxy >/dev/null 2>&1
    command -v iio-sensor-proxy >/dev/null 2>&1 && PROXY_INSTALLED="yes"
fi

if [ "$PROXY_INSTALLED" = "yes" ] && ! pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
    # Ping the well-known name — this is enough to trigger D-Bus
    # activation if a system-services file exists for it.
    dbus-send --system --print-reply --dest=net.hadess.SensorProxy \
        /net/hadess/SensorProxy org.freedesktop.DBus.Peer.Ping \
        > "$ACTIVATE_ERR_LOG" 2>&1

    for i in 1 2 3 4 5; do
        pgrep -x iio-sensor-proxy >/dev/null 2>&1 && break
        sleep 1
    done
fi

if [ "$PROXY_INSTALLED" = "yes" ] && ! pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
    # D-Bus activation didn't bring it up (no .service file installed,
    # or this account isn't allowed to trigger/own it). Fall back to
    # running it directly with verbose logging, so if it's a permission
    # problem the daemon's own error ends up in PROXY_LOG.
    iio-sensor-proxy -v > "$PROXY_LOG" 2>&1 &
    disown
    sleep 2
fi

if pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
    PROXY_RUNNING="yes"
elif [ "$PROXY_INSTALLED" = "yes" ]; then
    if [ "$SENSOR_FOUND" = "yes" ]; then
        if grep -qiE 'not.*provided.*service|ServiceUnknown' "$ACTIVATE_ERR_LOG" 2>/dev/null; then
            REASON="No D-Bus service-activation file for iio-sensor-proxy"
            HINT="The package didn't install /usr/share/dbus-1/system-services/net.hadess.SensorProxy.service (some builds only ship a systemd unit, which Devuan/sysvinit won't use). See $PROXY_LOG for the direct-launch attempt's own error."
        elif grep -qiE 'AccessDenied|not authorized|denied' "$ACTIVATE_ERR_LOG" "$PROXY_LOG" 2>/dev/null; then
            REASON="D-Bus denied permission to start/own iio-sensor-proxy"
            HINT="This account (RAN_AS_ROOT=$RAN_AS_ROOT) likely isn't covered by the D-Bus system policy for net.hadess.SensorProxy. Re-run as root, or add an <allow own=\"net.hadess.SensorProxy\"/> rule for this user in /etc/dbus-1/system.d/."
        else
            REASON="iio-sensor-proxy exited immediately despite a detected sensor"
            HINT="See $PROXY_LOG and $ACTIVATE_ERR_LOG for the exact error."
        fi
    elif [ -z "$REASON" ]; then
        REASON="No accelerometer to start iio-sensor-proxy against"
    fi
elif [ -z "$REASON" ]; then
    REASON="iio-sensor-proxy is not installed and the automatic install failed"
    HINT="Install it manually (e.g. 'sudo apt-get install iio-sensor-proxy') and re-run this scan."
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
echo "ORIENTATION=$ORIENTATION"
echo "RAN_AS_ROOT=$RAN_AS_ROOT"
echo "REASON=$REASON"
echo "HINT=$HINT"
echo "LOG_PATH=$PROXY_LOG"
echo "DMESG_LOG=$DMESG_LOG"
echo "ACTIVATE_LOG=$ACTIVATE_ERR_LOG"
