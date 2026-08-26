# scroller.py - horizontal scrolling layout for qtile (PaperWM / niri style)
#
# Windows live on an infinite horizontal strip. The screen is a viewport onto
# that strip; anything outside it is simply placed off-screen.

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
        ("column_widths", [0.34, 0.5, 0.67, 1.0],
         "Selectable column widths, as a fraction of the viewport."),
        ("default_width", 2, "Index into column_widths used for new windows."),
        ("single_full_width", True,
         "Ignore column_widths when the group holds one window."),
        ("centre_focused", False,
         "Keep the focused column centred instead of just visible."),
        ("scroll_step", 160, "Pixels moved by scroll_left / scroll_right."),
        ("unmap_offscreen", True,
         "Unmap windows fully outside the viewport (saves CPU on Atom)."),
        ("offscreen_buffer", 400,
         "Extra pixels beyond the viewport kept mapped."),
        ("min_column", 120, "Minimum column width in pixels."),
        ("drag_settle_delay", 0.25,
         "Seconds of pointer stillness that ends a drag."),
    ]

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
        self._scrolling = False
        self._drag_base = 0
        self._drag_seq = 0

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
        c._scrolling = False
        c._drag_base = 0
        c._drag_seq = 0
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

    def _do_reveal(self, xs, ws, sr):
        if not self.clients:
            return
        view = self._viewport(sr)
        i = max(0, min(self.current, len(xs) - 1))
        if self.centre_focused:
            self.offset = xs[i] - (view - ws[i]) // 2
        elif xs[i] < self.offset:
            self.offset = xs[i]
        elif xs[i] + ws[i] > self.offset + view:
            self.offset = xs[i] + ws[i] - view

    def _clamp(self, total, sr):
        view = self._viewport(sr)
        if total <= view:
            self.offset = -(view - total) // 2
        else:
            self.offset = max(0, min(self.offset, total - view))

    def layout(self, windows, screen_rect):
        self._rect = screen_rect
        xs, ws, total = self._compute(screen_rect)
        if self._reveal:
            self._do_reveal(xs, ws, screen_rect)
            self._reveal = False
        self._clamp(total, screen_rect)
        self._geo = (xs, ws, total)
        Layout.layout(self, windows, screen_rect)

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
            max(1, screen_rect.height - 2 * bw),
            bw,
            self.border_focus if focused else self.border_normal,
            margin=self.margin,
        )
        client.unhide()

    def _relayout(self):
        if self.group:
            self.group.layout_all()

    # --------------------------------------------------------- drag plumbing

    def _arm_drag_timer(self):
        """Qtile gives no button-release hook, so settle on pointer stillness."""
        qtile = getattr(self.group, "qtile", None) if self.group else None
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
    def drag_start(self):
        """Bind as a Drag start=. Anchors the strip and returns an origin."""
        self._drag_base = self.offset
        self._scrolling = True
        self._arm_drag_timer()
        return (0, 0)

    @expose_command()
    def drag_scroll(self, dx=0, dy=0):
        """Bind as a Drag command. dx is pointer travel since drag_start."""
        self._scrolling = True
        self.offset = self._drag_base - int(dx)
        self._arm_drag_timer()
        self._relayout()

    @expose_command()
    def scroll_by(self, dx):
        """Scroll the viewport by dx pixels. Used by the swipe daemon."""
        self._scrolling = True
        self.offset += int(dx)
        self._relayout()

    @expose_command()
    def scroll_settle(self):
        """End a swipe or drag: snap to the nearest column and focus it."""
        self._scrolling = False
        self._drag_seq += 1
        if not self.clients or self._rect is None:
            self._relayout()
            return
        xs, ws, _ = self._geo
        centre = self.offset + self._viewport(self._rect) / 2.0
        best, best_d = 0, None
        for i, (x, w) in enumerate(zip(xs, ws)):
            d = abs(x + w / 2.0 - centre)
            if best_d is None or d < best_d:
                best, best_d = i, d
        self.current = best
        self._reveal = True
        win = self.clients[best]
        if self.group and self.group.current_window is not win:
            self.group.focus(win, False)
        self._relayout()

    @expose_command()
    def scroll_left(self, step=None):
        self.offset -= int(step or self.scroll_step)
        self._relayout()

    @expose_command()
    def scroll_right(self, step=None):
        self.offset += int(step or self.scroll_step)
        self._relayout()

    @expose_command()
    def left(self):
        """Focus the column to the left."""
        if self.clients and self.current > 0:
            self.group.focus(self.clients[self.current - 1], False)

    @expose_command()
    def right(self):
        """Focus the column to the right."""
        if self.clients and self.current < len(self.clients) - 1:
            self.group.focus(self.clients[self.current + 1], False)

    @expose_command()
    def first(self):
        if self.clients:
            self.group.focus(self.clients[0], False)

    @expose_command()
    def last(self):
        if self.clients:
            self.group.focus(self.clients[-1], False)

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
            i = self.current
            self.offset = xs[i] - (self._viewport(self._rect) - ws[i]) // 2
            self._relayout()

    @expose_command()
    def info(self):
        d = Layout.info(self)
        d["clients"] = [c.name for c in self.clients]
        d["current"] = self.current
        d["offset"] = self.offset
        return d