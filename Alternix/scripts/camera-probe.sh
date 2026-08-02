#!/bin/sh
# camera-probe.sh - NexOS / Alternix camera hardware evidence collector
#
# Run on each target device:   sudo sh camera-probe.sh
# Writes:                      /tmp/camera-probe-<hostname>.txt
#
# This script ONLY READS. It loads no modules, writes no config, changes
# nothing. Its sole job is to produce the evidence needed to write
# detect_camera_hardware() in hardware-detect.sh without guessing.
#
# Deliberately POSIX sh + coreutils only. No bashisms, no Rust tools.

OUT="/tmp/camera-probe-$(hostname 2>/dev/null || echo unknown).txt"
: > "$OUT"

say() { printf '%s\n' "$*" >> "$OUT"; }
hdr() {
    say ""
    say "==============================================================="
    say "== $*"
    say "==============================================================="
}
# run a command, always succeed, always record whether the tool existed
run() {
    _cmd="$1"
    say ""
    say "--- \$ $_cmd"
    _bin=$(printf '%s' "$_cmd" | awk '{print $1}')
    if ! command -v "$_bin" >/dev/null 2>&1; then
        say "    [TOOL MISSING: $_bin]"
        return 0
    fi
    sh -c "$_cmd" >> "$OUT" 2>&1 || say "    [exit $?]"
    return 0
}

if [ "$(id -u)" != "0" ]; then
    echo "WARNING: not running as root; ACPI/i2c/dmesg sections will be incomplete." >&2
fi

say "NexOS camera probe"
say "date (host clock): $(date -u 2>/dev/null)"
say "kernel: $(uname -srm 2>/dev/null)"

# ---------------------------------------------------------------------------
hdr "1. MACHINE IDENTITY (used to key per-device quirks)"
for f in sys_vendor product_name product_version product_sku board_name bios_version bios_date; do
    if [ -r "/sys/class/dmi/id/$f" ]; then
        say "$f: $(cat "/sys/class/dmi/id/$f" 2>/dev/null)"
    fi
done

# ---------------------------------------------------------------------------
hdr "2. PCI - ISP / IMAGING UNIT"
# atomisp (BYT/CHT) lives at 00:03.0; IPU3 (SKL/KBL) at 00:14.3
run "lspci -nnk"
say ""
say "--- ISP-relevant PCI slots, verbose:"
for slot in 0000:00:03.0 0000:00:14.3; do
    if [ -d "/sys/bus/pci/devices/$slot" ]; then
        say ""
        say "  [$slot present]"
        say "    vendor:  $(cat /sys/bus/pci/devices/$slot/vendor 2>/dev/null)"
        say "    device:  $(cat /sys/bus/pci/devices/$slot/device 2>/dev/null)"
        say "    class:   $(cat /sys/bus/pci/devices/$slot/class 2>/dev/null)"
        _drv=$(readlink -f "/sys/bus/pci/devices/$slot/driver" 2>/dev/null)
        say "    driver:  ${_drv:-<NONE BOUND>}"
        say "    power:   $(cat /sys/bus/pci/devices/$slot/power_state 2>/dev/null)"
    else
        say ""
        say "  [$slot ABSENT]"
    fi
done

# ---------------------------------------------------------------------------
hdr "3. USB - is there a plain UVC camera after all?"
run "lsusb"
run "lsusb -t"

# ---------------------------------------------------------------------------
hdr "4. EXISTING V4L2 / MEDIA NODES"
run "ls -l /dev/video* /dev/media* /dev/v4l 2>&1"
run "ls -lR /dev/v4l 2>&1"
run "v4l2-ctl --list-devices"
say ""
say "--- per-node capability query:"
for n in /dev/video*; do
    [ -e "$n" ] || continue
    run "v4l2-ctl -d $n --all"
done
say ""
say "--- media topology:"
for m in /dev/media*; do
    [ -e "$m" ] || continue
    run "media-ctl -d $m -p"
done

