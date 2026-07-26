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
#echo "No Settings or Configs to update..."
echo ""
echo ""
echo "[Config] Updating Grub Entry.."

# Rename Grub entry from "Devuan GNU/Linux" to "Alternix"
sudo sed -i 's/Devuan GNU\/Linux/Alternix/g' /boot/grub/grub.cfg
sudo update-grub

sudo nala install xserver-xlibre-input-libinput ntfs-3g exfatprogs exfat-fuse udisks2 pmount -y

echo "[Config] Installing updated configs..."
cp -r "$ALT_ROOT/Alternix/configs/." "$HOME/.config/"

echo ""
#===========================================================
# Apps to be updated (compilation commands & icons):
#===========================================================
echo ""
#echo "No Apps need updating..."
echo ""

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

        # Install each .deb file
        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true

        # Fix missing dependencies automatically
        #sudo apt-get update -y
        sudo nala install -f -y

        # try to Install each .deb file AGAIN
        sudo dpkg -i "$INSTALLER_DIR"/*.deb || true

        echo "• Local installer packages installed."
    else
        echo "• No .deb files in installers folder, skipping."
    fi
else
    echo "• No installers folder found, skipping."
fi

cd "$ALT_ROOT/Alternix/apps/osm-settings"

echo "• Updating wifi.so..."
g++ -fPIC -shared wifi.cpp -o wifi.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv wifi.so /usr/local/bin/

echo "• Updating bluetooth.so..."
g++ -fPIC -shared bluetooth.cpp -o bluetooth.so $(pkg-config --cflags --libs Qt5Widgets)
sudo mv bluetooth.so /usr/local/bin/

echo "• Updating kernel.so..."
g++ -std=c++17 -fPIC -shared kernel.cpp -o kernel.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo mv kernel.so /usr/local/bin/

echo "• Updating sound.so..."
g++ -std=c++17 -fPIC -shared sound.cpp -o sound.so `pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core`
sudo mv sound.so /usr/local/bin/

echo "• Updating display.so..."
g++ display.cpp -o display.so -shared -fPIC $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
sudo mv display.so /usr/local/bin/

cd "$ALT_ROOT/Alternix/apps"

echo "• Updating osm-power..."
g++ -fPIC osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/

echo "• Compiling osm-powerd..."
sudo g++ -O2 osm-powerd.cpp -o osm-powerd
sudo chmod +x osm-powerd && sudo mv osm-powerd /usr/local/bin/
sudo chown root:root /usr/local/bin/osm-powerd
sudo chmod 4755 /usr/local/bin/osm-powerd


echo "• Updating osm-files..."
g++ -fPIC osm-files.cpp -o osm-files $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-files && sudo mv osm-files /usr/local/bin/

echo "• Updating osm-status..."
g++ -fPIC osm-status.cpp -o osm-status -ldl $(pkg-config --cflags --libs Qt5Widgets Qt5DBus) -lX11
chmod +x osm-status && sudo mv osm-status /usr/local/bin/

echo "• Updating osm-paper..."
g++ -fPIC osm-paper.cpp -o osm-paper $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-paper && sudo mv osm-paper /usr/local/bin/

echo "• Updating osm-clock..."
g++ -fPIC osm-clock.cpp -o osm-clock $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-clock && sudo mv osm-clock /usr/local/bin/

echo "• Updating osm-notify..."
g++ -fPIC osm-notify.cpp -o osm-notify $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core Qt5DBus) -lX11 -lXtst
chmod +x osm-notify && sudo mv osm-notify /usr/local/bin/


echo "• Updating sounds..."
cp -r "$ALT_ROOT/Alternix/sounds" ~/.config/Alternix/

echo "• Updating scripts..."
cp -r "$ALT_ROOT/Alternix/scripts" ~/.config/Alternix/
cd ~/.config/Alternix/scripts
chmod +x alternix-rotate-monitor.sh
chmod +x alternix-rotate-setup.sh
chmod +x alternix-rotate-toggle.sh


echo "• Updating Wave Music Player.."
cd ~
sudo rm -r music_player
git clone https://github.com/DansDesigns/music_player
cd music_player
chmod +x install_music_player.sh
./install_music_player.sh


echo "• Updating Ponder.."
cd ~
sudo rm -r Ponder
git clone https://github.com/DansDesigns/Ponder
cd Ponder
chmod +x install.sh
./install.sh


echo "• Update & Install Complete."
echo ""
#===========================================================
# Updater:
#===========================================================
echo "[Updating Alternix Updater]"

# Un-Comment to update icon:
echo "• Updating Icon..."
sudo cp icons/os-check-update.png /usr/share/icons/hicolor/64x64/apps/os-check-update.png

#=========================================
# Un-Comment to remove old update and upgrade shortcuts:
echo "• Revmoing old shortcuts..."
sudo rm -r /usr/share/applications/update.desktop
sudo rm -r /usr/share/applications/upgrade.desktop

#=========================================
# Update Updater:
echo "• Installing the Updater..."
sudo chmod +x "$ALT_ROOT/update/os-check-update"
sudo cp "$ALT_ROOT/update/os-check-update" /usr/bin/

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
