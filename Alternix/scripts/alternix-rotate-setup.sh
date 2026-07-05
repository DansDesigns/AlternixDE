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
# found, captures dmesg + iio-sensor-proxy's own verbose log so the
# Display settings page can show *why* it failed instead of just "no".
#
# Output: KEY=VALUE lines, parsed by the Display settings page. Safe to
# re-run any time — nothing here is destructive.

set -u

LOGDIR="$HOME/.config/Alternix/logs"
mkdir -p "$LOGDIR"
PROXY_LOG="$LOGDIR/iio-sensor-proxy.log"
DMESG_LOG="$LOGDIR/rotate-dmesg.log"

SENSOR_FOUND="no"
SENSOR_PATH=""
SENSOR_NAME=""
DRIVER_LOADED="none"
PROXY_INSTALLED="no"
PROXY_RUNNING="no"
ORIENTATION="unknown"
REASON=""
HINT=""

# ---------------------------------------------------------------------
# Scans /sys/bus/iio/devices for anything that looks like an
# accelerometer, by sysfs attribute or by name. Sets the SENSOR_* vars.
# Returns 0 if found, 1 if not.
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
# Pass 2 — load every plausible driver stack and rescan.
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
# rather than a missing module. Try installing common firmware packages
# and reloading the ISH modules once more before giving up.
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

# Capture recent sensor/accel/ISH-related kernel log lines regardless of
# outcome — useful even on success, essential on failure.
dmesg 2>/dev/null | grep -iE 'ish|sensor|accel|iio' | tail -60 > "$DMESG_LOG" 2>/dev/null || true

# ---------------------------------------------------------------------
# Build a diagnosis if we still came up empty.
# ---------------------------------------------------------------------
if [ "$SENSOR_FOUND" = "no" ]; then
    if grep -qiE 'ish.*(fail|error)|ishtp.*(fail|error)' "$DMESG_LOG" 2>/dev/null; then
        REASON="Sensor hub driver loaded but reported an error"
        HINT="Check $DMESG_LOG for ish/ishtp error lines — often a missing firmware blob, or the sensor hub disabled in BIOS/UEFI firmware settings."
    elif grep -qi 'firmware' "$DMESG_LOG" 2>/dev/null; then
        REASON="A firmware file the sensor driver needs may be missing"
        HINT="linux-firmware / firmware-misc-nonfree were installed automatically — reboot and scan again."
    elif [ ! -s "$DMESG_LOG" ]; then
        REASON="No sensor-related kernel messages at all"
        HINT="This device may not have a rotation sensor fitted, or the sensor hub is disabled in BIOS/UEFI settings."
    else
        REASON="No accelerometer detected after trying known driver stacks"
        HINT="See $DMESG_LOG for the sensor-related kernel log lines captured during this scan."
    fi
fi

# ---------------------------------------------------------------------
# iio-sensor-proxy: install, then start with verbose logging so a
# same-second exit (it quits immediately if it finds nothing usable)
# leaves behind a real reason instead of a silent "not running".
# ---------------------------------------------------------------------
if command -v iio-sensor-proxy >/dev/null 2>&1; then
    PROXY_INSTALLED="yes"
elif command -v apt-get >/dev/null 2>&1; then
    apt-get install -y iio-sensor-proxy >/dev/null 2>&1
    command -v iio-sensor-proxy >/dev/null 2>&1 && PROXY_INSTALLED="yes"
fi

if [ "$PROXY_INSTALLED" = "yes" ]; then
    if ! pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
        iio-sensor-proxy -v > "$PROXY_LOG" 2>&1 &
        disown
        sleep 2
    fi

    if pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
        PROXY_RUNNING="yes"
    elif [ "$SENSOR_FOUND" = "yes" ]; then
        # We found a device but the proxy still wouldn't stay up —
        # its own log is the authoritative reason here.
        REASON="iio-sensor-proxy exited immediately despite a detected sensor"
        HINT="See $PROXY_LOG for its own reported reason."
    elif [ -z "$REASON" ]; then
        REASON="No accelerometer to start iio-sensor-proxy against"
    fi
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
echo "REASON=$REASON"
echo "HINT=$HINT"
echo "LOG_PATH=$PROXY_LOG"
echo "DMESG_LOG=$DMESG_LOG"
