# Snow Leopard

A Mac OS X 10.6 desktop for Linux, written in C++ with Qt 6, cairo and pango. It contains no Apple code:
the resources (icons, fonts, desktop pictures, nibs, strings) are read from your own Snow Leopard install DVD.

![Desktop](docs/images/desktop.jpg)

## Screenshots

| | |
|---|---|
| ![Finder](docs/images/finder.png) | ![System Preferences](docs/images/system-preferences.png) |
| ![Preview](docs/images/preview.png) | ![Dock](docs/images/dock.png) |

## Features

- Menu bar (SystemUIServer): Apple menu, application menus (global menu over D-Bus), Spotlight, clock, battery, volume, AirPort and Bluetooth
- Dock: glass floor, magnification, Stacks, drag and drop, Trash
- Window server: 10.6 title bars, buttons and shadows, on X11 or as a Wayland compositor of its own
- Finder: desktop icons, icon, list, column and Cover Flow views, Get Info, View Options, Quick Look, labels, copy and move, `.DS_Store` support
- System Preferences: all panes, Desktop & Screen Saver
- Preview: PDF and image viewer
- About This Mac, and Software Update (Mac OS X 10.6.8 from Apple's catalog)
- Mail: IMAP and SMTP (early)
- iTunes 8.2.1 and 11.4, TextEdit, Stickies, Calculator, Photo Booth
- Grab and screen shots (⇧⌘3, ⇧⌘4), Activity Monitor, Help Viewer
- Linux applications in the Dock and the menu bar

## Requirements

- Linux with X11 or Wayland (tested on CachyOS / Arch Linux)
- A Mac OS X 10.6 Snow Leopard install DVD image
- Build dependencies:

```sh
sudo pacman -S --needed cmake qt6-base qt6-imageformats libxcb xcb-util-keysyms cairo pango libjpeg-turbo libtiff lcms2 zlib libxkbcommon wayland wayland-protocols poppler-glib 7zip cpio python-xlib
sudo pacman -S --needed qt6-webengine   # Help Viewer
sudo pacman -S --needed mpv taglib curl   # iTunes (curl: its podcasts, radio and Genius, found on the PATH)
```

## Usage

Run it in a window of your current desktop:

```sh
git clone https://github.com/AshesTom/SnowLarpard.git
cd SnowLarpard
./sl.sh /path/to/snowleopard.iso
```

The first run extracts the DVD files and builds the programs. Later runs: `./sl.sh --`.
A window size can be given: `./sl.sh -- 1600x1000`.

To add **Snow Leopard** to the login screen (SDDM, GDM, Plasma Login, LightDM...), once the DVD files
are extracted: `./install.sh` (`./install.sh --uninstall` to remove it). It runs natively on Wayland, with
a "Snow Leopard (X11)" entry as an option.

To use it as an X session instead, add `exec /path/to/session/snowleopard-session` to `~/.xinitrc`.
Restart and Shut Down only act on the computer when `SNOWLEOPARD_POWER=1` is set.

⌘ is the Super key; in the shell's own applications Control works as ⌘ too.

Settings are stored in `~/.config/snowleopard/`, using the Mac's own keys (`dock.conf` for `com.apple.dock`).

## Project layout

| Folder | Contents |
|---|---|
| `shell/src/systemuiserver` | Menu bar (SystemUIServer), its extras in `menuextras/` |
| `shell/src/dock` | Dock |
| `shell/src/windowserver` | Window server (X11, and `wayland/`) |
| `shell/src/finder` | Finder |
| `shell/src/frameworks` | AquaKit: Foundation, CoreGraphics, CoreText, CoreServices, CoreUI, AppKit |
| `shell/src/linux` | Linux glue: windows, D-Bus, the Aqua style for Qt applications |
| `shell/src/prefs`, `preview`, `mail`, `itunes`, `textedit`... | Applications |
| `session/`, `install.sh` | The sessions and the login screen entry |
| `tools/extract.py`, `tools/softwareupdate.py` | DVD extraction, Software Update |

Documentation: [architecture](docs/architecture.md), [reverse engineering status](docs/reverse-status.md),
[iTunes](docs/itunes-reverse.md), [10.6.8 update](docs/update-10.6.8.md).

## Notice

Experimental, unofficial project, not affiliated with Apple. No Apple files are distributed: you must provide
your own Snow Leopard DVD. Extracted files and the build stay in `.local/`.

## License

LGPL-2.1-or-later, for the code in this repository only. Mac OS X and its resources are copyrighted by Apple.
