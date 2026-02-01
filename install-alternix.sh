#!/bin/bash
set -e

echo "=============================================="
echo "  Alternix / OSM-Phone Installer"
echo "=============================================="
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

#echo "-[System] Installing XLibre.."
#chmod +x install_xlibre.sh
#sudo ./install_xlibre.sh

# ────────────────────────────────────────────────
# 1. Install apps & dependencies
# ────────────────────────────────────────────────
echo "[1/10] Installing system dependencies..."

echo "- Adding Nala dependencies.."
echo "deb http://deb.volian.org/volian/ nala main" | sudo tee /etc/apt/sources.list.d/volian.list
wget -qO - https://deb.volian.org/volian/volian.gpg | sudo tee /etc/apt/trusted.gpg.d/volian.gpg


echo "- Installing Nala.."
sudo apt install nala nala -y

sudo rm /etc/apt/sources.list.d/volian.list
sudo rm /etc/apt/trusted.gpg.d/volian.gpg


#echo "- Converting APT to Nala.."
#cat <<EOF >> "$HOME/.bashrc"
#apt() { 
#  command nala "$@"
#}
#sudo() {
#  if [ "$1" = "apt" ]; then
#    shift
#    command sudo nala "$@"
#  else
#    command sudo "$@"
#  fi
#}
#EOF
#
#echo "- Adding root Nala.."
#sudo cat <<EOF >> "/root/.bashrc"
#apt() { 
#  command nala "$@"
#}
#sudo() {
#  if [ "$1" = "apt" ]; then
#    shift
#    command sudo nala "$@"
#  else
#    command sudo "$@"
#  fi
#}
#EOF
#echo "- Nala conversion complete.."

echo "- Running Nala Server Fetch.."
sudo nala fetch



sudo nala update
sudo nala install -y \
    fastfetch qtbase5-dev qt5-qmake qtdeclarative5-dev \
    fonts-noto-color-emoji libxcomposite-dev libxrender-dev libxfixes-dev \
    xwallpaper pkg-config libpoppler-qt5-dev htop python3-pip curl git \
    python3-venv picom qtile redshift onboard samba xdotool alacritty \
    synaptic brightnessctl pavucontrol pulseaudio alsa-utils flatpak libevdev-dev\
    snapd power-profiles-daemon xprintidle libx11-dev libxtst-dev ntfs-3g \
    kalk vlc qt5-style-kvantum network-manager


sudo nala install -y --no-install-recommends plasma-dialer spacebar plasma-discover



#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# RPI NON-COMPATIBLE - Comment out:

#sudo nala install thermald
#sudo systemctl enable --now power-profiles-daemon
#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo "[System] Installing Flatpaks.."

# Delete APT Archive to free up space
echo "[System] Clearing space in /var/cache/apt/archive..."
sudo rm -r /var/cache/apt/archives

# Enable Flathub repository (safe to run multiple times)
sudo flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
echo "[System] Installing Flatpaks..."

# Install flatpaks
sudo flatpak install -y flathub com.github.joseexposito.touche
sudo flatpak install -y flathub io.github.kolunmi.Bazaar
#sudo flatpak install -y flathub net.retrodeck.retrodeck
sudo flatpak install -y flathub org.kde.kweather
#sudo flatpak install -y flathub net.sourceforge.ExtremeTuxRacer
#sudo flatpak install -y flathub io.github.swordpuffin.hunt
#sudo flatpak install -y flathub com.github.avojak.warble
sudo flatpak install -y flathub org.kde.qrca


# Install Snaps

# ────────────────────────────────────────────────
# Install all .deb packages in ~/Alternix/installers/
# ────────────────────────────────────────────────
echo "[System] Installing local .deb packages..."

INSTALLER_DIR="$ALT_ROOT/Alternix/installers"

