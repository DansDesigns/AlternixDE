#!/usr/bin/env python3
# alternix-touchscroll - multi-finger horizontal swipe -> qtile Scroller layout
#
# Detects touch devices automatically by querying evdev capabilities directly.
# Monitors every candidate at once, rescans on hotplug, needs no configuration.
#
# libinput only synthesises GESTURE_SWIPE for touchpads, never touchscreens,
# so raw ABS_MT events are read and the gesture composed here.
#
# Events are NOT taken away from X by default. EVIOCGRAB mid-gesture leaves X
# holding touch sequences that never receive a TouchEnd, which wedges the
# pointer for the rest of the session: clicks stop resolving and edge-swipe
# daemons stop matching. --grab opts back in; it only releases once every
# finger is off the glass, which avoids partial sequences but cannot repair
# the ones already open when the grab was taken.
#
# Requires membership of the 'input' group.

import argparse
import errno
import fcntl
import glob
import os
import select
import signal
import struct
import sys
import time

# ---------------------------------------------------------------- evdev defs

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT, SYN_MT_REPORT = 0x00, 0x02
ABS_MT_SLOT, ABS_MT_POSITION_X = 0x2F, 0x35
ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID = 0x36, 0x39
BTN_TOUCH, BTN_TOOL_FINGER = 0x14A, 0x145
PROP_POINTER, PROP_DIRECT = 0x00, 0x01

EV_FMT = "llHHi"
EV_SIZE = struct.calcsize(EV_FMT)

PEN_HINTS = ("stylus", " pen", "digitiser", "digitizer", "wacom", "eraser")
TOUCH_HINTS = ("touchscreen", "touch screen", "goodix", "silead", "atmel",
               "sis touch", "elan", "raydium", "melfas", "chipone", "hid touch")

GRAB_WATCHDOG = 0.75      # seconds of no touches before a stuck grab is dropped


def _ioc(direction, size, nr):
    """Build an ioctl request number, returned signed so fcntl accepts it."""
    req = (direction << 30) | (size << 16) | (0x45 << 8) | nr
    return struct.unpack("i", struct.pack("I", req & 0xFFFFFFFF))[0]


def _read_bits(fd, nr, nbytes):
    buf = bytearray(nbytes)
    try:
        fcntl.ioctl(fd, _ioc(2, nbytes, nr), buf, True)
    except OSError:
        return bytearray(nbytes)
    return buf


def _bit(buf, i):
    idx = i >> 3
    return (buf[idx] >> (i & 7)) & 1 if idx < len(buf) else 0


def _name(fd):
    buf = bytearray(256)
    try:
        fcntl.ioctl(fd, _ioc(2, 256, 0x06), buf, True)
    except OSError:
        return ""
    return buf.split(b"\x00", 1)[0].decode("utf-8", "replace").lower()


def _absinfo(fd, axis):
    buf = bytearray(24)
    try:
        fcntl.ioctl(fd, _ioc(2, 24, 0x40 + axis), buf, True)
    except OSError:
        return None
    _, lo, hi, _, _, _ = struct.unpack("6i", bytes(buf))
    return (lo, hi) if hi > lo else None


def _grab(fd, on):
    try:
        fcntl.ioctl(fd, _ioc(1, 4, 0x90), struct.pack("i", 1 if on else 0))
        return True
    except OSError:
        return False


# ------------------------------------------------------------------ scanning

class Device:
    """One open touch device and its per-frame slot state."""

    def __init__(self, path, fd, name, score, rng, slotted, touchpad):
        self.path = path
        self.fd = fd
        self.name = name
        self.score = score
        self.range = rng
        self.slotted = slotted
        self.touchpad = touchpad
        self.scale = 1.0
        self.slots = {}
        self.slot = 0
        self.frame = []          # protocol A accumulator
        self.gesture = None
        self.grabbed = False
        self.idle_since = None   # first moment the glass was seen empty

    def points(self):
        return list(self.slots.values())

    def ungrab(self):
        if self.grabbed:
            _grab(self.fd, False)
            self.grabbed = False

    def close(self):
        self.ungrab()
        try:
            os.close(self.fd)
        except OSError:
            pass


