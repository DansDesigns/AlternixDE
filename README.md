# Alternix
A Linux Desktop Environment designed for touchscreen devices.
![Alternix Desktop](https://github.com/DansDesigns/OSM-Phone/blob/main/OS%20Concepts/Alternix_Preview.png)
Using Qtile & custom Qt5 app launcher, app switcher, quick settings, file editor/viewer & drawing app


# !-DESIGNED FOR DEBIAN/DEVUAN BASED DISTROS ONLY-!


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

CTRL + SPACE: open Ulauncher Application, file & search bar

```


# Install

Recomended to use NexOS Linux ([here](https://github.com/DansDesigns/NexOS))
or manually:

start with a fresh install of Debain 13 with NO desktop,

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
give permission & run (Devuan version):
```
chmod +x install-alternix_devuan.sh
./install-alternix_devuan.sh
```

if it doesnt run, use:
```
sed -i 's/\r$//' install-alternix_devuan.sh
chmod +x install-alternix_devuan.sh
./install-alternix_devuan.sh
```

Follow the onscreen prompts..

You can edit the install-alternix.sh script to enable or disable packages.


# Notification & Alarm Sounds

Alternix plays alert sounds through osm-status (the notification panel).
Sound files live in:
```
~/.config/Alternix/sounds/
```

Drop your own files in with these names:
```
notify.*   plays for ordinary app notifications
alarm.*    plays for alarms and critical notifications
boot.*     plays once per boot, after the first unlock
```

Supported formats: WAV, OGG, FLAC and MP3 (checked in that order).
For example, alarm.wav, alarm.ogg, alarm.flac or alarm.mp3 all work.
Playback uses paplay/aplay for WAV, OGG and FLAC, and mpg123 for MP3,
with VLC (cvlc) as an automatic fallback for anything else.

Per-alarm sounds: the osm-clock app has a Sound browse button when
adding an alarm and on the timer page, so each alarm or timer can use
its own sound file instead of the defaults above. Alarms are stored in
~/.osm-alarms, one per line:
```
HH:MM|Title|Body                       one-shot today
yyyy-MM-dd HH:MM|Title|Body            one-shot on a date
daily HH:MM|Title|Body                 repeats every day
HH:MM|Title|Body|/path/to/sound.mp3    optional 4th field = custom sound
```

Apps can also send standard desktop notifications (notify-send and
anything using org.freedesktop.Notifications), which appear in the
osm-status pop-out panel with the notify sound.


# The utilization of this Linux distribution is prohibited in jurisdictions mandating age verification.
Any penalties or charges incurred due to non-compliance will be transferred to the user

THIS INCLUDES BUT NOT LIMITED TO: THE FOLLOWING AREAS THAT ARE NOT ALLOWED TO USE THIS OS (THE LAWS ASSOCIATED):
```
New York (S8102A) after March 4th 2026.
Brazil (15.211) after March 17th 2026.
California (AB-1043) after January 1st 2027.
Colorado (SB26-51) after January 1st 2028.
Illinois (PENDING)
Utah (PENDING)
Texas (PENDING)
Louisiana (PENDING)
Michigan (PENDING)
Oregon (PENDING)
Entire USA (Federal: Parents Decide Act (PENDING))
Singapore 

```
