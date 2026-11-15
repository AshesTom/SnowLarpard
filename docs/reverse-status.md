# Reverse-engineering status

What was recovered from Snow Leopard's own binaries and files, and what the shell does with it. No
Mac code runs and no Apple file is in the repository: the shell reads the DVD's files at run time
(`tools/extract.py`), and re-implements what the code below does.

Tools: REA with its Ghidra provider (12.1), on x86_64 slices taken out of the universal binaries.
retool reads ELF and PE only. Verification: the session in Xvfb, driven through XTEST and captured
with ImageMagick.

| Mark | Meaning |
|---|---|
| Done | Recovered from the code or the resources, ported, and tested |
| Partial | Ported in part, or from indirect evidence |
| Missing | Not ported |

## 0. What can be read

| Binary | `__TEXT` | Notes |
|---|---|---|
| Dock, Finder, SystemUIServer | protected (`SG_PROTECTED_VERSION_1`, both slices) | not decrypted: their resources and observed behaviour only |
| HIToolbox, AppKit, CoreUI | clear | reversed below |
| Menu Extras (Battery, Displays, Bluetooth, Spaces, TimeMachine, User, Eject, ...) | clear | to do |
| SystemUIPlugin (the menu extras' base classes) | clear | to do |

## 1. CoreUI's shared art (`SArtFile.bin`)

Reader: `CUISharedArtReader::Setup` (0x35758), `GetImageHeader` (0x35890),
`CreateImageFromBytes` (0x35982). Port: [`art.cpp`](../shell/src/common/art.cpp).

| Item | Source | Status |
|---|---|---|
| Header: u16 version, u16 count, u32 data offset; u32 entry offsets from 8 | `Setup` | Done |
| Image ID n = entry n − 20000, valid for 0 < n − 20000 < count | `GetImageHeader` | Done |
| Representation r at entry + 12r: width +0xC, height +0xE, length +0x10, offset +0x14 (from the data offset) | `CreateImageFromBytes` | Done |
| Pixels: 8 bpc, 32 bpp, rows of width × 4, alpha premultiplied first (`CGImageCreate` bitmap info 2), generic RGB | `CreateImageFromBytes` | Done |
| `ArtFile.bin`: u16 elements, u16 key length (8), u16 tokens; u32 token table, token names, path table, data (from 8) | `ArtFileSetupOnce` (0x3424d) | Done |
| Tokens: u32 name offset, u8 value; a path's key: the widget's token first, then the others ascending, 0-padded (checked on all 3324 keys) | `LoadFromArtFile` (0x34618), the path table | Done |
| Element record: u16 rows, u16 columns, margins; piece offsets (u32, from the record) at +0x20, widths at +0x44, heights at +0x56; pieces as in SArtFile | `LoadFromArtFile` | Done ([`artfile.cpp`](../shell/src/common/artfile.cpp)) |
| Gamma (CUIGamma, ColorSync) applied to the pieces | `ArtFileSetupOnce` | Missing |

## 2. The menu bar

| Item | Source | Status |
|---|---|---|
| The bar: shared art 20102 (1 × 21) stretched over the width | `CUIRenderer::DrawMenuBar` (0x1b630) | Done |
| Variant 0xC5: art 20116, then black at alpha 0.55 over it | `DrawMenuBar` | Missing (no caller found yet) |
| Translucency (with a graphics card): the window server's standard filter, `CIMenuGenerator`, made by `_CGXSetMenuBarFilter` (CoreGraphics 0x29099d) with inputBarWhiteParams (0.6, 0.25), inputBarRampParams (0.35, 0.15, 0, 0.75), inputBarTopLine 0.5, inputBarRadius 1.6, inputBarColor 0 | CoreGraphics | Done |
| `CIMenuGenerator::outputImage` (QuartzCore 0xff062): the bar from `CIMenuBarGenerator` over the desktop, the Apple from `CIMenuAppleGenerator` over it | QuartzCore | Done (the bar) |
| `CIMenuBarGenerator::outputImage` (0xfdbf9): the desktop blurred, its average over the bar, and the `menuBar` kernel (a fragment program stored with each byte's high bit set): the white veil and the lift set by the average's brightness, the lift fading from the top as a 0.75 power, the top line, the bottom 1.5 px as the blurred desktop at 0.59. Gamma-encoded RGB: checked on a 10.6 screenshot (Aurora), within 3 levels per row | QuartzCore | Done ([`menubarfilter.cpp`](../shell/src/menubar/menubarfilter.cpp)) |
| The Apple over the translucent bar: `CIMenuAppleGenerator::outputImage` (QuartzCore 0xfe28b) and its `menuApple` kernel: the desktop blurred (8) and darkened (black params 0.3 / 0.075 by brightness), lighter downwards (ramp 0.75 / 0.5, 0.5, power 0.75), a shine past a rounded corner (radius 8 around 7.5, 4) growing as the 2.75 power downwards (0.6 / 0.85), an inner shadow (the Apple's alpha blurred by 1.5, inverted), kept inside the Apple, over a white copy at 0.6 one pixel lower; at x 17 | QuartzCore | Done (checked against a 10.6 screenshot) |
| The Apple's picture: `_imageApple`, 32 x 22 RGBA in QuartzCore's `__TEXT,__const` (an 88 px one, `_imageApple88`, for taller bars); `extract.py` takes it from your DVD through the Mach-O symbol table | QuartzCore | Done |
| Translucency off (HIToolbox's preference `AppleEnableMenuBarTransparency`, here in `~/.config/snowleopard/menubar.conf`): the measured opaque look below | HIToolbox strings; screenshots | Done |
| `ResetMenuBarWindowFilter` (HIToolbox 0x25e2e) asks the application for a filter (event 'menu' 2010, parameter 'mbfi') and otherwise passes nil to `CGSSetMenuBarFilter`: the default filter is the window server's, not in HIToolbox | HIToolbox | Partial |
| What that filter gives, measured on two 10.6 screenshots (different desktops, same result): the art as if over a backing white to mid-height, then grey down to 150; nothing of the desktop shows | measured | Done |
| The bar's 22nd row (its edge): grey 75, opaque | measured | Done |
| Selected title: art 20103 (Graphite theme: 20104) stretched over the title | `CUIRenderer::DrawMenuTitle` (0x1e460), state 0xA8 | Done (Blue only) |
| The Apple menu's title (title kind 0xCF): art 20061, black, 22 × 22 (open: 20062, white), centred, 1 px right | `DrawMenuTitle` | Done |
| HIToolbox's `HiResAppleMenu*.png` (blue, 76 × 88) | HIToolbox resources | Not 10.6's menu bar Apple: left over from earlier systems, unused |
| CoreUI's menu bar recipe: `selectiontab` (`menubrtab`, sides `menutabsides1`), `search` (`searchtab`, `searchheader`) | `UI.bundle/Contents/Recipes/menubar.xml` | Partial: read; used by Spotlight's tab later |

## 3. The menus

| Item | Source | Status |
|---|---|---|
| Selected item: art 20114 (Graphite: 20115) stretched over the item | `CUIRenderer::DrawMenuItem` (0x1b942), state 0xA8 | Done (Blue only) |
| Item kinds 200 / 0xCA: two fills (colours 0xE1, 0xE3) and a separator line (0xE2) at x + 128 (or the descriptor's value) | `DrawMenuItem` | Missing: CoreUI's colour table not read yet |
| Kind 0xC9: art 10050 in one or three pieces | `DrawMenuItem` | Missing |
| The Dock's menus: see section 4 | `CUIRenderer::DrawDockMenu` (0x8610) | Done |
| The menus' background | `CUIRenderer::DrawMenu` (0x8df4) | Missing |

## 4. The Dock

The Dock binary is protected; what CoreUI draws for it and the Dock's own resources are used.
Port: [`dock.cpp`](../shell/src/dock/dock.cpp), [`dockmenu.cpp`](../shell/src/dock/dockmenu.cpp).

| Item | Source | Status |
|---|---|---|
| Menus: nine pieces (`Prepare9PieceInstructions`), shared art 20074 20075 20076 / 20070 20069 20072 / 20065 20066 20067 (top to bottom as stored); corners 33 × 33, edges 1 × 33 and 33 × 1, centre 1 × 1 (21, 21, 21, alpha 217) | `DrawDockMenu` | Done |
| Callout by the Dock's edge (descriptor +0x70): 0x3E → 20068 (points down, horizontal layout), 0x43 → 20077 (up), 0x40 → 20071, 0x42 → 20073 (vertical layouts) | `DrawDockMenu`, `LayoutCalloutHorizontal`/`Vertical` | Done for the bottom Dock |
| The body's insets in the pieces: 26 left and right, 21 top, 23 bottom; the callout inside the bottom row, its tip 9 px above the window's bottom | measured on the art (alpha reaches 217) | Done |
| The callout's tip about 12 px above the icon | measured on a 10.6 screenshot | Done |
| Submenus (Options ▸): the same frame without a callout | observed | Partial |
| Running lights: `indicator_large.png` (42 × 14) from 60 px icons, `indicator_medium.png` under that, at the screen's edge | Dock resources; observed | Partial: thresholds chosen by eye |
| The floor: `scurve-*.png` on a trapezoid, `frontline.png`, `separator.png`; proportions measured on a 10.6 screenshot (64 px icons: floor 0.82 icon deep, icons a third in, front edge 0.75 floor-depth wider each side) | Dock resources; observed | Partial |
| Magnification, bounce, labels | observed | Partial: curves chosen by eye |
| Stacks: Applications, Documents, Downloads and their settings (arrangement 1-5, displayas, showas) from the Dock's `default.plist`; yours in dock.conf's [stacks] | Dock resources | Done |
| A stack's tile: its first three items piled (the first on top, the others turned -7 and 6 degrees), or its folder's icon | observed | Partial: angles by eye |
| The items' icons: the applications', the pictures' thumbnails, the documents' of Preview, TextEdit, iTunes, QuickTime Player, Font Book, Mail and Safari (a download in progress), CoreTypes' folders and generic document | the applications' resources | Done |
| The fan: 48 point icons 54 apart from the tile, bending right, as many as fit under the menu bar (fifteen at most), the last place then "%@ More in Finder" with the Dock's `openinfinder` arrow (the grid's Open in Finder too); Automatic picks it while everything fits (a dozen at most), the names on dark lozenges over the Dock's `stackitemshadow` | observed | Partial: the curve by eye |
| The grid: the Dock's `stackbackground-*` panel and its tip, the folder's name, 56 point icons in 96 x 94 cells, two-line names, scrolling, subfolders inside it (◀ back), "Open in Finder" | Dock resources; observed | Partial: the cells by eye |
| The list: a Dock menu of the items, folders as submenus | observed | Done |
| The menu: Sort by, Display as, View content as (DockMenus.strings), Open "%@"; Automatic: a fan up to 12 items, a grid after | DockMenus.strings | Done |
| Downloads hops once when a file comes into it | observed | Done |
| A new account's PDFs: "About Stacks.pdf" in Documents, "About Downloads.pdf" in Downloads (the DVD's User Template), put there once by the Dock, never over a file | User Template | Done |

## 5. The windows (WindowServer)

Port: [`windowserver.cpp`](../shell/src/windowserver/windowserver.cpp). The title bar's gradient is AppKit's
code (`NSThemeFrame`), not art: it was measured.

| Item | Source | Status |
|---|---|---|
| Close, minimize, zoom: `titlebarcontrols` + `close`/`minimize`/`zoom` + `colorsryg` + `regularwin`, 14 × 16; states none, `rollover` (the glyphs, all three together), `pressed`, `activenokey`, `inactive`, `disabled`; `graphite`, `closedirty`, `utilwin` variants | ArtFile.bin | Done (Blue, regular windows) |
| Title bar: 22 px; first row 255, then 239 down to 217, a 153 line over the content; content 238 | measured on a 10.6 screenshot | Done |
| Inactive title bar | not measured | Partial: estimated lighter |
| The buttons' places (8 px in, 21 apart, 3 down), the title in Lucida Grande 13, engraved | by eye | Partial |
| Toolbar button (`titlebarcontrols` + `toolbar` + `colorsrgb` + `regularwin`, 20 x 13): 7 px from the right, 5 down; it shows and hides the toolbar (`_SNOWLEOPARD_TOGGLE_TOOLBAR` to the window). The proxy icon: 16 x 16, 4 px before the title, the two centred together, half faded when inactive (`_SNOWLEOPARD_PROXY_ICON`) | ArtFile.bin; measured on a 10.6 screenshot | Done |
| The windows' corners: the top ones rounded, radius 4, the bottom ones too for a textured window (`kCUIWindowFrameRoundedBottomCornersKey`; the Finder's); WindowServer shapes its frames (X SHAPE, whole pixels), AquaKit clips its own on Wayland | `CreateWindowPath(CGRect, bool, bool)` (CoreUI 0x1e65): arcs of radius 4.0 (0x3f078), π/2 and π | Done |
| The shadows: the key window's standard deviation 20, density 0.49, 15 under it (stretch 6); another's 10, 0.44, 10 under (stretch 3); a window of the other style 8, 0.5, 6 under. AquaKit draws them around its windows on Wayland (a blurred rectangle, the surface's window geometry and input region without it); on X11 WindowServer's compositor draws them under every frame (the key window's, another's, the other style's for panels and menus), `windowserver/compositor.cpp` | `__NSSetWindowShadow` (AppKit 0x1db12): `CGSSetWindowShadowAndRimParametersWithStretch` and its floats at 0x836f98-0x836fac, 0x8380b0, 0x835654 | Done (the stretch not kept; a blurred rectangle for the frames, the blurred alpha for translucent menus) |
| Sheets, the genie effect | observed | Missing |

## 6. The global menu

Port: [`appmenu.cpp`](../shell/src/menubar/appmenu.cpp). Not reversed from the Mac: the freedesktop
protocols Linux applications speak.

| Item | Status |
|---|---|
| `com.canonical.AppMenu.Registrar` in SystemUIServer: RegisterWindow, UnregisterWindow, GetMenuForWindow; an application's windows dropped when it leaves the bus | Done |
| KDE's `_KDE_NET_WM_APPMENU_SERVICE_NAME` / `_OBJECT_PATH` window properties | Done |
| `com.canonical.dbusmenu`: GetLayout, AboutToShow, Event "clicked", LayoutUpdated, ItemsPropertiesUpdated; labels, separators, submenus, check and radio items, shortcuts shown as ⌃⌥⇧⌘ | Done (checked with Konsole: its menus, New Window) |
| GTK applications (appmenu-gtk-module: the session sets `GTK_MODULES`) | Partial: not tried yet |
| GetMenus; GTK 4's and GNOME's org.gtk.Menus (GMenuModel) | Missing |

## 7. Desktop & Screen Saver, the desktop's menu

Binaries: `DesktopPictures.prefPane` and `ScreenEffects.prefPane` inside `DesktopScreenEffectsPref.prefPane`
(clear); the Finder (protected). Port: [`desktoppane.cpp`](../shell/src/prefs/desktoppane.cpp),
[`desktopsettings.cpp`](../shell/src/common/desktopsettings.cpp), [`desktop.cpp`](../shell/src/finder/desktop.cpp).

| Item | Source | Status |
|---|---|---|
| The pane's size (668 wide) and its controls | `DesktopScreenEffectsPref.nib`, `DSKDesktopWindow.nib` (keyed archives, decoded; the latter is the secondary displays' window) | Done |
| The main Desktop tab's layout: the well 128 x 86, the name beside it, the box 603 x 253, five thumbnails a row 82 x 55 apart, the controls under it, the help button; the unified title bar and toolbar (218, then 207 to 167 over 53 points, a 97 line, a 188 row), back / forward, Show All, the search field | measured on a 10.6 screenshot | Done |
| No placement for Apple's pictures | `DSKAppleRootSource::disableCustomPlacement` | Done |
| Apple's sources and their order: /Library/Desktop Pictures, then Nature, Plants, Art, Black & White, Abstract, Patterns, Solid Colors (the colour panel's icon, shadowed thumbnails); "Folders": Pictures and yours | `DSKAppleRootSource::init` (0x38a3), the CFStrings at 0x1f7d0 to 0x1f9b0; Localizable.strings | Done |
| Placements: Fill Screen, Fit to Screen, Stretch to Fill Screen, Center, Tile (DSK 1 to 5 to DesktopPicture 1, 5, 4, 3, 2); the colour well for the ones that don't cover the screen | `_DSKPlacementConversion` table; the popup | Done |
| Change picture: when logging in, when waking from sleep, every 5 seconds, minute, 5, 15, 30 minutes, hour, day; random order | the popup; `_DSKChangePictureConversion` | Done (waking from sleep: missing) |
| Translucent menu bar | the checkbox; HIToolbox's AppleEnableMenuBarTransparency | Done |
| The Screen Saver tab's layout: the list, the preview (347 x 246), Options, Test, Start screen saver's stops (3, 5, 15, 30 min, 1, 2 h, never), Hot Corners | `ScreenSaverPref.nib` | Partial: the savers are slideshows of the picture collections (10.6's .saver bundles are Mac code); Options, Hot Corners, clock: missing |
| The desktop's contextual menu: New Folder | Get Info | Change Desktop Background…, Arrange By ▸, Show View Options (Clean Up only with icons on the desktop) | the Finder's Localizable.strings (N156, N165, N164, N148, N169.5); the order and the items as seen on 10.6 (the code is protected) | Done (Get Info: a simple window; Arrange By, Show View Options: for the desktop's icons, later) |

## 8. Aqua for Qt

Port: [`aquastyle.cpp`](../shell/src/common/aquastyle.cpp), a Qt style drawing the controls with ArtFile.bin's
elements (section 5's reader), as AppKit draws them through CoreUI.

| Control | ArtFile path (+ state: active, inactive, pressed, disabled) | Status |
|---|---|---|
| Menus | `menu/allround` (contextual), `menu/straighttop` (the menu bar's), 3 x 3; `menu/selection`; items 21 points, separators 12 (two rows, 242 and 224), text 21 in (measured) | Done |
| Check boxes | `checkbox/clear/{off,onchecked,mixed}/regular`, 18 x 18 | Done |
| Push buttons | `pushbutton/{clear,blue}/regular`, 1 x 3, 32 high (blue: the default button) | Done |
| Toolbar buttons (NSSegmentStyleTexturedRounded) | drawn in code, not from the art (`segment/fortexture` is the older metal look): a 1 point border 88 to 104, the face 253 to 186, selected or pressed dark (55 under the top, 100 five points down, 116 at the bottom) with its template white, 1 point separators (104, 66 beside a dark one), a white emboss under it, corners of 4 | measured on a 10.6 screenshot | Done (inactive window estimated) |
| Popup buttons | `popup/{blue,clear}/regular/rightsidebutton`, 1 x 2: the left end (its last column stretched) and the arrows | Done |
| Tabs | `tabs/{blue,clear}/horizontal/regular`, one segmented control; `tabpane/opaque` 3 x 3 under them | Done |
| Scroll bars (vertical) | `scrollbars/.../track/vertical`, `.../blue/regular/thumb/vertical`, the arrows together at the bottom (`arrows/bottom/double`) | Done (horizontal: Fusion's) |
| Disclosure triangles, the help button, the round search field, the image well | `disclosuretriangle/{open,close}`, `roundbutton/help/regular`, `textfieldround/fortoolbars/regular`, `imagewell/active` | Done |
| Radio buttons, sliders, progress indicators, list headers, focus rings | in ArtFile.bin | Missing |

## 9. AquaKit: CoreUI's renderer without Qt

The Mac applications (System Preferences, Preview) don't use Qt: [`aquakit`](../shell/src/aquakit) is a small
AppKit on xcb, cairo and pango, and its [`coreui.cpp`](../shell/src/aquakit/coreui.cpp) is CoreUI's renderer,
reversed from the x86_64 slice. The art is section 1's (ArtFile.bin, SArtFile.bin), read at run time.

| Item | Source | Status |
|---|---|---|
| A piece at its own size in the bounds: alignment 3 at the minimum edge, 2 at the maximum, otherwise `floor((bounds - piece) / 2)` | `__CUICalcAlignedBounds` | Done |
| 1 x 3: the ends at their size (left aligned 3, right 2), the middle stretched between them (`CGContextDrawImages`) | `Draw3PieceHorizontalWithImages` (0xa8fc), `CUIPenCG::DrawImages` | Done |
| 1 x 2 (popups): the first piece cut into its left end and its last column, stretched; the second piece is the right end | `Draw3PieceHorizontalWith2Images` (0xadfa) | Done |
| 3 x 3: corners at their size, edges and centre stretched | `Draw9Piece` (0x162da), `Prepare9PieceInstructions` | Done |
| Segments (tabs, textured toolbar segments): the left end only on the first, the right end only on the last; a segment after another starts with its `…/separator` element's middle piece (1 point) | `DrawSegment` (0xd984) | Done |
| Sliders: the track centred, 5 points towards the thumb's back (6 when it points up or left); the thumb at `round((length - thumb) * value)`; ticks every `(length - thumb) / (n - 1)` from `floor(thumb / 2)`, 10 points past the track | `DrawSlider` (0xf3b4), `_kTickOffsetHAdjustment` (0x3f020) | Done (horizontal, pointing down) |
| Focus ring: `CGFocusRing` in `_CUIColorGet(0x3E)` (Graphite 0x3F), alpha 0.8, about 2 points out, around the control | `BeginFocus` (0x8380), `kColorTable` (0x3fb60, 255 RGBA doubles) | Partial: the ring's falloff is approximated |
| Colours: 0x3E focus (75, 137, 208), 0x3F Graphite focus, 0xE1 / 0xE2 / 0xE3 the menus' fills and separator | `_CUIColorGet`, `kColorTable` | Done |
| Controls from the art: push buttons, check boxes, radio buttons, popups and their menus, tabs and the tab pane, textured segments, the help button, the round search field, the image well, the vertical scroller (arrows together at the bottom), sliders, disclosure triangles | ArtFile.bin | Done |
| The small square buttons (+ and - under lists), the colour well's bezel | AppKit draws them, not CoreUI | Partial: measured |
| Text: Lucida Grande from the DVD, unhinted, fractional advances | pango, fontconfig | Done |
| System Preferences ▸ Desktop & Screen Saver on AquaKit, checked against a 10.6 screenshot (1440 x 900 scaled to 1280): the sources' rows 20 points, the grid 82.6 x 55 from (10, 7), the search field holding the keyboard when the window opens | measured | Done |
| Wayland: xdg-shell windows in shared memory, menus as `xdg_popup`s with their grab, the keyboard through xkbcommon with its repeat; 10.6's title bar drawn by AquaKit as WindowServer draws it (the unified gradient, CoreUI's titlebarcontrols, moving by `xdg_toplevel.move`). `AQUAKIT_CAPTURE=file.png` writes each frame, for checking without a screen | | Done (window and screen saver checked on a headless KWin; menus and typing not driven there) |
| The colour panel (the well opens a menu of the Solid Colors collection's colours), the open panel (+ asks zenity or kdialog) | | Partial |

## 10. System Preferences: every pane, from its nib

Port: [`archive.cpp`](../shell/src/aquakit/archive.cpp), [`nib.cpp`](../shell/src/aquakit/nib.cpp) (AquaKit's NSNib),
[`prefs/`](../shell/src/prefs). The panes' nibs, pictures and strings are the DVD's (`tools/extract.py`), read at run time;
their code is not: each pane's behaviour on Linux is written against the outlets and actions its nib names.

| Item | Source | Status |
|---|---|---|
| Binary plists and NSKeyedArchiver archives ($objects, UIDs, $classes) | bplist00, `keyedobjects.nib` | Done |
| A nib's objects as AquaKit views: NSTextField (label, field, secure, wrapping), NSButton (push, check box, radio, help, textured, square, round, disclosure, image), NSPopUpButton (and pull-down), NSSlider, NSBox, NSTabView, NSMatrix, NSImageView, NSScrollView / NSTableView (columns, header, check boxes), NSProgressIndicator, NSSegmentedControl, NSStepper, NSComboBox, NSLevelIndicator, NSColorWell, NSDatePicker (field, calendar, clock), SFAuthorizationView, AddRemoveView | the archives' keys (decoded from the 108 nibs: NSCellFlags, NSButtonFlags2's bezel, NSAlternateImage NSSwitch / NSRadioButton...) | Done |
| The File's Owner's outlets and the controls' actions (NSNibOutletConnector, NSNibControlConnector) | NSIBObjectData's NSConnections | Done |
| The texts a nib leaves for its code ("<< do not localize >>", "<do not loc>") hidden; hidden tabs | the nibs | Done |
| Show All: NSPrefPaneGroups.xml's groups and order, each pane's label (InfoPlist.strings), icon; Trackpad, Ink and Fibre Channel only with their hardware | System Preferences.app's resources, the panes' Info.plist | Done (layout by eye: no 10.6 picture of Show All measured yet) |
| Spotlight's search: the panes' `.searchTerms`, the window dimmed, `spotlight.tif` (the shade around the light: its alpha inverted) on each match; Return opens the first | the resources | Done |
| Back, forward, Show All; the window resized to each pane; Show All at launch (`applicationDidFinishLaunching:` sends `showAll:` unless a pane is already open) | | Done |
| Each control remembers its state (`~/.config/snowleopard/panes/<identifier>.conf`) | | Done |
| On Linux: Dock (dock.conf: size, magnification, hiding, position, effect), Sound (pactl: volume, mute, outputs, inputs), Keyboard and Mouse (xset on X11), Sharing (the computer's name, the services that run), Date & Time (its three sub-panes, the time zone, the clock), Software Update (checkupdates), Network (the interfaces, their state), Print & Fax (CUPS), Bluetooth (bluetoothctl: power, discoverable), Accounts (you, the guest), Startup Disk (the system that runs), Energy Saver (a battery or not), Appearance and Exposé (the lists their code fills) | | Done |
| Displays (the Display and Color monitor panels' nibs), Language & Text (its four Intl panes' nibs) | /System/Library/MonitorPanels, the nested panes | Done |
| Speech: NSArchiver's typedstream (`objects.nib`: "streamtyped" little endian, "typedstream" big endian) read and translated into the keyed model ([`typedstream.cpp`](../shell/src/aquakit/typedstream.cpp)); the classes' value groups read from the nibs | | Partial: the slider's doubles' order and the button cells' flags inferred, not checked against AppKit |
| Sheets (Schedule…, Options…, Advanced…: from the pane's nib or a nib of their own), buttons that open another pane, the window's animated resize | | Done (Network's Advanced… put together from five nibs) |
| The panes' own drawing views (Accounts' picture, Network's details as 10.6 draws them), the Mac's colour and open panels | | Missing |

## 11. Preview: PDFs and pictures

Port: [`preview/`](../shell/src/preview). Reversed from Preview 5.0's x86_64 slice (`Preview-501`, not
protected) and PDFKit's (`Quartz.framework/Frameworks/PDFKit.framework`, BaseSystem.pkg; the page view
is PDFKit's PDFView, which PVPDFView subclasses); its nibs, toolbar plist, pictures and strings are the
DVD's, read at run time.

| Item | Source | Status |
|---|---|---|
| The toolbar: `CommonToolbar.proplist`'s default items (Previous/Next, Zoom, Move/Text/Select, Annotate, Sidebar, Search) with their `TB_*` pictures, labels from Localizable.strings; items that don't fit go from the end | the plist (OpenStep text format, [`plist.cpp`](../shell/src/preview/plist.cpp)) | Done (NSToolbar's spacing by eye; no customization sheet) |
| The window's size: the screen's visible frame inset 4 (28 less across); a PDF at 100% (`PVPDFDefaultDocumentScaling`), as wide as its widest page, as tall as its page (the screen's height with more than one) plus the scroller; a picture at its size scaled down to fit; at least 408 x 224, 573 wide with the sidebar | `-[PVWindowController bestWindowFrameForCurrentMediaContainer]`, `windowDidLoad` | Done |
| The matte: device RGB 0.5 (PDFKit's default; Preview only sets the `PVGeneralWindowBackgroundColor` preference's); the paper white (`pageColor`); continuous display, page breaks shown | PDFKit `-[PDFView(PDFViewInternal) commonInit]` (0x8966), Preview `-[PVWindowController setupPDFView]` (0x100054d7f) | Done |
| The pages' layout, in page points then scaled with the zoom: 4 at each side (narrower pages centred on the widest), nothing above the first page, 12 between pages, 8 under the last; the view `round(scale × floor(widest + 8))` wide; each page's rect rounded inwards to whole pixels | PDFKit `generatePageViewBounds` (0x12ddd), `viewSizeForPage:` (0x137aa), `pageViewHeight:` (0xdb2f), `pageViewSizeForPage:` (0xd931), `drawPagePre:` (0xf2a7) | Done |
| The pages centred in what is seen when smaller: `round((visible − pages) / 2)` across and down | PDFKit `positionInternalViews:` (0x11963) | Done |
| The shadow: seven premultiplied black tiles (a 8 × 8 corner, its mirror, the bottom edge 8 high, the sides 4 wide, the top 8 of each side fading), 4 pixels at the sides, 8 under, none above: the page shifted 4 down and blurred over 8, α 0.97 against the bottom edge; in pixels, not scaled. The corner's 64 values are in the port, the others derived from it. (Preview's own `CGContextSetShadow((0, −5), 5)` in `drawPagePost:` is for a page being rotated only) | PDFKit `createShadowMaskParts` (0xf9d6, `_gShadowLowerLeftData` at 0xacbc0), `drawShadowedRect:blur:` (0x1600e); Preview `-[PVPDFView drawShadowedRect:blur:]` (0x100057122, none on the checkerboard), `drawPagePost:` (0x10005792f) | Done (the upper corners' one odd value, 10 for 20, not kept) |
| Zoom to Fit: PDFKit's `autoScales`, in continuous display (a single page too) the width less the scroller's: `(width − 15) / floor(widest + 8)`; the height not fitted; kept while the view is resized | Preview `-[PVPDFView zoomToFit]` (0x10006269f); PDFKit `autoScaleFactorForPage:` (0xb4a2), `functionalDisplayMode` (0x3e46) | Done |
| Zoom in and out by Preview's steps (1% to 8000%, 100% kept within half a percent), the point in the middle staying there; Zoom to Fit, Actual Size; ⌘+ ⌘- ⌘0 | `_PVViewZoomFactorNext` / `Max` (the table at 0x1001320e0), `-[PVWindowController doZoomIn:]` | Done |
| Tools: Move drags the pages, Text selects their text (poppler's glyph selection, ⌘C to the clipboard), Select draws a rectangle; the tool at start is Text (`setToolMode: 1`) | `-[PVPDFView awakeFromNib]` | Done (the Select menu's other shapes and copying a picture missing) |
| Links: to a page (named destinations too) or a URL | PDFKit | Done |
| The sidebar, on the right: 190 points for thumbnails, 245 for the table of contents, 165 at least (`kDefaultSidebarWidthForThumbnails`, `...ForOutline`, `kMinimumSidebarWidth` at 0x100132230); a 1 point divider (`dividerThickness`), dragged; opened for a PDF of more than one page | `PVNSSplitView`, `-[PVNSSplitView currentlyPreferredSidebarWidth]` | Done |
| Thumbnails: `floor((sidebarWidth − 20 − 15) / columns − 20)`; on IKImageBrowserBackgroundColorKey calibrated (0.9, 0.92, 0.96) over the clip view's (0.844, 0.875, 0.903); the layout manager's margin 10 across and 15 down, cells 20 apart (cell margin 20 × 20), shadowed and titled (`setCellsStyleMask: 5`); the page shown selected | `-[PVWindowController sidebarThumbnailSizeForCurrentColumnMode]` (0x10000f837), `setupThumbnailView` (0x10000f1f4), `PVDocument.nib` (object 477) | Done (IKImageBrowserView's selection, cell shadow and title height by eye: they are ImageKit's) |
| The table of contents: PVOutlineView (`PVDocument.nib` object 133): no header, rows 17 high and 2 apart, Lucida Grande 13, on (0.906, 0.918, 0.961); the document's group row (its file name) above the entries; line breaks in a label shown as spaces | the nib; `outlineView:isGroupItem:` (0x100056293), `outlineView:objectValueForTableColumn:byItem:` (0x100056d1c), `outlineView:child:ofItem:` (0x1000561c3) | Done (the group row drawn as AquaKit's source list header) |
| The search's results, in the same outline view: the document's group row, one row per page with a match (`pagesSeen`), in page order, labelled with the contents' entry the page is in, `%@ (Page %@)` when pages share one, `Page %@` without contents; the first selected; the relevance column 64 wide, its bar against the best page's (`setMaxValue: maxRelevanceScore`) | `-[PVWindowController displaySearchResults:isDone:]` (0x1000d83a0), the nib's `relevance` column | Partial: Search Kit's relevance replaced by the page's number of matches; the bars' look by eye |
| The sidebar's bottom bar: `BottomTB_toolbarButtonBackground`, the four views (contact sheet, thumbnails, table of contents, annotations) on `SidebarButtonBackground`, the thumbnail size slider in contact sheet mode | `PVDocument.nib` (_utilityBottomView) | Done (annotations' list missing) |
| Search: the toolbar's field, the matches highlighted on the pages, the first selected; their list in the sidebar (above) | | Done |
| The annotation bar (`BottomTB_*` tools under the pages) | `PVAnnotationBanner.nib` | Partial: shown, its tools don't annotate |
| Pictures: one page without paper or shadow, never enlarged to fit | | Done (no adjustments, no Instant Alpha) |
| Inspector, printing, slideshow, the page number panel, saving | | Missing |
| AquaKit: resizable windows (WM_NORMAL_HINTS' minimum and ConfigureNotify on X11, `xdg_toplevel` configure and `resize` on Wayland, the size box); the horizontal scroller (CoreUI's `horizontal` track and thumb, arrows at the right); X11 frames flushed before waiting | | Done |

## 12. About This Mac (loginwindow)

Port: [`loginwindow/`](../shell/src/loginwindow). In 10.6 the panel is loginwindow's (`AboutThisMac.nib`,
`AboutThisMac.strings`, `MacOSX.tif`), its controller `AboutThisMacController`. loginwindow's Intel slices are
encrypted (`SG_PROTECTED_VERSION_1`); its PowerPC slice isn't, and was read with Ghidra (REA takes no
PowerPC): the Objective-C 1 metadata gives the methods' addresses.

| Item | Source | Status |
|---|---|---|
| The panel: the nib's (307 x 377), on every Space, above the windows (`CGWindowLevelForKey(kCGModalPanelWindowLevelKey)`), not hiding when another application is active, not taking the menu bar, centred | `-[AboutThisMacController init]` (0x26f84), the nib | Done |
| The version line: a borderless button in grey (`disabledControlTextColor`, 12 points) going round "Version %@", "Build %@", "Serial Number %@" (the serial when there is one); 10.6 and 10A432 from the DVD's `SystemVersion.plist` | `toggleBuildField:` (0x26bfc), `reloadBuildInfoStrings` (0x27720) | Done (the serial: Linux lets root alone read it) |
| The copyright: the nib's "TM and ©" and the product's copyright ("1983-2009 Apple Inc.") | `reloadBuildInfoStrings` | Done |
| Processor: "%@" of the clock and the brand ("2.66 GHz Intel Core i7"), "N x" for several packages; its L3 cache line ("%@ L3 cache", `hw.l3cachesize`) or, without one, the line hidden, the top half 14 points down and the panel 14 shorter | `processorFieldString` (0x2871c), `l3CacheFieldString` (0x28808), `setL3CacheHidden:` (0x27c48: 14.0 at 0x6ade8) | Done (from /proc/cpuinfo, cpufreq, sysfs' caches) |
| Memory: "%@%@": the size (AppleSystemInfo's "%d MB" under a gigabyte, else "%.*f GB" with no decimal unless a hundredth is left) and the modules' speed and type (" 1067 MHz DDR3") | `updateAboutBox` (0x27dc4), 0x46b04, 0x46770 | Done (udev's SMBIOS modules) |
| Startup Disk: shown when /Volumes has more than the startup disk, else hidden and the panel 28 points shorter | `updateAboutBox` (28.0 at 0x6adec) | Done (the root's label; other volumes under /run/media, /media, /mnt) |
| The lines laid out: the labels sized to fit, right-aligned, the values 5 points after them, the two columns centred with 40 points around at least (the values shortened, or the panel widened) | `updateAboutBox` (0.5, 5.0, 40.0 at 0x6add8...) | Done |
| More Info... and Software Update...: the button gives way to "Opening %@…" and the wheel while the application starts, then the panel closes; disabled without the application | `moreInfoClicked:` (0x275e4), `softwareUpdateClicked:` (0x274a8) | Done (KDE's Info Center or HardInfo for System Profiler, System Preferences' Software Update pane) |

## 13. The Finder: what can be read

The Finder's executable is protected on both its slices (`SG_PROTECTED_VERSION_1` on `__TEXT` past its
first three pages: code and strings), and 10.6's Finder has no PowerPC slice. `tools/deprotect.py` decrypts a
thin slice for analysis, locally, in `.local/media/re` (nothing of it is in this repository): each 4 KiB page is
Blowfish-CBC under the 64-byte key phrase, zero IV per page; the header's zero padding encrypts to the
`40 cf 69 2e` seen at the start of the range. The clear Finder is stripped of its local symbols, but its
Objective-C metadata names its 431 classes. What is readable:

| Source | What it gives | Status |
|---|---|---|
| Its nibs (`BrowserWindow`, `Toolbar`, `Sidebar`, `SidebarSplitter`, `StatusBar`, `BottomBar`, `ListView`, `ColumnView`, `FlowView`, the `InfoWindow*` views, `PreferencesWindow`, `ViewOptionsWindow`, `ConnectToWindow`, `GotoWindow`, the `Progress*` views, `SearchCriteria`, `MenuBar`...) and its strings and pictures | its windows, as AquaKit builds System Preferences' panes | Extracted |
| `__DATA` and `__LINKEDIT`: its imports (1,641 symbols), 166 of them from DesktopServicesPriv | which engine calls the Finder makes | Read |
| **DesktopServicesPriv** (not encrypted, 4,111 symbols): the Finder's back end (`Finder_BE-547/VTwin`), C++: `TNode` (403 methods), `TFSInfo`, `TCFURLInfo`, `TOperation` and its copies, moves, Trash and deletion (`TCopyMoveOperation`, `TCopyWriter`, `TTrashOperation`, `TDeleteImmediatelyOperation`), `TUniqueNamer`, `TFolderSizingThread`, `TNodeQuery`; the `.DS_Store` store (`BuddyStorage`, `BTree`, `Page`, `PageCache`, `THFSPlusPropertyStore`) | what the Finder does to files and what it remembers of each folder | Mapped |
| `.DS_Store`: "Bud1" at offset 4, then the root block's address, its size and the address again; the root block: the blocks' addresses, the table of contents ("DSDB": the B-tree), the free lists | `BuddyStorage::Open` (0x1f7f4) | Read |
| The Finder's classes (431): `TBrowserWindowController`, `TBrowserWindow`, `TIconView`/`TIconViewController`/`TIconViewSettings`, `TListView`, `TColumnView`, `TFlowView`, `TDesktopViewController`/`TDesktopIconView`/`TDesktopIconLayoutController`, `TInfoWindow*ViewController`, `TGeneralPaneController`/`TLabelsPaneController`/`TAdvancedPaneController`, `TBottomBar`, `TContextMenu`, `TDragAndDrop`... | which nib each controller drives | Listed |
| The names of copies ("name copy 2") | `TUniqueNamer::NextUniqueName` (0x815a4) calls back into the Finder: 0x100175b42 splits the extension off (unless the item is a folder or a package) and formats `Localizable` "N4" ("^0 copy") on the base name, or "N1" ("^0 ^1") once a number is needed, then puts the extension back | Read (the extension test's table at 0x10042c060 not yet) |
| The properties' codes | immediates in the Finder's code: 'Iloc' (icon positions), 'icvp'/'icvo'/'icvt'/'icsp'/'icgo' (icon view), 'lsvp'/'lsvo'/'lsvt'/'lssp' (list view), 'bwsp' (window), 'fwi0'/'fwsw'/'fwvh' (Finder window: frame, sidebar width, view height), 'vstl' (view style), 'BKGD' (background), 'dscl' (disclosed), 'cmmt' (comments), 'dilc', 'ptbL'/'ptbN'; no 'vSrn'/'modD' (later systems) | Found: their layouts still to read |
| `.DS_Store`, checked on the DVD's own three (written by Apple in 2009): the header ("Bud1", the root block's address and size twice, then the root node's address), the blocks' addresses (offset, log2 of the size in the low 5 bits), the TOC's "DSDB" (root node, levels 0 for one leaf, records, nodes, page 4096), the 32 buddy free lists over 2^31, nodes sized to fit; records sorted by name without case then code | DesktopServicesPriv's BuddyStorage, the DVD's files | Read, written (shell/src/finder/browser/dsstore.cpp) |
| The properties used: `Iloc` (the icon's anchor x, y, then ffffffffffff0000), `vstl` (icnv, Nlsv, clmv, Flwv), `fwi0` (older: its bytes 8-11 the view), `icvo` (older: "icv4", the icon size), and the 10.6 Finder's binary plists `bwsp` (WindowBounds in Cocoa's coordinates with the title bar, SidebarWidth, ShowSidebar, ShowToolbar, ShowStatusBar, ShowPathbar), `icvp` (iconSize, gridSpacing, textSize, arrangeBy...), `lsvp` (sortColumn, columns: ascending, width, index, visible) | the keys are cfstrings of the Finder's code; the DVD's files | Read |

### The Finder's window (FinderWindows, shell/src/finder/browser)

| What | Source | Status |
|---|---|---|
| The window: content 770 x 383 | `BrowserWindow.nib` (TBrowserWindow) | Read |
| The sidebar's width: 192 by default, at least 120 + ... | `+[TSidebarViewController defaultSidebarWidth]` (0x10003fc27), `minimumSidebarWidth` (0x10003fc6b) | Read (the minimum's added term not yet) |
| The toolbar's items: Back/Forward 53 (segments 23, 23), View 111 (25, 26, 26, 25), Quick Look 40, Action 40, Search 112 | `Toolbar.nib` | Read |
| The default set: BACK, flxs, SWCH, sSpc, QUIK, ACTN, flxs, flxs, SRCH (four-char kinds; sSpc and spac are both NSToolbarSpaceItem, flxs NSToolbarFlexibleSpaceItem, sepa the separator) | the table at 0x1002784c0 (the allowed set at 0x100278500), the identifiers' switch at 0x10003e9ab, `ToolbarItemInfo` (0x38 bytes) at 0x10042a600 | Read |
| The first flexible space tracks the sidebar's divider (index 0), so View starts by it | 0x10003f543 (`splitViewForToolbarAlignment`, `setTrackedSplitView:`, `setTrackedSplitViewDividerIndex:`); AppKit's `-[NSToolbarItemViewer _spaceRequiredToSatisfySplitViewTrackingForMinimumLeftOrigin:inWindow:]`: the divider's right edge less the spacer's origin | Read |
| The spacing: none between item viewers (the Aqua metrics' field 0x20, `_defaultAquaMetrics` at 0x838200, is 0); each control's viewer 4 wider on both sides (`frameWidthForIconWidth:`: the icon width less `_minIconFrame`'s, plus `_minViewerSize`'s); the spaces unpadded: NSToolbarSpaceItem 32 (16 for the small space), a flexible space at least 1 (`minSize`); 4 in from the toolbar's ends. On a 770 wide window: View at 198, Quick Look 349, Action 397, Search 532 to 759 (a 10.6 screenshot: 198, 351, 399, 537 to 761) | AppKit `-[NSToolbarView _layoutRowStartingAtIndex:...]`, `-[NSToolbarSpaceItem minSize]`, `-[NSToolbarFlexibleSpaceItem minSize]` | Read (the ends' 4 measured) |
| The Search item: 60 to 250 wide | `TSearchToolbarItem` init (0x10003f6f4: `setMinSize:`, `setMaxSize:` with the view's height) | Read |
| The room left: by weight, 0.25 a flexible space (`_NSToolbarFlexibleSpaceResizeWeight` default, not shared), 1 an item that grows, each to its maximum, repeated until it is gone | AppKit `-[NSToolbarView _distributeAvailableSpaceIfNecessary:toFlexibleSizedItems:...]` (0xa5406), `+[NSToolbarFlexibleSpaceItem _resizeWeight]` | Read |
| Quick Look enabled with nothing selected | a 10.6 screenshot | Observed |
| The status bar's icon size slider (icon view): the Finder's `trackCapLeft`, `trackFill`, `trackCapRight`, `knob{Normal,Hover,Disabled}` pictures, 77 long, 25 from the right; 16 to 512 by the square root (64 a fifth along) | the Finder's resources; measured on a 10.6 screenshot | Estimated (the scale fits the screenshot, not read from the code) |
| The sidebar's headers' disclosure triangles (7 wide, in the header's colour), the headers 20 in, the rows' icons 23, their names 43 | measured on a 10.6 screenshot | Estimated |
| The toolbar's images: AppKit's NSGoLeftTemplate... are CoreUI's `image.GoLeft`...: `kCUIImageConstantIDs` (CoreUI 0x3d540) give a map ID, `kCUIImageEntries` (0x3bda0, 16 bytes: kind, then the SArtFile ID) the art: GoLeft 20136, GoRight 20159, IconView 20235, ListView 20236, ColumnView 20233, FlowView 20147, Action 20163, QuickLook 20170 | CoreUI `CUIGetImageIDForMapID`, `DictionaryInitializerProc` | Read |
| SArtFile's template entries (type 3): a PDF, then the bitmap | `CUISharedArtReader` | Read |
| How a template image is drawn on a textured control (dark grey, white emboss) | AppKit | Estimated |
| The icon-only toolbar's height: 37 (Preview's 54 less the label and its gap) | | Estimated |
| The sidebar's colours: background 214,221,229 (inactive 232); the selection in a key window a gradient 91,147,213 to 22,83,170 with a top line 69,128,200 (`_kPairs` 0x10, 0x11; without the list's focus 162,177,208 to 110,130,170, line 145,160,192: 0x12, 0x13), inactive 181 to 137, line 151 | `CUIRenderer::DrawGradient` (CoreUI 0x232f8): variants 0xBD and 0xBE, `_kPairs` (0x3f460) into `kColorTable` | Read |
| The sidebar's rows (20 high), headers, text placement | | Estimated |
| The sidebar's icons: the places' toolbar icons (CoreTypes' `ToolbarDesktopFolderIcon`, `ToolbarAppsFolderIcon`, `ToolbarDocumentsFolderIcon`, `HomeFolderIcon`), the times' searches their bundles' clocks, the kinds' `SmartFolderIcon` | CoreTypes' Info.plist ('tDsk', 'tAps', 'tDoc', 'thom', 'sfld'); checked against a 10.6 screenshot | Done |
| The icon view's grid (iconview.h has the formula): label width from 4 to 17 characters of "0" + 18 by the grid spacing, pad icon/16, 2 label lines, the anchor 4 into the cell | `TDesktopViewController::calculateGridSize` (0x100023f62), `updateIconMetrics`, `setTextSize:`, `TLayoutGrid` (`calculateGridOriginForGridSize:`, `anchorAtGridLocation:`, `calculateGridDimensions`); the FX* defaults' fallbacks | Read (the desktop's; the windows' icon view assumed the same) |
| The view options' bounds: icon size 16-512, grid spacing 1-512, text size 10-16 | `TIconViewSettings::checkIntegrity` (0x1000da5f0) | Read |
| The defaults: icon 64, text 12, grid spacing 54 | 10.6's `StandardViewSettings` | From the defaults, not the code |
| The selection's look (the icon's dark square, the label's band) | | Estimated |
| The status bar: 24 high, Lucida Grande 11 centred; "^0 items, ^1 available", "^0 of ^1 selected" | `StatusBar.nib`, `LocalizableCore` SB1, SB18, SB_COUNT_* | Read |
| The disks' icons | IOStorageFamily.kext `Internal`, `External`, `Removable` | Extracted |
| SEARCH FOR's names and icons | each `CannedSearches/*.cannedSearch` | Extracted (the searches don't run yet) |

### Copy, move, the Trash (shell/src/finder/browser/fileops.cpp, progress.cpp)

| What | Source | Status |
|---|---|---|
| A drop on the same disk moves, on another copies; Option down copies; Copy then Paste (no Cut); Duplicate; Move to Trash; Empty Trash after "Are you sure you want to permanently erase the items in the Trash?" (A15, A16) | the Finder's behaviour, Localizable | Read (the strings), the rules 10.6's |
| Copies' names: "^0 copy" (N4) then "^0 ^1" (N1) before the extension, an ending "copy" or "copy N" not doubled | the Finder's 0x100175b42, its strings | Read (the "copy" ending's test seen in use, its code not followed) |
| The Copy window ("Copy", PW32): ProgressStatusView2Line.nib per operation ("Copying “^0” to “^1”" CP4, MV4, DU3, MT8, ET2 and their plurals; "^0 of ^1 - ^2" PW3; U20-U26 the time left; PW2, PW6); ProgressReplaceView.nib when a name is taken (PE1, PE32, PE61-PE64; Stop, Don't Replace, Replace, Apply to All as the nib has them) | the nibs, Localizable | Read |
| The window shown after a second, the time left's steps | | Estimated |
| The Trash: the freedesktop one (files/, info/*.trashinfo), the Dock's and the desktop's | | Linux's |
| AquaKit's determinate bar: the track (left cap, middle, right cap) under the blue fill, "complete" once full | ArtFile's progressindicator elements | Read (fixed) |
| Not yet: Undo, putting items back from the Trash, dragging from the list and column views, Secure Empty Trash, the errors' own view (ProgressErrorView) | | |

### Get Info (shell/src/finder/browser/infowindow.cpp)

| What | Source | Status |
|---|---|---|
| The window: no nib, one slice per nib stacked (InfoWindowHeaderView, GeneralView, MoreInfoView, NameView, OpenWithView, PreviewView, PermissionsView), "^0 Info" (IN1) | TInfoWindowController, the nibs | Read |
| 265 wide (265 to 400 by a preference); a closed slice 20 high, the header's 74; the bottom margins 8, the header's 5, Open with's 1; General and Preview open at first, each slice's state kept | `+[IInfoWindowControllerBase preferredWindowWidth]`, `-disclosedHeight`, `-bottomMargin`, `-expandedDefaultValue`, `-expandedPrefKey` | Read |
| The rows that don't apply hidden, the ones under them moved up; a long value on more lines pushing them down | TLayoutBinder (each view bound to its "master view", the nibs' masterView outlets) | The rule read, its code not |
| The fields: found by their row's title ("Kind:", "Size:"...) in the nib; the sizes "^0 on disk (^1)", "^0 on disk (^1) for ^2 items" (IN_G1, IN_G4), "Calculating Size" while a folder is counted on a thread (TFolderSizingThread); "^0 bytes" (U4) | Localizable, LocalizableCore | Read |
| Spotlight Comments: the folder's `.DS_Store` `cmmt` (ustr) | the Finder's record codes | Read |
| Sharing & Permissions: the Unix mode as the Finder's privileges (Read & Write, Read Only, Write Only (Drop Box), No Access: IN_S1-S4), "You can read and write" (IN_S7...), the owner "(Me)" | Localizable | Read |
| Open with: xdg-mime's default application for the type | | Linux's |
| The windows' placement (`-positionWindow`: from the screen's visible top left + 10, each new one 20 right of the others): AquaKit can't place a window yet | | Not done |
| The label swatches' colours, More Info's rows (Last opened, Dimensions), the Preview (the item's icon or picture) | | Estimated |
| Not yet: changing permissions (the lock), Change All…, Hide extension, Locked, the labels | | |

### View Options (shell/src/finder/browser/viewoptions.cpp)

| What | Source | Status |
|---|---|---|
| The panel: `ViewOptionsWindow.nib` built by AquaKit, its tab view's pages Icon, List, Column, Desktop; each control found by its binding's key path (`targetedViewOptionsSettings.iconViewIconSize`, `listViewShowKind`, `columnViewShowPreviewColumn`...), now read by AquaKit's nib loader (`Nib::bound`) | the nib's NSNibBindingConnectors | Read |
| "Always open in icon view" (VO14-VO17): the folder's `vstl`; without it the view is the window's, not the folder's | `nodeHasCustomViewStyle`, `customViewStyleString` | Read |
| Arrange By's values in icvp: none, grid, name, dateModified, dateCreated, size, kind, label | the pop-up's items, cfstrings of the Finder | Read |
| Labels at the right: the grid's other branch (`calculateGridSize`: the label's width + the icon box + 2; the height the taller + the vertical pad's multiples), the label 2 right of the icon box, one line beside icons of 16; one line also under a grid spacing of 26 (FXTightLabelLines) | `TDesktopViewController::calculateGridSize`, `updateIconMetrics`, `FUN_1000267c9` | Read |
| The item info's contents (a folder's count, a picture's size) and its blue; the arrangements' orders (dates and sizes newest and largest first) | | Estimated |
| The defaults (Use as Defaults) and the column view's options: `~/.config/snowleopard/finder.conf`, as com.apple.finder's StandardViewSettings | | |
| Not yet: the background's colour and picture, Calculate all sizes, the column view's Arrange By, Restore to Defaults | | |

### The list view (shell/src/finder/browser/listview.cpp)

| What | Source | Status |
|---|---|---|
| The columns: Name 300 (at least 110), Date Modified 181, Date Created 181, Size 100, Kind 115, Version 75, Comments 300, Label 100, Last Opened 181; 2 between columns, 3 between rows, a header of 17, the last column taking the rest (NSColumnAutoresizingStyle 4) | `ListView.nib` | Read |
| The defaults: Name, Date Modified, Size, Kind; text 12, icons 16, relative dates, sorted by name, no folder sizes | `+[TListViewSettings defaultSettingsForNewUser]` (0x10008c31b) | Read |
| A row's height: with icons of 16, min(16, text size) + 4, else the icon size + 4 | `-[TListViewController rowHeightForIconAndText]` (0x1000814da) | Read |
| A level's indentation: the icon size + 5 | `-configureIndentation` (0x100081568) | Read |
| The sizes: `NSLocalizedFileSizeDescription` with TFileSizeFormatter's options (0x200000): base 1000, fraction digits by unit (bytes 0, KB 0, MB 1, GB on 2: `_numberOfFractionDigits`), the next unit once the rounded value reaches 1000, "Zero KB"; the size on disk | Finder `-[TFileSizeFormatter initCommon]`, Foundation 0x32fa2 | Read |
| Kind: the default application's document type name (its InfoPlist.strings: TextEdit's "Plain Text"), else CoreTypes' `UTTypeDescription`, else LaunchServices' "%@ Document", "Folder", "Application", "Unix Executable File" | CoreTypes' Info.plist, LaunchServices' Localized.strings | Read (which applications are the defaults: 10.6's, assumed) |
| The headers: CoreUI's `listheader` (the sorted column's `blue`, its `arrow ... selectup/selectdown` apart, the `separator`) | ArtFile | Read |
| The dates: TDateFormatter's relative formatters ("Today, 5:25 PM"), the longest style that fits the column | `+[TDateFormatter relativeMediumDateTime:]`... | The styles' patterns and the choice by width estimated |
| The alternating rows, the selection's colours, the cells' insets, the disclosure triangle's place | | Estimated |

### The column view (shell/src/finder/browser/columnview.cpp)

| What | Source | Status |
|---|---|---|
| An NSBrowser, columns at least 100 wide, resized one by one | `ColumnView.nib` (NSMinColumnWidth, NSColumnResizingType 1) | Read |
| The defaults: text 12, columns 205 wide, icons, icon previews and the preview column shown, sorted by name | `-[TNodeViewSettings refreshColumnViewSettings]` (0x10001fe87: FontSize 12, ColumnWidth 0xCD...) | Read |
| A row: the text size + 5 | `-[TColumnViewController rowHeightForVariableIconAndText]` (0x10006ec7d) | Read |
| The preview column: at least 208 wide | `-browser:shouldSizeColumn:forUserResize:toWidth:` (0x100070100) | Read |
| The folders' arrow: CoreUI's `image.ChildContainer` (20146) | CoreUI's image tables | Read |
| The preview's contents (the icon at 128, Name/Kind/Size/Created/Modified/Last opened, the labels of Get Info's nib), the dividers and their resize handles, the cells' insets | | Estimated |

### Cover Flow (shell/src/finder/browser/flowview.cpp)

| What | Source | Status |
|---|---|---|
| The flow over the list view (TFlowListView, the list's columns and cells), in a split view with no divider | `FlowView.nib`, `-[TFlowSplitView dividerThickness]` (0) | Read |
| The flow two thirds of the height at first, at least 180 | `-[TFlowViewController awakeFromNib]` (FlowViewHeight), `+[TFlowSplitView flowViewMinHeight]` | Read |
| The flow itself (IKImageFlowView: OpenGL in ImageKit): the covers' size, turn, spacing and darkening, the reflections, the background, the title and kind, the slider, the gliding | | Estimated (ImageKit not reversed) |

## Next

- `_CUIGetTheme` and the Graphite appearance (AppleAquaColorVariant).
- CoreUI's colour table (`_CUIColorSetFill` indices 0xE1–0xE3) for the menus' backgrounds.
- `DrawMenu`: the menus' shape and shadow.
- The Dock's labels over the icons: look for their drawing in CoreUI (a widget like the menus').
- HIToolbox's menu metrics (item height, indents, the command keys' column) for the menus' layout.
- The menu extras, one by one, from their clear binaries.