def probe(path, allow_touchpad):
    """Open path and decide whether it is a usable multitouch device."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return None

    absb = _read_bits(fd, 0x20 + EV_ABS, 8)
    if not (_bit(absb, ABS_MT_POSITION_X) and _bit(absb, ABS_MT_POSITION_Y)):
        os.close(fd)
        return None

    name = _name(fd)
    if any(h in name for h in PEN_HINTS):
        os.close(fd)
        return None

    props = _read_bits(fd, 0x09, 4)
    keyb = _read_bits(fd, 0x20 + EV_KEY, 96)
    direct = _bit(props, PROP_DIRECT)
    pointer = _bit(props, PROP_POINTER)
    tool_finger = _bit(keyb, BTN_TOOL_FINGER)
    touch = _bit(keyb, BTN_TOUCH)
    touchpad = bool(pointer or (tool_finger and not direct))

    if touchpad and not allow_touchpad:
        os.close(fd)
        return None

    score = 0
    if direct:
        score += 10
    elif not pointer and touch and not tool_finger:
        score += 6          # unlabelled panel, looks like a touchscreen
    if any(h in name for h in TOUCH_HINTS):
        score += 3
    if touchpad:
        score += 1          # usable, but rank below a real panel
    if score <= 0:
        os.close(fd)
        return None

    return Device(path, fd, name, score,
                  _absinfo(fd, ABS_MT_POSITION_X),
                  _bit(absb, ABS_MT_SLOT), touchpad)


def scan(allow_touchpad):
    found = []
    for path in sorted(glob.glob("/dev/input/event*")):
        dev = probe(path, allow_touchpad)
        if dev:
            found.append(dev)
    found.sort(key=lambda d: -d.score)
    return found


# ------------------------------------------------------------------ qtile IPC

class Qtile:
    def __init__(self):
        self.client = None

    def _connect(self):
        from libqtile.command.client import InteractiveCommandClient
        try:
            from libqtile.command.interface import IPCCommandInterface
            from libqtile.ipc import Client, find_sockfile
            self.client = InteractiveCommandClient(
                IPCCommandInterface(Client(find_sockfile()))
            )
        except Exception:
            self.client = InteractiveCommandClient()

    def _run(self, fn):
        for _ in range(2):
            try:
                if self.client is None:
                    self._connect()
                return fn(self.client)
            except Exception:
                self.client = None
        return None

    def call(self, name, *args):
        return self._run(lambda c: getattr(c.layout, name)(*args)) is not None

    def layout_name(self):
        r = self._run(lambda c: c.layout.info().get("name"))
        return r or ""

    def screen_width(self):
        r = self._run(lambda c: int(c.screen.info()["width"]))
        return r or 0


def ensure_qtile_python():
    """Re-exec under the interpreter that owns libqtile (qtile lives in a venv)."""
    try:
        import libqtile  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("ALTERNIX_TOUCHSCROLL_REEXEC"):
        return

    cands = []
    for d in os.environ.get("PATH", "").split(os.pathsep):
        q = os.path.join(d, "qtile")
        if os.path.isfile(q):
            try:
                with open(q, "rb") as fh:
                    line = fh.readline(256).decode("utf-8", "replace").strip()
                if line.startswith("#!"):
                    interp = line[2:].split()
                    if interp and os.path.basename(interp[0]) == "env":
                        interp = interp[1:]
                    if interp:
                        cands.append(interp[0])
            except OSError:
                pass
    home = os.path.expanduser("~")
    for pat in ("/opt/qtile/venv/bin/python3",
                home + "/.local/share/qtile/venv/bin/python3",
                home + "/.venv/qtile/bin/python3",
                home + "/venv/qtile/bin/python3",
                "/opt/*/venv/bin/python3"):
        cands.extend(glob.glob(pat))

    seen = set()
    for py in cands:
        if py in seen or not os.path.isfile(py) or not os.access(py, os.X_OK):
            continue
        seen.add(py)
        if os.path.realpath(py) == os.path.realpath(sys.executable):
            continue
        env = dict(os.environ, ALTERNIX_TOUCHSCROLL_REEXEC="1")
        try:
            os.execve(py, [py, os.path.abspath(__file__)] + sys.argv[1:], env)
        except OSError:
            continue


# ---------------------------------------------------------------- event logic

def feed(dev, data):
    """Decode a read() buffer into a list of complete frames (point lists)."""
    frames = []
    for off in range(0, len(data) - EV_SIZE + 1, EV_SIZE):
        _, _, etype, code, value = struct.unpack_from(EV_FMT, data, off)

        if etype == EV_ABS:
            if code == ABS_MT_SLOT:
                dev.slot = value
            elif code == ABS_MT_TRACKING_ID:
                if dev.slotted:
                    if value == -1:
                        dev.slots.pop(dev.slot, None)
                    else:
                        dev.slots.setdefault(dev.slot, [0, 0])
            elif code == ABS_MT_POSITION_X:
                if dev.slotted:
                    dev.slots.setdefault(dev.slot, [0, 0])[0] = value
                else:
                    if not dev.frame or dev.frame[-1][0] is not None:
                        dev.frame.append([None, None])
                    dev.frame[-1][0] = value
            elif code == ABS_MT_POSITION_Y:
                if dev.slotted:
                    dev.slots.setdefault(dev.slot, [0, 0])[1] = value
                else:
                    if not dev.frame:
                        dev.frame.append([None, None])
                    dev.frame[-1][1] = value

        elif etype == EV_SYN:
            if code == SYN_MT_REPORT and not dev.slotted:
                if dev.frame and dev.frame[-1][1] is not None:
                    dev.frame.append([None, None])
            elif code == SYN_REPORT:
                if dev.slotted:
                    frames.append(dev.points())
                else:
                    pts = [p for p in dev.frame if p[0] is not None
                           and p[1] is not None]
                    dev.frame = []
                    frames.append(pts)
    return frames


def handle(dev, pts, qtile, args, state):
    n = len(pts)
    g = dev.gesture

    # Track how long the glass has been completely clear. A grab is only ever
    # released here, never while fingers remain down: releasing part-way leaves
    # X receiving motion and release events for touches it never saw begin.
    dev.idle_since = time.monotonic() if n == 0 else None

    if n != args.fingers:
        if g and g["armed"]:
            if state["pending"]:
                qtile.call("scroll_by", int(round(state["pending"])))
                state["pending"] = 0.0
            qtile.call("scroll_settle")
        dev.gesture = None
        if n == 0:
            dev.ungrab()
        return

    cx = sum(p[0] for p in pts) / float(n)
    cy = sum(p[1] for p in pts) / float(n)

    if g is None:
        dev.gesture = {"x0": cx, "y0": cy, "last": cx,
                       "armed": False, "dead": False}
        return
    if g["dead"]:
        return

    if not g["armed"]:
        dx = abs(cx - g["x0"]) * abs(dev.scale)
        dy = abs(cy - g["y0"]) * abs(dev.scale)
        if dx > args.threshold and dx > dy:
            if qtile.layout_name() != "scroller":
                g["dead"] = True
                return
            g["armed"] = True
            g["last"] = cx
            state["pending"] = 0.0
            if args.grab:
                dev.grabbed = _grab(dev.fd, True)
        elif dy > args.threshold:
            g["dead"] = True
        return

    state["pending"] += -(cx - g["last"]) * dev.scale
    g["last"] = cx
    now = time.monotonic()
    if abs(state["pending"]) >= 1.0 and now - state["last_send"] >= args.interval:
        step = int(round(state["pending"]))
        state["pending"] -= step
        qtile.call("scroll_by", step)
        state["last_send"] = now


# --------------------------------------------------------------------- driver

def rescale(devs, qtile, args):
    sw = qtile.screen_width()
    for d in devs:
        s = args.sensitivity
        if d.range and sw:
            s *= float(sw) / float(d.range[1] - d.range[0])
            if d.touchpad:
                s *= args.touchpad_factor
        d.scale = -s if args.invert else s


def watchdog(devs):
    """Drop a grab that outlived its gesture, so input can never stay wedged."""
    now = time.monotonic()
    for d in devs:
        if not d.grabbed:
            continue
        if not d.slots and not d.frame:
            if d.idle_since is None:
                d.idle_since = now
            elif now - d.idle_since > GRAB_WATCHDOG:
                d.ungrab()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fingers", type=int, default=3)
    ap.add_argument("--threshold", type=float, default=18.0,
                    help="pixels of travel before the gesture engages")
    ap.add_argument("--sensitivity", type=float, default=1.0)
    ap.add_argument("--touchpad-factor", type=float, default=1.6,
                    help="extra gain for touchpads, which are small")
    ap.add_argument("--interval", type=float, default=0.016,
                    help="minimum seconds between IPC updates")
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--no-touchpad", dest="touchpad", action="store_false",
                    default=True)
    ap.add_argument("--grab", action="store_true", default=False,
                    help="take the device from X during a swipe (see header; "
                         "this wedges X's touch state and is off by default)")
    ap.add_argument("--rescan", type=float, default=3.0,
                    help="seconds between hotplug rescans")
    args = ap.parse_args()

    ensure_qtile_python()
    qtile = Qtile()
    devs = []
    state = {"pending": 0.0, "last_send": 0.0}
    next_scan = 0.0

    def _bail(signum, frame):
        for d in devs:
            d.close()
        sys.exit(0)

    signal.signal(signal.SIGTERM, _bail)
    signal.signal(signal.SIGINT, _bail)

    try:
        while True:
            now = time.monotonic()
            if now >= next_scan:
                next_scan = now + args.rescan
                live = set(d.path for d in devs)
                fresh = scan(args.touchpad)
                names = set(d.path for d in fresh)
                for d in list(devs):
                    if d.path not in names:
                        d.close()
                        devs.remove(d)
                added = False
                for d in fresh:
                    if d.path in live:
                        d.close()
                    else:
                        devs.append(d)
                        added = True
                if added:
                    rescale(devs, qtile, args)

            if not devs:
                time.sleep(args.rescan)
                continue

            try:
                ready, _, _ = select.select(
                    [d.fd for d in devs], [], [], min(args.rescan, 0.5)
                )
            except OSError as e:
                if e.errno == errno.EINTR:
                    continue
                next_scan = 0.0
                continue

            if not ready:
                watchdog(devs)
                continue

            for d in list(devs):
                if d.fd not in ready:
                    continue
                try:
                    data = os.read(d.fd, EV_SIZE * 64)
                except OSError as e:
                    if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                        continue
                    d.close()
                    devs.remove(d)
                    next_scan = 0.0
                    continue
                if not data:
                    d.close()
                    devs.remove(d)
                    next_scan = 0.0
                    continue
                for pts in feed(d, data):
                    handle(d, pts, qtile, args, state)
    finally:
        for d in devs:
            d.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
