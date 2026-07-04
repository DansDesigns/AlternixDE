#!/bin/bash
set -e
clear

echo "=============================================="
echo "       Alternix Desktop Installer"
echo "          (Devuan Edition)"
echo "=============================================="
echo "--------> Teaching Penguins to fly! <--------"
echo " "
echo "--------------------------------------------------------------------"
echo " Setup can take a while, be sure to have a cuppa & some good music!"
echo "--------------------------------------------------------------------"
echo ""
echo ""
echo "NOTE: You will be asked for input several times."
echo ""
echo ""
echo "-------------------------------------------"
echo "              User Setup"
echo "-------------------------------------------"
echo ""

# Root of the Alternix repo
ALT_ROOT="$HOME/Alternix"

if [ ! -d "$ALT_ROOT" ]; then
    echo "ERROR: $ALT_ROOT not found. Please place install.sh inside ~/Alternix."
    exit 1
fi

# Ask for username
while true; do
    read -rp "Enter the username for Alternix: " TARGET_USER

    # Basic validation
    if [[ "$TARGET_USER" =~ ^[a-z_][a-z0-9_-]*$ ]]; then
        break
    else
        echo "Invalid username. Use only lowercase letters, digits, hyphens, and underscores."
    fi
done

# Check if user exists
if id "$TARGET_USER" >/dev/null 2>&1; then
    echo "User '$TARGET_USER' already exists. Skipping creation."
else
    echo "Creating user '$TARGET_USER'..."
    sudo useradd -m -s /bin/bash "$TARGET_USER"
    echo "User '$TARGET_USER' created."
fi

# Add to sudo (optional)
if ! groups "$TARGET_USER" | grep -q "\bsudo\b"; then
    echo "Adding '$TARGET_USER' to sudo group..."
    sudo usermod -aG sudo "$TARGET_USER"
fi

echo ""
echo "User setup complete. Username set to: $TARGET_USER"
echo ""




# ────────────────────────────────────────────────
# 1. Install apps & dependencies
# ────────────────────────────────────────────────
echo "[1/10] Installing system dependencies..."

# NOTE: nala's volian repo targets Debian codenames. On Devuan, the codename
# (e.g. "daedalus") maps to Debian bookworm, so the repo entry should still
# work — but if nala install fails, the script falls back to plain apt.

echo "- Adding nala dependencies.."
echo "deb http://deb.volian.org/volian/ nala main" | sudo tee /etc/apt/sources.list.d/volian.list
wget -qO - https://deb.volian.org/volian/volian.gpg | sudo tee /etc/apt/trusted.gpg.d/volian.gpg

echo "[System] Installing nala.."
if ! sudo apt install nala -y 2>/dev/null; then
    echo "[System] nala not available for this Devuan release — using apt instead."
    # Define nala as an alias to apt for the rest of this script
    nala() { sudo apt "$@"; }
    export -f nala
fi

echo "[System] Removing nala Install Components.."
sudo rm -f /etc/apt/sources.list.d/volian.list
sudo rm -f /etc/apt/trusted.gpg.d/volian.gpg

#echo "[System] Running nala server fetch.."
#echo " "
#echo "----------------------------------------"
#echo " PLEASE ENTER 1, 2, 3, 4, WHEN PROMPTED"
#echo "----------------------------------------"
#echo " "
# nala fetch is nala-specific; skip if using plain apt
#if command -v nala >/dev/null 2>&1; then
#    sudo nala fetch
#fi

echo "[System] Running nala update.."
sudo nala update

echo "[System] Installing XLibre.."
sudo nala install -y ca-certificates curl

sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://xlibre-deb.github.io/key.asc | sudo tee /etc/apt/keyrings/xlibre-deb.asc
sudo chmod a+r /etc/apt/keyrings/xlibre-deb.asc

# Use the dedicated Devuan repo — Devuan codenames used directly
DEVUAN_CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME")
ARCH=$(dpkg --print-architecture)
echo "[System] Installing XLibre for Devuan '$DEVUAN_CODENAME'..."
sudo tee /etc/apt/sources.list.d/xlibre-deb.sources > /dev/null << XLIBRESRC
Types: deb deb-src
URIs: https://xlibre-deb.github.io/devuan/
Suites: ${DEVUAN_CODENAME}
Components: main
Architectures: ${ARCH}
Signed-By: /etc/apt/keyrings/xlibre-deb.asc
XLIBRESRC

sudo nala update
sudo nala install xlibre -y

