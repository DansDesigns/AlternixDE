# Copyright (c) 2010 Aldo Cortesi
# Copyright (c) 2010, 2014 dequis
# Copyright (c) 2012 Randall Ma
# Copyright (c) 2012-2014 Tycho Andersen
# Copyright (c) 2012 Craig Barnes
# Copyright (c) 2013 horsik
# Copyright (c) 2013 Tao Sauvage
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import subprocess
import os
from os.path import expanduser
from libqtile import bar, layout, qtile, widget, hook
from libqtile.config import Click, Drag, Group, Key, Match, Screen
from libqtile.lazy import lazy
from libqtile.utils import guess_terminal
from qtile_extras.popup import PopupGridLayout, PopupWidget, PopupImage, PopupText
from datetime import datetime
import threading
import time
# from pathlib import Path
# import configparser


mod = "mod4"
terminal = guess_terminal()
bg = '28282899'
# @lazy.function

###############
## AutoStart ##
###############

# Lock on DPMS wake (screen turning back on)
@hook.subscribe.screen_change
def lock_on_screen_wake(event, screen):
    subprocess.Popen(["systemctl", "restart", "osm-lockscreen.service"])

@hook.subscribe.resume
def lock_on_resume():
    subprocess.Popen(["systemctl", "restart", "osm-lockscreen.service"])


@hook.subscribe.startup
def autostart():
    # Autostart Programs
    subprocess.Popen(['osm-lockd'])
    subprocess.Popen(['osm-paper-restore'])
    subprocess.Popen(['osm-notify'])
#    subprocess.Popen(['osm-status'])
    subprocess.Popen(['osm-running'])
    subprocess.Popen(['onboard'])
    subprocess.Popen(['picom', '-b'])
    subprocess.Popen(['osm-powerd'])
    subprocess.Popen(['touchegg'])
    subprocess.Popen(["python3", "/usr/bin/rounded_corners"])

    # Function to update the time
    def update_time():
        clock_text.update(datetime.now().strftime("%H:%M:%S"))
        qtile.call_later(1, update_time)  # refresh every second

    update_time()

###################
## System Graphs ##
###################

def show_graphs(qtile):
    screen = qtile.current_screen
    screen_width = screen.width
    screen_height = screen.height

    controls = [
        PopupWidget(
            widget=widget.CPUGraph(),
            row=0, col=0,
            row_span=2, col_span=6
        ),
        PopupWidget(
            widget=widget.NetGraph(),
            row=2, col=3,
            col_span=3
        ),
        PopupWidget(
            widget=widget.MemoryGraph(),
            row=2, col=0,
            col_span=3
        ),

        # Close button:
        # PopupImage(
        #     filename=expanduser("~/close_bar.png"),
        #     row=3, col=2,
        #     col_span=2,
        #     mouse_callbacks={"Button1": lambda: layout.hide()}
        # ),

    ]

    layout_popup = PopupGridLayout(
        qtile,
        rows=4,
        cols=6,
        height=screen_height // 3,
        width=screen_width,
        controls=controls,
        background="#000000",
        close_on_click=True
    )
    layout_popup.show(centered=False, x=0, y=65)

##########
## Keys ##
##########

