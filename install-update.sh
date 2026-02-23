#!/bin/bash
set -e

echo "=============================================="
echo "          Alternix Desktop Updater"
echo "=============================================="
echo "-------> Re-teaching Penguins how to fly! <-------"
echo " "
echo " "
echo " "
echo " "
# Root of the Alternix repo
ALT_ROOT="$HOME/Alternix"

if [ ! -d "$ALT_ROOT" ]; then
    echo "ERROR: $ALT_ROOT not found. Please place install-update.sh inside ~/Alternix."
    exit 1
fi
echo "[UPDATE] Updating Alternix apps..."
cd "$ALT_ROOT/Alternix" || { echo "ERROR: $ALT_ROOT not found"; exit 1; }



echo "• Updating osm-launcher..."
g++ -O3 -fPIC apps/osm-launcher.cpp -o osm-launcher $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-launcher && sudo mv osm-launcher /usr/local/bin/



echo "• Updating osm-lock..."
g++ apps/osm-lock.cpp -o osm-lock -fPIC $(pkg-config --cflags --libs Qt5Widgets)
chmod +x osm-lock && sudo mv osm-lock /usr/local/bin/



echo "• Updating osm-running..."
g++ apps/osm-running.cpp -o osm-running -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-running && sudo mv osm-running /usr/local/bin/



echo "• Updating osm-notify..."
g++ -fPIC apps/osm-notify.cpp -o osm-notify $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core Qt5DBus) -lX11 -lXtst
chmod +x osm-notify && sudo mv osm-notify /usr/local/bin/



echo "• Updating osm-status..."
g++ apps/osm-status.cpp -o osm-status -fPIC -ldl $(pkg-config --cflags --libs Qt5Widgets) -lX11
chmod +x osm-status && sudo mv osm-status /usr/local/bin/



echo "• Updating osm-paper..."
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



echo "• Updating osm-paper-restore..."
chmod +x apps/osm-paper-restore && sudo cp apps/osm-paper-restore /usr/local/bin/



echo "• Updating osm-styling..."
g++ -fPIC apps/osm-styling.cpp -o osm-styling $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-styling && sudo mv osm-styling /usr/local/bin/



echo "• Updating osm-files..."
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



echo "• Updating osm-viewer..."
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



#echo "• Updating osm-notes..."
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



echo "• Updating osm-draw..."
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



echo "• Updating osm-rocker..."
g++ -fPIC apps/osm-rocker.cpp -o osm-rocker $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-rocker && sudo mv osm-rocker /usr/local/bin/



echo "• Updating osm-sudo..."
sudo cp apps/osm-sudo /usr/local/bin/osm-sudo
sudo chmod 755 /usr/local/bin/osm-sudo

#echo "• Adding osm-sudo alias to ~/.bashrc..."
#if ! grep -q "alias sudo='/usr/local/bin/osm-sudo'" "$HOME/.bashrc" 2>/dev/null; then
#    echo "alias sudo='/usr/local/bin/osm-sudo'" >> "$HOME/.bashrc"
#fi
#
#echo "• osm-sudo installed..."



echo "• Updating osm-power..."
g++ -fPIC apps/osm-power.cpp -o osm-power $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-power && sudo mv osm-power /usr/local/bin/



echo "• Updating osm-powerd..."
sudo g++ -O2 apps/osm-powerd.cpp -o osm-powerd
sudo chmod +x osm-powerd && sudo mv osm-powerd /usr/local/bin/
sudo chown root:root /usr/local/bin/osm-powerd
sudo chmod 4755 /usr/local/bin/osm-powerd




echo "• Updating osm-lockscreen..."
g++ -fPIC apps/osm-lockscreen.cpp -o osm-lockscreen $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)
chmod +x osm-lockscreen && sudo mv osm-lockscreen /usr/local/bin/




# ────────────────────────────────────────────────
# 6. Build OSM Settings + modules
# ────────────────────────────────────────────────
echo " "

echo "[UPDATE] Updating osm-settings..."

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
#            Custom App Shortcuts & Icons
#────────────────────────────────────────────────
echo " "
echo "• Updating App Icons..."

cd $HOME/Alternix/Alternix

sudo cp icons/update.png /usr/share/icons/hicolor/64x64/apps/update.png
sudo cp icons/upgrade.png /usr/share/icons/hicolor/64x64/apps/upgrade.png
sudo cp icons/bauh.png /usr/share/icons/hicolor/64x64/apps/bauh.png

echo "• Updating htop.desktop launcher..."
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


echo "• Updating system-update launcher..."
sudo tee /usr/share/applications/update.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=System Update
Comment=System Update
Exec=alacritty -e sudo nala update && sudo nala upgrade
Terminal=false
Icon=update
Categories=System;
EOF

#echo "• Updating system-upgrade launcher..."
#sudo tee /usr/share/applications/upgrade.desktop >/dev/null <<EOF
#[Desktop Entry]
#Type=Application
#Name=System Upgrade
#Comment=System Upgrade
#Exec=alacritty -e sudo nala upgrade
#Terminal=false
#Icon=upgrade
#Categories=System;
#EOF

echo "• Updating bauh Shortcut..."
sudo tee /usr/share/applications/bauh.desktop >/dev/null <<EOF
[Desktop Entry]
Type=Application
Name=Apps (bauh)
Comment=Application Manager
Exec=bauh
Icon=bauh
Categories=System;
EOF


echo "• Updating Alternix Updater..."
sudo chmod $HOME/Alternix/update/os-check-update
sudo cp $HOME/Alternix/update/os-check-update /usr/bin/
sudo rm -r /usr/share/alternix/version.txt
sudo cp $HOME/Alternix/update/version.txt /usr/share/alternix/

sudo tee /usr/share/applications/os-check-update.desktop >/dev/null <<EOF
[Desktop Entry]
Name=OS Update (GUI)
Exec=os-check-update
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
echo "     Alternix Update Complete!"
echo "=============================================="
echo " "
echo " "
echo " "
echo " "
