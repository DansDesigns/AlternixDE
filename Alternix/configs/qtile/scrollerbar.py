# scrollerbar.py - a draggable strip for the Scroller layout.
#
# Draws a miniature of the whole window strip into a qtile Internal window
# placed inside the usable screen area, at its bottom edge: above the bottom
# bar, below the windows. Scroller.bottom_reserve must match `height` so the
# layout leaves room for it.
#
# Works with plain pointer events, so it is driven equally well by a mouse or
# by a touchscreen running in pointer-emulation mode.

import time

from libqtile import hook
from libqtile.log_utils import logger

from scroller import Scroller

# Surviving across lazy.reload_config(), which re-execs config.py but keeps
# already-imported modules, so the previous window can be torn down.
_instance = None


class ScrollerBar:
    def __init__(
        self,
        height=16,
        background="#00000000",
        track="#494d6455",
        column="#494d64cc",
        column_focus="#8aadf4ff",
        viewport="#8aadf433",
        viewport_border="#8aadf4aa",
        padding=6,
        radius=True,
        min_viewport=28,
        interval=0.016,
    ):
        self.height = int(height)
        self.background = background
        self.track = track
        self.column = column
        self.column_focus = column_focus
        self.viewport = viewport
        self.viewport_border = viewport_border
        self.padding = int(padding)
        self.radius = bool(radius)
        self.min_viewport = int(min_viewport)
        self.interval = float(interval)

        self.qtile = None
        self.window = None
        self.drawer = None
        self.x = self.y = 0
        self.width = 1
        self._visible = False
        self._grab = None
        self._last_apply = 0.0

    # ------------------------------------------------------------- lifecycle

    def start(self, qtile):
        """Call once from a startup_complete hook."""
        self.qtile = qtile
        self._build()
        Scroller.add_observer(self._on_layout)
        hook.subscribe.setgroup(self._sync)
        hook.subscribe.layout_change(lambda *args: self._sync())
        hook.subscribe.screens_reconfigured(self._rebuild)

    def finalize(self):
        Scroller.remove_observer(self._on_layout)
        if self.drawer is not None:
            try:
                self.drawer.finalize()
            except Exception:
                pass
            self.drawer = None
        if self.window is not None:
            try:
                self.window.kill()
            except Exception:
                pass
            self.window = None

    def _geometry(self):
        screen = self.qtile.current_screen
        if screen is None:
            return None
        return (
            screen.dx,
            screen.dy + screen.dheight - self.height,
            max(1, screen.dwidth),
            self.height,
        )

    def _build(self):
        geo = self._geometry()
        if geo is None:
            return
        self.x, self.y, self.width, _ = geo
        try:
            self.window = self.qtile.core.create_internal(
                self.x, self.y, self.width, self.height, 32
            )
            self.drawer = self.window.create_drawer(self.width, self.height)
        except Exception:
            logger.exception("ScrollerBar: could not create window")
            self.window = None
            return

        self.window.process_window_expose = self.draw
        self.window.process_button_click = self._press
        self.window.process_button_release = self._release
        self.window.process_pointer_motion = self._motion
        self.window.process_pointer_enter = lambda *a: None
        self.window.process_pointer_leave = lambda *a: None
        self.window.process_key_press = lambda *a: None
        self.window.unhide()
        self._visible = True
        self._sync()

    def _rebuild(self, *args):
        self.finalize()
        self._build()

    # ---------------------------------------------------------------- layout

    def _layout(self):
        """The Scroller instance on the visible group, or None."""
        if self.qtile is None:
            return None
        screen = self.qtile.current_screen
        group = getattr(screen, "group", None) if screen else None
        lay = getattr(group, "layout", None) if group else None
        return lay if isinstance(lay, Scroller) else None

    def _sync(self, *args):
        if self.window is None:
            return
        want = self._layout() is not None
        if want != self._visible:
            self._visible = want
            try:
                self.window.unhide() if want else self.window.hide()
            except Exception:
                pass
        if want:
            self.draw()

    def _on_layout(self, lay):
        if self.window is None or lay is not self._layout():
            return
        if not self._visible:
            self._sync()
            return
        self.draw()

    # --------------------------------------------------------------- drawing

    def _track_rect(self):
        pad = self.padding
        return pad, 2, max(1, self.width - 2 * pad), max(1, self.height - 4)

    def _fill(self, colour, x, y, w, h):
        if w <= 0 or h <= 0:
            return
        self.drawer.set_source_rgb(colour)
        if self.radius:
            self.drawer.rounded_fillrect(int(x), int(y), int(w), int(h), 1)
        else:
            self.drawer.fillrect(int(x), int(y), int(w), int(h), 1)

    def draw(self, *args):
        lay = self._layout()
        if self.window is None or self.drawer is None or lay is None:
            return
        m = lay.strip_metrics()
        self.drawer.clear(self.background)
        tx, ty, tw, th = self._track_rect()
        self._fill(self.track, tx, ty, tw, th)

        if m and m["total"] > 0:
            total = float(m["total"])
            for i, (cx, cw) in enumerate(m["cols"]):
                colour = self.column_focus if i == m["current"] else self.column
                self._fill(
                    colour,
                    tx + cx / total * tw,
                    ty + 1,
                    max(2, cw / total * tw - 1),
                    th - 2,
                )
            vx, vw = self._viewport_rect(m, tw)
            if vw < tw:
                self._fill(self.viewport, tx + vx, ty, vw, th)
                self.drawer.set_source_rgb(self.viewport_border)
                self.drawer.rectangle(int(tx + vx), int(ty), int(vw), int(th), 1)

        self.drawer.draw(offsetx=0, offsety=0, width=self.width,
                         height=self.height)

    # ---------------------------------------------------------------- events

    def _viewport_rect(self, m, tw):
        """Position and width of the viewport marker within the track."""
        total = float(m["total"])
        if total <= 0:
            return 0.0, float(tw)
        vw = max(self.min_viewport, min(tw, m["view"] / total * tw))
        span = float(max(1, m["total"] - m["view"]))
        frac = 0.0 if span <= 1 else max(0.0, min(1.0, m["offset"] / span))
        return frac * (tw - vw), vw

    def _apply(self, px, force=False):
        lay = self._layout()
        if lay is None:
            return
        m = lay.strip_metrics()
        if not m or m["total"] <= m["view"]:
            return
        now = time.monotonic()
        if not force and now - self._last_apply < self.interval:
            return
        self._last_apply = now

        tx, _, tw, _ = self._track_rect()
        _, vw = self._viewport_rect(m, tw)
        vx = px - tx - self._grab
        span = float(max(1, tw - vw))
        frac = max(0.0, min(1.0, vx / span))
        lay.set_offset(round(frac * (m["total"] - m["view"])), dragging=True)

    def _press(self, x, y, button):
        lay = self._layout()
        if lay is None:
            return
        if button == 4:
            lay.scroll_left()
            return
        if button == 5:
            lay.scroll_right()
            return
        if button != 1:
            return

        m = lay.strip_metrics()
        if not m or m["total"] <= m["view"]:
            return
        tx, _, tw, _ = self._track_rect()
        vx, vw = self._viewport_rect(m, tw)
        local = x - tx
        if local < vx or local > vx + vw:
            self._grab = vw / 2.0        # tapped the track: centre on the tap
        else:
            self._grab = local - vx      # grabbed the marker: keep the offset
        self._apply(x, force=True)

    def _motion(self, x, y):
        if self._grab is not None:
            self._apply(x)

    def _release(self, x, y, button):
        if self._grab is None:
            return
        self._apply(x, force=True)
        self._grab = None
        lay = self._layout()
        if lay is not None:
            lay.scroll_settle()


def install(qtile, **kwargs):
    """Create (or replace) the scrollbar. Call from a startup_complete hook."""
    global _instance
    if _instance is not None:
        _instance.finalize()
    _instance = ScrollerBar(**kwargs)
    _instance.start(qtile)
    return _instance
