# SystemUIServer on AquaKit

SystemUIServer, the Dock and the Finder's desktop are Qt programs; the applications are on AquaKit's
frameworks (Foundation, CoreGraphics, CoreText, CoreUI, AppKit). The aim: one reimplementation of each
framework, shared by every program as on the Mac, each component separate so that it can follow
macOS's releases on its own. SystemUIServer goes first. Its real binary is not Qt (AppKit, HIToolbox,
SystemUIPlugin): it is decrypted (`tools/deprotect.py`, as the Dock and the Finder were) and reversed
with REA, and the port follows it.

Linux's own Qt applications keep 10.6's look: AquaStyle (shell/src/common/aquastyle.cpp, Qt's style
drawn from CoreUI's art) becomes a Qt style plugin the session gives every Qt program, so that a KDE or
Qt application opened in the session is drawn in Aqua, without the shell itself being Qt.

## What SystemUIServer needs that AquaKit hasn't

| Need | Today (Qt) | On AquaKit |
|---|---|---|
| The run loop: timers, file descriptors | QTimer, QSocketNotifier | Foundation's RunLoop (done) |
| Child processes (nmcli, bluetoothctl, pactl, System Preferences) | QProcess | Foundation's Task, NSTask (done; Software Update uses it) |
| Menus: a model (NSMenu, NSMenuItem: tags, states, images, submenus), changed while open (the AirPort scan), a view in an item (the volume's slider, the Finder's label row, the AirPort network rows), a delegate (menuWillOpen:, menuNeedsUpdate:) | QMenu, QWidgetAction, AquaStyle | AppKit's Menu is a fixed list of values: NSMenu to write |
| The bar: a window of type dock, its strut, above the others, its translucency (the window server's filter) | QWidget, X11 helpers | AppKit's X11 platform: the window type, the strut; CoreGraphics for the filter |
| NSStatusBar / NSMenuExtra: the extras' images (templates), their menus, their views | SystemUIPlugin (Qt) | SystemUIPlugin on AppKit, the .menu bundles ported one by one |
| D-Bus: the global menu (com.canonical.AppMenu.Registrar, dbusmenu), NetworkManager | QtDBus | a D-Bus library of its own (sd-bus), Linux's glue, separate from the Mac's frameworks |
| LaunchServices: the applications, their icons, launching them; the session (log out, sleep) | aquashell's Apps, Session (Qt) | LaunchServices and the session's calls without Qt |
| Spotlight's field | QLineEdit, QListWidget | AppKit's TextField and TableView |

## The stages

Each stage leaves the menu bar working; the Qt SystemUIServer stays until the AquaKit one does all it
does.

1. Foundation: RunLoop and Task (done).
2. Reverse the real SystemUIServer (10.6.8): decrypt it, its classes (the menu bar's extras host,
   SUISStartupObject, the status items' layout, the clock, Spotlight's place), what it loads and how
   (menuExtras, NSMenuExtra's protocol in SystemUIPlugin).
3. AppKit: NSMenu (the model, live changes, item views, the delegate), NSStatusBar's window (dock type,
   strut), template images.
4. SystemUIPlugin on AppKit; the extras ported: Clock, Battery, Volume, then AirPort and Bluetooth (with
   the D-Bus library).
5. SystemUIServer on AppKit: the bar, the Apple menu, the applications' menus (AquaKit's socket, the
   Finder's, the global menu over D-Bus), Spotlight; the Qt one removed.
6. AquaStyle as a Qt style plugin for the session's Qt applications.
7. Then the Dock, then the Finder's desktop, the same way.
