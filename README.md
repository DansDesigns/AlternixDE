# Alternix
A Linux Desktop Environment designed for touchscreen devices.
![Alternix Desktop](https://github.com/DansDesigns/OSM-Phone/blob/main/OS%20Concepts/Alternix/desktop.png)

to install:

recomended to use a fresh install of Debain 13 with NO desktop,

install git:
```
sudo apt install git
```
clone repo:
```
git clone https://github.com/DansDesigns/Alternix.git
```
cd into the newly created repo folder
```
cd Alternix
```
give permission & run:
```
chmod +x install-alternix.sh
./install-alternix.sh
```

if it doesnt run, use:
```
sed -i 's/\r$//' install-alternix.sh
chmod +x install-alternix.sh
./install-alternix.sh
```

Follow the onscreen prompts - it will ask for a username, use the same as your username to install for yourself..

nala is used as a package manager front-end (inplace of apt) after setting a username nala will fetch the local mirrors, when asked input: 1 2 3 4 and press enter.


once nala is updated the following packages will be downloaded:
```
fastfetch qtbase5-dev qt5-qmake qtdeclarative5-dev

fonts-noto-color-emoji libxcomposite-dev libxrender-dev libxfixes-dev

xwallpaper pkg-config libpoppler-qt5-dev htop python3-pip curl git

python3-venv picom qtile redshift onboard samba xdotool alacritty

synaptic brightnessctl pavucontrol pulseaudio alsa-utils flatpak libevdev-dev

snapd power-profiles-daemon xprintidle libx11-dev libxtst-dev ntfs-3g

kalk vlc qt5-style-kvantum thermald network-manager
```



edit the install-alternix.sh script to enable or disable packages, the following flatpaks are set to be installed automatically:
```
sudo flatpak install -y flathub com.github.joseexposito.touche

sudo flatpak install -y flathub io.github.kolunmi.Bazaar

sudo flatpak install -y flathub net.retrodeck.retrodeck

sudo flatpak install -y flathub org.kde.kweather

sudo flatpak install -y flathub net.sourceforge.ExtremeTuxRacer

sudo flatpak install -y flathub io.github.swordpuffin.hunt

sudo flatpak install -y flathub com.github.avojak.warble

sudo flatpak install -y flathub org.kde.qrca
```

