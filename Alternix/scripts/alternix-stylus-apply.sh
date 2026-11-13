#!/usr/bin/env bash
# AlternixOS persistent X11 touchscreen/stylus configuration (Wacom or libinput).
# scan: list devices for osm-settings
# save PEN_NAME PRESSURE BUTTON_1 BUTTON_2 TOUCH_ENABLED OUTPUT: persist + apply
# apply: replay the saved user settings at login
# Settings change only X11 user devices; no system-wide driver overrides.
set -u
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_DIR="$HOME/.config/Alternix"
CFG="$CONFIG_DIR/stylus.conf"
AUTOSTART="$HOME/.config/autostart/alternix-stylus.desktop"
mkdir -p "$CONFIG_DIR"

value() {
    local key="$1"
    [ -r "$CFG" ] || return 0
    sed -n "s/^${key}=//p" "$CFG" | tail -n1
}

trim() { sed 's/^[[:space:]↳⎜⎣⎡├─]*//;s/[[:space:]]*$//'; }

# XInput names do not necessarily say 'Wacom', particularly on integrated
# AES pens. Use the udev input classification when possible.
classify() {
    local id="$1" name="$2" node udev=''
    node=$(xinput list-props "$id" 2>/dev/null | sed -n 's/.*Device Node.*"\([^"]*\)".*/\1/p' | head -n1)
    if [[ "$node" == /dev/input/event* ]] && command -v udevadm >/dev/null 2>&1; then
        udev=$(udevadm info --query=property --name="$node" 2>/dev/null) || :
    fi
    if grep -qx 'ID_INPUT_TOUCHSCREEN=1' <<< "$udev"; then echo TOUCH; return; fi
    if grep -qxE 'ID_INPUT_(TABLET|TABLET_TOOL|PEN|STYLUS)=1' <<< "$udev"; then echo PEN; return; fi
    if grep -qxE 'ID_INPUT_(TOUCHPAD|MOUSE|POINTINGSTICK|TABLET_PAD)=1' <<< "$udev"; then return; fi
    case "${name,,}" in *touchpad*|*trackpoint*|*mouse*|*\ pad*) return ;; esac
    local props
    props=$(xinput list-props "$id" 2>/dev/null) || props=''
    if [[ "$props" == *'libinput Tablet Tool Pressurecurve'* ]] ||
       [[ "$props" == *'Wacom Tool Type'* ]]; then echo PEN; return; fi
    if [[ "$props" == *'libinput Calibration Matrix'* ]]; then echo TOUCH; return; fi
    case "${name,,}" in
        *touchpad*|*trackpoint*|*mouse*|*virtual\ core*|*keyboard*|*\ pad*) return ;;
        *touchscreen*|*touch\ screen*|*\ finger*|*multitouch*|*multi-touch*) echo TOUCH ;;
        *stylus*|*eraser*|*digitizer*|*\ pen*|*pen\ *) echo PEN ;;
    esac
}

declare -A WACOM=()
declare -a IDS=() NAMES=() KINDS=() DRIVERS=()

discover() {
    local line id name kind driver type
    IDS=(); NAMES=(); KINDS=(); DRIVERS=(); WACOM=()
    if command -v xsetwacom >/dev/null 2>&1; then
        while IFS= read -r line; do
            [[ "$line" =~ id:[[:space:]]*([0-9]+) ]] || continue
            id="${BASH_REMATCH[1]}"
            name=$(printf '%s' "${line%%id:*}" | trim)
            type="${line##*type:}"
            type=$(printf '%s' "$type" | trim)
            case "${type,,}" in
                stylus|eraser) kind=PEN ;;
                touch) kind=TOUCH ;;
                *) continue ;;
            esac
            WACOM[$id]="$kind"
            IDS+=("$id"); NAMES+=("$name"); KINDS+=("$kind"); DRIVERS+=(wacom)
        done < <(xsetwacom --list devices 2>/dev/null)
    fi
    command -v xinput >/dev/null 2>&1 || return 0
    while IFS= read -r line; do
        [[ "$line" =~ id=([0-9]+) ]] || continue
        id="${BASH_REMATCH[1]}"
        [ -z "${WACOM[$id]+set}" ] || continue
        name=$(printf '%s' "${line%%id=*}" | trim)
        kind=$(classify "$id" "$name")
        [ -n "$kind" ] || continue
        IDS+=("$id"); NAMES+=("$name"); KINDS+=("$kind"); DRIVERS+=(libinput)
    done < <(xinput --list --short 2>/dev/null)
}