# ---------------------------------------------------------------------------
hdr "5. ACPI DEVICES - THIS IS THE KEY SECTION"
# Sensors are described in ACPI, not PCI. The HID tells us which sensor
# driver we need. INT3472 entries are the sensor power/clock providers.
say ""
say "--- all ACPI HIDs with status, sorted:"
for d in /sys/bus/acpi/devices/*; do
    [ -d "$d" ] || continue
    _hid=$(cat "$d/hid" 2>/dev/null)
    [ -n "$_hid" ] || continue
    _path=$(cat "$d/path" 2>/dev/null)
    _sta=$(cat "$d/status" 2>/dev/null)
    printf '%s\t%s\tstatus=%s\n' "$_hid" "${_path:-?}" "${_sta:-?}"
done 2>/dev/null | sort >> "$OUT"

say ""
say "--- ACPI entries matching known camera/sensor/PMIC vendor prefixes:"
say "    (OVTI=OmniVision GCTI/AZND=Galaxycore HIMX=Himax INT3472=sensor PMIC/GPIO"
say "     INT33BE=ov5693 INT33F0=?? INT33FE/INT33FB=CHT PMIC TXNW/INT3479=tps68470)"
for d in /sys/bus/acpi/devices/*; do
    [ -d "$d" ] || continue
    _hid=$(cat "$d/hid" 2>/dev/null)
    case "$_hid" in
        OVTI*|OVCM*|INT33BE|INT33F0|INT33FE|INT33FB|INT3472|INT3479|GCTI*|AZND*|HIMX*|SONY*|ADV7*|XXOV*|TXNW*|CHT*|SMO*)
            say ""
            say "  HID=$_hid  dir=$d"
            say "    path:    $(cat "$d/path" 2>/dev/null)"
            say "    status:  $(cat "$d/status" 2>/dev/null)"
            say "    uid:     $(cat "$d/uid" 2>/dev/null)"
            say "    modalias:$(cat "$d/modalias" 2>/dev/null)"
            say "    physnode:$(readlink -f "$d/physical_node" 2>/dev/null)"
            ;;
    esac
done

# ---------------------------------------------------------------------------
hdr "6. I2C - where the sensors actually hang"
run "i2cdetect -l"
say ""
say "--- /sys/bus/i2c/devices:"
for d in /sys/bus/i2c/devices/*; do
    [ -e "$d" ] || continue
    _n=$(basename "$d")
    _name=$(cat "$d/name" 2>/dev/null)
    _drv=$(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)
    printf '  %-16s name=%-24s driver=%s\n' "$_n" "${_name:-?}" "${_drv:-<none>}"
done >> "$OUT" 2>&1

say ""
say "--- v4l2 subdevs registered:"
run "ls -l /sys/class/video4linux/"
for s in /sys/class/video4linux/*; do
    [ -e "$s" ] || continue
    say "  $(basename "$s"): name=$(cat "$s/name" 2>/dev/null)"
done

# ---------------------------------------------------------------------------
hdr "7. KERNEL MODULES - loaded"
run "lsmod"
say ""
say "--- camera-relevant loaded modules:"
lsmod 2>/dev/null | grep -Ei 'atomisp|ipu3|cio2|imgu|int3472|tps68470|crystal|ov[0-9]{4}|gc[0-9]{4}|hm[0-9]{4}|mt9m|videodev|videobuf|v4l2|uvc|intel_skl|cht_int33fe|cht_wc' >> "$OUT" 2>&1
say "    (empty above == nothing camera-related is loaded)"

hdr "8. KERNEL MODULES - available but not loaded"
say ""
say "--- does this kernel even ship the drivers?"
for m in atomisp atomisp-ov2680 atomisp-ov5693 atomisp-gc0310 atomisp-gc2235 \
         atomisp-mt9m114 atomisp-ov2722 atomisp_gmin_platform \
         intel_atomisp2_pm intel_atomisp2_led \
         ipu3-cio2 ipu3-imgu intel-skl-int3472 intel_skl_int3472_discrete \
         intel_skl_int3472_tps68470 tps68470-regulator clk-tps68470 \
         intel_cht_int33fe ov2680 ov5693 ov5670 ov8865 hm11b1 \
         uvcvideo videodev v4l2loopback; do
    if modinfo "$m" >/dev/null 2>&1; then
        _f=$(modinfo -n "$m" 2>/dev/null)
        say "  AVAILABLE  $m  -> ${_f:-builtin}"
    else
        say "  missing    $m"
    fi
done

say ""
say "--- kernel config (if readable):"
_cfg="/boot/config-$(uname -r)"
if [ -r "$_cfg" ]; then
    grep -E 'ATOMISP|IPU3|CIO2|INT3472|TPS68470|VIDEO_OV|VIDEO_GC|VIDEO_HM|VIDEO_MT9M|USB_VIDEO_CLASS|STAGING_MEDIA' "$_cfg" >> "$OUT" 2>&1
else
    say "  [$_cfg not readable]"
fi

# ---------------------------------------------------------------------------
hdr "9. BLACKLISTS - is something suppressing the driver?"
say ""
say "--- /etc/modprobe.d and /lib/modprobe.d matches:"
grep -rIn -E 'atomisp|ipu3|cio2|int3472|ov[0-9]{4}|gc[0-9]{4}' \
    /etc/modprobe.d /lib/modprobe.d /usr/lib/modprobe.d 2>/dev/null >> "$OUT"
say ""
say "--- kernel cmdline (check for module_blacklist= / atomisp options):"
run "cat /proc/cmdline"

# ---------------------------------------------------------------------------
hdr "10. FIRMWARE PRESENCE"
say ""
for f in /lib/firmware/intel/ipu/shisp_2400b0_v21.bin \
         /lib/firmware/intel/ipu/shisp_2401a0_v21.bin \
         /lib/firmware/intel/ipu/shisp_2401a0_legacy_v21.bin \
         /lib/firmware/shisp_2400b0_v21.bin \
         /lib/firmware/shisp_2401a0_v21.bin; do
    if [ -f "$f" ]; then
        say "  PRESENT  $f"
        say "           version: $(strings "$f" 2>/dev/null | head -n1)"
        say "           sha256:  $(sha256sum "$f" 2>/dev/null | awk '{print $1}')"
    else
        say "  absent   $f"
    fi
done
say ""
say "--- ipu3 / other imaging firmware in tree:"
run "ls -l /lib/firmware/intel/ipu/ 2>&1"
say ""
say "--- installed firmware packages:"
run "dpkg -l firmware-misc-nonfree firmware-intel-graphics firmware-intel-misc firmware-linux-nonfree 2>&1"

# ---------------------------------------------------------------------------
hdr "11. LIBCAMERA USERSPACE"
run "dpkg -l 'libcamera*' 'gstreamer1.0-libcamera' 2>&1"
run "ls -l /usr/lib/*/libcamera.so* /usr/lib/libcamera.so* 2>&1"
run "ls -l /usr/share/libcamera/ipa/ 2>&1"
run "ls -l /usr/lib/*/libcamera/ 2>&1"
run "cam -l"
run "libcamera-hello --list-cameras"
say ""
say "--- libcamera pipeline handlers built in this package:"
run "ls /usr/lib/*/libcamera/ 2>&1"