if [ -d "$INSTALLER_DIR" ]; then
    DEB_COUNT=$(ls -1 "$INSTALLER_DIR"/*.deb 2>/dev/null | wc -l)

    if [ "$DEB_COUNT" -gt 0 ]; then
        echo "• Found $DEB_COUNT installer package(s). Installing..."

        # Install each .deb file
        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true

        # Fix missing dependencies automatically
        #sudo apt-get update -y
        sudo nala install -f -y

        # Install each .deb file AGAIN
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

# Install Starship
echo "[System] Installing Starship prompt..."
curl -sS https://starship.rs/install.sh | sh -s -- --yes

# Add starship init to ~/.bashrc if it's not already present
if ! grep -Fxq 'eval "$(starship init bash)"' "$HOME/.bashrc"; then
    echo 'eval "$(starship init bash)"' >> "$HOME/.bashrc"
fi

# Install Brave
echo "[System] Installing Brave Browser..."
curl -fsS https://dl.brave.com/install.sh | sh -s -- --yes



# ────────────────────────────────────────────────
# Move ALL configs from ~/Alternix/configs → ~/.config
# ────────────────────────────────────────────────
echo " "
echo "[Config] Installing user configs..."

CONFIG_SRC="$ALT_ROOT/Alternix/configs"
CONFIG_DST="$HOME/.config"

sudo mv "$ALT_ROOT/Alternix/configs/.alacritty.toml" ~/

mkdir -p "$CONFIG_DST"

if [ -d "$CONFIG_SRC" ]; then
    # Move everything (folders AND files) into ~/.config
    echo "• Moving configs from $CONFIG_SRC → $CONFIG_DST"
    cp -r "$CONFIG_SRC/"* "$CONFIG_DST/"
    echo "• Configs installed."
    #echo " "
else
    echo "• No configs folder found, skipping."
fi


sudo cp -r "$ALT_ROOT/Alternix/onboard" /usr/share/onboard

cp "$ALT_ROOT/Alternix/decky/decky_installer.desktop" ~/


# ────────────────────────────────────────────────
# Install Fonts (from ~/Alternix/fonts)
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
# Install Wallpapers (to ~/Pictures/wallpapers)
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

# ────────────────────────────────────────────────
# 3. Configure xinitrc autostart
# ────────────────────────────────────────────────
echo " "

echo "[4/10] Creating .xinitrc autostart..."
cat <<EOF > "$HOME/.xinitrc"
#!/bin/sh
source "\$HOME/.qtile_venv/bin/activate"
exec "\$HOME/.qtile_venv/bin/qtile" start
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
# 5. Build Alternix Apps (from ~/Alternix/apps)
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
g++ apps/osm-status.cpp -o osm-status -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-status && sudo mv osm-status /usr/local/bin/



echo "• Building osm-paper..."
g++ -fPIC apps/osm-paper.cpp -o osm-paper $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-paper && sudo mv osm-paper /usr/local/bin/

# Icons
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

# Icons
if [ -f "icons/osm-files.png" ]; then
    sudo cp icons/osm-files.png /usr/share/icons/hicolor/64x64/apps/osm-files.png
fi

mkdir -p "$HOME/.local/share/applications"
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



#echo "• Building osm-notes..."
#g++ -fPIC apps/osm-notes.cpp -o osm-notes -std=c++17 $(pkg-config --cflags --libs Qt5Widgets)
#chmod +x osm-notes && sudo mv osm-notes /usr/local/bin/
#
#if [ -f "icons/osm-notes.png" ]; then
#    sudo cp icons/osm-notes.png /usr/share/icons/hicolor/64x64/apps/osm-notes.png
#fi
#
#cat <<EOF > "$HOME/.local/share/applications/osm-notes.desktop"
#[Desktop Entry]
#Type=Application
#Name=Notes
#Comment=Notes App for Alternix / OSM-Phone
#Exec=/usr/local/bin/osm-notes %U
#Icon=osm-notes
#Terminal=false
#Categories=Utility;Notes;
#StartupNotify=false
#EOF
#chmod +x "$HOME/.local/share/applications/osm-notes.desktop"



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

#echo "• Adding osm-sudo alias to ~/.bashrc..."
#if ! grep -q "alias sudo='/usr/local/bin/osm-sudo'" "$HOME/.bashrc" 2>/dev/null; then
#    echo "alias sudo='/usr/local/bin/osm-sudo'" >> "$HOME/.bashrc"
#fi
#
#echo "• osm-sudo installed..."



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

echo "• Building storage.so..."
g++ -fPIC -shared storage.cpp -o storage.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv storage.so /usr/local/bin/

echo "• Building accounts.so..."
g++ -std=c++11 accounts.cpp -o accounts.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv accounts.so /usr/local/bin/

echo "• Building system.so..."
g++ -fPIC -shared system.cpp -o system.so $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv system.so /usr/local/bin/


#────────────────────────────────────────────────
#            Custom App Shortcuts
#────────────────────────────────────────────────
echo " "

cd "$ALT_ROOT/Alternix"
sudo cp icons/update.png /usr/share/icons/hicolor/64x64/apps/update.png
sudo cp icons/upgrade.png /usr/share/icons/hicolor/64x64/apps/upgrade.png

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


echo "• Creating system-update launcher..."
sudo tee /usr/share/applications/update.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=System Update
Comment=System Update
Exec=alacritty -e sudo nala update
Terminal=false
Icon=update
Categories=System;
EOF

echo "• Creating system-upgrade launcher..."
sudo tee /usr/share/applications/upgrade.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=System Upgrade
Comment=System Upgrade
Exec=alacritty -e sudo nala upgrade
Terminal=false
Icon=upgrade
Categories=System;
EOF


# ────────────────────────────────────────────────
# Create Brave YouTube WebApp launcher
# ────────────────────────────────────────────────
echo " "

echo "[System] Installing Brave WebApps..."

APP_DIR="$HOME/.local/share/applications"
mkdir -p "$APP_DIR"

# Optional: install an icon (replace if you have your own)
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

# ────────────────────────────────────────────────
# Create Alternitech Forums WebApp 
# ────────────────────────────────────────────────
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


# ────────────────────────────────────────────────
# Create OpenStreetMap Forums WebApp 
# ────────────────────────────────────────────────
echo "[Apps] Installing AlterniTech Forum WebApp..."


if [ -f "icons/alternitech-forum.png" ]; then
    sudo cp icons/openmap.png /usr/share/icons/hicolor/64x64/apps/openmap.png
fi

cat <<EOF > "$HOME/.local/share/applications/openmap.desktop"
[Desktop Entry]
Version=1.0
Type=Application
Name=OpenStreetMap
Comment=OpenStreenMap WebApp (Brave)
Exec=brave-browser --app=https://openstreetmap.org/ --new-window --force-device-scale-factor=1.3
Icon=openmap
Terminal=false
Categories=Network;WebBrowser;Utility;
StartupNotify=true
EOF

chmod +x "$HOME/.local/share/applications/openmap.desktop"

# Refresh desktop database
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications" || true
fi

echo "• OpenStreetMap WebApp installed."


#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
# 	^^^^	Add more WebApps here:	^^^^
#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

# ────────────────────────────────────────────────
# 7. Installing Samba Network Updates
# ────────────────────────────────────────────────
echo " "


echo "• Updating /etc/samba/smb.conf..."

sudo sed -i '/workgroup = WORKGROUP/a client min protocol = NT1\nserver min protocol = NT1' /etc/samba/smb.conf

echo "• Restarting Samba service..."
sudo systemctl restart smbd nmbd || true

# ────────────────────────────────────────────────
# OSM-Lockscreen Security Layer
# Prevents killing osm-lockscreen via SSH or pkill
# ────────────────────────────────────────────────
echo " "


echo "[X] Setting up secure lockscreen supervisor..."

# 1. Create dedicated lockscreen user (no login, no shell)
if ! id "lockscreen" >/dev/null 2>&1; then
    sudo adduser --disabled-password --gecos "" --shell /usr/sbin/nologin lockscreen
fi

# 2. Install supervisor daemon (osm-lockd)
sudo tee /usr/local/bin/osm-lockd >/dev/null <<'EOF'
#!/bin/bash

FLAG="/tmp/osm_unlock_success"

while true; do
    # Clear flag before launching
    rm -f "$FLAG"

    /usr/local/bin/osm-lockscreen

    # If flag exists, stop (unlock was successful)
    if [ -f "$FLAG" ]; then
        rm -f "$FLAG"
        exit 0
    fi

    # Otherwise restart the lockscreen after short delay
    sleep 0.05
done
EOF

sudo chmod +x /usr/local/bin/osm-lockd

# 3. Create systemd service
sudo tee /etc/systemd/system/osm-lockscreen.service >/dev/null <<'EOF'
[Unit]
Description=OSM-Phone Lockscreen Supervisor
After=graphical.target

[Service]
Type=simple
User=lockscreen
ExecStart=/usr/local/bin/osm-lockd
Restart=always
RestartSec=0.05

[Install]
WantedBy=graphical.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable osm-lockscreen.service
echo "[✓] Lockscreen security layer installed."



# ────────────────────────────────────────────────
# Lockscreen auto-activate on wake (Sleep / Hibernate)
# Debian 13 Compatible (systemd 255+)
# ────────────────────────────────────────────────
#echo " "

#
#echo "[X] Enabling wake lockscreen auto-launch..."
#
#sudo mkdir -p /etc/systemd/system-sleep
#
#sudo tee /etc/systemd/system-sleep/osm-lockscreen >/dev/null <<'EOF'
##!/bin/bash
#case "$1" in
#    post)
#        systemctl restart osm-lockscreen.service
#        ;;
#esac
#EOF
#
#sudo chmod +x /etc/systemd/system-sleep/osm-lockscreen
#echo "[✓] Lockscreen wake handler installed."



# ────────────────────────────────────────────────
# Alternix Idle Lock (auto-lock after N seconds)
# ────────────────────────────────────────────────
#echo " "
#
#
#echo "[X] Installing Alternix auto-idle lock..."
#
#
#sudo tee /usr/local/bin/osm-idle-lockd >/dev/null <<'EOF'
##!/bin/bash
#
#LOCKSCREEN_TIMEOUT=30
#FLAG="/tmp/osm_unlock_success"
#
#while true; do
#    IDLE_MS=$(xprintidle 2>/dev/null)
#    if [ -z "$IDLE_MS" ]; then
#        sleep 1
#        continue
#    fi
#
#    IDLE=$((IDLE_MS / 1000))
#
#    if [ $IDLE -ge $LOCKSCREEN_TIMEOUT ]; then
#        rm -f "$FLAG"
#        systemctl restart osm-lockscreen.service
#    fi
#
#    sleep 1
#done
#EOF
#
#sudo chmod +x /usr/local/bin/osm-idle-lockd
#
#sudo tee /etc/systemd/system/osm-idle-lockd.service >/dev/null <<'EOF'
#[Unit]
#Description=Alternix Idle Auto-Lock Daemon
#After=graphical.target
#
#[Service]
#ExecStart=/usr/local/bin/osm-idle-lockd
#Restart=always
#RestartSec=1
#
#[Install]
#WantedBy=graphical.target
#EOF
#
#sudo systemctl daemon-reload
#sudo systemctl enable --now osm-idle-lockd.service
#echo "[✓] Idle lock enabled."



# ────────────────────────────────────────────────
# systemd-logind override
# Ignore power button ACTIONS, but do NOT swallow events
# ────────────────────────────────────────────────
echo " "


echo "[X] Configuring systemd-logind power key handling..."

sudo mkdir -p /etc/systemd/logind.conf.d

sudo tee /etc/systemd/logind.conf.d/alternix-power.conf >/dev/null <<'EOF'
[Login]
HandlePowerKey=ignore
PowerKeyIgnoreInhibition=no
EOF

sudo systemctl restart systemd-logind

echo "[✓] logind configured."


# ────────────────────────────────────────────────
# Unified Lock Trigger
# ────────────────────────────────────────────────
#echo " "
#
#
#sudo tee /usr/local/bin/alternix-lock-now >/dev/null <<'EOF'
##!/bin/bash
#FLAG="/tmp/osm_unlock_success"
#
#rm -f "$FLAG"
#systemctl restart osm-lockscreen.service
#EOF
#
#sudo chmod +x /usr/local/bin/alternix-lock-now
#
#
#echo "[✓] Unified lock trigger installed.."


# ────────────────────────────────────────────────
# 8. Autologin via systemd
# ────────────────────────────────────────────────
echo " "

echo "[5/10] Enabling autologin on tty1 for user $TARGET_USER..."

sudo mkdir -p /etc/systemd/system/getty@tty1.service.d

sudo tee /etc/systemd/system/getty@tty1.service.d/autologin.conf >/dev/null <<EOF
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin $TARGET_USER --noclear %I \$TERM
Type=idle
EOF

sudo systemctl daemon-reload
#sudo systemctl restart getty@tty1

echo "Setting NOPASSWD for $TARGET_USER..."
echo "$TARGET_USER ALL=(ALL) NOPASSWD: ALL" | sudo tee /etc/sudoers.d/alternix-nopasswd >/dev/null
sudo chmod 440 /etc/sudoers.d/alternix-nopasswd

# ────────────────────────────────────────────────
# 9. Cleanup Alternix folder
# ────────────────────────────────────────────────
echo " "

echo "[Cleanup] Removing Alternix source folder at $ALT_ROOT ..."
rm -rf "$ALT_ROOT"


# ────────────────────────────────────────────────
# 10. Install grub theme & plymouth boot animation - NOT RPI COMPATIBLE
# ────────────────────────────────────────────────
#echo " "
#
#cd $HOME
#git clone https://github.com/hashirsajid58200p/forest-dawn-grub-theme.git
#cd forest-dawn-grub-theme
#chmod +x install.sh
#sudo ./install.sh
#sudo update-grub

# ────────────────────────────────────────────────
# 11. auto-cpufreq
# ────────────────────────────────────────────────
#echo " "
#
#echo "- Installing auto-cpufreq..."
#cd $HOME
#git clone https://github.com/AdnanHodzic/auto-cpufreq.git
#cd auto-cpufreq
#sudo ./auto-cpufreq-installer
#sudo auto-cpufreq --install

# ────────────────────────────────────────────────
# 12. Finish + prompt
# ────────────────────────────────────────────────
echo " "

echo "=============================================="
echo "     Alternix installation complete!"
echo "=============================================="
echo "Everything installed to /usr/local/bin/"
echo "Autologin + startx enabled for user: $TARGET_USER"
echo ".xinitrc configured for Qtile."
echo "Rebooting to Alternix."
sudo reboot now
