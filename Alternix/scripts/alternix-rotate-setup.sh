#!/bin/bash
# alternix-rotate-setup.sh
#
# Scans for a rotation (accelerometer) sensor, tries to get the right
# kernel drivers loaded, and starts iio-sensor-proxy so the desktop can
# read orientation over D-Bus (net.hadess.SensorProxy). This is the same
# stack GNOME/KDE use for tablet auto-rotate, so it's reused here instead
# of hand-rolling per-chip register access.
#
# On the Miix 520 the accelerometer sits behind Intel's Sensor Hub (ISH),
# so the relevant kernel path is: intel_ishtp -> hid-sensor-hub ->
# hid-sensor-accel-3d -> IIO. Older/other hardware may instead expose a
# plain I2C accelerometer (kxcjk1013, bmc150) directly to IIO — we probe
# for both.
#
# Output: simple KEY=VALUE lines, parsed by the Display settings page.
# Called by the "Scan / Setup" button — safe to re-run any time.

set -u

SENSOR_FOUND="no"
SENSOR_PATH=""
SENSOR_NAME=""
DRIVER_LOADED="none"
PROXY_INSTALLED="no"
PROXY_RUNNING="no"
ORIENTATION="unknown"

# 1. Load the kernel modules most likely to expose the sensor via IIO.
#    modprobe on a module that's already built-in or doesn't exist for
#    this kernel just fails silently — harmless either way.
MODULES="intel_ishtp intel_ishtp_hid hid_sensor_hub hid_sensor_accel_3d hid_sensor_trigger industrialio kxcjk1013 bmc150_accel_i2c cros_ec_accel_legacy"
for m in $MODULES; do
    modprobe "$m" >/dev/null 2>&1
done

# Give the sensor hub / IIO subsystem a moment to enumerate.
sleep 1

# 2. Scan /sys/bus/iio/devices for anything that looks like an
#    accelerometer.
for dev in /sys/bus/iio/devices/iio:device*; do
    [ -d "$dev" ] || continue
    name_file="$dev/name"
    [ -f "$name_file" ] || continue
    name=$(cat "$name_file" 2>/dev/null)

    if [ -f "$dev/in_accel_x_raw" ] || [ -f "$dev/in_accel_x_input" ] || \
       echo "$name" | grep -qi "accel"; then
        SENSOR_FOUND="yes"
        SENSOR_PATH="$dev"
        SENSOR_NAME="$name"
        break
    fi
done

# 3. Work out which driver actually bound to it, best-effort.
if [ -n "$SENSOR_PATH" ]; then
    real=$(readlink -f "$SENSOR_PATH/device/driver" 2>/dev/null)
    if [ -n "$real" ]; then
        DRIVER_LOADED=$(basename "$real")
    fi
fi

# 4. Make sure iio-sensor-proxy is installed. It needs only udev + dbus
#    to run, so it's fine on a systemd-free (sysvinit) Devuan install —
#    we just don't rely on systemctl to manage it below.
if command -v iio-sensor-proxy >/dev/null 2>&1; then
    PROXY_INSTALLED="yes"
elif command -v apt-get >/dev/null 2>&1; then
    apt-get install -y iio-sensor-proxy >/dev/null 2>&1
    if command -v iio-sensor-proxy >/dev/null 2>&1; then
        PROXY_INSTALLED="yes"
    fi
fi

# 5. Start it directly if it isn't already running.
if [ "$PROXY_INSTALLED" = "yes" ]; then
    if ! pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
        iio-sensor-proxy >/dev/null 2>&1 &
        disown
        sleep 1
    fi
    if pgrep -x iio-sensor-proxy >/dev/null 2>&1; then
        PROXY_RUNNING="yes"
    fi
fi

# 6. Read back the current orientation, if the proxy is up.
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
