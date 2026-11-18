# Architecture

## The DVD

The 10.6 retail DVD is a hybrid disc: an ISO 9660 side (Boot Camp's Windows drivers) and an Apple
partition map whose `Apple_HFS` partition is the installer. `tools/extract.py`:

1. reads the partition map and copies the HFS+ partition out (`.local/media/MacOSX.hfs`);
2. takes packages out of `System/Installation/Packages` with 7z (HFS+);
3. reads each flat package (xar: a zlib'd XML table of contents, then the heap) and streams its
   `Payload` (gzip'd cpio, odc format; the TOC sometimes says octet-stream: the gzip magic
   decides) through `cpio` with only the wanted paths;
4. takes the pictures Photo Booth's effects use out of three compositions, then prunes what the
   patterns took beyond the shell's needs (cpio's `*` crosses `/`): the other languages' `.lproj`,
   Mach-O code, the `.qtz` files;
5. optimizes the JPEGs' Huffman tables (`jpegtran -optimize`, when there: the same pixels, ~5% less);
6. converts the plists and `.strings` files (UTF-16, OpenStep syntax) to JSON beside them.

It builds a fresh tree beside `.local/assets` and swaps it in when complete, so a run never leaves
files of an earlier one behind. Given `.local/media/MacOSX.hfs` (kept by the first run) instead of
the DVD, it extracts again without it. The result is about 150 MiB, half of it the desktop pictures.

| package | what the shell takes |
|---|---|
| BaseSystem | CoreUI's `ArtFile.bin`/`SArtFile.bin` (the controls' art), HIToolbox's pictures, CoreTypes' and the disks' icons and the types' names, LaunchServices' strings, Lucida Grande, Helvetica Neue, the .dfont fonts and Menlo, DefaultDesktop.jpg, SystemVersion.plist, AirPort.menu's pictures, Apple80211's ClientDialogs panels and security types' names, Utilities' icons, Parental Controls' pane; QuartzCore's menu bar Apple (32 x 22 RGBA, out of the library) |
| Essentials | Dock.app's, Finder.app's (nibs, canned searches), SystemUIServer's and the Menu Extras' resources; loginwindow's About This Mac; System Preferences' and its panes' and monitor panels' nibs, pictures and search terms; Preview's, Photo Booth's, Stickies' and TextEdit's resources; the fonts of /Library/Fonts (Stickies' notes); three compositions' embedded pictures; the User Template's PDFs; the applications' icons and Info.plist |
| MediaFiles | the desktop pictures |
| Mail | Mail.app's pictures, toolbars, English nibs and strings, its icon |
| Safari, iTunes, iCal, AddressBook, iChat, DVDPlayer | their icons and Info.plist |

Only English resources are kept. HIToolbox's `.rsrc`, CoreUI's `UI.bundle` recipes, the frameworks'
code and everything else stay in the packages: for reverse engineering, take them out of the
packages into `.local/media/re`, not from `.local/assets`.

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
shell/src/systemuiserver  SystemUIServer.app (AquaKit): the menu bar (the Apple menu, the applications'
                   menus: the Finder's, AquaKit's, the global menu over D-Bus), NSMenuToolbar's extras as
                   10.6.8 loads them (menuExtras and Autoload.plist), the Clock, Battery and Volume built
                   in, Spotlight (Search.bundle). The session ends with it
shell/src/systemuiplugin  SystemUIPlugin.framework (AquaKit): NSMenuExtra, NSMenuExtraView, a shared library
                   (apps/PrivateFrameworks/SystemUIPlugin.framework)
shell/src/menuextras  each .menu's Info.plist; AirPort and Bluetooth have code (apps/Menu Extras/<Name>.menu/
                   Contents/MacOS/<Name>, a library SystemUIServer loads)
shell/src/linux    Linux's glue, not a Mac framework: the session's windows (EWMH) and D-Bus (sd-bus) on
                   Foundation's run loop; qtstyle: AquaStyle as a Qt 6 style plugin for the session's Linux
                   applications (apps/Linux/qt6/styles/aquastyle.so)
shell/src/privateframeworks  the private frameworks' programs: Apple80211's ClientDialogs.bundle (AirPort's
                   panels, on AquaKit)

