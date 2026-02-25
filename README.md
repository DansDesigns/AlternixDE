# Alternix
A Linux Desktop Environment designed for touchscreen devices.
![Alternix Desktop](https://github.com/DansDesigns/OSM-Phone/blob/main/OS%20Concepts/Alternix_Preview.png)
Using Qtile & custom Qt5 app launcher, app switcher, quick settings, file editor/viewer & drawing app


# *note: While in a working state, Alternix is still in Development


Key commands:
```
WIN + A: App Launcher

WIN + N: Popup Shortcut Menu

WIN + P: Power Menu

WIN + ENTER: Open Terminal

WIN + F: Fullscreen Toggle

WIN + R: Spawn Run Prompt

WIN + W: Close Window

WIN + T: Floating Window Toggle

WIN + 1, 2, 3: Switch to Desktop 1, 2, 3

WIN + SHIFT + 1 ,2 ,3: Move Window & Switch to Desktop 1, 2, 3

```


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

Follow the onscreen prompts..

You can edit the install-alternix.sh script to enable or disable packages.