echo "[System] Installing Required Components.."
sudo nala install -y \
    fastfetch qtbase5-dev qt5-qmake qtdeclarative5-dev xdg-utils \
    fonts-noto-color-emoji libxcomposite-dev libxrender-dev libxfixes-dev \
    xwallpaper pkg-config libpoppler-qt5-dev htop python3-pip python3-lxml \
    python3-venv picom qtile redshift onboard samba xdotool alacritty sqlite3 fuse \
    synaptic brightnessctl pavucontrol pulseaudio alsa-utils mpg123 flatpak libevdev-dev \
    elogind libpam-elogind xserver-xlibre-input-libinput \
    xprintidle libx11-dev libxtst-dev ntfs-3g aria2 \
    kalk vlc qt5-style-kvantum thermald network-manager aptitude timeshift \
    python3-yaml python3-dateutil python3-pyqt5 python3-packaging python3-requests \
    podman podman-compose gvfs gvfs-backends gvfs-fuse gvfs-daemons fuse3 dbus-x11 \
    sysvinit-utils pm-utils ntfs-3g exfatprogs exfat-fuse udisks2 pmount

echo "NOTE: snapd is NOT available on Devuan (it depends on systemd)."
echo "If snap packages are needed, use flatpak equivalents instead."

# ────────────────────────────────────────────────
# Mobile Telephony Components (optional)
# ────────────────────────────────────────────────
echo ""
echo "-------------------------------------------"
echo "   Install Mobile Telephony Components?"
echo "-------------------------------------------"
echo " plasma-dialer & spacebar (KDE phone apps)"
echo ""
echo "  1) Install telephony components"
echo "  2) Skip"
echo ""

while true; do
    read -rp "Enter choice [1/2]: " TELEPHONY_CHOICE
    if [[ "$TELEPHONY_CHOICE" == "1" ]]; then
        echo "[System] Installing Mobile Telephony Components.."
        sudo nala install -y --no-install-recommends plasma-dialer spacebar
        echo "• Telephony components installed."
        break
    elif [[ "$TELEPHONY_CHOICE" == "2" ]]; then
        echo "• Skipping telephony components."
        break
    else
        echo "Invalid choice. Please enter 1 or 2."
    fi
done


# ────────────────────────────────────────────────
#  Install Flatpaks
# ────────────────────────────────────────────────

echo "[System] Clearing space in /var/cache/apt/archive..."
sudo rm -rf /var/cache/apt/archives

echo "[System] Adding flatpak Components.."
sudo flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo

echo "[System] Installing Flatpaks..."

flatpak install -y flathub com.github.joseexposito.touche
flatpak install -y flathub chat.delta.desktop
flatpak install -y flathub org.kde.kweather
flatpak install -y flathub org.kde.qrca
flatpak install -y flathub com.freerdp.FreeRDP
flatpak install -y flathub com.github.tchx84.Flatseal
flatpak install -y flathub net.retrodeck.retrodeck

echo "[System] Installing Bauh Application Manager.."
sudo pip3 install bauh --break-system-packages


# ────────────────────────────────────────────────
# Install all .deb packages in ~/Alternix/installers/
# ────────────────────────────────────────────────
echo " "
echo "[System] Installing local .deb packages..."

INSTALLER_DIR="$ALT_ROOT/Alternix/installers"

