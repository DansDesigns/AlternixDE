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

sudo nala install xserver-xlibre-input-libinput -y

echo "[Config] Installing updated configs..."
cp -r "$ALT_ROOT/Alternix/configs/"* "$HOME/.config/"

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
echo "• Installing Updated wifi.so..."
sudo mv wifi.so /usr/local/bin/

echo "• Building osm-files..."
g++ -fPIC apps/osm-files.cpp -o osm-files $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-files && sudo mv osm-files /usr/local/bin/

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
