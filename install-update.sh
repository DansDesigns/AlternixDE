#!/bin/bash


# Root of the Alternix repo
ALT_ROOT="$(cd "$(dirname "$0")" && pwd)"

echo ""
echo "Current working directory:"
pwd
cd "$ALT_ROOT/Alternix"
echo ""
sleep 1
#if [ ! -d "$ALT_ROOT" ]; then
#    echo "ERROR: $ALT_ROOT not found. Please place install-update.sh inside ~/Alternix."
#    exit 1
#fi


#===========================================================
# Settings to be updated (configs, system icons & settings):
#===========================================================
echo ""
# echo "No Settings or Configs to update..."
echo ""
echo ""
echo "[Config] Updating Grub Entry.."

# Rename Grub entry from "Devuan GNU/Linux" to "Alternix"
sudo sed -i 's/Devuan GNU\/Linux/Alternix/g' /boot/grub/grub.cfg
sudo update-grub
#sudo dpkg --add-architecture i386

#sudo nala install xserver-xlibre-input-libinput ntfs-3g exfatprogs exfat-fuse udisks2 pmount /
#   libmbim-utils libqmi-utils modemmanager mobile-broadband-provider-info x11-apps tlp fake-hwclock /
#   iio-sensor-proxy libxext-dev wine32 win64 -y

sudo nala install fonts-urw-base35 fonts-freefont-ttf

echo "[Config] Installing updated configs..."
cp -r "$ALT_ROOT/Alternix/configs/." "$HOME/.config/"



echo ""
#===========================================================
# Apps to be updated (compilation commands & icons):
#===========================================================
echo ""
# echo "No Apps need updating..."

echo "• Creating fetch.desktop launcher..."
sudo tee /usr/share/applications/fetch.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=About
Comment=System monitor
Exec=alacritty -e fetch
Terminal=false
Icon=fetch
Categories=System;
EOF

echo ""

# ────────────────────────────────────────────────
# Install all .deb packages in ~/Alternix/installers/
# ────────────────────────────────────────────────
echo " "
#echo "[System] Installing local .deb packages..."
#
#INSTALLER_DIR="$ALT_ROOT/Alternix/installers"
#
#if [ -d "$INSTALLER_DIR" ]; then
#    DEB_COUNT=$(ls -1 "$INSTALLER_DIR"/*.deb 2>/dev/null | wc -l)
#
#    if [ "$DEB_COUNT" -gt 0 ]; then
#        echo "• Found $DEB_COUNT installer package(s). Installing..."
#
#        # Install each .deb file
#        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true
#
#        # Fix missing dependencies automatically
#        #sudo apt-get update -y
#        sudo nala install -f -y
#
#        # try to Install each .deb file AGAIN
#        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true
#
#        echo "• Local installer packages installed."
#    else
#        echo "• No .deb files in installers folder, skipping."
#    fi
#else
#    echo "• No installers folder found, skipping."
#fi


#echo " "
#echo "[Config] Installing Extras..."
#
#if [ -d "$ALT_ROOT/Alternix/extras" ]; then
#    mkdir "$HOME/extras"
#    sudo cp -r "$ALT_ROOT/Alternix/extras/"* "$HOME/extras/"
#
#    echo "• Extras installed successfully."
#else
#    echo "--------------------------------"
#    echo "• [ERROR] Installing Extras..."
#    echo "--------------------------------"
#fi


cd "$ALT_ROOT/Alternix/apps/osm-settings"

echo "• Updating osm-settings..."
g++ osm-settings.cpp -o osm-settings -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-settings && sudo mv osm-settings /usr/local/bin/

echo "• Updating reticulum.so..."
g++ -std=c++17 -Wall -Wextra -fPIC -shared -o reticulum.so reticulum.cpp $(pkg-config --cflags --libs Qt5Widgets)
sudo install -m755 reticulum.so /usr/local/bin/reticulum.so


cd "$ALT_ROOT/Alternix"

echo "• Building osm-clock..."
g++ apps/osm-clock.cpp -o osm-clock -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-clock && sudo mv osm-clock /usr/local/bin/

echo "• Updating osm-power..."
g++ -fPIC apps/osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/

echo "• Building osm-files..."
g++ -fPIC apps/osm-files.cpp -o osm-files $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-files && sudo mv osm-files /usr/local/bin/