if [ -d "$INSTALLER_DIR" ]; then
    DEB_COUNT=$(ls -1 "$INSTALLER_DIR"/*.deb 2>/dev/null | wc -l)

    if [ "$DEB_COUNT" -gt 0 ]; then
        echo "• Found $DEB_COUNT installer package(s). Installing..."
        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true
        sudo nala install -f -y
        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true
        echo "• Local installer packages installed."
    else
        echo "• No .deb files in installers folder, skipping."
    fi
else
    echo "• No installers folder found, skipping."
fi



# ────────────────────────────────────────────────
#            Curl Commands
# ────────────────────────────────────────────────

cd "$ALT_ROOT/Alternix"

echo "[System] Installing Starship prompt..."
curl -sS https://starship.rs/install.sh | sh -s -- --yes

if ! grep -Fxq 'eval "$(starship init bash)"' "$HOME/.bashrc"; then
    echo 'eval "$(starship init bash)"' >> "$HOME/.bashrc"
fi

echo "[System] Installing Brave Browser..."
curl -fsS https://dl.brave.com/install.sh | sh -s -- --yes



# ────────────────────────────────────────────────
# Copy ALL configs from ~/Alternix/configs → ~/.config
# ────────────────────────────────────────────────
echo " "
echo "[Config] Installing user configs..."

CONFIG_SRC="$ALT_ROOT/Alternix/configs"
CONFIG_DST="$HOME/.config"

sudo cp "$ALT_ROOT/Alternix/configs/.alacritty.toml" ~/

mkdir -p "$CONFIG_DST"

if [ -d "$CONFIG_SRC" ]; then
    echo "• Copying configs from $CONFIG_SRC → $CONFIG_DST"
    cp -r "$CONFIG_SRC/"* "$CONFIG_DST/"
    echo "• Configs installed."
else
    echo "• No configs folder found, skipping."
fi

sudo cp -r "$ALT_ROOT/Alternix/onboard" /usr/share/onboard



# ────────────────────────────────────────────────
# Install Fonts
# ────────────────────────────────────────────────
echo " "
echo "[Config] Installing Fonts..."

if [ -d "$ALT_ROOT/Alternix/fonts" ]; then
    sudo mkdir -p /usr/local/share/fonts/alternix
    sudo cp -r "$ALT_ROOT/Alternix/fonts/"* /usr/local/share/fonts/alternix/
    sudo fc-cache -f
    echo "• Fonts installed successfully."
else
    echo "• No fonts folder found, skipping."
fi

# ────────────────────────────────────────────────
# Install Wallpapers
# ────────────────────────────────────────────────
echo " "
echo "[Config] Installing Wallpapers..."

WALL_DST="$HOME/Pictures/wallpapers"
mkdir -p "$WALL_DST"

if [ -d "$ALT_ROOT/Alternix/wallpapers" ]; then
    cp -r "$ALT_ROOT/Alternix/wallpapers/"* "$WALL_DST/"
    echo "• Wallpapers installed to $WALL_DST"
else
    echo "• No wallpapers folder found, skipping."
fi

# ────────────────────────────────────────────────
# 2. Create & activate Qtile venv
# ────────────────────────────────────────────────
echo " "
echo "[2/10] Creating virtual environment (qtile_venv)..."

if [ ! -d "$HOME/.qtile_venv" ]; then
    python3 -m venv "$HOME/.qtile_venv"
fi

source "$HOME/.qtile_venv/bin/activate"

echo "[3/10] Installing Python pip dependencies..."
pip3 install qtile qtile-extras mypy

# Symlink qtile binary to where udev rules expect it
sudo mkdir -p /usr/lib/udev
sudo ln -sf "$HOME/.qtile_venv/bin/qtile" /usr/lib/udev/qtile
sudo udevadm control --reload-rules

# ────────────────────────────────────────────────
# 3. Configure xinitrc autostart
# ────────────────────────────────────────────────
echo " "
echo "[4/10] Creating .xinitrc autostart..."
cat <<EOF > "$HOME/.xinitrc"
#!/bin/sh
source "\$HOME/.qtile_venv/bin/activate"
exec dbus-launch --exit-with-session "\$HOME/.qtile_venv/bin/qtile" start
EOF
chmod +x "$HOME/.xinitrc"


# ────────────────────────────────────────────────
# 4. Bash profile auto startx
# ────────────────────────────────────────────────
echo " "
echo "[6/10] Creating ~/.bash_profile auto-start..."
cat <<EOF > "$HOME/.bash_profile"
# auto-start X only on tty1 and only if not already running
if [ -z "\$DISPLAY" ] && [ "\$(tty)" = "/dev/tty1" ]; then
    startx
fi
EOF

# ────────────────────────────────────────────────
# 5. Build Alternix Apps
# ────────────────────────────────────────────────
echo " "
echo "[7/10] Building Alternix apps..."
cd "$ALT_ROOT/Alternix" || { echo "ERROR: $ALT_ROOT not found"; exit 1; }

echo "• Building osm-launcher..."
g++ -O3 -fPIC apps/osm-launcher.cpp -o osm-launcher $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-launcher && sudo mv osm-launcher /usr/local/bin/

echo "• Building osm-lock..."
g++ apps/osm-lock.cpp -o osm-lock -fPIC $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-lock && sudo mv osm-lock /usr/local/bin/

echo "• Building osm-running..."
g++ apps/osm-running.cpp -o osm-running -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-running && sudo mv osm-running /usr/local/bin/

echo "• Building osm-notify..."
g++ -fPIC apps/osm-notify.cpp -o osm-notify $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core Qt5DBus) -lX11 -lXtst
chmod +x osm-notify && sudo mv osm-notify /usr/local/bin/

echo "• Building osm-status..."
g++ -fPIC apps/osm-status.cpp -o osm-status -ldl $(pkg-config --cflags --libs Qt5Widgets Qt5DBus) -lX11
chmod +x osm-status && sudo mv osm-status /usr/local/bin/

echo "• Installing sounds..."
# Drop notify.* and alarm.* (wav/ogg/flac/mp3) in ~/.config/Alternix/sounds/
#mkdir -p "$HOME/.config/Alternix/sounds"
cp -r "$ALT_ROOT/Alternix/sounds" ~/.config/Alternix/


echo "• Building osm-clock..."
g++ apps/osm-clock.cpp -o osm-clock -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-clock && sudo mv osm-clock /usr/local/bin/

cat <<EOF > "$HOME/.local/share/applications/osm-clock.desktop"
[Desktop Entry]
Type=Application
Name=Clock
Comment=Clock, Alarms, Timer & Stopwatch for Alternix / OSM-Phone
Exec=/usr/local/bin/osm-clock
Icon=osm-clock
Terminal=false
Categories=Utility;Clock;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-clock.desktop"

echo "• Building osm-paper..."
g++ -fPIC apps/osm-paper.cpp -o osm-paper $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-paper && sudo mv osm-paper /usr/local/bin/

if [ -f "icons/osm-paper.png" ]; then
    sudo cp icons/osm-paper.png /usr/share/icons/hicolor/64x64/apps/osm-paper.png
fi

mkdir -p "$HOME/.local/share/applications"
cat <<EOF > "$HOME/.local/share/applications/osm-paper.desktop"
[Desktop Entry]
Type=Application
Name=Wallpapers
Comment=Picture Manager for Alternix / OSM-Phone
Exec=/usr/local/bin/osm-paper
Icon=osm-paper
Terminal=false
Categories=Utility;FileManager;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-paper.desktop"

echo "• Installing osm-paper-restore..."
chmod +x apps/osm-paper-restore && sudo cp apps/osm-paper-restore /usr/local/bin/

echo "• Building osm-styling..."
g++ -fPIC apps/osm-styling.cpp -o osm-styling $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-styling && sudo mv osm-styling /usr/local/bin/

echo "• Building osm-files..."
g++ -fPIC apps/osm-files.cpp -o osm-files $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-files && sudo mv osm-files /usr/local/bin/

if [ -f "icons/osm-files.png" ]; then
    sudo cp icons/osm-files.png /usr/share/icons/hicolor/64x64/apps/osm-files.png
fi

cat <<EOF > "$HOME/.local/share/applications/osm-files.desktop"
[Desktop Entry]
Type=Application
Name=Files
Comment=File Manager for Alternix / OSM-Phone
Exec=/usr/local/bin/osm-files
Icon=osm-files
Terminal=false
Categories=Utility;FileManager;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-files.desktop"

echo "• Building osm-viewer..."
g++ -fPIC apps/osm-viewer.cpp -o osm-viewer $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core poppler-qt5) -Wno-deprecated-declarations
chmod +x osm-viewer && sudo mv osm-viewer /usr/local/bin/

if [ -f "icons/osm-viewer.png" ]; then
    sudo cp icons/osm-viewer.png /usr/share/icons/hicolor/64x64/apps/osm-viewer.png
fi

cat <<EOF > "$HOME/.local/share/applications/osm-viewer.desktop"
[Desktop Entry]
Type=Application
Name=File Editor/Viewer
Comment=File Viewer for Alternix / OSM-Phone
Exec=/usr/local/bin/osm-viewer %U
Icon=osm-viewer
Terminal=false
MimeType=application/octet-stream;application/pdf;text/plain;image/*;inode/directory;
Categories=Utility;FileManager;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-viewer.desktop"

echo "• Building osm-draw..."
g++ -fPIC apps/osm-draw.cpp -o osm-draw -std=c++17 $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-draw && sudo mv osm-draw /usr/local/bin/

if [ -f "icons/osm-draw.png" ]; then
    sudo cp icons/osm-draw.png /usr/share/icons/hicolor/64x64/apps/osm-draw.png
fi

cat <<EOF > "$HOME/.local/share/applications/osm-draw.desktop"
[Desktop Entry]
Type=Application
Name=Draw
Comment=Drawing App for Alternix / OSM-Phone
Exec=/usr/local/bin/osm-draw %U
Icon=osm-draw
Terminal=false
Categories=Utility;Drawing;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-draw.desktop"

echo "• Building osm-rocker..."
g++ -fPIC apps/osm-rocker.cpp -o osm-rocker $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-rocker && sudo mv osm-rocker /usr/local/bin/

echo "• Installing osm-sudo..."
sudo cp apps/osm-sudo /usr/local/bin/osm-sudo
sudo chmod 755 /usr/local/bin/osm-sudo

echo "• Building osm-power..."
g++ -fPIC apps/osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/

echo "• Compiling osm-powerd..."
sudo g++ -O2 apps/osm-powerd.cpp -o osm-powerd
sudo chmod +x osm-powerd && sudo mv osm-powerd /usr/local/bin/
sudo chown root:root /usr/local/bin/osm-powerd
sudo chmod 4755 /usr/local/bin/osm-powerd

echo "• Building osm-lockscreen..."
g++ -fPIC apps/osm-lockscreen.cpp -o osm-lockscreen $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-lockscreen && sudo mv osm-lockscreen /usr/local/bin/



# ────────────────────────────────────────────────
# 6. Build OSM Settings + modules
# ────────────────────────────────────────────────
echo " "
echo "[8/10] Building osm-settings..."

cd "$ALT_ROOT/Alternix/apps/osm-settings" || { echo "ERROR: apps/osm-settings folder missing"; exit 1; }

g++ osm-settings.cpp -o osm-settings -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-settings && sudo mv osm-settings /usr/local/bin/

if [ -f "$ALT_ROOT/Alternix/icons/osm-settings.png" ]; then
    sudo cp "$ALT_ROOT/Alternix/icons/osm-settings.png" /usr/share/icons/hicolor/64x64/apps/osm-settings.png
fi

cat <<EOF > "$HOME/.local/share/applications/osm-settings.desktop"
[Desktop Entry]
Type=Application
Name=Settings
Comment=Settings App for Alternix / OSM-Phone
Exec=/usr/local/bin/osm-settings
Icon=osm-settings
Terminal=false
Categories=Utility;
StartupNotify=false
EOF
chmod +x "$HOME/.local/share/applications/osm-settings.desktop"

echo "• Building wifi.so..."
g++ -fPIC -shared wifi.cpp -o wifi.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv wifi.so /usr/local/bin/

echo "• Building bluetooth.so..."
g++ -fPIC -shared bluetooth.cpp -o bluetooth.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv bluetooth.so /usr/local/bin/

echo "• Building apps.so..."
g++ -fPIC -shared apps.cpp -o apps.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv apps.so /usr/local/bin/

echo "• Building mobile.so..."
g++ -fPIC -shared mobile.cpp -o mobile.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv mobile.so /usr/local/bin/

echo "• Building location.so..."
g++ location.cpp -o location.so -shared -fPIC -O2 $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv location.so /usr/local/bin/

echo "• Building battery.so..."
g++ -fPIC -shared battery.cpp -o battery.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv battery.so /usr/local/bin/

echo "• Building emulation.so..."
g++ emulation.cpp -o emulation.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv emulation.so /usr/local/bin/

echo "• Building security.so..."
g++ -shared -fPIC security.cpp -o security.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo mv security.so /usr/local/bin/

echo "• Building ethernet.so..."
g++ -fPIC -shared ethernet.cpp -o ethernet.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv ethernet.so /usr/local/bin/

echo "• Building display.so..."
g++ display.cpp -o display.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv display.so /usr/local/bin/

echo "• Building sound.so..."
g++ -std=c++17 -fPIC -shared sound.cpp -o sound.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo mv sound.so /usr/local/bin/

echo "• Building kernel.so..."
g++ -std=c++17 -fPIC -shared kernel.cpp -o kernel.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo mv kernel.so /usr/local/bin/

echo "• Building storage.so..."
g++ -fPIC -shared storage.cpp -o storage.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv storage.so /usr/local/bin/

echo "• Building accounts.so..."
g++ -std=c++11 accounts.cpp -o accounts.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv accounts.so /usr/local/bin/

echo "• Building system.so..."
g++ -fPIC -shared system.cpp -o system.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv system.so /usr/local/bin/

echo "• Building ui.so..."
g++ -fPIC -shared ui.cpp -o ui.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv ui.so /usr/local/bin/


# ────────────────────────────────────────────────
#            Custom App Shortcuts & Icons
# ────────────────────────────────────────────────
echo " "
echo "• Installing App Icons..."

cd "$ALT_ROOT/Alternix"

sudo cp icons/update.png /usr/share/icons/hicolor/64x64/apps/update.png
sudo cp icons/upgrade.png /usr/share/icons/hicolor/64x64/apps/upgrade.png
sudo cp icons/bauh.png /usr/share/icons/hicolor/64x64/apps/bauh.png
sudo cp icons/os-check-update.png /usr/share/icons/hicolor/64x64/apps/os-check-update.png

echo "• Creating htop.desktop launcher..."
sudo tee /usr/share/applications/htop.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=htop
Comment=System monitor
Exec=alacritty -e htop
Terminal=false
Icon=htop
Categories=System;
EOF

echo "• Creating bauh Shortcut..."
sudo tee /usr/share/applications/bauh.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=Apps (bauh)
Comment=Application Manager
Exec=bauh
Icon=bauh
Categories=System;
EOF

echo "• Installing Alternix Updater..."
sudo chmod +x "$HOME/Alternix/update/os-check-update"
sudo cp "$HOME/Alternix/update/os-check-update" /usr/bin/
sudo mkdir -p /usr/share/alternix/
sudo cp "$HOME/Alternix/update/version.txt" /usr/share/alternix/

sudo tee /usr/share/applications/os-check-update.desktop >/dev/null <<EOF
[Desktop Entry]
Name=System Update
Exec=alacritty -e /usr/bin/os-check-update
Icon=os-check-update
Type=Application
Terminal=true
Categories=System;
EOF



# ────────────────────────────────────────────────
# Create Brave YouTube WebApp launcher
# ────────────────────────────────────────────────
echo " "
echo "[System] Installing Brave WebApps..."

APP_DIR="$HOME/.local/share/applications"
mkdir -p "$APP_DIR"

YOUTUBE_ICON="/usr/share/icons/hicolor/256x256/apps/youtube.png"
if [ -f "$ALT_ROOT/Alternix/icons/youtube.png" ]; then
    sudo cp "$ALT_ROOT/Alternix/icons/youtube.png" "$YOUTUBE_ICON"
fi

cat <<EOF > "$APP_DIR/youtube-webapp.desktop"
#!/usr/bin/env xdg-open
[Desktop Entry]
Version=1.0
Type=Application
Name=Youtube
Comment=Youtube WebApp (Brave)
Exec=brave-browser --app=https://www.youtube.com/ --new-window --disable-frame --force-device-scale-factor=1.3
Icon=youtube
Terminal=false
Categories=Network;WebBrowser;Utility;
StartupNotify=true
EOF
chmod +x "$APP_DIR/youtube-webapp.desktop"
echo "• YouTube WebApp installed."

echo "[Apps] Installing AlterniTech Forum WebApp..."
if [ -f "icons/alternitech-forum.png" ]; then
    sudo cp icons/alternitech-forum.png /usr/share/icons/hicolor/64x64/apps/alternitech-forum.png
fi

cat <<EOF > "$HOME/.local/share/applications/alternitech-forums.desktop"
[Desktop Entry]
Version=1.0
Type=Application
Name=Alternitech Forums
Comment=Alternitech Community Forum WebApp (Brave)
Exec=brave-browser --app=https://alternitech.freeforums.net/ --new-window --force-device-scale-factor=1.3
Icon=alternitech-forum
Terminal=false
Categories=Network;WebBrowser;Utility;
StartupNotify=true
EOF
chmod +x "$HOME/.local/share/applications/alternitech-forums.desktop"
echo "• Alternitech Forums WebApp installed."

echo "[Apps] Installing OpenStreetMap WebApp..."
if [ -f "icons/openmap.png" ]; then
    sudo cp icons/openmap.png /usr/share/icons/hicolor/64x64/apps/openmap.png
fi

cat <<EOF > "$HOME/.local/share/applications/openmap.desktop"
[Desktop Entry]
Version=1.0
Type=Application
Name=OpenStreetMap
Comment=OpenStreetMap WebApp (Brave)
Exec=brave-browser --app=https://openstreetmap.org/ --new-window --force-device-scale-factor=1.3
Icon=openmap
Terminal=false
Categories=Network;WebBrowser;Utility;
StartupNotify=true
EOF
chmod +x "$HOME/.local/share/applications/openmap.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications" || true
fi
echo "• OpenStreetMap WebApp installed."


# ────────────────────────────────────────────────
# 7. Samba Network Updates
# ────────────────────────────────────────────────
echo " "
echo "• Updating /etc/samba/smb.conf..."
sudo sed -i '/workgroup = WORKGROUP/a client min protocol = NT1\nserver min protocol = NT1' /etc/samba/smb.conf

echo "• Restarting Samba service..."
# SysVinit: use service(8) instead of systemctl
sudo service smbd restart || true
sudo service nmbd restart || true


# ────────────────────────────────────────────────
# OSM-Lockscreen Security Layer
# ────────────────────────────────────────────────
echo " "
echo "[X] Setting up secure lockscreen supervisor..."

# 1. Create dedicated lockscreen user (no login, no shell)
if ! id "lockscreen" >/dev/null 2>&1; then
    sudo adduser --disabled-password --gecos "" --shell /usr/sbin/nologin lockscreen
fi

# 2. Install supervisor daemon (osm-lockd)
sudo tee /usr/local/bin/osm-lockd >/dev/null <<'LOCKD'
#!/bin/bash

FLAG="/tmp/osm_unlock_success"

while true; do
    rm -f "$FLAG"
    /usr/local/bin/osm-lockscreen

    if [ -f "$FLAG" ]; then
        rm -f "$FLAG"
        # signal the user session (osm-status plays the boot sound once per boot)
        touch /tmp/osm_boot_unlocked 2>/dev/null
        chmod 666 /tmp/osm_boot_unlocked 2>/dev/null
        exit 0
    fi

    sleep 0.05
done
LOCKD
sudo chmod +x /usr/local/bin/osm-lockd

# 3. Create a SysVinit init script for osm-lockscreen
# ── This replaces the systemd .service unit ───────────────────────────────
sudo tee /etc/init.d/osm-lockscreen >/dev/null <<'INITSCRIPT'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          osm-lockscreen
# Required-Start:    $remote_fs $syslog $local_fs
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: OSM-Phone Lockscreen Supervisor
# Description:       Runs osm-lockd as the lockscreen user, restarting on crash.
### END INIT INFO

NAME="osm-lockscreen"
DAEMON="/usr/local/bin/osm-lockd"
DAEMON_USER="lockscreen"
PIDFILE="/var/run/$NAME.pid"

. /lib/lsb/init-functions

case "$1" in
  start)
    log_daemon_msg "Starting $NAME"
    start-stop-daemon --start --quiet --background \
        --make-pidfile --pidfile "$PIDFILE" \
        --chuid "$DAEMON_USER" \
        --exec "$DAEMON"
    log_end_msg $?
    ;;
  stop)
    log_daemon_msg "Stopping $NAME"
    start-stop-daemon --stop --quiet --pidfile "$PIDFILE"
    rm -f "$PIDFILE"
    log_end_msg $?
    ;;
  restart|force-reload)
    $0 stop
    sleep 1
    $0 start
    ;;
  status)
    status_of_proc -p "$PIDFILE" "$DAEMON" "$NAME" && exit 0 || exit $?
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|force-reload|status}"
    exit 1
    ;;
esac
INITSCRIPT

sudo chmod +x /etc/init.d/osm-lockscreen

# Enable at runlevels 2-5, disable at 0/1/6
sudo update-rc.d osm-lockscreen defaults
sudo service osm-lockscreen start || true

echo "[✓] Lockscreen security layer installed."


# ────────────────────────────────────────────────
# logind / elogind power key handling
# Devuan ships elogind as a drop-in for systemd-logind.
# Its config lives in /etc/elogind/logind.conf (same format).
# ────────────────────────────────────────────────
echo " "
echo "[X] Configuring elogind power key handling..."

sudo mkdir -p /etc/elogind/logind.conf.d

sudo tee /etc/elogind/logind.conf.d/alternix-power.conf >/dev/null <<'EOF'
[Login]
HandlePowerKey=ignore
PowerKeyIgnoreInhibition=no
EOF

# Also update the main logind.conf as a fallback for older elogind versions
# that don't support drop-in dirs
sudo sed -i 's/^#*HandlePowerKey=.*/HandlePowerKey=ignore/' /etc/elogind/logind.conf 2>/dev/null || true

sudo service elogind restart || true

echo "[✓] elogind configured."


# ────────────────────────────────────────────────
# 8. Autologin via agetty override
# Devuan uses /etc/inittab (SysVinit) rather than systemd drop-ins.
# We replace the tty1 line to pass --autologin.
# ────────────────────────────────────────────────
echo " "
echo "[5/10] Enabling autologin on tty1 for user $TARGET_USER..."

INITTAB="/etc/inittab"

if [ -f "$INITTAB" ]; then
    # Back up inittab before editing
    sudo cp "$INITTAB" "${INITTAB}.bak"

    # Replace the tty1 getty line with an autologin version.
    # Typical Devuan line:  1:2345:respawn:/sbin/getty 38400 tty1
    # We change it to use agetty with --autologin.
    if grep -q "^1:.*getty.*tty1" "$INITTAB"; then
        sudo sed -i "s|^1:.*getty.*tty1.*|1:2345:respawn:/sbin/agetty --autologin $TARGET_USER --noclear tty1 linux|" "$INITTAB"
        echo "• inittab tty1 line updated for autologin."
    else
        # Line not found in expected format — append a safe fallback
        echo "1:2345:respawn:/sbin/agetty --autologin $TARGET_USER --noclear tty1 linux" | sudo tee -a "$INITTAB" >/dev/null
        echo "• Autologin line appended to inittab."
    fi

    # Signal init to re-read inittab (SysVinit equivalent of daemon-reload)
    sudo kill -HUP 1 || true
else
    echo "WARNING: /etc/inittab not found."
    echo "If using OpenRC or runit, configure autologin in your getty service config."
    echo "For OpenRC: edit /etc/conf.d/agetty.tty1 and set agetty_options."
fi

echo "Setting NOPASSWD for $TARGET_USER..."
echo "$TARGET_USER ALL=(ALL) NOPASSWD: ALL" | sudo tee /etc/sudoers.d/alternix-nopasswd >/dev/null
sudo chmod 440 /etc/sudoers.d/alternix-nopasswd

sudo usermod -aG video,input "$TARGET_USER"

# ────────────────────────────────────────────────
# 9. Cleanup Alternix folder
# ────────────────────────────────────────────────

echo " "
echo "[Cleanup] Removing Alternix source folder at $ALT_ROOT ..."
cd ~
rm -rf "$ALT_ROOT"


# ────────────────────────────────────────────────
# 10. Install grub theme
# ────────────────────────────────────────────────
echo " "
cd "$HOME"
git clone https://github.com/hashirsajid58200p/forest-dawn-grub-theme.git
cd forest-dawn-grub-theme
chmod +x install.sh
sudo ./install.sh

# Rename Grub entry from "Devuan GNU/Linux" to "Alternix"
sudo sed -i 's/Devuan GNU\/Linux/Alternix/g' /boot/grub/grub.cfg

sudo update-grub


# ────────────────────────────────────────────────
# 11. auto-cpufreq
# ────────────────────────────────────────────────
echo " "
echo "- Installing auto-cpufreq..."
cd "$HOME"
git clone https://github.com/AdnanHodzic/auto-cpufreq.git
cd auto-cpufreq
sudo ./auto-cpufreq-installer
sudo auto-cpufreq --install


# ────────────────────────────────────────────────
# 12. rounded-corners
# ────────────────────────────────────────────────
echo " "
echo "- Installing rounded-corners..."
cd "$HOME"
git clone https://github.com/DansDesigns/rounded_corners
cd rounded_corners
chmod +x install_corners.sh
sudo ./install_corners.sh


# ────────────────────────────────────────────────
# GLX compatibility check → patch picom.conf
# Detects whether the GPU supports GLX/OpenGL well
# enough for picom's glx backend. If not, switches
# picom.conf to xrender (safe on all hardware).
# ────────────────────────────────────────────────
echo " "
echo "[Picom] Checking GLX compatibility..."

PICOM_CONF="$HOME/.config/picom/picom.conf"
GLX_OK=false

# glxinfo must be available — install if missing
if ! command -v glxinfo >/dev/null 2>&1; then
    echo "• glxinfo not found, installing mesa-utils..."
    sudo nala install -y mesa-utils 2>/dev/null || sudo apt install -y mesa-utils 2>/dev/null || true
fi

if command -v glxinfo >/dev/null 2>&1; then
    GL_RENDERER=$(glxinfo 2>/dev/null | grep "OpenGL renderer" | head -1)
    GL_VERSION=$(glxinfo 2>/dev/null | grep "OpenGL version" | head -1)

    echo "• Renderer : $GL_RENDERER"
    echo "• Version  : $GL_VERSION"

    # Extract the major OpenGL version number
    GL_MAJOR=$(echo "$GL_VERSION" | grep -oP 'OpenGL version string: \K[0-9]+' | head -1)

    # GLX backend requires OpenGL 2.0+ and a real renderer (not software/llvm)
    if [[ "$GL_MAJOR" -ge 2 ]] 2>/dev/null && \
       ! echo "$GL_RENDERER" | grep -qiE "llvmpipe|softpipe|software|swrast|virgl"; then
        GLX_OK=true
    fi
else
    echo "• glxinfo unavailable — assuming GLX not supported."
fi

if [ "$GLX_OK" = true ]; then
    echo "[✓] GLX compatible — picom will use glx backend."
else
    echo "[!] GLX not supported or too limited — switching picom to xrender backend."

    if [ -f "$PICOM_CONF" ]; then
        # Replace backend line if it exists
        if grep -q "^backend" "$PICOM_CONF"; then
            sed -i 's/^backend\s*=\s*"glx"/backend = "xrender"/' "$PICOM_CONF"
            sed -i 's/^backend\s*=\s*"egl"/backend = "xrender"/' "$PICOM_CONF"
        else
            # Prepend if no backend line found
            sed -i '1s/^/backend = "xrender";\n/' "$PICOM_CONF"
        fi

        # Disable vsync — unreliable with xrender on old Intel
        if grep -q "^vsync" "$PICOM_CONF"; then
            sed -i 's/^vsync\s*=\s*true/vsync = false/' "$PICOM_CONF"
        else
            echo "vsync = false;" >> "$PICOM_CONF"
        fi

        echo "[✓] picom.conf patched: backend = xrender, vsync = false"
    else
        echo "• $PICOM_CONF not found — creating minimal xrender config..."
        mkdir -p "$(dirname "$PICOM_CONF")"
        cat <<'PICOMCFG' > "$PICOM_CONF"
# picom config — xrender backend (auto-set: GLX not supported on this GPU)
backend = "xrender";
vsync = false;
PICOMCFG
        echo "[✓] Minimal xrender picom.conf created."
    fi
fi


# ────────────────────────────────────────────────
# Finish + prompt
# ────────────────────────────────────────────────
echo " "
echo " "
echo " "
echo " "
echo "=============================================="
echo "     Alternix installation complete!"
echo "     (Devuan / SysVinit Edition)"
echo "=============================================="
echo " "
echo ""
echo "Everything installed to /usr/local/bin/"
echo ""
echo "Autologin + startx enabled for user: $TARGET_USER"
echo ""
echo ".xinitrc configured for Qtile."
echo ""
echo "Init system: SysVinit  |  logind: elogind"
echo ""
echo "Press 1 to Restart"
echo "Press 2 to Continue"
echo ""

while true; do
    read -n 1 -s -r KEY
    if [[ "$KEY" == "1" ]]; then
        echo "Beginning Reboot Sequence..."
        echo " "
        echo "This window will self-destruct in 5 seconds..."
        sleep 1; echo "5.."
        sleep 1; echo "4.."
        sleep 1; echo "3.."
        sleep 1; echo "2.."
        sleep 1; echo "1.."
        sleep 1
        sudo reboot
        break
    elif [[ "$KEY" == "2" ]]; then
        echo ""
        echo "Continuing to shell..."
        break
    else
        echo ""
        echo "Invalid choice. Press 1 to Restart or 2 to Continue."
    fi
done
