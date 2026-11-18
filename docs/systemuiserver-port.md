# SystemUIServer on AquaKit

SystemUIServer is on AquaKit (stage 5); the Dock and the Finder's desktop are still Qt programs; the applications are on AquaKit's
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
| Menus: a model (NSMenu, NSMenuItem: tags, states, images, submenus), changed while open (the AirPort scan), a view in an item (the volume's slider, the Finder's label row, the AirPort network rows), a delegate (menuWillOpen:, menuNeedsUpdate:) | QMenu, QWidgetAction, AquaStyle | AppKit's MenuItem and Menu (done: stage 3) |
| The bar: a window of type dock, its strut, above the others, its translucency (the window server's filter) | QWidget, X11 helpers | Window::setStatusBar (done: stage 3, X11); CoreGraphics for the filter |
| NSStatusBar / NSMenuExtra: the extras' images (templates), their menus, their views | SystemUIPlugin (Qt) | SystemUIPlugin on AppKit (done: stage 4), every extra ported (stage 5) |
| D-Bus: the global menu (com.canonical.AppMenu.Registrar, dbusmenu), NetworkManager | QtDBus | sd-bus on Foundation's run loop, Linux's glue (src/linux/dbus.h, done) |
| LaunchServices: the applications, their icons, launching them; the session (log out, sleep) | aquashell's Apps, Session (Qt) | CoreServices' LaunchServices (done); the windows through src/linux/windows.h; loginwindow's panels (done) |
| Spotlight's field | QLineEdit, QListWidget | AppKit's SearchField (done) |

## The stages

Each stage leaves the menu bar working.

1. Foundation: RunLoop and Task (done).
2. Reverse the real SystemUIServer (10.6.8) (done: reverse-status.md, section 19). What it changes
   for the port: the Clock, Battery and Volume extras are SystemUIServer's own classes (their .menu
   bundles keep only the resources); Spotlight is Search.bundle, loaded by SystemUIServer; the extras
   come from `menuExtras` and Autoload.plist (each entry's can-load method, its position from the
   right), allowed by their principal class; the items from 4 points apart by 8, an extra 22 points
   wide unless its Info.plist says (`NSMenuExtraWidth`).
3. AppKit (done): NSMenuItem's tag, action, indentation (10 points a level), a picture at its right
   (a template drawn white while highlighted), a view made when the menu opens (setView:, its own
   events); `Menu::update` changes the open menu (its items, its size, the highlighted item kept),
   `Menu::close`, `Menu::isOpen`; `Window::setStatusBar` (X11: a dock window, above, on every
   desktop, its strut); CoreGraphics' template images (`Image::tinted`). Still to come with stage 5:
   the status bar on Wayland (no layer shell in AquaKit yet).
4. SystemUIServer on AquaKit beside the Qt bar (done). The Qt program is now **MenuBar** (MenuBar.app:
   the bar the Mac's applications draw themselves, Spotlight, the Qt extras; the session ends with
   it); **SystemUIServer** is AquaKit's, as 10.6.8's: SUISStartupObject's startup list (`menuExtras`,
   then Autoload.plist's entries by their can-load method and `-mapPosition:`, `dontAutoLoad`; the
   list written back, left to right, which MenuBar reads too), the 36 principal classes it allows
   (read from the binary, 0x10006b140), NSMenuToolbar's layout (from 4 points, each at its length + 8,
   its place 4 points wider each side), in a transparent Status window left of Spotlight's place.
   SystemUIPlugin on AquaKit (src/systemuiplugin): NSMenuExtra (`NSMenuExtraWidth` or 22, its image
   and alternate image, its menu, menu down), NSMenuExtraView, the classes by name. The Clock is
   SystemUIServer's own as on the Mac (AppleClockExtra: CLKDigitalView at 14 points, the cell's width
   its length, every second with seconds or flashing separators; CLKAnalogView from Clock.pdf and the
   reversed hands; its menu from Clock.menu's strings). The window tells MenuBar what it hosts (the
   root's `_SNOWLEOPARD_SYSTEM_STATUS`: its id, its width, its bundles); MenuBar leaves those extras
   and the place, and takes them back when the window goes (its DestroyNotify); WindowServer keeps
   the window over the bar (kCGStatusWindowLevel over kCGMainMenuWindowLevel). The session restarts
   SystemUIServer if it ends, as launchd does.
   Still to come: Battery and Volume into SystemUIServer (the Mac's own classes too), AirPort and
   Bluetooth as AquaKit bundles SystemUIServer loads (dlopen, SYSTEMUI_REGISTER_EXTRA; with the D-Bus
   library), the Clock's preferences in Date & Time's Clock tab, Command-drag of the extras.
5. SystemUIServer takes the whole bar (done); the Qt MenuBar and the Qt extras are gone. On Linux
   SystemUIServer draws the bar the Mac's applications draw themselves: the Apple menu, the active
   application's name and menus (the Finder's MenuBar.nib commands; an AquaKit application's
   MainMenu.nib over its socket; a Linux application's over D-Bus, SystemUIServer being
   com.canonical.AppMenu.Registrar; else the common ones), sliding from one menu to the next (AppKit's
   `Menu::trackOutside`), over the window server's filter of the desktop (menubarfilter.cpp) or
   CoreUI's art. NSMenuToolbar is a view of the bar. Spotlight is Search.bundle's MDStatusItem (its
   magnifier and strings from the DVD), loaded first, at the edge. BatteryExtra and AppleVolumeExtra
   are SystemUIServer's own as on the Mac (reversed: -createMenu, -updateMenu, the menu bar's
   string, BatteryImageCreator's picture; the volume's pictures and slider); AirPort.menu and
   Bluetooth.menu are bundles with code on AquaKit, loaded by SystemUIServer (dlopen) and registering
   their principal classes, on NetworkManager (nmcli, sd-bus) and BlueZ. Linux's glue (src/linux):
   the windows (EWMH) and D-Bus, without Qt. CoreServices' LaunchServices (the .desktop entries and
   the Mac applications they stand for). loginwindow's Restart…, Shut Down… (LogOutAlert, their
   strings) and Force Quit (ProcessPanel.nib), which the Qt bar had as Qt dialogs.
   Then: Command-drag of the extras (along the bar, or off it to remove one: `menuExtras`,
   `dontAutoLoad`); Date & Time's Clock tab (ClockPref.nib's bindings: the DateFormat it makes,
   IsAnalog, FlashDateSeparators, the clock shown or not) and Energy Saver's "Show battery status in
   the menu bar", which SystemUIServer follows as they change; Volume's view from MenuItem.nib (30 x
   104, the slider at {4, 2}, 21 x 100) and its Option-click device menu; Spotlight's files (Baloo's
   index, else plocate's, grouped as Documents, Folders, PDF Documents, Images, CoreTypes' icons);
   Force Quit's applications without a window.
   Still to come: the bar on Wayland (no layer shell in AquaKit, and no window list for it: the
   session runs on its X server, Xwayland under a Wayland login).
6. AquaStyle as a Qt style plugin (done): src/linux/qtstyle, apps/Linux/qt6/styles/aquastyle.so,
   which the session gives every Qt 6 program (QT_PLUGIN_PATH, QT_STYLE_OVERRIDE=Aqua): CoreUI's
   controls (a push button taller than the capsule as the square bevel button, as Qt's own Mac style
   does), Lucida Grande 13, an Aqua palette whatever the desktop's colours, no mnemonics' underlines.
   Still to come: Qt 5's applications (the same plugin built against Qt 5), GTK's (a theme of their own).
7. Then the Dock, then the Finder's desktop, the same way.
