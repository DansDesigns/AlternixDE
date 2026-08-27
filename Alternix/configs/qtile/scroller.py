# scroller.py - horizontal scrolling layout for qtile (PaperWM / niri style)
#
# Windows live on an infinite horizontal strip. The screen is a viewport onto
# that strip; anything outside it is simply placed off-screen.
#
# Bringing a column into view is eased rather than jumped: every reveal, from
# a swipe settling to a new window opening, runs through _start_anim. Set
# snap_duration to 0 for the old instant behaviour, or snap="free" to stop the
# viewport being pulled toward the focused column at all.

import time

from libqtile.command.base import expose_command
from libqtile.layout.base import Layout


class Scroller(Layout):
    defaults = [
        ("border_focus", "#8aadf4c0", "Border colour for the focused window."),
        ("border_normal", "#494d6480", "Border colour for unfocused windows."),
        ("border_width", 3, "Border width in pixels."),
        ("margin", 0, "Margin inside each column (int or [t, r, b, l])."),
        ("gap", 10, "Horizontal gap between columns, in pixels."),
        ("edge_padding", 10, "Padding kept at the left/right screen edges."),
        ("bottom_reserve", 0,
         "Pixels left free at the bottom of the screen for the scrollbar."),
        ("column_widths", [0.34, 0.5, 0.67, 1.0],
         "Selectable column widths, as a fraction of the viewport."),
        ("default_width", 2, "Index into column_widths used for new windows."),
        ("single_full_width", True,
         "Ignore column_widths when the group holds one window."),
        ("centre_focused", False,
         "Keep the focused column centred instead of just visible."),
        ("snap", "column",
         "'column' eases the viewport onto the nearest column after a swipe; "
         "'free' leaves the viewport exactly where it was let go."),
        ("snap_duration", 0.18,
         "Seconds an eased move takes. 0 restores instant jumps."),
        ("snap_overshoot", 0.12,
         "Fraction of the travelled distance the ease overshoots before "
         "settling back. 0 is a plain ease-out with no bounce."),
        ("snap_overshoot_max", 40,
         "Ceiling in pixels on that overshoot, so long jumps do not fling."),
        ("snap_fps", 30, "Frames per second while easing."),
        ("scroll_step", 160, "Pixels moved by scroll_left / scroll_right."),
        ("unmap_offscreen", True,
         "Unmap windows fully outside the viewport (saves CPU on Atom)."),
        ("offscreen_buffer", 400,
         "Extra pixels beyond the viewport kept mapped."),
        ("min_column", 120, "Minimum column width in pixels."),
        ("drag_settle_delay", 0.25,
         "Seconds of pointer stillness that ends a drag."),
    ]

    # Callables notified after every layout pass, so an external scrollbar can
    # redraw without polling. Class level, so every cloned layout shares them.
    _observers = []

    def __init__(self, **config):
        Layout.__init__(self, **config)
        self.add_defaults(Scroller.defaults)
        self.clients = []
        self.widths = {}
        self.current = 0
        self.offset = 0
        self._rect = None
        self._geo = ([], [], 0)
        self._reveal = True
        self._no_reveal = False
        self._scrolling = False
        self._drag_base = 0
        self._drag_seq = 0
        self._anim_seq = 0
        self._anim_from = 0
        self._anim_to = 0
        self._anim_t0 = 0.0
        self._anim_c1 = 0.0

    # ------------------------------------------------------------- observers

    @classmethod
    def add_observer(cls, fn):
        if fn not in Scroller._observers:
            Scroller._observers.append(fn)

    @classmethod
    def remove_observer(cls, fn):
        if fn in Scroller._observers:
            Scroller._observers.remove(fn)

    def _notify(self):
        for fn in list(Scroller._observers):
            try:
                fn(self)
            except Exception:
                pass

    # ------------------------------------------------------------------ core

    def clone(self, group):
        c = Layout.clone(self, group)
        c.clients = []
        c.widths = {}
        c.current = 0
        c.offset = 0
        c._rect = None
        c._geo = ([], [], 0)
        c._reveal = True
        c._no_reveal = False
        c._scrolling = False
        c._drag_base = 0
        c._drag_seq = 0
        c._anim_seq = 0
        c._anim_from = 0
        c._anim_to = 0
        c._anim_t0 = 0.0
        c._anim_c1 = 0.0
        return c

    def add_client(self, client):
        self.widths[client] = self.default_width
        if self.clients:
            self.clients.insert(self.current + 1, client)
            self.current += 1
        else:
            self.clients.append(client)
            self.current = 0
        self._reveal = True

    def remove(self, client):
        if client not in self.clients:
            return None
        i = self.clients.index(client)
        del self.clients[i]
        self.widths.pop(client, None)
        if not self.clients:
            self.current = 0
            self.offset = 0
            return None
        if i <= self.current:
            self.current -= 1
        self.current = max(0, min(self.current, len(self.clients) - 1))
        self._reveal = True
        return self.clients[self.current]

    def focus(self, client):
        if client in self.clients:
            self.current = self.clients.index(client)
            self._reveal = True

    def focus_first(self):
        return self.clients[0] if self.clients else None

    def focus_last(self):
        return self.clients[-1] if self.clients else None

    def focus_next(self, win):
        if win in self.clients:
            i = self.clients.index(win) + 1
            if i < len(self.clients):
                return self.clients[i]
        return None

    def focus_previous(self, win):
        if win in self.clients:
            i = self.clients.index(win) - 1
            if i >= 0:
                return self.clients[i]
        return None

    # -------------------------------------------------------------- geometry

    def _viewport(self, sr):
        return max(1, sr.width - 2 * self.edge_padding)

    def _compute(self, sr):
        view = self._viewport(sr)
        single = self.single_full_width and len(self.clients) == 1
        xs, ws, x = [], [], 0
        for c in self.clients:
            if single:
                w = view
            else:
                i = self.widths.get(c, self.default_width)
                i = max(0, min(i, len(self.column_widths) - 1))
                w = max(self.min_column, int(view * self.column_widths[i]))
            xs.append(x)
            ws.append(w)
            x += w + self.gap
        total = x - self.gap if self.clients else 0
        return xs, ws, total

    def _reveal_offset(self, xs, ws, sr, i):
        """Where the viewport would sit to show column i, without moving it."""
        if not xs:
            return self.offset
        i = max(0, min(i, len(xs) - 1))
        view = self._viewport(sr)
        if self.centre_focused:
            return xs[i] - (view - ws[i]) // 2
        if xs[i] < self.offset:
            return xs[i]
        if xs[i] + ws[i] > self.offset + view:
            return xs[i] + ws[i] - view
        return self.offset

    def _clamp(self, total, sr):
        view = self._viewport(sr)
        if total <= view:
            self.offset = -(view - total) // 2
        else:
            self.offset = max(0, min(self.offset, total - view))

    # ------------------------------------------------------------- animation

    @staticmethod
    def _back_peak(c1):
        """Peak of the back-ease for a given constant, as a fraction over 1."""
        u = -2.0 * c1 / (3.0 * (c1 + 1.0))
        return (c1 + 1.0) * u * u * u + c1 * u * u

    def _solve_back(self, k):
        """Constant whose back-ease peaks k above the target.

        Peak overshoot is strongly non-linear in the constant, so scaling the
        textbook 1.70158 gives almost no bounce. Bisection gets the parameter
        to mean the fraction it claims.
        """
        lo, hi = 0.0, 12.0
        for _ in range(40):
            mid = (lo + hi) / 2.0
            if self._back_peak(mid) < k:
                lo = mid
            else:
                hi = mid
        return (lo + hi) / 2.0

    def _ease(self, t):
        """Ease-out, optionally overshooting the target before settling."""
        c1 = self._anim_c1
        if c1 <= 0.0:
            u = 1.0 - t
            return 1.0 - u * u * u
        c3 = c1 + 1.0
        u = t - 1.0
        return 1.0 + c3 * u * u * u + c1 * u * u

    def _cancel_anim(self):
        self._anim_seq += 1

    def _start_anim(self, target):
        """Begin easing toward target. False if it must be done instantly."""
        if self.snap_duration <= 0 or self._group is None:
            return False
        qtile = getattr(self._group, "qtile", None)
        if qtile is None:
            return False
        self._anim_seq += 1
        seq = self._anim_seq
        self._anim_from = self.offset
        self._anim_to = int(target)
        self._anim_t0 = time.monotonic()
        self._scrolling = True

        # Overshoot is a fraction of the distance, capped in pixels so a jump
        # across the whole strip does not fling far past its destination.
        span = abs(self._anim_to - self._anim_from)
        k = float(self.snap_overshoot)
        if k > 0 and span > 0:
            k = min(k, float(self.snap_overshoot_max) / span)
        self._anim_c1 = self._solve_back(k) if k > 0.0005 else 0.0
        try:
            qtile.call_later(1.0 / max(1, self.snap_fps), self._anim_step, seq)
        except Exception:
            return False
        return True

    def _anim_step(self, seq):
        if seq != self._anim_seq or self._group is None:
            return
        elapsed = time.monotonic() - self._anim_t0
        t = elapsed / max(0.001, self.snap_duration)
        if t >= 1.0:
            self.offset = self._anim_to
            self._scrolling = False
            self._anim_seq += 1
            self._relayout()
            return
        span = self._anim_to - self._anim_from
        self.offset = int(round(self._anim_from + span * self._ease(t)))
        self._relayout()
        qtile = getattr(self._group, "qtile", None)
        if qtile is not None:
            try:
                qtile.call_later(
                    1.0 / max(1, self.snap_fps), self._anim_step, seq
                )
            except Exception:
                pass

    # ----------------------------------------------------------------- passes

    def layout(self, windows, screen_rect):
        self._rect = screen_rect
        xs, ws, total = self._compute(screen_rect)
        if self._reveal:
            self._reveal = False
            if not self._no_reveal:
                target = self._reveal_offset(xs, ws, screen_rect, self.current)
                if target != self.offset and not self._start_anim(target):
                    self.offset = target
        self._clamp(total, screen_rect)
        self._geo = (xs, ws, total)
        Layout.layout(self, windows, screen_rect)
        self._notify()

    def configure(self, client, screen_rect):
        if client not in self.clients:
            client.hide()
            return
        xs, ws, _ = self._geo
        i = self.clients.index(client)
        if i >= len(xs):
            client.hide()
            return

        bw = self.border_width
        x = screen_rect.x + self.edge_padding + xs[i] - self.offset
        w = ws[i]
        h = screen_rect.height - self.bottom_reserve
        focused = getattr(client, "has_focus", False) or i == self.current

        if self.unmap_offscreen and not self._scrolling and not focused:
            left = screen_rect.x - self.offscreen_buffer
            right = screen_rect.x + screen_rect.width + self.offscreen_buffer
            if x + w <= left or x >= right:
                client.hide()
                return

        client.place(
            x,
            screen_rect.y,
            max(1, w - 2 * bw),
            max(1, h - 2 * bw),
            bw,
            self.border_focus if focused else self.border_normal,
            margin=self.margin,
        )
        client.unhide()

    def _relayout(self):
        if self._group is not None:
            self._group.layout_all()

    def _focus_client(self, client):
        if self._group is not None:
            self._group.focus(client, False)

    # --------------------------------------------------------- drag plumbing

    def _arm_drag_timer(self):
        """Qtile gives no button-release hook, so settle on pointer stillness."""
        if self._group is None:
            return
        qtile = getattr(self._group, "qtile", None)
        if qtile is None:
            return
        self._drag_seq += 1
        seq = self._drag_seq
        try:
            qtile.call_later(self.drag_settle_delay, self._drag_timeout, seq)
        except Exception:
            pass

    def _drag_timeout(self, seq):
        if seq == self._drag_seq and self._scrolling:
            self.scroll_settle()

    # -------------------------------------------------------------- commands

    @expose_command()
    def strip_metrics(self):
        """Geometry of the strip and viewport, for an external scrollbar."""
        if self._rect is None:
            return None
        xs, ws, total = self._geo
        return {
            "offset": self.offset,
            "view": self._viewport(self._rect),
            "total": total,
            "current": self.current,
            "count": len(self.clients),
            "cols": [[xs[i], ws[i]] for i in range(len(xs))],
        }

    @expose_command()
    def set_offset(self, value, dragging=False):
        """Move the viewport to an absolute strip position."""
        self._cancel_anim()
        self._scrolling = bool(dragging)
        self.offset = int(value)
        self._relayout()

    @expose_command()
    def next(self):
        """Focus the next column, wrapping at the end of the strip."""
        if self.clients:
            i = (self.current + 1) % len(self.clients)
            self._focus_client(self.clients[i])

    @expose_command()
    def previous(self):
        """Focus the previous column, wrapping at the start of the strip."""
        if self.clients:
            i = (self.current - 1) % len(self.clients)
            self._focus_client(self.clients[i])

    @expose_command()
    def drag_start(self):
        """Bind as a Drag start=. Anchors the strip and returns an origin."""
        self._cancel_anim()
        self._drag_base = self.offset
        self._scrolling = True
        self._arm_drag_timer()
        return (0, 0)

    @expose_command()
    def drag_scroll(self, dx=0, dy=0):
        """Bind as a Drag command. dx is pointer travel since drag_start."""
        self._cancel_anim()
        self._scrolling = True
        self.offset = self._drag_base - int(dx)
        self._arm_drag_timer()
        self._relayout()

    @expose_command()
    def scroll_by(self, dx):
        """Scroll the viewport by dx pixels. Used by the swipe daemon."""
        self._cancel_anim()
        self._scrolling = True
        self.offset += int(dx)
        self._relayout()

    @expose_command()
    def scroll_settle(self):
        """End a swipe or drag: ease onto the nearest column and focus it."""
        self._cancel_anim()
        self._scrolling = False
        self._drag_seq += 1
        if not self.clients or self._rect is None:
            self._relayout()
            return
        xs, ws, _ = self._geo
        if not xs:
            self._relayout()
            return
        centre = self.offset + self._viewport(self._rect) / 2.0
        best, best_d = 0, None
        for i, (x, w) in enumerate(zip(xs, ws)):
            d = abs(x + w / 2.0 - centre)
            if best_d is None or d < best_d:
                best, best_d = i, d
        self.current = best
        win = self.clients[best]

        if self.snap == "free":
            # Focus follows the eye, but the viewport stays put.
            self._no_reveal = True
            try:
                if self._group is not None and \
                        self._group.current_window is not win:
                    self._group.focus(win, False)
                else:
                    self._reveal = False
                    self._relayout()
            finally:
                self._no_reveal = False
            return

        self._reveal = True
        if self._group is not None and self._group.current_window is not win:
            self._group.focus(win, False)
        else:
            self._relayout()

    @expose_command()
    def scroll_left(self, step=None):
        self._cancel_anim()
        self.offset -= int(step or self.scroll_step)
        self._relayout()

    @expose_command()
    def scroll_right(self, step=None):
        self._cancel_anim()
        self.offset += int(step or self.scroll_step)
        self._relayout()

    @expose_command()
    def left(self):
        """Focus the column to the left."""
        if self.clients and self.current > 0:
            self._focus_client(self.clients[self.current - 1])

    @expose_command()
    def right(self):
        """Focus the column to the right."""
        if self.clients and self.current < len(self.clients) - 1:
            self._focus_client(self.clients[self.current + 1])

    @expose_command()
    def first(self):
        if self.clients:
            self._focus_client(self.clients[0])

    @expose_command()
    def last(self):
        if self.clients:
            self._focus_client(self.clients[-1])

    @expose_command()
    def shuffle_left(self):
        """Move the focused window one column left along the strip."""
        i = self.current
        if i > 0:
            self.clients[i - 1], self.clients[i] = self.clients[i], self.clients[i - 1]
            self.current = i - 1
            self._reveal = True
            self._relayout()

    @expose_command()
    def shuffle_right(self):
        i = self.current
        if 0 <= i < len(self.clients) - 1:
            self.clients[i + 1], self.clients[i] = self.clients[i], self.clients[i + 1]
            self.current = i + 1
            self._reveal = True
            self._relayout()

    @expose_command()
    def set_width(self, index):
        """Set the focused column to column_widths[index]."""
        if self.clients:
            n = len(self.column_widths) - 1
            self.widths[self.clients[self.current]] = max(0, min(int(index), n))
            self._reveal = True
            self._relayout()

    @expose_command()
    def grow(self):
        if self.clients:
            c = self.clients[self.current]
            self.set_width(self.widths.get(c, self.default_width) + 1)

    @expose_command()
    def shrink(self):
        if self.clients:
            c = self.clients[self.current]
            self.set_width(self.widths.get(c, self.default_width) - 1)

    @expose_command()
    def cycle_width(self):
        if self.clients:
            c = self.clients[self.current]
            n = len(self.column_widths)
            self.widths[c] = (self.widths.get(c, self.default_width) + 1) % n
            self._reveal = True
            self._relayout()

    @expose_command()
    def centre(self):
        """Centre the focused column in the viewport."""
        if self.clients and self._rect is not None:
            xs, ws, _ = self._geo
            if xs:
                i = max(0, min(self.current, len(xs) - 1))
                target = xs[i] - (self._viewport(self._rect) - ws[i]) // 2
                self._cancel_anim()
                if not self._start_anim(target):
                    self.offset = target
                self._relayout()

    @expose_command()
    def set_snap(self, mode):
        """Switch between 'column' and 'free' at runtime."""
        if mode in ("column", "free"):
            self.snap = mode

    @expose_command()
    def info(self):
        d = Layout.info(self)
        d["clients"] = [c.name for c in self.clients]
        d["current"] = self.current
        d["offset"] = self.offset
        d["snap"] = self.snap
        return d