# Wacom AES pens can expose the lower barrel button as an "eraser" tool,
# even though the eraser switch is on the barrel, not at the back of the pen.
# Match endpoints by their common device name; never mix unrelated tablets.
pen_base_name() {
    local name="${1,,}"
    case "$name" in
        *' stylus') name="${name% stylus}" ;;
        *' eraser') name="${name% eraser}" ;;
    esac
    printf '%s' "$name"
}

eraser_for_stylus() {
    local stylus_index="$1" i base
    base=$(pen_base_name "${NAMES[stylus_index]}")
    ERASER_ID=''; ERASER_DRIVER=''
    for (( i=0; i<${#IDS[@]}; i++ )); do
        [[ "${NAMES[i],,}" == *' eraser' ]] || continue
        [[ "${DRIVERS[i]}" == "${DRIVERS[stylus_index]}" ]] || continue
        [[ "$(pen_base_name "${NAMES[i]}")" == "$base" ]] || continue
        ERASER_ID="${IDS[i]}"
        ERASER_DRIVER="${DRIVERS[i]}"
        return 0
    done
    return 1
}

# For an integrated Lenovo/Wacom AES device, the matching eraser endpoint
# is the lower barrel's erase switch. A traditional external drawing tablet
# may instead have a physical eraser on the back of its stylus; don't
# automatically repurpose that tool unless it looks like an integrated pen.
uses_eraser_barrel() {
    local index="$1" name
    name="${NAMES[index],,}"
    [[ -n "$ERASER_ID" ]] || return 1
    case "$name" in
        *lenovo*|*aes*|*hid*|*isdv*|*'pen and multitouch'*|*'pen tablet/digitizer'*) return 0 ;;
    esac
    return 1
}

outputs() {
    command -v xrandr >/dev/null 2>&1 || return 0
    xrandr --query | awk '$2=="connected"{print "OUTPUT\t" $1}'
}

scan() {
    [ -n "${DISPLAY:-}" ] || { echo 'ERROR=No X11 DISPLAY; pen configuration requires the Qtile/X11 desktop session'; return 1; }
    command -v xinput >/dev/null 2>&1 || { echo 'ERROR=xinput not installed'; return 1; }
    discover
    local i
    for (( i=0; i<${#IDS[@]}; i++ )); do
        printf '%s\t%s\t%s\t%s\n' "${KINDS[i]}" "${IDS[i]}" "${NAMES[i]}" "${DRIVERS[i]}"
        if [[ "${KINDS[i]}" == PEN && "${NAMES[i],,}" != *' eraser' ]]; then
            eraser_for_stylus "$i" || :
            if uses_eraser_barrel "$i"; then
                printf 'LAYOUT\t%s\taes-eraser\n' "${IDS[i]}"
            else
                printf 'LAYOUT\t%s\tstandard\n' "${IDS[i]}"
            fi
        fi
    done
    outputs
    printf 'CURRENT_PEN=%s\nCURRENT_PRESSURE=%s\nCURRENT_BUTTON_1=%s\nCURRENT_BUTTON_2=%s\nCURRENT_TOUCH=%s\nCURRENT_OUTPUT=%s\n' \
        "$(value PEN_NAME)" "$(value PRESSURE)" "$(value BUTTON_1)" \
        "$(value BUTTON_2)" "$(value TOUCH_ENABLED)" "$(value OUTPUT)"
}

button_number() {
    case "$1" in
        right) echo 3 ;; middle) echo 2 ;; left) echo 1 ;;
        back) echo 8 ;; forward) echo 9 ;; disabled) echo 0 ;;
        *) return 1 ;;
    esac
}

