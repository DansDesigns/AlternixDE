# float_snap.py - Free-positioning X11 layout for Alternix / Qtile.
#
# A traditional floating layout with Qtile-managed client windows, custom
# titlebar internal windows, edge/window snapping and movable snap groups.
# Import with: from float_snap import FloatSnap
#
# Add FloatSnap(...) to the layouts list in config.py.
#
# This module deliberately uses Qtile's internal-window/drawer API. It is
# intended for the X11 (Xlibre) backend used by Alternix.

from libqtile.command.base import expose_command
from libqtile.layout.base import Layout


class FloatSnap(Layout):
    defaults = [
        ("border_width", 2, "Client border width."),
        ("border_focus", "#5B8DEF", "Focused client border colour."),
        ("border_normal", "#454955", "Unfocused client border colour."),
        ("titlebar_height", 28, "Height of each application titlebar."),
        ("titlebar_bg", "#282C34", "Unfocused titlebar background."),
        ("titlebar_focus", "#3A465B", "Focused titlebar background."),
        ("titlebar_text", "#E6EAF2", "Titlebar text colour."),
        ("titlebar_text_focus", "#FFFFFF", "Focused titlebar text colour."),
        ("titlebar_font", "Sans", "Titlebar font family."),
        ("titlebar_fontsize", 11, "Titlebar font size."),
        ("titlebar_padding", 9, "Horizontal titlebar text padding."),
        ("titlebar_close", True, "Show a close button in titlebars."),
        ("titlebar_close_size", 28, "Close button hit-area width."),
        ("joint_titlebar_height", 25, "Height of a shared snap-group titlebar."),
        ("joint_titlebar_bg", "#20242C", "Shared titlebar background."),
        ("joint_titlebar_text", "#FFFFFF", "Shared titlebar text colour."),
        ("snap_distance", 18, "Distance in pixels at which snapping engages."),
        ("snap_to_edges", True, "Snap windows to screen edges."),
        ("snap_to_windows", True, "Snap windows to other windows."),
        ("snap_gap", 0, "Gap maintained between snapped windows."),
        ("min_width", 180, "Minimum client width."),
        ("min_height", 120, "Minimum client height."),
        ("default_width", 0.68, "Initial width as a fraction of screen width."),
        ("default_height", 0.68, "Initial height as a fraction of usable screen height."),
        ("margin", 8, "Outer screen margin."),
        ("group_snap_distance", 8, "Alignment tolerance for shared titlebar groups."),
        ("raise_on_focus", True, "Raise the focused floating window."),
    ]

    def __init__(self, **config):
        Layout.__init__(self, **config)
        self.add_defaults(FloatSnap.defaults)
        self.clients = []
        self.geometry = {}
        self.decorations = {}
        self.joint = {}
        self._rect = None
        self._drag = None
        self._groups = []

    def clone(self, group):
        c = Layout.clone(self, group)
        c.clients = []
        c.geometry = {}
        c.decorations = {}
        c.joint = {}
        c._rect = None
        c._drag = None
        c._groups = []
        return c

    def add_client(self, client):
        self.clients.append(client)
        sr = self._rect
        if sr:
            w = max(self.min_width, int(sr.width * self.default_width))
            h = max(self.min_height, int(sr.height * self.default_height))
            x = sr.x + (sr.width - w) // 2
            y = sr.y + (sr.height - h) // 2
        else:
            x, y, w, h = 80, 80, 800, 600
        self.geometry[client] = [x, y, w, h]
        self._relayout()

    def remove(self, client):
        if client not in self.clients:
            return None
        self.clients.remove(client)
        self.geometry.pop(client, None)
        self._destroy_decoration(client)
        self._groups = [g for g in self._groups if client not in g]
        self._refresh_groups()
        self._relayout()
        if self.clients:
            return self.clients[-1]
        return None

    def focus(self, client):
        if client in self.clients and self.raise_on_focus:
            try:
                client.bring_to_front()
            except Exception:
                pass
        self._relayout()

    def focus_first(self):
        return self.clients[0] if self.clients else None

    def focus_last(self):
        return self.clients[-1] if self.clients else None

    def focus_next(self, win):
        if win in self.clients:
            i = self.clients.index(win) + 1
            return self.clients[i] if i < len(self.clients) else None
        return None

    def focus_previous(self, win):
        if win in self.clients:
            i = self.clients.index(win) - 1
            return self.clients[i] if i > 0 else None
        return None

    def _qtile(self):
        return getattr(self._group, "qtile", None) if self._group else None

    def _create_decoration(self, key, x, y, width, height, draw, click, motion):
        qtile = self._qtile()
        if qtile is None or getattr(qtile.core, "name", "") != "x11":
            return None
        try:
            depth = qtile.core.conn.default_screen.root_depth
            win = qtile.core.create_internal(int(x), int(y), max(1, int(width)),
                                             max(1, int(height)), depth)
            win.unhide()
            win.process_window_expose = lambda event=None: draw(key)
            win.process_button_click = lambda event: click(key, event)
            win.process_pointer_motion = lambda event: motion(key, event)
            drawer = win.create_drawer(max(1, int(width)), max(1, int(height)))
            item = {"window": win, "drawer": drawer, "size": (int(width), int(height))}
            return item
        except Exception:
            return None

    def _destroy_item(self, item):
        if not item:
            return
        try:
            item["window"].kill()
        except Exception:
            try:
                item["window"].hide()
            except Exception:
                pass

    def _destroy_decoration(self, client):
        self._destroy_item(self.decorations.pop(client, None))

    def _destroy_joint(self, key):
        self._destroy_item(self.joint.pop(key, None))

    def _draw_titlebar(self, client):
        item = self.decorations.get(client)
        if not item:
            return
        d = item["drawer"]
        w, h = item["size"]
        focused = bool(getattr(client, "has_focus", False))
        bg = self.titlebar_focus if focused else self.titlebar_bg
        fg = self.titlebar_text_focus if focused else self.titlebar_text
        try:
            d.clear(bg)
            name = str(getattr(client, "name", "") or "Application")
            # Keep the close control clear of the title text.
            max_chars = max(1, int((w - self.titlebar_padding * 2 -
                                    (self.titlebar_close_size if self.titlebar_close else 0))
                                   / max(6, self.titlebar_fontsize * 0.62)))
            if len(name) > max_chars:
                name = name[:max(1, max_chars - 1)] + "…"
            d.text(name, self.titlebar_padding, max(0, (h - self.titlebar_fontsize) // 2 - 1),
                   fg, self.titlebar_font, self.titlebar_fontsize)
            if self.titlebar_close:
                d.text("×", w - self.titlebar_close_size + 8,
                       max(0, (h - self.titlebar_fontsize) // 2 - 2),
                       fg, self.titlebar_font, self.titlebar_fontsize + 3)
            d.draw()
        except Exception:
            pass

    def _draw_joint(self, key):
        item = self.joint.get(key)
        if not item:
            return
        d = item["drawer"]
        w, h = item["size"]
        group = next((g for g in self._groups if self._group_key(g) == key), [])
        try:
            d.clear(self.joint_titlebar_bg)
            label = "  ◈  Window group  ·  %d windows" % len(group)
            d.text(label, 9, max(0, (h - self.titlebar_fontsize) // 2 - 1),
                   self.joint_titlebar_text, self.titlebar_font,
                   self.titlebar_fontsize)
            d.draw()
        except Exception:
            pass

    def _group_key(self, group):
        return tuple(sorted(id(c) for c in group))

    def _event_root(self, event):
        x = getattr(event, "root_x", getattr(event, "x_root", None))
        y = getattr(event, "root_y", getattr(event, "y_root", None))
        if x is None or y is None:
            q = self._qtile()
            try:
                return q.core.get_mouse_position()
            except Exception:
                return (0, 0)
        return int(x), int(y)

    def _title_click(self, client, event):
        button = getattr(event, "detail", 1)
        if self.titlebar_close and button == 1:
            ex = int(getattr(event, "event_x", getattr(event, "x", 0)))
            item = self.decorations.get(client)
            width = item["size"][0] if item else 0
            if ex >= width - self.titlebar_close_size:
                try:
                    client.kill()
                except Exception:
                    pass
                return
        if button not in (1, 3):
            return
        qtile = self._qtile()
        if qtile:
            try:
                qtile.current_group.focus(client, False)
            except Exception:
                pass
        px, py = self._event_root(event)
        g = self.geometry.get(client)
        if not g:
            return
        self._drag = {
            "kind": "move" if button == 1 else "resize",
            "client": client,
            "start": (px, py),
            "origin": tuple(g),
            "last": (px, py),
        }

    def _joint_click(self, key, event):
        if getattr(event, "detail", 1) != 1:
            return
        group = next((g for g in self._groups if self._group_key(g) == key), None)
        if not group:
            return
        px, py = self._event_root(event)
        self._drag = {
            "kind": "group", "clients": list(group), "start": (px, py),
            "origins": {c: tuple(self.geometry[c]) for c in group},
            "last": (px, py),
        }

    def _motion(self, key, event):
        if not self._drag:
            return
        px, py = self._event_root(event)
        drag = self._drag
        sx, sy = drag["start"]
        dx, dy = px - sx, py - sy
        drag["last"] = (px, py)
        if drag["kind"] == "group":
            for c, origin in drag["origins"].items():
                if c in self.geometry:
                    self.geometry[c] = [origin[0] + dx, origin[1] + dy,
                                        origin[2], origin[3]]
            self._relayout()
            return
        client = drag["client"]
        if client not in self.geometry:
            self._drag = None
            return
        ox, oy, ow, oh = drag["origin"]
        if drag["kind"] == "move":
            self.geometry[client] = [ox + dx, oy + dy, ow, oh]
            self._snap(client)
        else:
            self.geometry[client] = [
                ox, oy, max(self.min_width, ow + dx),
                max(self.min_height, oh + dy)
            ]
        self._relayout()

    def _snap(self, client):
        if not self._rect or client not in self.geometry:
            return
        x, y, w, h = self.geometry[client]
        sr = self._rect
        d = int(self.snap_distance)
        gap = int(self.snap_gap)
        if self.snap_to_edges:
            if abs(x - sr.x) <= d:
                x = sr.x + gap
            if abs((x + w) - (sr.x + sr.width)) <= d:
                x = sr.x + sr.width - w - gap
            if abs(y - sr.y) <= d:
                y = sr.y + gap
            if abs((y + h) - (sr.y + sr.height)) <= d:
                y = sr.y + sr.height - h - gap
        if self.snap_to_windows:
            for other in self.clients:
                if other is client or other not in self.geometry:
                    continue
                ox, oy, ow, oh = self.geometry[other]
                vertical_overlap = y < oy + oh and y + h > oy
                horizontal_overlap = x < ox + ow and x + w > ox
                if vertical_overlap:
                    if abs(x - (ox + ow)) <= d:
                        x = ox + ow + gap
                    elif abs((x + w) - ox) <= d:
                        x = ox - w - gap
                if horizontal_overlap:
                    if abs(y - (oy + oh)) <= d:
                        y = oy + oh + gap
                    elif abs((y + h) - oy) <= d:
                        y = oy - h - gap
        self.geometry[client] = [int(x), int(y), int(w), int(h)]

    def _refresh_groups(self):
        # A snap group is a connected set of windows whose edges touch or align.
        # Existing groups persist while their members remain geometrically joined.
        joined = []
        for i, a in enumerate(self.clients):
            if a not in self.geometry:
                continue
            ax, ay, aw, ah = self.geometry[a]
            for b in self.clients[i + 1:]:
                if b not in self.geometry:
                    continue
                bx, by, bw, bh = self.geometry[b]
                tol = int(self.group_snap_distance)
                vertical_touch = (abs((ax + aw) - bx) <= tol or
                                  abs((bx + bw) - ax) <= tol) and \
                                 min(ay + ah, by + bh) - max(ay, by) > 30
                horizontal_touch = (abs((ay + ah) - by) <= tol or
                                    abs((by + bh) - ay) <= tol) and \
                                   min(ax + aw, bx + bw) - max(ax, bx) > 30
                if vertical_touch or horizontal_touch:
                    joined.append((a, b))
        groups = []
        for a, b in joined:
            ga = next((g for g in groups if a in g), None)
            gb = next((g for g in groups if b in g), None)
            if ga and gb and ga is not gb:
                ga.update(gb)
                groups.remove(gb)
            elif ga:
                ga.add(b)
            elif gb:
                gb.add(a)
            else:
                groups.append({a, b})
        self._groups = [list(g) for g in groups if len(g) > 1]

    def _position_item(self, item, x, y, width, height):
        if not item:
            return
        try:
            item["window"].place(int(x), int(y), max(1, int(width)),
                                 max(1, int(height)), 0, None)
            item["drawer"].width = max(1, int(width))
            item["drawer"].height = max(1, int(height))
        except Exception:
            pass

    def layout(self, windows, screen_rect):
        self._rect = screen_rect
        Layout.layout(self, windows, screen_rect)
        self._refresh_groups()
        self._relayout()

    def configure(self, client, screen_rect):
        if client not in self.clients:
            client.hide()
            return
        if client not in self.geometry:
            self.add_client(client)
        x, y, w, h = self.geometry[client]
        sr = screen_rect
        # Keep a useful portion of every window reachable after resolution changes.
        w = max(self.min_width, min(int(w), max(self.min_width, sr.width)))
        h = max(self.min_height, min(int(h), max(self.min_height, sr.height - self.titlebar_height)))
        x = min(max(int(x), sr.x - w + 80), sr.x + sr.width - 80)
        y = min(max(int(y), sr.y), sr.y + sr.height - self.titlebar_height - 40)
        self.geometry[client] = [x, y, w, h]
        bw = int(self.border_width)
        focused = bool(getattr(client, "has_focus", False))
        try:
            client.place(x, y + self.titlebar_height, max(1, w - 2 * bw),
                         max(1, h - self.titlebar_height - 2 * bw), bw,
                         self.border_focus if focused else self.border_normal)
            client.unhide()
        except Exception:
            pass
        item = self.decorations.get(client)
        if item is None:
            item = self._create_decoration(
                client, x, y, w, self.titlebar_height,
                self._draw_titlebar, self._title_click, self._motion
            )
            if item:
                self.decorations[client] = item
        if item:
            item["size"] = (w, self.titlebar_height)
            self._position_item(item, x, y, w, self.titlebar_height)
            self._draw_titlebar(client)

    def _relayout(self):
        if self._group is None:
            return
        try:
            self._group.layout_all()
        except Exception:
            pass
        self._update_joint_bars()

    def _update_joint_bars(self):
        active = set()
        for group in self._groups:
            key = self._group_key(group)
            active.add(key)
            boxes = [self.geometry[c] for c in group if c in self.geometry]
            if len(boxes) < 2:
                continue
            left = min(g[0] for g in boxes)
            top = min(g[1] for g in boxes)
            right = max(g[0] + g[2] for g in boxes)
            width = right - left
            height = int(self.joint_titlebar_height)
            item = self.joint.get(key)
            if item is None:
                item = self._create_decoration(
                    key, left, top - height, width, height,
                    self._draw_joint, self._joint_click, self._motion
                )
                if item:
                    self.joint[key] = item
            if item:
                item["size"] = (width, height)
                self._position_item(item, left, max(0, top - height), width, height)
                self._draw_joint(key)
        for key in list(self.joint):
            if key not in active:
                self._destroy_joint(key)

    @expose_command()
    def move(self, dx, dy):
        """Move the focused window by a relative amount."""
        c = self._group.current_window if self._group else None
        if c in self.geometry:
            g = self.geometry[c]
            g[0] += int(dx)
            g[1] += int(dy)
            self._snap(c)
            self._relayout()

    @expose_command()
    def resize(self, dw, dh):
        """Resize the focused window by a relative amount."""
        c = self._group.current_window if self._group else None
        if c in self.geometry:
            g = self.geometry[c]
            g[2] = max(self.min_width, g[2] + int(dw))
            g[3] = max(self.min_height, g[3] + int(dh))
            self._relayout()

    @expose_command()
    def centre(self):
        """Centre the focused window on the usable screen."""
        c = self._group.current_window if self._group else None
        if c and self._rect and c in self.geometry:
            g = self.geometry[c]
            g[0] = self._rect.x + (self._rect.width - g[2]) // 2
            g[1] = self._rect.y + (self._rect.height - g[3]) // 2
            self._relayout()

    @expose_command()
    def unsnap(self):
        """Remove the focused window from any shared snap group."""
        c = self._group.current_window if self._group else None
        self._groups = [g for g in self._groups if c not in g]
        self._update_joint_bars()

    @expose_command()
    def info(self):
        d = Layout.info(self)
        d["clients"] = [getattr(c, "name", "") for c in self.clients]
        d["geometry"] = {
            str(getattr(c, "name", "")): list(g)
            for c, g in self.geometry.items()
        }
        d["snap_groups"] = [
            [getattr(c, "name", "") for c in group] for group in self._groups
        ]
        return d