Each component's source folder has its **Info.plist**: Apple's identifier and version of the bundle
it reproduces, and two keys of ours, `SLReproducesSystem` (Mac OS X 10.6.8 (10K549)) and
`SLReproducesBundle` (the Mac path). The build copies it where the Mac has it (`Dock.app/Contents`,
`Menu Extras/AirPort.menu/Contents`, `Frameworks/AppKit.framework/Resources`...), so that each
component can be brought to a later release (Lion, Mountain Lion…) on its own, and what each one is
stays readable from the installed programs.
shell/src/dock     Dock.app
shell/src/finder   Finder.app: the desktop picture (Qt)
  browser          FinderWindows, on AquaKit: the Finder's windows (icon, list, column and Cover Flow views, sidebar, toolbar, status
                   bar), one process for all of them (a second launch hands its folders over an
                   abstract socket); their WM_CLASS "Finder", so the Dock and the menu bar take them
                   for the Finder's; Apps::finder() launches it
shell/src/frameworks  AquaKit: the Mac's frameworks without Qt, one reimplementation of each, shared
                   by every program as on the Mac, each its own library (so each can follow macOS's
                   releases on its own); `aquakit` is their umbrella, as Cocoa.framework is:
  Foundation       Assets (the DVD's files), .strings, the shell's settings files, NSKeyedUnarchiver
                   (archive) and NSUnarchiver's typedstream, NSRunLoop and NSTask (runloop)
  CoreGraphics     images (PNG, JPEG, TIFF, icns, PDF as NSPDFImageRep: poppler) and their colour
                   matching, template images tinted (graphics)
  CoreText         text as Quartz draws it, in Lucida Grande (text); the DVD's fonts (FontManager,
                   as CTFontManager)
  CoreUI           CoreUI's renderer (coreui): the controls', menus' and windows' art, its named images
  AppKit           windows, views, controls, the unified toolbar (Preview's and the Finder's), menus,
                   nibs; NSTextView (textview) and NSAttributedString's RTF / RTFD (attributedstring),
                   shared by Stickies and TextEdit; NSRulerView with the text ruler's accessory (rulerview); the standard panels (panels: NSOpenPanel and
                   NSSavePanel by kdialog or zenity, NSAlert as a window or a sheet, one instance per
                   application); on Wayland (xdg-shell, drawing 10.6's title bar itself) or X11 (platform_*.cpp)
shell/src/prefs    System Preferences.app, on AquaKit: Show All, every pane from its nib (nib.cpp), their Linux side
shell/src/loginwindow  loginwindow's About This Mac, on AquaKit: its nib, filled as AboutThisMacController does
shell/src/preview  Preview.app, on AquaKit: PDFs (poppler) and pictures, the toolbar from CommonToolbar.proplist,
                   the sidebar; the Dock's stacks open their documents in it (Apps::openFile)
shell/src/photobooth  Photo Booth.app, on AquaKit: the camera through Video4Linux (camera), the effects' compositions
                   on the processor (effects), ~/Pictures/Photo Booth and its Recents.plist (library)
shell/src/stickies  Stickies.app, on AquaKit: ~/Library/StickiesDatabase (NSArchiver's typedstream, database),
                   the notes' own frames (their text is AppKit's NSTextView and RTFD)
shell/src/textedit  TextEdit.app, on AquaKit: rich and plain text documents (RTF, RTFD, text in its
                   encoding), DocumentWindow.nib, Edit.nib's menus, Preferences.nib, AppKit's Find panel;
                   Wrap to Page through TextView's pages (MultiplePageView's geometry)
shell/src/windowserver  WindowServer: the window manager, 10.6's title bars; its compositor (compositor.cpp:
                   Composite, Damage and Render), which puts the windows together over their shadows
shell/src/systemuiserver/dbusmenu  the global menu: com.canonical.AppMenu.Registrar and dbusmenu (sd-bus)
session/           snowleopard-session: the X session
```

The programs talk through X11 only (the active window, the client list, struts) and through
`~/.config/snowleopard/*.conf`, which they watch (the Apple menu's Dock ▸ writes `dock.conf`, the
Dock reloads it).

WindowServer is the display's compositor (`_NET_WM_CM_S0`). Without one (another window manager, a
server without Composite) the Dock and the menu bar draw the desktop picture under themselves
(pseudo-transparency); the Dock and its stacks are shaped (X's SHAPE) to what they draw so that the windows
under them show around it (`X11::shapeToContent`); with one they are translucent windows. Windows
stop above the Dock: WindowServer publishes the struts' work area (`_NET_WORKAREA`), which AquaKit's
`App::visibleFrame()` reads.

## Next steps

1. **WindowServer's compositor** is there with the windows' shadows, the genie and scale effects and the
   minimized windows' tiles in the Dock; next sheets, the unified toolbars. The window
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

SystemUIServer's move from Qt to AquaKit, stage by stage: [systemuiserver-port.md](systemuiserver-port.md).