keys = [
    # A list of available commands that can be bound to keys can be found
    # at https://docs.qtile.org/en/latest/manual/config/lazy.html
    # Switch between windows
    Key([mod], "g", lazy.function(show_graphs)),
    Key([mod], "p", lazy.spawn("osm-power")),
    Key([mod], "a", lazy.spawn("osm-launcher")),
    Key([mod], "l", lazy.spawn("osm-lockd")),
    Key([mod], "n", lazy.spawn("osm-rocker")),
    Key([mod], "Return", lazy.spawn(terminal), desc="Launch terminal"),
    Key([mod], "Tab", lazy.next_layout(), desc="Toggle between layouts"),
    Key([mod], "w", lazy.window.kill(), desc="Kill focused window"),
    Key([mod], "f", lazy.window.toggle_fullscreen(), desc="Toggle fullscreen on the focused window",),
    Key([mod], "t", lazy.window.toggle_floating(), desc="Toggle floating on the focused window"),
    Key([mod, "control"], "r", lazy.reload_config(), desc="Reload the config"),
    Key([mod, "control"], "q", lazy.shutdown(), desc="Shutdown Qtile"),
    Key([mod], "r", lazy.spawncmd(), desc="Spawn a command using a prompt widget"),

    Key([], "XF86AudioRaiseVolume", lazy.spawn("amixer -q set Master 5%+")),
    Key([], "XF86AudioLowerVolume", lazy.spawn("amixer -q set Master 5%-")),
    Key([], "XF86AudioMute", lazy.spawn("amixer -q set Master toggle")),
]

# Add key bindings to switch VTs in Wayland.
# We can't check qtile.core.name in default config as it is loaded before qtile is started
# We therefore defer the check until the key binding is run by using .when(func=...)
for vt in range(1, 8):
    keys.append(
        Key(
            ["control", "mod1"],
            f"f{vt}",
            lazy.core.change_vt(vt).when(func=lambda: qtile.core.name == "wayland"),
            desc=f"Switch to VT{vt}",
        )
    )

groups = [Group(i) for i in "123"]

for i in groups:
    keys.extend(
        [
            # mod + group number = switch to group
            Key(
                [mod],
                i.name,
                lazy.group[i.name].toscreen(),
                desc=f"Switch to group {i.name}",
            ),
            # mod + shift + group number = switch to & move focused window to group
            Key(
                [mod, "shift"],
                i.name,
                lazy.window.togroup(i.name, switch_group=True),
                desc=f"Switch to & move focused window to group {i.name}",
            ),
        ]
    )


layouts = [
    # layout.Columns(border_focus_stack=["#d75f5f", "#8f3d3d"], border_width=8),
    layout.Max(border_width=0, margin=6),
    #layout.RatioTile(margin=6),
    layout.MonadWide(border_width=0, margin=6),
    layout.MonadTall(border_width=0, margin=6),
]

widget_defaults = dict(
    font="Ubuntu",
    fontsize=15,
    padding=5,
)
extension_defaults = widget_defaults.copy()

##################
## Screen Setup ##
##################

close_button = widget.TextBox(
    text="",  # start empty
    fontsize=25,
    mouse_callbacks={'Button1': lazy.window.kill()},
)

float_button = widget.TextBox(
    text="",
    fontsize=35,
    mouse_callbacks={'Button1': lazy.window.toggle_floating()},
)

# --- Floating indicator glyphs (edit to taste) ---
FLOAT_OFF = "⎘"  # focused window is tiled
FLOAT_ON  = "⎗"  # focused window is floating

def _update_topbar_window_buttons(qtile_obj=None):
    """
    Updates close_button and float_button based on current group + focused window state.
    This updates the same TextBox objects placed in the bar widgets list.
    """
    q = qtile_obj or qtile
    g = getattr(q, "current_group", None)

    # No group or no windows => hide both buttons
    if not g or not getattr(g, "windows", None):
        close_button.update("")
        float_button.update("")
        return

    # At least one window exists in this group => show close button
    close_button.update("❌")

    # Show floating state for the currently focused window
    w = getattr(q, "current_window", None)
    if w and getattr(w, "floating", False):
        float_button.update(FLOAT_ON)
    else:
        float_button.update(FLOAT_OFF)

@hook.subscribe.client_managed
def _on_client_managed(window):
    _update_topbar_window_buttons(window.qtile)

@hook.subscribe.client_killed
def _on_client_killed(window):
    _update_topbar_window_buttons(window.qtile)

@hook.subscribe.client_focus
def _on_client_focus(window):
    _update_topbar_window_buttons(window.qtile)

