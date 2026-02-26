#!/bin/bash


# Root of the Alternix repo
ALT_ROOT="$(cd "$(dirname "$0")" && pwd)"

#if [ ! -d "$ALT_ROOT" ]; then
#    echo "ERROR: $ALT_ROOT not found. Please place install-update.sh inside ~/Alternix."
#    exit 1
#fi


#===========================================================
# Add the Settings to be updated (configs, system icons & settings):
#===========================================================

echo ""
echo "No Settings or Configs to update..."
echo ""

#===========================================================
# Add the Apps to be updated (compilation commands & icons):
#===========================================================

echo ""
echo "No Apps need updating..."
echo ""


#===========================================================
# Update the Updater:
#===========================================================
echo "[Updating Alternix Updater]"
cd "$ALT_ROOT/Alternix"

# Un-Comment to update icon:
echo "• Updating Icon..."
sudo cp icons/os-check-update.png /usr/share/icons/hicolor/64x64/apps/os-check-update.png

#=========================================
# Un-Comment to remove old update and upgrade shortcuts:
echo "• Revmoing old shortcuts..."
#sudo rm -r /usr/share/applications/update.desktop
#sudo rm -r /usr/share/applications/upgrade.desktop

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
Name=System Update (GUI)
Exec= alacritty -e /usr/bin/os-check-update
Icon=os-check-update
Type=Application
Terminal=true
Categories=System;
EOF

cd /
sudo rm -rf "$ALT_ROOT"
echo " "
echo " "
echo " "
echo " "
echo "=============================================="
echo "     Alternix Update Complete!"
echo "=============================================="
echo " "
echo " "
echo " "
echo " "