# Architecture

## The DVD

The 10.6 retail DVD is a hybrid disc: an ISO 9660 side (Boot Camp's Windows drivers) and an Apple
partition map whose `Apple_HFS` partition is the installer. `tools/extract.py`:

1. reads the partition map and copies the HFS+ partition out (`.local/media/MacOSX.hfs`);
2. takes packages out of `System/Installation/Packages` with 7z (HFS+);
3. reads each flat package (xar: a zlib'd XML table of contents, then the heap) and streams its
   `Payload` (gzip'd cpio, odc format; the TOC sometimes says octet-stream: the gzip magic
   decides) through `cpio` with only the wanted paths;
4. converts the plists and `.strings` files (UTF-16, OpenStep syntax) to JSON beside them.

| package | what the shell takes |
|---|---|
| BaseSystem | CoreTypes' icons, Lucida Grande, Helvetica Neue, DefaultDesktop.jpg, Utilities' icons |
| Essentials | Dock.app's and Finder.app's resources, the Menu Extras' art, the applications' icons |
| MediaFiles | the desktop pictures |
| Safari, Mail, iTunes, iCal, AddressBook, iChat, DVDPlayer | their icons |

Everything lands in `.local/assets` at its Mac path, so the code names files as the Mac does
(`Assets::image("/System/Library/CoreServices/Dock.app/Contents/Resources/scurve-m.png")`).
Qt reads `.icns` (qt6-imageformats) and PDFs (the menu extras' art) directly.

## Reverse engineering

The shell is rebuilt from the frameworks' code (docs/reverse-status.md) and from the resources. Dock, SystemUIServer and
Finder are "protected" binaries in 10.6: their `__TEXT` segment carries
`SG_PROTECTED_VERSION_1` (encrypted). `tools/deprotect.py` decrypts a thin slice of your own copy
for analysis, locally (Blowfish, docs/reverse-status.md 13); nothing decrypted goes into this
repository. Besides their code: their resources (images, plists, `DockMenus.strings`,
`default.plist`), the documented `com.apple.dock` defaults, and the behaviour of the real system. Unprotected binaries
(HIToolbox, AppKit, CoreUI, the menu extras) are reversed with REA's Ghidra provider on their x86_64
slices; retool only reads ELF and PE.

## The programs

```
shell/src/common   aquashell, shared by every program
  assets           the DVD's files, images at a size, localized strings, Lucida Grande
  apps             .desktop entries; the Mac applications and the Linux programs standing for them
  x11              EWMH (or the root's children without a window manager), the shell's window types
  session          hide, quit, force quit, About This Mac, power, log out
  aqua             start-up: X11 platform, fonts, the menus' style
shell/src/menubar  SystemUIServer.app: the menu bar, the menu extras
shell/src/dock     Dock.app
shell/src/finder   Finder.app: the desktop picture (Qt)
  browser          FinderWindows, on AquaKit: the Finder's windows (icon, list, column and Cover Flow views, sidebar, toolbar, status
                   bar), one process for all of them (a second launch hands its folders over an
                   abstract socket); their WM_CLASS "Finder", so the Dock and the menu bar take them
                   for the Finder's; Apps::finder() launches it
shell/src/aquakit  AquaKit, for the Mac applications, without Qt: AppKit's skeleton on cairo and pango,
                   CoreUI's renderer (coreui), text in Lucida Grande, the shell's settings files;
                   on Wayland (xdg-shell, drawing 10.6's title bar itself) or X11 (platform_*.cpp);
                   the unified toolbar (toolbar.cpp: Preview's and the Finder's), CoreUI's named images
shell/src/prefs    System Preferences.app, on AquaKit: Show All, every pane from its nib (nib.cpp), their Linux side
shell/src/loginwindow  loginwindow's About This Mac, on AquaKit: its nib, filled as AboutThisMacController does
shell/src/preview  Preview.app, on AquaKit: PDFs (poppler) and pictures, the toolbar from CommonToolbar.proplist,
                   the sidebar; the Dock's stacks open their documents in it (Apps::openFile)
shell/src/windowserver  WindowServer: the window manager, 10.6's title bars
shell/src/menubar/appmenu  the global menu: com.canonical.AppMenu.Registrar and dbusmenu
session/           snowleopard-session: the X session
```

The programs talk through X11 only (the active window, the client list, struts) and through
`~/.config/snowleopard/*.conf`, which they watch (the Apple menu's Dock ▸ writes `dock.conf`, the
Dock reloads it).

Without a compositor the Dock and the menu bar draw the desktop picture under themselves
(pseudo-transparency); the Dock and its stacks are shaped (X's SHAPE) to what they draw so that the windows
under them show around it (`X11::shapeToContent`); with one they are translucent windows. Windows
stop above the Dock: WindowServer publishes the struts' work area (`_NET_WORKAREA`), which AquaKit's
`App::visibleFrame()` reads.

## Next steps

1. **WindowServer's compositor**: the windows' shadows on X11 (AquaKit draws them on Wayland), the genie and scale effects (the Dock's
   `MINIMIZE_USING`), minimized windows in the Dock, sheets, the unified toolbars. The window
   manager itself is there (docs/reverse-status.md, section 5).
2. **The global menu** is there (section 6); GTK 4 and GNOME applications (org.gtk.Menus) are next.
3. **The Finder**: its windows are there with their four views and `.DS_Store` (FinderWindows);
   next Undo and Put Back, the searches, then the desktop's icons
   (disks, `~/Desktop`).
4. **Aqua for Qt and GTK**, as TigerInstaller's QtAqua and gtkaqua: a Qt style and platform theme,
   a GTK theme, so that Linux applications draw with 10.6's controls.
5. **The applications**, each a program of its own on AquaKit (`System Preferences.app` and
   `Preview.app` are there; `TextEdit.app`... next).
6. **loginwindow** as a display manager, Spaces, Exposé, Dashboard.
