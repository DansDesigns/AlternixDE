# ---------------------------------------------------------------------------
# detect_camera_hardware()
#
# Drop into hardware-detect.sh next to detect_network_hardware().
#
# INTEGRATION POINT: this uses CAMERA_PKGS as its accumulator and calls
# _pkg_add(). Rename these to match whatever detect_network_hardware()
# already uses in your copy -- I don't want to invent a second mechanism.
#
# Detects the imaging path rather than guessing from the model name, because
# all three target tablets present a "Multimedia controller" with no bound
# driver and no /dev/video node, which looks identical until you read the
# PCI slot and device ID.
#
#   BYT  ISP2400 (ElitePad 1000 G2)  -> PCI 0000:00:03.0  8086:0f38
#   CHT  ISP2401 (BBen C100)         -> PCI 0000:00:03.0  8086:22b8
#   IPU3         (Miix 520)          -> PCI 0000:00:14.3  vendor 8086
#   UVC          (external webcams)  -> USB bInterfaceClass 0e
#
# Writes /etc/alternix/camera.conf describing the stack, so that
# osm-settings / osm-camera / install-update.sh all key off one fact
# instead of each re-detecting.
# ---------------------------------------------------------------------------

detect_camera_hardware() {
    local stack="none"
    local isp_slot=""
    local isp_id=""
    local sensors=""

    CAMERA_PKGS=""

    _pci_id() {
        # $1 = pci slot; echoes "vvvv:dddd" or nothing
        local d="/sys/bus/pci/devices/$1"
        [ -d "$d" ] || return 1
        local v p
        v=$(cat "$d/vendor" 2>/dev/null) || return 1
        p=$(cat "$d/device" 2>/dev/null) || return 1
        printf '%s:%s' "${v#0x}" "${p#0x}"
    }

    # --- Intel AtomISP (Bay Trail / Cherry Trail) --------------------------
    isp_id=$(_pci_id 0000:00:03.0)
    case "$isp_id" in
        8086:0f38)
            stack="atomisp2400"; isp_slot="0000:00:03.0"
            info "Camera: Intel AtomISP ISP2400 (Bay Trail) at $isp_slot"
            ;;
        8086:22b8)
            stack="atomisp2401"; isp_slot="0000:00:03.0"
            info "Camera: Intel AtomISP ISP2401 (Cherry Trail) at $isp_slot"
            ;;
    esac

    # --- Intel IPU3 (Skylake / Kaby Lake Imaging Unit) ---------------------
    if [ "$stack" = "none" ]; then
        isp_id=$(_pci_id 0000:00:14.3)
        case "$isp_id" in
            8086:*)
                # class 0x0480xx == multimedia controller, other
                local cls
                cls=$(cat /sys/bus/pci/devices/0000:00:14.3/class 2>/dev/null)
                case "$cls" in
                    0x0480*)
                        stack="ipu3"; isp_slot="0000:00:14.3"
                        info "Camera: Intel IPU3 imaging unit ($isp_id) at $isp_slot"
                        ;;
                esac
                ;;
        esac
    fi

    # --- Which sensors does ACPI declare? ----------------------------------
    # The HID is what tells us which sensor module to pull in. Collected even
    # when the ISP is unknown, because it is the single most useful fact for
    # diagnosing a device we have not seen before.
    local d hid
    for d in /sys/bus/acpi/devices/*; do
        [ -d "$d" ] || continue
        hid=$(cat "$d/hid" 2>/dev/null) || continue
        case "$hid" in
            OVTI*|XXOV*|INT33BE|GCTI*|AZND*|HIMX*|SONY*|INT3472|INT3479|TXNW*)
                sensors="$sensors $hid"
                ;;
        esac
    done
    sensors="${sensors# }"
    [ -n "$sensors" ] && info "Camera: ACPI sensor/PMIC IDs: $sensors"

    # --- USB UVC ------------------------------------------------------------
    local uvc="no" ic
    for d in /sys/bus/usb/devices/*:*; do
        [ -r "$d/bInterfaceClass" ] || continue
        ic=$(cat "$d/bInterfaceClass" 2>/dev/null)
        if [ "$ic" = "0e" ]; then
            uvc="yes"
            break
        fi
    done
    if [ "$uvc" = "yes" ]; then
        info "Camera: USB Video Class device present"
        [ "$stack" = "none" ] && stack="uvc"
    fi

    # --- Package selection --------------------------------------------------
    # Common userspace. libcamera is C++/meson throughout -- no Rust in the
    # dependency chain, and the SoftISP path is what makes these raw MIPI
    # sensors usable at all.
    if [ "$stack" != "none" ]; then
        _pkg_add v4l-utils
    fi

    case "$stack" in
        atomisp2400|atomisp2401)
            # Firmware landed upstream under intel/ipu/ -- Debian splits
            # intel/* across two packages depending on release, so take both
            # rather than losing the camera to a packaging detail.
            _pkg_add firmware-misc-nonfree
            _pkg_add firmware-intel-graphics
            _pkg_add libcamera-tools
            _pkg_add gstreamer1.0-libcamera
            ;;
        ipu3)
            _pkg_add firmware-intel-graphics
            _pkg_add libcamera-tools
            _pkg_add gstreamer1.0-libcamera
            ;;
        uvc)
            : # uvcvideo is in-kernel; nothing extra needed
            ;;
        none)
            warn "Camera: no imaging unit or UVC device detected"
            ;;
    esac

    # --- Record the verdict for the application layer -----------------------
    mkdir -p "${NEXOS_MOUNT}/etc/alternix" 2>/dev/null
    {
        printf '# Generated by hardware-detect.sh - do not edit by hand\n'
        printf 'CAMERA_STACK=%s\n' "$stack"
        printf 'CAMERA_ISP_SLOT=%s\n' "$isp_slot"
        printf 'CAMERA_ISP_PCI_ID=%s\n' "$isp_id"
        printf 'CAMERA_ACPI_IDS="%s"\n' "$sensors"
        printf 'CAMERA_UVC=%s\n' "$uvc"
    } > "${NEXOS_MOUNT}/etc/alternix/camera.conf"

    # --- Module policy ------------------------------------------------------
    # intel_atomisp2_pm is a dummy PM driver whose entire purpose is to put
    # the ISP into D3 to save power. If it binds the PCI device first, atomisp
    # can never bind and the camera is dead with no error message anywhere.
    # It must be blacklisted on any device where we actually want the camera.
    case "$stack" in
        atomisp2400|atomisp2401)
            cat > "${NEXOS_MOUNT}/etc/modprobe.d/alternix-camera.conf" <<'EOF'
# Alternix camera support - DO NOT REMOVE
# intel_atomisp2_pm claims the ISP PCI device purely to power it down.
# It wins the bind race against atomisp and silently kills the camera.
blacklist intel_atomisp2_pm
EOF
            info "Camera: blacklisted intel_atomisp2_pm so atomisp can bind"
            ;;
    esac

    CAMERA_STACK="$stack"
    export CAMERA_STACK
}
