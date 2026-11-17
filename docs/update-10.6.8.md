# 10.6.8 against 10.6 (10A432): what changed for the shell

The DVD's 10.6 (build 10A432) compared with a VM brought to 10.6.8 (10K549) by the Combo Update
v1.1, with Safari 5.1.10, iTunes 10.6.3, Java for Mac OS X 10.6 Update 17 and the Mac App Store
installed separately. Nothing of either system is in this repository.

Method: the x86_64 slices of every binary cited in [`reverse-status.md`](reverse-status.md) and
[`mail-reverse.md`](mail-reverse.md), old and new (Dock, Finder, SystemUIServer and loginwindow
decrypted with `tools/deprotect.py`). Each function is named from its symbol or its Objective-C
metadata (class and category method lists), disassembled with `llvm-objdump --macho` (selectors,
classes, literals and stubs symbolised), local branches made relative, and compared with its
namesake; the stripped C/C++ functions (the Dock's genie, the Finder's copy namer) are matched by
their references and their instructions. A function that still differs is compared again with
stack slots, registers and `nop`s masked, then decompiled (Ghidra) where something is left. The
data tables the port uses are compared byte for byte. The resources by content: nibs by their
strings, classes and values (their object order changes at every compile), `.strings` by entry,
plists by key.

## Versions

| Binary | 10.6 | 10.6.8 |
|---|---|---|
| AppKit | 6.6 (1038) | 6.6.8 (1038.36) |
| Foundation | 6.6 (751) | 6.6.8 (751.63) |
| CoreGraphics / WindowServer / libRIP | 1.535.5 | 1.545.0 |
| HIToolbox | 1.6.0 | 1.6.5 |
| HIServices | 1.8.0 | 1.8.3 |
| CoreUI | 0.2 (112) | 2 (114) |
| QuartzCore | 1.6.0 (226.0) | 1.6.3 (227.37) |
| ImageKit | 2.0 | 2.0.3 |
| PDFKit | 2.5 | 2.5.5 |
| DesktopServicesPriv | 1.5.0 | 1.5.11 |
| Dock | 1.7 (747) | 1.7 (769.18) |
| Finder | 10.6 | 10.6.8 |
| SystemUIServer | 1.6 (298.5) | 1.6 (298.17) |
| loginwindow | 6.0 | 6.1.2 |
| Preview | 5.0 (501) | 5.0.3 (504.1) |
| Photo Booth | 3.0 (115) | 3.0.3 (120) |
| Calculator | 4.5.2 (97) | 4.5.3 (99.2) |
| Mail / Message | 4.0 (1075) | 4.6 (1085) |
| Stickies | 7.0 | 7.0 (identical) |

## The code the port follows: unchanged

Every function cited in the two documents was found again in 10.6.8. All of them are the same,
or differ only by things that change nothing the port does:

| Function | 10.6.8 | Difference |
|---|---|---|
| CoreUI: `CUISharedArtReader::*`, `ArtFileSetupOnce`, `LoadFromArtFile`, `DrawDockMenu`, `DrawMenu`, `CreateWindowPath`, the 3- and 9-piece drawing, `DrawSegment`, `BeginFocus` | same | none |
| CoreUI: `DrawMenuBar`, `DrawMenuTitle`, `DrawMenuItem` | 0x1b610, 0x1e450, 0x1b926 | NULL checks added (no image, no context) |
| CoreUI: `DrawSlider`, `DrawGradient` | 0xf394, 0x232fa | registers; `_kTickOffset*Adjustment`, `_kPairs`, `kCUIImageConstantIDs` byte for byte the same |
| CoreUI: `ArtFile.bin`, `SArtFile.bin` | | identical files |
| QuartzCore: `CIMenuGenerator`, `CIMenuBarGenerator`, `CIMenuAppleGenerator outputImage` | 0x1041c4, 0x102d5b, 0x1033ed | the `menuBar` / `menuApple` kernels' copyright line only (2009 → 2011) |
| CoreGraphics: `_CGXSetMenuBarFilter`, `CGXWindowShadowGet`, `gamma_table_create`, the font dilation, `CGSScanConvolveAndIntegrateRGB`, `CGSColorDeepMaskCopyXRGB8888`; `vKernel`, `rKernel`, `gKernel`, `bKernel`, `mKernel` | | same (scheduling) |
| CoreGraphics: `CGSScanconverterRenderMask` | 0x20d652 | hardened: widths and heights over 0x7fff refused, the buffer size checked for overflow; the scaled size rounded with `ceilf` where 10.6 set the rounding mode upwards: the same result |
| HIToolbox `ResetMenuBarWindowFilter`, HIServices `CopyLabelColorAndName`, AppKit `__NSSetWindowShadow`, the toolbar's space distribution, the group rows, the relevancy bars | | same |
| Foundation `NSLocalizedFileSizeDescription` | 0x32cee | same |
| libRIP `ripc_RenderGlyphs` | 0x8db8 | one `nop` |
| ImageKit, PDFKit, Preview: every method of section 11 | | same |
| DesktopPictures `DSKAppleRootSource::init`; DesktopServicesPriv `TUniqueNamer::NextUniqueName`, `BuddyStorage::Open` | | same (the assertion's source path only) |
| Dock: minimizing, the genie and its step | 0x1000851a1, 0x1000f2436 / 0x1000f26a0 | the stack frame only |
| Finder: every method of section 13 (context menu, desktop grid, list and column views, sidebar widths, the copy names' callback) | | same |
| Photo Booth, Calculator (app, BasicAndSci, Hexadecimal), Stickies: every method of sections 14-16 | | same |
| Mail and Message: toolbars, fonts, `.emlx`, folders, colours, the assistant's tabs | | same |
| loginwindow (About This Mac) | | same |

## What did change

### Mail 4.6: threading (`-[MessageMall _deduceThreadHierarchyForMessages:firstTime:]`, 0x10003a4aa)

Steps 1-5 of [`mail-reverse.md` §6](mail-reverse.md) stay; 4.6 adds:

- **Siblings whose parent is missing.** When no message has the `Message-ID` that a message's
  `In-Reply-To` names (and it is not the first pass), the messages waiting on that same
  `In-Reply-To` are scanned and the **earliest sent** of them becomes the parent, unless that
  one's parent is already this message.
- **No loops.** By subject, a candidate is refused when it is the message itself or its own parent
  is this message. When an earlier message takes over a subject, the former holder's threading
  data gets it as parent only if it had no parent or its parent was that holder.
- The references (with `UseReferencesWhenThreading`) are walked by fast enumeration: the first
  entry found wins, every entry is indexed as before.

Our Mail does not thread yet; this is for when it does.

### Finder 10.6.8

- **Preferences > Sidebar** (`PreferencesWindow.nib`): the 18 check boxes are now
  `TSidebarPrefsItem` buttons with a `TSidebarPrefsItemCell` (10.6: the button drew itself,
  `-[TSidebarPrefsItem drawRect:]`). Same look: the 16 x 16 icon 4 px past the check box's title
  rectangle (10.6: at x 21), centred vertically (10.6: at the top, the title 1 px lower), the title
  4 px after it, `finderFontOfSize: 11`, black; mirrored right to left. The four headings are
  `TSidebarPrefsCategory` and seven labels `TLabelField`, both swapped classes.
- `Localizable.strings`: N138 "Cancel" → "Search App Store", N139 "There is no application set to
  open the document “^0”." and N139.1 (the App Store's advice): the Open With alert.
- `FFKView.nib` accepts the `SRW`/`srw` file types; `AboutWindow.nib` 1983-2011.
- Code: the sidebar's eject, burn, backup and iDisk buttons share one handler, its `mouseDown:`
  grew, `selectNode:`/`selectedNode` moved into the controller; nothing the port draws.

### Dock 1.7 (769.18): the App Store

- `default.plist`: `/Applications/App Store.app` first in the persistent apps, before Dashboard.
- `DockMenus.plist` / `DockMenus.strings`: `appstoreitem` (Pause 10000, Resume 10001, a separator,
  Cancel Download 10002) for an application being downloaded; `Localizable.strings`: its title
  states (Installing, Downloading…, Paused, Waiting…, "%1@ — %2@").

### Other resources

- Unchanged in content (recompiled): the Finder's `MenuBar.nib`, `FFKBrowserWindow.nib` and the Get
  Info views, `Calculator.nib`, Photo Booth's `MainMenu.nib`, Preview's nibs, Mail's nibs.
- System Preferences' panes: Keyboard, Bluetooth, Security, Display, Mouse, Trackpad, Energy
  Saver, Parental Controls, Sound, MobileMe (Back to My Mac, Sync), Network's 802.1X, Universal
  Access: new controls (see their nibs).
- `CoreTypes.bundle/Info.plist`: new types (the Kind column's names).
- Fonts: `HelveticaNeue.ttc` is replaced by `HelveticaNeue.dfont` (a data-fork suitcase);
  `LucidaGrande.ttc` is unchanged.
- The desktop pictures were re-encoded (same names).

## Software Update: 10.6.8 for a DVD's 10.6

The DVD is 10.6 (10A432); 10.6.8 is not on it. The shell gets it as 10.6 does, from Apple.

**The catalog.** SoftwareUpdate.framework reads
`swscan.apple.com/content/catalogs/others/index-leopard-snowleopard.merged-1.sucatalog` (the VM's
own Software Update did, on 16 November 2026). Product `zzz041-98179` (posted again in 2019) is the
client's **Mac OS X Update Combined 10.6.8 v1.1**: `MacOSXUpdCombo10.6.8.Part0`-`Part12.pkg` and
`SUBaseSystemCombo10.6.8.pkg` (1.07 GB), plus X11, QuickTime Player 7 and Rosetta's, installed only
where those are; its `.smd` holds the English title, version, "For Desktop Systems" and the
description (HTML). Its `.dist`: `InstallationCheck` and `VolumeCheck` take 10.6 to 10.6.7, builds
10A432 to 10K1999, not a server (`ServerVersion.plist`); the choice is visible below 10.6.8; the
first package's `onConclusion="RequireRestart"`. The packages are flat (xar) with a gzip'd cpio
Payload, as the DVD's: [`tools/softwareupdate.py`](../tools/softwareupdate.py) reads the catalog
and downloads them (https, resumable), `extract.py --update` lays their files over the DVD's.

**Software Update 4.0's windows** (its x86_64 slice, not protected):

| Step | 10.6 | Port |
|---|---|---|
| Checking | `-[SUAppScanController initWithParentWindow:]`: SUAppScanPanel.nib (NSPanel, title only: "Checking for new software…", an indeterminate bar, Cancel → `cancel:`) | Done (its title bar's buttons shown: WindowServer has no way to hide them yet) |
| The offer | `-[SUAppSimplifiedUIController alertForProducts:]`: by `SUProduct _installActionRequiredByProducts:` (0 none, 1 log out, 2 restart, 3 shut down) and whether all are downloaded, one of eight alerts; the combo: `_downloadAndRestartUpdatesAlert` (0x10000a0f1): UPDATES_AVAILABLE_DOWNLOAD_AND_RESTART_MESSAGE / _INFORMATIVE, Continue, Show Details, Not Now (SUAppNotif.strings) | Done |
| Its frame | `_adjustAlert:legalTextType:helpButton:helpAnchor:connectNotNowButton:` (0x10000a681): the help button, SUAppSimplifiedView.nib's legal text (type 1: `legalUpdateTextView`) as the accessory, Not Now → `terminate:`; `_loadSimplifiedView` (0x10000cad9): the alert's content view in the window, button 0 → `install:`, Show Details → `toggleDetails:`, the window's three buttons hidden | Done but the help button and the hidden window buttons |
| Show Details | SUAppDetailedView.nib (512 × 598): the icon, NEW_SOFTWARE_TITLE / _MESSAGE, a split view (the products' table: check, state, name, version, size; the description's WebView), the restart line (RestartReq.tif, RestartRequired_Single), the legal text, Hide Details, Not Now, Install 1 Item | Done (the description as text) |
| Download | SUAppInstallWindow.nib: "Downloading 1 item", "Downloading %@…", "%@ of %@ — %@", Stop → `stopDownload:`; then "Expanding and verifying %@…" | Done (sizes in 10.6's decimal units) |
| Restart | SUAppSession.strings: UPDATES_DOWNLOADED_INSTALL_AND_RESTART_MESSAGE / _INFORMATIVE, Restart, Not Now; the update installs at log out | Done: the session ends (this display's SystemUIServer), `snowleopard-session` runs `Software Update --install-at-logout` (the login screen's DefaultDesktop.jpg, "Installing 1 item", "Installing updates…", SYSTEM_WILL_RESTART_MESSAGE) and starts again |
| Password | SUAppAuthenticationHandler.nib | When the files are root's (install.sh's), `pkexec` asks |

About This Mac reads `SystemVersion.plist` from the files: after the update, "Version 10.6.8", "Build
10K549", "1983-2011". What the update changes in the shell's resources is listed above; the
behaviours that differ in code (the Finder's Sidebar preferences, the Dock's App Store) are not
switched by version yet.
