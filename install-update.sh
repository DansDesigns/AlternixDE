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

echo "[Config] Installing updated configs..."
cp -r "$ALT_ROOT/Alternix/configs/"* "$HOME/.config/"

echo ""
#===========================================================
# Apps to be updated (compilation commands & icons):
#===========================================================
echo ""
#echo "No Apps need updating..."
echo ""



echo "• Building osm-notify..."
pkill osm-notify
g++ -fPIC apps/osm-notify.cpp -o osm-notify $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core Qt5DBus) -lX11 -lXtst
chmod +x osm-notify && sudo mv osm-notify /usr/local/bin/
echo "re-launching osm-notify..."

echo "• Building osm-power..."
g++ -fPIC apps/osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/

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


#=========================================
# Un-Comment to Update the App Launcher:

echo "• Updating Launcher..."
sudo tee /usr/share/applications/os-check-update.desktop >/dev/null <<EOF
[Desktop Entry]
Name=System Update
Exec= alacritty -e /usr/bin/os-check-update
Icon=os-check-update
Type=Application
Terminal=true
Categories=System;
EOF

echo " "
echo " "
echo " "
echo " "
echo "=============================================="
echo "         Alternix Update Complete!"
echo "=============================================="
echo "Now running version:" cat /usr/share/alternix/version.txt
echo " "
echo "============================================================="
echo "THIS UPDATE REQUIRES A RESTART, PLEASE SELECT [REBOOT] BELOW"
echo "============================================================="