echo "• Building osm-viewer..."
g++ -fPIC apps/osm-viewer.cpp -o osm-viewer $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core poppler-qt5) -Wno-deprecated-declarations
chmod +x osm-viewer && sudo mv osm-viewer /usr/local/bin/

echo "• Building osm-draw..."
g++ -fPIC apps/osm-draw.cpp -o osm-draw -std=c++17 $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-draw && sudo mv osm-draw /usr/local/bin/

echo "• Building osm-paper..."
g++ -fPIC apps/osm-paper.cpp -o osm-paper $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-paper && sudo mv osm-paper /usr/local/bin/



#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo "• Removing old lockscreen service..."
# The old service ran the lockscreen as a separate user with no access
# to the display, so it crashed and restarted endlessly. The lockscreen
# now runs in the user's own session instead.
if [ -f /etc/init.d/osm-lockscreen ]; then
    sudo service osm-lockscreen stop
    sudo update-rc.d -f osm-lockscreen remove
    sudo rm -f /etc/init.d/osm-lockscreen
fi
if id lockscreen >/dev/null 2>&1; then
    sudo pkill -u lockscreen
fi

echo "• Updating osm-lockd..."
sudo tee /usr/local/bin/osm-lockd >/dev/null <<'LOCKD'
#!/bin/bash

# Only one lockscreen at a time. Waking and the Lock button can both
# start this, and a second copy would stack another lockscreen on top.
exec 9>"/tmp/osm-lockd-$(id -u).lock"
flock -n 9 || exit 0

FLAG="/tmp/osm_unlock_success"

while true; do
    rm -f "$FLAG"
    /usr/local/bin/osm-lockscreen 9>&-

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

echo "• Installing dbus-fast for Qtile..."
"$HOME/.qtile_venv/bin/pip" install dbus-fast

#~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

echo "• Compiling osm-powerd..."
sudo g++ -O2 osm-powerd.cpp -o osm-powerd
sudo chmod +x osm-powerd && sudo mv osm-powerd /usr/local/bin/
sudo chown root:root /usr/local/bin/osm-powerd
sudo chmod 4755 /usr/local/bin/osm-powerd

echo "• Updating Icons..."
cd "$ALT_ROOT/Alternix"
sudo install -Dm644 icons/*.png /usr/share/icons/hicolor/64x64/apps/

echo "• Updating sounds..."
cp -r "$ALT_ROOT/Alternix/sounds" ~/.config/Alternix/

echo "• Updating scripts..."
cp -r "$ALT_ROOT/Alternix/scripts" ~/.config/Alternix/
cd ~/.config/Alternix/scripts
chmod +x alternix-rotate-monitor.sh
chmod +x alternix-rotate-setup.sh
chmod +x alternix-rotate-toggle.sh

chmod +x alternix-touchscroll.py
cp alternix-touchscroll.py ~/.local/bin/alternix-touchscroll.py

chmod +x alternix-waydroid-session
sudo cp alternix-waydroid-session /usr/local/bin

chmod +x alternix-exe
sudo cp alternix-exe /usr/local/bin



echo "• App Update & Install Complete."
echo ""
#===========================================================
# Updater:
#===========================================================

#=========================================
# Create Folder if not already existing:
#sudo mkdir /usr/share/alternix

#=========================================
# Update Version Number:
echo "• Updating Version Number..."
sudo cp "$ALT_ROOT/update/version.txt" /usr/share/alternix/version.txt


echo "• Updating udev rules..."
# Symlink qtile binary to where udev rules expect it
sudo mkdir -p /usr/lib/udev
sudo ln -sf "$HOME/.qtile_venv/bin/qtile" /usr/lib/udev/qtile
sudo udevadm control --reload-rules

echo "• Updating usermod access..."
sudo usermod -aG video,input "$USER"
#=========================================
# Un-Comment to Update the App Launcher:

#echo "• Updating Launcher..."
#sudo tee /usr/share/applications/os-check-update.desktop >/dev/null <<EOF
#[Desktop Entry]
#Name=System Update
#Exec= alacritty -e /usr/bin/os-check-update
#Icon=os-check-update
#Type=Application
#Terminal=true
#Categories=System;
#EOF

echo " "
echo " "
echo " "
echo " "
echo "=============================================="
echo "         Alternix Update Complete!"
echo "=============================================="
echo " "
echo "=============================================="
echo "       THIS UPDATE REQUIRES A RESTART"
echo "=============================================="
