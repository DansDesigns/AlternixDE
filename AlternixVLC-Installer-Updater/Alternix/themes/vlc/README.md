# Alternix VLC assets for the AlternixDE repository

Commit the entire `Alternix/themes/vlc/` folder into AlternixDE.
The existing Alternix installer and updater will install these assets automatically.

- `Kvantum/AlternixVLC/`: **active native Qt/Kvantum styling** for VLC. Set on a per-app basis, without changing the system-wide theme.
- `Alternix-VLC.vlt`: an **optional legacy Skins2 alternative**, installed but NOT activated: selecting it switches off the native Qt player and reintroduces the fixed floating skin.
- `qt5-style/`: optional alternative Qt QStyle source; not compiled/activated because it cannot be active concurrently with Kvantum. Provided for later native plugin work.

The scripts put the Kvantum theme under `/usr/share/Kvantum/AlternixVLC/`, map `vlc` to `AlternixVLC` in the user's Kvantum configuration, set the VLC .desktop launcher to use the Kvantum style, and configure VLC to use native Qt with automatic window resizing disabled. Existing Kvantum settings for other applications are preserved.