# ---------------------------------------------------------------------------
hdr "12. DMESG - the real diagnosis"
say ""
say "--- camera-relevant dmesg lines:"
dmesg 2>/dev/null | grep -Ei 'atomisp|isp2|ipu3|cio2|imgu|int3472|tps68470|gmin|ov[0-9]{4}|gc[0-9]{4}|hm[0-9]{4}|mt9m|v4l2|video4linux|uvcvideo|firmware.*shisp|Direct firmware load' >> "$OUT" 2>&1
say ""
say "--- all firmware load failures:"
dmesg 2>/dev/null | grep -Ei 'Direct firmware load|firmware.*fail' >> "$OUT" 2>&1
say ""
say "--- i2c / ACPI errors (sensor power-up problems show here):"
dmesg 2>/dev/null | grep -Ei 'i2c_designware|i2c-designware|ACPI Error|ACPI Warning|_DSM|GPIO' | head -n 120 >> "$OUT" 2>&1

# ---------------------------------------------------------------------------
hdr "13. FULL DMESG TAIL (context for anything above)"
run "dmesg | tail -n 400"

# ---------------------------------------------------------------------------
hdr "END OF PROBE"
say "Collected to: $OUT"

echo ""
echo "Done. Report written to: $OUT"
echo "Send that file back for all three devices."
echo ""
echo "Quick summary of what matters most:"
printf '  ISP at 00:03.0 (atomisp): '
[ -d /sys/bus/pci/devices/0000:00:03.0 ] && echo "PRESENT" || echo "absent"
printf '  ISP at 00:14.3 (IPU3):    '
[ -d /sys/bus/pci/devices/0000:00:14.3 ] && echo "PRESENT" || echo "absent"
printf '  /dev/video* nodes:        '
ls /dev/video* >/dev/null 2>&1 && ls /dev/video* | tr '\n' ' ' && echo "" || echo "NONE"
printf '  atomisp module:           '
modinfo atomisp >/dev/null 2>&1 && echo "available" || echo "NOT IN THIS KERNEL"
printf '  libcamera:                '
command -v cam >/dev/null 2>&1 && echo "installed" || echo "not installed"