# Physical barrel numbering (from nib towards the end cap):
#   Button 1 = lower, nearest tip; Button 2 = upper, nearest end cap.
# On integrated Wacom AES the LOWER barrel changes to a separate eraser
# XInput tool, so writing stylus Button 2/3 ignores the lower button.
# The end-cap Bluetooth shortcut is a different HID device and must not be
# mistaken for a third stylus button or for the eraser endpoint.
apply_pen() {
    local id="$1" driver="$2" pressure="$3" b1="$4" b2="$5" eraser_id="$6" aes="$7"
    local curve number1 number2 props buttonmap
    local -a buttons=() erase_buttons=()
    case "$pressure" in
        soft)   curve='0 25 65 100' ;;
        firm)   curve='30 0 100 75' ;;
        *)      curve='0 0 100 100' ;;
    esac
    if [ "$driver" = wacom ]; then
        xsetwacom set "$id" Mode Absolute 2>/dev/null || :
        # shellcheck disable=SC2086
        xsetwacom set "$id" PressureCurve $curve 2>/dev/null || echo "WARNING=Pressure curve not supported by pen $id"
    else
        props=$(xinput list-props "$id" 2>/dev/null)
        if [[ "$props" == *'libinput Tablet Tool Pressurecurve'* ]]; then
            local a b c d
            read -r a b c d <<< "$curve"
            xinput set-prop "$id" 'libinput Tablet Tool Pressurecurve' \
                "$(awk -v n="$a" 'BEGIN{printf "%.2f",n/100}')" \
                "$(awk -v n="$b" 'BEGIN{printf "%.2f",n/100}')" \
                "$(awk -v n="$c" 'BEGIN{printf "%.2f",n/100}')" \
                "$(awk -v n="$d" 'BEGIN{printf "%.2f",n/100}')" 2>/dev/null || \
                echo "WARNING=Could not apply pressure curve to pen $id"
        else
            echo "WARNING=Pen $id does not expose configurable pressure via its X11 driver"
        fi
    fi

    number1=$(button_number "$b1") || return 1
    number2=$(button_number "$b2") || return 1
    if [ "$driver" = wacom ]; then
        if [ "$aes" = yes ] && [ -n "$eraser_id" ]; then
            # Wacom AES lower barrel -> eraser tool, button 1 (requires a tip
            # tap while held on some firmware). Upper barrel -> stylus button 2.
            # Use xsetwacom action syntax (+N) for presses on the AES tools.
            local lower="$number1" upper="$number2"
            [ "$lower" = 0 ] || lower="+$lower"
            [ "$upper" = 0 ] || upper="+$upper"
            xsetwacom set "$eraser_id" Button 1 "$lower" 2>/dev/null || \
                echo "WARNING=Lower barrel (eraser tool $eraser_id) could not be mapped"
            xsetwacom set "$id" Button 2 "$upper" 2>/dev/null || \
                echo "WARNING=Upper barrel (stylus $id) could not be mapped"
            echo "INFO=Lower barrel is on the Wacom AES eraser endpoint; hold it and tap the screen to activate the assigned click."
        else
            # Regular pen with both barrel switches on the stylus endpoint.
            xsetwacom set "$id" Button 2 "$number1" 2>/dev/null || \
                echo "WARNING=Pen $id has no programmable lower barrel button"
            xsetwacom set "$id" Button 3 "$number2" 2>/dev/null || \
                echo "WARNING=Pen $id has no programmable upper barrel button"
        fi
    else
        buttonmap=$(xinput --get-button-map "$id" 2>/dev/null) || buttonmap=''
        read -r -a buttons <<< "$buttonmap"
        if [ "$aes" = yes ] && [ -n "$eraser_id" ]; then
            # libinput equivalent where the side switch is a separate tool.
            if (( ${#buttons[@]} >= 2 )); then
                buttons[1]="$number2"
                xinput --set-button-map "$id" "${buttons[@]}" 2>/dev/null || \
                    echo "WARNING=Upper barrel mapping unsupported for pen $id"
            else
                echo "WARNING=Pen $id lacks a programmable upper barrel button"
            fi
            buttonmap=$(xinput --get-button-map "$eraser_id" 2>/dev/null) || buttonmap=''
            read -r -a erase_buttons <<< "$buttonmap"
            if (( ${#erase_buttons[@]} >= 1 )); then
                erase_buttons[0]="$number1"
                xinput --set-button-map "$eraser_id" "${erase_buttons[@]}" 2>/dev/null || \
                    echo "WARNING=Lower barrel mapping unsupported for eraser tool $eraser_id"
            else
                echo "WARNING=Lower barrel eraser tool $eraser_id has no configurable button map"
            fi
            echo "INFO=Lower barrel is handled by a separate eraser device and may require a tip tap."
        elif (( ${#buttons[@]} >= 3 )); then
            # Preserve tip mapping and unrelated buttons.
            buttons[1]="$number1"; buttons[2]="$number2"
            xinput --set-button-map "$id" "${buttons[@]}" 2>/dev/null || \
                echo "WARNING=Side-button remapping not supported for pen $id"
        else
            echo "WARNING=Pen $id does not expose three XInput buttons"
        fi
    fi
}

apply() {
    [ -n "${DISPLAY:-}" ] || { echo 'ERROR=DISPLAY is not set'; return 1; }
    command -v xinput >/dev/null 2>&1 || { echo 'ERROR=xinput not installed'; return 1; }
    local selected pressure button1 button2 touch output count=0 i
    selected=$(value PEN_NAME)
    pressure=$(value PRESSURE); button1=$(value BUTTON_1); button2=$(value BUTTON_2)
    touch=$(value TOUCH_ENABLED); output=$(value OUTPUT)
    [ -n "$pressure" ] || pressure=normal
    [ -n "$button1" ] || button1=right
    [ -n "$button2" ] || button2=middle
    [ -n "$touch" ] || touch=yes
    discover
    for (( i=0; i<${#IDS[@]}; i++ )); do
        if [ "${KINDS[i]}" = TOUCH ]; then
            if [ "$touch" = no ]; then
                xinput --disable "${IDS[i]}" 2>/dev/null || echo "WARNING=Cannot disable touch device ${NAMES[i]}"
            else
                xinput --enable "${IDS[i]}" 2>/dev/null || echo "WARNING=Cannot enable touch device ${NAMES[i]}"
            fi
        elif [ "${KINDS[i]}" = PEN ]; then
            # When no pen is selected, configure every detected stylus, not
            # standalone erasers. A saved selection matches by stable name.
            case "${NAMES[i],,}" in *eraser*) continue ;; esac
            if [ -z "$selected" ] || [ "${NAMES[i]}" = "$selected" ]; then
                eraser_for_stylus "$i" || :
                local aes=no
                if uses_eraser_barrel "$i"; then aes=yes; fi
                apply_pen "${IDS[i]}" "${DRIVERS[i]}" "$pressure" "$button1" "$button2" "$ERASER_ID" "$aes"
                (( count += 1 ))
            fi
        fi
    done
    if [ -n "$selected" ] && [ "$count" = 0 ]; then
        echo "WARNING=Saved pen '$selected' is currently disconnected"
    fi
    # The rotation component knows how to map XInput/libinput and Wacom
    # to the selected screen with correct XRandR orientation/geometry.
    if [ -f "$SCRIPT_DIR/alternix-rotate-monitor.sh" ]; then
        bash "$SCRIPT_DIR/alternix-rotate-monitor.sh" --sync-input || \
            echo 'WARNING=Could not synchronize input coordinates with display'
    fi
    echo 'RESULT=Applied available touchscreen/pen settings'
}

install_autostart() {
    local path escaped
    path="$SCRIPT_DIR/alternix-stylus-apply.sh"
    escaped=${path//\\/\\\\}
    escaped=${escaped//\"/\\\"}
    escaped=${escaped//\$/\\\$}
    escaped=${escaped//\`/\\\`}
    mkdir -p "$(dirname "$AUTOSTART")"
    cat > "$AUTOSTART" <<ALT_DESKTOP
[Desktop Entry]
Type=Application
Name=Alternix Pen and Touch Settings
Comment=Restore user pen and touchscreen configuration on login
Exec=/bin/bash "$escaped" apply
X-GNOME-Autostart-enabled=true
ALT_DESKTOP
}

save() {
    local pen="${1:-}" pressure="${2:-normal}" b1="${3:-right}" b2="${4:-middle}" touch="${5:-yes}" output="${6:-auto}"
    local temp
    case "$pressure" in soft|normal|firm) ;; *) echo 'ERROR=Invalid pressure setting'; return 2;; esac
    button_number "$b1" >/dev/null || { echo 'ERROR=Invalid button 1 setting'; return 2; }
    button_number "$b2" >/dev/null || { echo 'ERROR=Invalid button 2 setting'; return 2; }
    case "$touch" in yes|no) ;; *) echo 'ERROR=Invalid touch setting'; return 2;; esac
    case "$output" in *[!a-zA-Z0-9_.-]*|'') echo 'ERROR=Invalid output name'; return 2;; esac
    case "$pen" in *$'\n'*|*$'\r'*|*$'\t'*|*'='*) echo 'ERROR=Invalid pen device name'; return 2;; esac
    temp=$(mktemp "$CONFIG_DIR/.stylus.XXXXXXXX") || return 1
    if ! printf 'PEN_NAME=%s\nPRESSURE=%s\nBUTTON_1=%s\nBUTTON_2=%s\nTOUCH_ENABLED=%s\nOUTPUT=%s\n' \
        "$pen" "$pressure" "$b1" "$b2" "$touch" "$output" > "$temp"; then
        rm -f "$temp"; return 1
    fi
    mv -f "$temp" "$CFG" || return 1
    chmod 600 "$CFG"
    install_autostart
    apply
}

case "${1:-}" in
    scan) scan ;;
    apply) apply ;;
    save) shift; save "$@" ;;
    *) echo 'Usage: alternix-stylus-apply.sh scan|apply|save PEN PRESSURE BUTTON1 BUTTON2 TOUCH OUTPUT' >&2; exit 2 ;;
esac