@hook.subscribe.float_change
def _on_float_change():
    _update_topbar_window_buttons()

@hook.subscribe.setgroup
def _on_setgroup():
    _update_topbar_window_buttons()


screens = [
    # Main Screen:
    Screen(
        top=bar.Bar([
            #widget.Spacer(length=15),
            # widget.Image(filename='~/.config/qtile/images/terminal.png', margin=2.5, mouse_callbacks={'Button1': lazy.spawn(terminal)}),
            widget.GroupBox(highlight_method='border', block_highlight_text_color='ffffff'),
            widget.Systray(),
            widget.Spacer(length=55),
            # widget.Prompt(),
            # widget.WindowName(),
            widget.Spacer(length=bar.STRETCH),
            # widget.Clock(format="%H:%M", mouse_callbacks = {'Button1': lazy.spawn("osm-calendar")}),
            widget.Clock(format="%H:%M"),
            widget.Spacer(length=bar.STRETCH),
            widget.Battery(format='{percent:2.0%}'),
            widget.BatteryIcon(scale=1.5),
            widget.Spacer(length=15),
            widget.Volume(emoji=True),
            widget.CurrentLayout(mode='icon', scale=0.6),
            widget.Spacer(length=15),
            float_button,
            widget.Spacer(length=15),
            close_button,
            #widget.Spacer(length=15),
            # widget.QuickExit(padding=8),
        ], size=35,
            # N,E,S,W compass directions for margin:
            margin=[0, 0, 0, 0], background=bg,
        ),
        bottom=bar.Bar(
            [
                #widget.Spacer(length=55),
                widget.TaskList(),
                #widget.Spacer(length=bar.STRETCH),
                widget.Image(filename='~/.config/qtile/images/apps.png', margin=6,
                             mouse_callbacks={'Button1': lazy.spawn("osm-launcher")}),
                widget.Prompt(),
                widget.Spacer(length=bar.STRETCH),
                # COMMENT OUT THE NEXT 3 LINES TO REMOVE THE KEYBOARD BUTTON:
                widget.Image(filename='~/.config/qtile/images/keyboard.png', margin=6,
                             mouse_callbacks={'Button1': lazy.spawn("onboard")}),
                widget.Spacer(length=55),
            ], size=35, margin=[0, 0, 0, 0], background=bg,
        ),
        # x11_drag_polling_rate = 60,
    ),
]

# Drag floating layouts.
mouse = [
    Drag([mod], "Button1", lazy.window.set_position_floating(), start=lazy.window.get_position()),
    Drag([mod], "Button3", lazy.window.set_size_floating(), start=lazy.window.get_size()),
    Click([mod], "Button2", lazy.window.bring_to_front()),
]

dgroups_key_binder = None
dgroups_app_rules = []  # type: list
follow_mouse_focus = True
bring_front_click = False
floats_kept_above = True
cursor_warp = False
floating_layout = layout.Floating(
    float_rules=[
        # Run the utility of `xprop` to see the wm class and name of an X client.
        *layout.Floating.default_float_rules,
        Match(wm_class="confirmreset"),  # gitk
        Match(wm_class="makebranch"),  # gitk
        Match(wm_class="maketag"),  # gitk
        Match(wm_class="ssh-askpass"),  # ssh-askpass
        Match(title="branchdialog"),  # gitk
        Match(title="pinentry"),  # GPG key password entry
    ]
)
auto_fullscreen = True
focus_on_window_activation = "smart"
reconfigure_screens = True

# If things like steam games want to auto-minimize themselves when losing
# focus, should we respect this or not?
auto_minimize = True

# When using the Wayland backend, this can be used to configure input devices.
wl_input_rules = None

# xcursor theme (string or None) and size (integer) for Wayland backend
wl_xcursor_theme = None
wl_xcursor_size = 24

wmname = "LG3D"
