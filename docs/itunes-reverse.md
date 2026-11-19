# iTunes (8.2.1, 10.6): what was recovered

First pass over the iTunes on Snow Leopard's DVD. No Apple file is in the repository: the material
below lives in `.local/media/re/itunes/` (ignored by git) and is rebuilt from the DVD. 10.6.8's
combo update brings iTunes 11.4, a different program (Cocoa, x86_64), not covered here.

## Where the files come from

| File | Source | Notes |
|---|---|---|
| `iTunes.app` (complete) | `iTunes.pkg` on the DVD's HFS+ partition (`System/Installation/Packages/`, 47.5 MB, xar; its `Payload` a gzip cpio) | `extract.py` only takes `Info.plist` and the icons; the whole package was unpacked into `.local/media/re/itunes/root/` |
| `iTunes.i386` (12.8 MB) | the i386 slice of `iTunes.app/Contents/MacOS/iTunes` (universal **ppc + i386**: no x86_64) | `__TEXT` in the clear; stripped (32 698 functions, no names); sha256 `6361e4ff…82df28` |
| `Resources/iTunes.rsrc` | data-fork resource file | the art: 355 `PNG `, 108 `PICT`, 104 `PBUT`, `PSLD`, `PSCR`, `PANI`, 75 `icns`, `TMPL` |
| `Resources/English.lproj/Localized.rsrc` | data-fork resource file | the menus (`MBAR`, 46 `xMNU`, 55 `MENU`), 78 `DLOG` / 98 `DITL` / 116 `CNTL` / 61 `dlgx`, `TXTS`, `Txtr`, `ACfg`, `CWid` |
| `English.lproj/Localizable.strings` | UTF-16 | 7 344 keys; every menu item's text (the `xMNU` keys) |
| `English.lproj/*.nib` | Carbon nibs (`objects.xib`, IBCarbonFramework XML) | EQWindow, GradientWindow, iPodSettings, MusicStoreBar, PartyShuffleSettings, Placards, Ringtone, SetupAssistant, SmartPlaylist, UniversalAccess |

iTunes 8 is a **Carbon** (HIToolbox) C++ program with a little Cocoa (WebKit for the Store, QTKit).
It links AGL/OpenGL (Cover Flow, the visualizer), QuickTime, DiscRecording, the private `iPod`
framework and its own `iPodUpdater.framework`.

## Tools

- **REA 6.9.0 with Ghidra 12.2** on `iTunes.i386` (cold import and auto-analysis of the 12.8 MB slice
  needed `ghidra.startup_timeout_ms` raised past the default 330 s).
- Python readers kept next to the material in `.local/media/re/itunes/`:
  `rsrc.py` (the resource map), `xmnu.py` (the `xMNU` menus, below), `tmpl.py` (any resource through
  the `TMPL` templates Apple left in the files). Outputs: `menus.txt` (every `xMNU`),
  `templated.txt` (every `PBUT` / `PSLD` / `PSCR` / `PANI` / `TXTS` / `ACfg` / `CWid`, decoded and
  named), `cntl.txt`, `png/` (every `PNG ` by ID), `sheet1.png` (a contact sheet).

## 1. The menus: `xMNU`

The menu bar is `MBAR` 129: menus 129, 131, 133, 135, 134, 141, 136, 138, i.e. **Apple (iTunes),
File, Edit, View, Controls, Store, Advanced, Window** (Help is the system's). They are not `MENU`
resources but iTunes' own `xMNU`, built at launch.

Reader: `FUN_0026c16c` (by ID, returning the menu) and `FUN_0026c0ba` (appending a resource's items to
an existing menu); one item: `FUN_0026be3e`.

| Item | Source | Status |
|---|---|---|
| Header: Pascal string at 0 (a key into Localizable.strings: the title; empty for submenus), the menu's 4-char ID at 0x100 (`appl`, `file`, `edit`, `viem`, `ctrl`, `stor`, `advc`, `wind`, `dock`, `kcmd`…), the item count (s16) at 0x152; the items from 0x154 | `FUN_0026c16c` | Done (decoded) |
| An item: s16 length at +0 (next item at item + length + 2); iTunes' own command (4cc) at +2; the key (u16) at +6; the mark at +8 (`SetItemMark`, skipped when ' '); the key glyph at +0xA (`SetMenuItemKeyGlyph`); the HIToolbox command ID at +0xC (`SetMenuItemCommandID`, ignored when 0 or `????`); the refcon at +0x10 (`SetMenuItemRefCon`); the modifiers at +0x14 (`SetMenuItemModifiers`: 1 ⇧, 2 ⌥, 4 ⌃, 8 no ⌘); +0x17 non-zero: the key is a virtual key code (+0x18); +0x1A non-zero: disabled (`DisableMenuItem`); the text's key, a Pascal string at +0x60 (a leading `##` stripped and flagged); `kMenuItemDivider` a separator | `FUN_0026be3e` and its callees `FUN_002f5434`, `FUN_002f563a`, `FUN_002f5718`, `FUN_002f54ec`, `FUN_002f5764`, `FUN_0026bdbe` | Done (decoded) |
| Submenus: the classic convention, key 0x1B (`hMenuCmd`) and the mark the submenu's `xMNU` ID (File ▸ Library 210, File ▸ Rating 200, View ▸ Sort 212, View ▸ Video Size 211, View ▸ Visualizer 204, Controls ▸ Audio & Subtitles 209, Controls ▸ Shuffle 214, Controls ▸ Repeat 215, the Dock menu's Rating 201) | `menus.txt` | Done (decoded) |
| Key 0xF0 with a glyph: an arrow key (Next / Previous ⌘→ ⌘←, glyphs 26 / 24; Volume Up / Down ⌘↑ ⌘↓, glyphs 25 / 16, the mute ⌥⌘↓) | `FUN_002f563a` (0xF0 passed through unconverted) | Done (decoded) |
| The menus' contents, keys and modifiers, as in `menus.txt`: e.g. File: New Playlist ⌘N, New Playlist from Selection ⇧⌘N, New Playlist Folder, New Smart Playlist… ⌥⌘N, Edit Smart Playlist, Close Window ⌘W, —, Add to Library… ⌘O, Library ▸, —, Get Info ⌘I, Rating ▸, Show in Finder ⌘R, Show Duplicates, —, Sync, Transfer Purchases, —, Page Setup…, Print… ⌘P; View: as List ⌥⌘3, as Grid ⌥⌘4, as Cover Flow ⌥⌘5, Sort ▸, View Options… ⌘J, Show Current Song ⌘L, Show Browser ⌘B, Show Genius Sidebar ⇧⌘G, Show Artwork Column ⌘G, Video Size ▸, Visualizer ▸, Show Visualizer ⌘T, Full Screen ⌘F; Controls: Play (space), Next / Previous, chapters (⇧), Audio & Subtitles ▸, Shuffle ▸, Repeat ▸, Eject Disc ⌘E; Window: Minimize ⌘M, Zoom ⌃⌘Z, iTunes ⌥⌘1, Equalizer ⌥⌘2, Bring All to Front | `Localized.rsrc`, `Localizable.strings` | Done (decoded); the alternates (Show Exact Duplicates and the ⌥ Convert are the same command with ⌥) |
| The hidden menu `kcmd` (160, "Commands", never shown): ⌥⌘F search field, ⌘Y preferences, ⌘↑ / ⌘↓ / ⌥⌘↓ volume and mute, ⇧⌘B buy, ⌥⌘I Get Info, ⌘{ ⌘} previous / next item, ⌥⌘T visualizer | `xMNU` 160 | Done (decoded) |
| The Dock menu (`xMNU` 139, `dock`): Now Playing (disabled title), Rating ▸, Repeat Off / All / One (`RPT0` `RPT2` `RPT1`), Shuffle (`SHUF`), Play (`PLPS`), Next, Previous, Mute | `xMNU` 139 | Done (decoded) |
| The contextual menus: the track list (400), Grid (409), a device's playlist (401), the column header (402), the source list (403), the Store (404), the artwork (405), Now Playing (408), the album display (520 / 521), Downloads (522), editing (406) | `menus.txt` | Done (decoded) |
| The classic `MENU`s: the popups of the dialogs (Smart Playlist criteria 300–315, Preferences 1341–1388, the encoders 3000–5002, Gracenote 189–191) | `MENU` | Done (decoded) |
| Ported | | Missing |

## 2. The art: `PBUT`, `PSLD`, `PSCR`, `PANI`, `TXTS`, read through their templates

Apple left `TMPL` resources describing iTunes' private art formats in both resource files; `tmpl.py`
decodes any resource with them. Every `PBUT` is named (`templated.txt`). The `PNG ` resources are
loaded as CGImages by `FUN_005a5888(&image, 'PNG ', id)` (an image view's setter: `FUN_0000f0a2`).

| Format | Fields (from the `TMPL`) | Examples |
|---|---|---|
| `PBUT` (picture button, template 5001) | image ID, mask `PICT` ID; flags (high-contrast dark / light variants, PNG with the mask ignored, rects from the artwork, Graphite variant, blended alpha, alpha, tile vertically / horizontally, alpha or bounding-rect click tracking, deep mask); fixed left / right / top / bottom sizes (a nine-slice); kind 0 (Disabled, Enabled, Pressed rects per button), 1 (+ Rollover), 2 (+ Inactive Disabled, Inactive Enabled) | 178: the Store's buttons (Buy, Gift, Get, Rent… for songs, albums, films, apps), Source Icons small / large / huge (441–443) and devices (466 / 468), the ratings (528–547), the Grid's selection frames (570–596), the badges (710–720), downloads (900–911), the HUD (2000–2040), the list header (304), track status icons (306) |
| `PSLD` (slider, 5003) | image, mask, count, offset; flags (Graphite variant, alpha-blended thumb, dual sided, snap to centre); thumb inactive / active / pressed rects, the slider's and its drag rects, fixed left / right | 512 Position Slider (the LCD's, 513, Graphite variant), 520 Vertical Slider (the equalizer's, snaps to centre), 2003 HUD Scrubber, 2006 / 2032 HUD Volume |
| `PSCR` (scroller, 5004) | as `PSLD` plus thumb offsets and fixed sizes, thumb and under-button rects | 2022 / 2023 (the HUD's) |
| `PANI` (animation, 5002) | flags (Graphite variant, many masks, bounces), pictures, mask, frame count, ticks between frames, frames per row, the frame's rect | 131 / 132 Downloading (6 frames, 3 ticks), 146 Parental Lock (17), 9900 Converting, 9902 / 9903 Spinner (6, 5 ticks), 9904 / 9905 Progress (12, 3 ticks) |
| `TXTS` (text style, 8103) | a `Txtr` ID, a baseline tweak, active shadow, inactive / pressed / rollover text and shadow colours, the shadow's vertical offset | 730 Gradient Window Title, 731 Gradient Utility Window Title, 301 Browser Window button title, 320 search bar buttons, 579 unread count, 1200–1204 the Genius bar |
| `ACfg` 128 (8102) | the browser window: button right margin 24, search button gap 15, minimum 700 × 294, minimum list width 150, default source width 200; the LCD's line gap 2 and centre tweak −1 (small and large text); help tags on | |
| `CWid` (8101) | the list's columns: minimum and default widths and justification per field (Name, Album, Artist, Time, My Rating, …) | |

The window's own widgets (not `PBUT`s, drawn by code):

| Item | Source | Status |
|---|---|---|
| The corner pieces 7000–7006 (TopBevel, TopLeft, TopRight and four masks, 11 × 11), loaded once into a table of seven | `FUN_00028f32` | Done (read) |
| Close / minimize / zoom: 13000 / 13001 / 13002 (56 × 30: four 14-point states, inactive, normal, rollover, pressed; colour above, Graphite below), 13004–13006 a lighter variant; 13010–13016 the same at 13 points (utility windows); 13042 / 13044 the resize grips. Which set is used: section 4 | `png/`, `tl.png` | Read |
| The count badge: 292, then 293 under 100, 294 under 1000, 295 above; the number drawn over it with `TXTS` 579 | `FUN_00101a72` | Done (read) |
| The transport (320: Previous / Play-Pause / Next, five states, three rows Play, Pause, Stop; 322 / 324 smaller), Repeat and Shuffle (432 / 433, 906 / 908), the views (540–542), the volume (434), the LCD's pieces (3003–3011), the Store bar (566–568) | `sheet1.png` | Identified by eye; their code not yet traced |

## 3. Dialogs

The Carbon nibs are plain XML (IBCarbonWindow, IBCarbonHIView with `controlID`s and `TXTS` styles);
`GradientWindow.nib` is the gradient utility window (440 × 257, a 16-point title bar view). The other
dialogs (Preferences, Smart Playlist, Get Info, the importers' settings) are `DLOG` + `DITL` + `CNTL`
+ `dlgx` in `Localized.rsrc` (`cntl.txt`): standard Carbon controls (popups 400–402 on the `MENU`s
above, group boxes 160, separators 144, tabs 128, sliders 50 / 51, Unicode text fields 912 / 914).

## 4. The browser window

The binary keeps its C++ RTTI: 493 class names (`rtti.txt`), each with its vtables and the code
that installs them (`classes.json`, made by `classes.py`). The UI is PowerPlant X (`PPx::CustomView`,
`PPx::CarbonEventHandler`) on HIToolbox compositing windows. The window: `BrowserWindow` (0x5F4 bytes;
vtable 0xbc45a8); its frame `GradientFrameView` < `CustomWindowFrame` < `PPx::CustomView`; the title
bar `WindowTitleBarView`, its text `WindowTitleTextView`, its buttons `WindowFrameButtonView`
< `UIPictureButton`; the LCD `BrowserLCD`, `BrowserLCDDisplay`, `BrowserLCDPlayer`, `LCDPlayerView`,
`LCDBackgroundView`, `LCDProgressBarView`, `LCDStaticTextView`; the source list `SourceDisplay`.
The whole program is disassembled with its imports named in `all.s` (`stubs.py`, `annot.py`); `calls.py`
lists a function's call sites with their constant arguments.

| Item | Source | Status |
|---|---|---|
| Creation: `FUN_0000fcaa` allocates the window object (0x5F4) and calls `FUN_0000fe66`, which creates the window through `FUN_0041764e`: class 6 (document), attributes 0x1080001F (close, zoom, collapse, resizable, compositing), its frame a `GradientFrameView`. Minimum size forced to at least 700 × 400 (`ACfg` says 700 × 294), minimum list width 300 (`ACfg` 150), source list width at least 150 | `FUN_0000fe66`, `FUN_0041764e` | Done (read) |
| The frame's two gradients come from Leopard's content borders: `FUN_00010fc6` makes the `GradientFrameView` (HIObject parameters `GrOp` flags, `CBTp` / `CBtm` the top and bottom border thicknesses, floats); the browser window passes **54** on top (0 when its saved state has bit 0x10 set) and **29** at the bottom. The bars: top = (0, 0, width, structure top + 54), bottom = the last structure bottom + 29 points (`FUN_0001daaa`, recomputed on resize; flag 4: one gradient over the whole height) | `FUN_00010fc6`, `FUN_00011c86` (reads `CBTp` / `CBtm` into +0xB8 / +0xBC), `FUN_0001daaa`, `FUN_0000fcaa` | Done |
| The gradient: a 1-pixel-wide ARGB strip as tall as the bar (90 at most, the rest filled with the end grey), v(t) = start − d·(0.5t + 1.5t² − t³), d = start − end, t from 0 at the top to 1 at the bottom; cached per bar and state | `FUN_00028bba`, `FUN_00028dc6`, `FUN_000289ec` | Done |
| The greys (`GradientFrameView` +0x84 → 0xa5fbc0): top 197 → 150, bottom 191 → 150; inactive (only from 10.5, `Gestalt 'sysv'` ≥ 0x1050, and when the window isn't hilited) top 233 → 207, bottom 229 → 207. A 1-point line between each bar and the content: grey 64 active, 135 inactive (the top bar's bottom row, the bottom bar's top row; not drawn with flag 4) | `FUN_0002776a`, the table at 0xa5fbc0 | Done |
| The corners: the four 9 × 9 corners cleared (`CGContextClearRect`); the top bevel 7000 stretched along the top between TopLeft 7001 and TopRight 7002 (at its own height); then in `destination-in` the masks 7003 / 7004 at the top corners and 7005 / 7006 at the bottom ones (rounded unless the window's flags 1 / 2 say square) | `FUN_0002776a` (vtable slot 82 of `GradientFrameView`) | Done |
| Close / minimize / zoom: `WindowFrameButtonView`, the HIObject class `com.apple.itunes.windowframebuttonview`, kind in parameter `WFBt` (1 close, 2 minimize, 3 zoom; 5, 6, 7 the small ones); pictures **13004 / 13005 / 13006** (small 13014–13016) from 10.5 on, 13000–13002 / 13010–13012 before; accessibility roles AXCloseButton, AXMinimizeButton, AXZoomButton | `FUN_0001c938` | Done (their position in the title bar not yet read) |
| The layout pass (on resize): the content = the window's bounds with top + 54 and bottom − 29; the source list on the left, `+0x13E` wide (at least 150), a 1-point divider, then the main area, whose list keeps at least `ACfg`'s minimum list width (the source list narrows first); the Genius sidebar on the right (its width × a scale at +0x544); a header strip of 27 points over the list for some sources; a 47-point area under the list when shown | `FUN_000249e4` (8 KB) | Partial (the main split read; the browser, Cover Flow, the Store bar and the bottom buttons' rects not yet) |
| The LCD's progress bar: 15 to 5 points above the LCD's bottom (10 high), 25 in from each side, at most 240 wide (centred) | `LCDProgressBarView` slot 35, `FUN_0005f712` | Done |
| The LCD's background and text drawing | section 5 | Done |

### The top and bottom bars' controls

All in the window's root view coordinates (W = its width, H = its height); "mini" is the window's
small mode (+0x172), where the same controls are smaller. The buttons are `UIPictureButton`s
(`FUN_0001f830`) made from a descriptor that `FUN_00142cb2(window, n, &desc)` fills for n = 0…19:
the picture at +8, the rectangle (`SetRect` 0, 0, w, h then `OffsetRect`), the tag at +0x14 (`Bb00`…).
`FUN_0014915a` seeds the descriptor (+0xA = 15, the left group's running x; +0xC = 18, the right
group's running margin) and makes them in order. Mapping index → case: the jump table at 0xa2ff58.

| Item | Source | Status |
|---|---|---|
| Previous `Bb00`: picture 316, 31 × 32 at (24, 8); mini 322, 29 × 29 at (3, 11) | `FUN_00142cb2` case 0 | Done |
| Play / Pause `Bb01`: 320, 39 × 39 at (60, 4); mini 326, 33 × 33 at (32, 9) | case 1 | Done |
| Next `Bb02`: 318, 31 × 32 at (104, 8); mini 324, 29 × 29 at (65, 11) | case 2 | Done |
| The volume slider (window +0x268): `FUN_0001fe98` with track 350 and thumb 352, 78 × 12 at (168, 18), narrower (LCD left − 200) when the LCD leaves less than 79; mini 58 × 12 at (19, 47); its control event `cntl` 0x97 | `FUN_0013be44` | Done (the slider's own drawing not read) |
| The speakers, picture 328: low `Bb03` 12 × 13 at (slider left − 12, 17) (156 without a slider), high `Bb04` 13 × 13 at (slider right + 4, 17); mini at (7, 46) and (81, 46) | cases 3, 4 | Done |
| The LCD (`BrowserLCD`'s background view, +0x278): 200 wide below a 700-point window (a compact flag +0x27C cleared), else 200 + ⅔ (W − 700); 46 high from y 0; centred horizontally in the window (`FUN_0027dd10`). Mini: (106, 10) to (W − 10, 56) | `FUN_000249e4` at 0x25ea1–0x260cd | Done (y relative to the root view as read; not checked against a screenshot) |
| The search field (+0x468): right edge W − 44; left edge W − 49 − ⌊7 · max(0, W − 69 − the LCD's right) / 16⌋ (it takes 7/16 of the room right of the LCD); centred vertically in the band y 4 to 35 (31 high, the float at 0xb7bf54) | `FUN_000249e4` at 0x25f42–0x25fdd, `FUN_0027dcc0` | Done |
| The view buttons, as List `Bb21` 540 (27 wide), Grid `Bb24` 541 (25), Cover Flow `Bb23` 542 (27), 22 high, side by side ending 25 points left of the search field (its left − 15 − 10), centred vertically on the LCD then 3 points up. Each picture: five states across, two rows (off / on) | cases 17–19, the widths at 0xa2ff50, `FUN_001386aa`, `FUN_0027ef78` (align: 1 top, 2 v-centre, 4 bottom, 0x10 left, 0x40 right) | Done |
| The bottom bar's buttons: 20 high, 4 points above the window's bottom (y = H − 24). Left group from x 15 (each after the previous): `Bb11` new playlist 431 (40 wide, main window only), `Bb12` shuffle 433 (39; 908 and 40 in other windows), `Bb13` repeat 432 (39; 906), `Bb14` show / hide artwork 654 (39, main window only). Right group from 18 points in, 7 apart, right to left: `Bb25` 470 (40, no gap after it), `Bb08` 436 (39), `Bb09` / `Bb10` the Import / Burn button 439 (its title's width + 16; on a CD source or a burnable playlist), `Bb15` 460, `Bb16` 472, `Bb17` 469 (39 each, on their sources' conditions), `Bb18` the AirTunes speakers popup 434 (its title + 52, at most 202, or 172 under a 730-point window) | cases 5–16 | Done (the conditions read as calls, not all named) |

## 5. Pictures in code, and the LCD's drawing

| Item | Source | Status |
|---|---|---|
| Every picture the code draws is described by a static table at 0xa622e0, 238 entries of 28 bytes: the `PNG ` ID; columns and rows of states (the cell = image size ÷ grid); fixed left, right, top, bottom margins (a nine-slice: the middle stretched); a flag byte; the state-mapping kind (u32 at +0x10); replacement IDs at +0x14 (when the application's mode at +8 is 6), +0x16 and +0x18 (two alternative appearances, `FUN_0000ee1e` = 1 or 2). Decoded into `pictures.txt` | `FUN_0000ebe4` (lookup and cache), `FUN_0000ee28` / `FUN_0000eef8` (the object: cell size +0x24 / +0x28, grid +0x2C, margins +0x30…+0x36) | Done |
| Drawing a cell: column = f(kind, state), row chosen by the caller (a button's value: Play / Pause / Stop for 320); cut at (col × cell width, row × cell height), then nine-sliced into the destination | `FUN_0002a340`, `FUN_0002a3d6`, `FUN_0002a75a` | Done (the nine-slice's own code not read: the margins' meaning by the resources, e.g. 434 keeps its 23-point speaker and 14-point arrow) |
| States: 0 disabled or inactive (`HIViewIsEnabled` / `HIViewIsActive` false), 1 normal, 2 pressed (control hilite not 0, 254 or 255); 3–5 from other callers. Columns by kind: kind 0 always 0; kind 1: states 1, 2, 3, 5 → 1, else 0; kind 2: 2 → 1, else 0; kind 3: 0 0, 1 1, 2 2, 3 1, 4 0, 5 1; kind 4 (the frame buttons): 0 0, 1 1, 2 3, 3 2, 4 0, 5 1; kind 5 (most five-column buttons): 0 0, 1 1, 2 2, 3 1, 4 3, 5 4; kind 6: 2 → 2, 3 → 1, else 0 | `FUN_0002c5f8`, `FUN_0002b9f0` (jump tables 0xa63d1c, 0xa63d38) | Done |
| The LCD's background (`LCDBackgroundView`, slot 79): picture 153 (96 × 46: two 48-point columns, 8-point fixed caps left and right, the middle stretched to the LCD's width); column 1 when the view isn't active on 10.5 and later (`FUN_00416f78` = `Gestalt 'sysv'` > 0x104F), else column 0. With nothing playing: picture 154 (the Apple, 27 × 33) centred in the LCD, 1 point lower | `FUN_0002a146`, `FUN_00026d52`, `FUN_0002b926` | Done |
| The LCD's own buttons (made when the background view initializes, `UIPictureButton`s): 487 `lcNE` (three columns, kind 3), 668 `lcGE` (two, kind 2), 461 `lcSN` (two, kind 2), 473 `lcST` (three, kind 3), with help tags 0x990005…7 | `FUN_0001f56a` | Done (their places not read) |
| The text (`LCDStaticTextView`, slot 80): clipped to the view; one line centred (justification 2) when its width fits; otherwise it scrolls left continuously, drawn twice with a 10-point gap, a frame every 1/15 s after a 1.5 s pause; a changing line slides in vertically (+0x25A) over the old one | `FUN_0004f66c`, `FUN_0004f7c6`, `FUN_0004ecf6`, `FUN_002985e2` | Done |
| The fonts: `FUN_00298df4` reads `TXTS` n, else `Txtr` n. `Txtr`: size, style, justification, (1), RGB, (0xFFFF), font name. The window loads 301 Window Buttons (Lucida Grande 11, centred), 307 Inline Window Buttons (11), **302 LCD (Lucida Grande 11, regular, black, centred)**, 303 Search Edit Text (12), 305 Browser Device Capacity (11), 306 LCD Large (13); `TXTS` 301 adds its shadow (#C4C4C4, 1 down) and inactive colour (#5E5E5E) | `FUN_0000fe66` at 0x104f1, `Localized.rsrc` | Done |
| The progress bar (`LCDProgressBarView`, slot 79): the frame 475 (12 × 10, caps 3 and 2) over the bar; inside it (2, 1) in, 4 narrower, 3 shorter: the fill 478 (1 × 7) stretched to ⌊inner width × the fraction played⌋; when the length is unknown (−1), the stripe 477 (8 × 7) repeated every 8 points from a phase that moves, clipped to the inside, on a 0.1 s timer; nothing when the player's flag 8 (+0x81C) is set | `FUN_0005f7ca`, `FUN_0005f658` | Done |
| The volume slider: track 350 (78 × 12, one cell), thumb 352 (three 12 × 12 cells, kind 3: disabled, normal, pressed) | the table; `FUN_0001fe98` | Partial (the thumb's travel not read) |

## 6. The port

[`shell/src/itunes`](../shell/src/itunes), a program on AquaKit (`iTunes.app/Contents/MacOS/iTunes`):
`resources.cpp` reads `iTunes.rsrc` and `Localized.rsrc` (the Resource Manager's map), the PNG
resources, the `xMNU` menus, the `Txtr` styles, with the picture table's entries the window needs;
`library.cpp` the library (TagLib's tags); `player.cpp` the playback (libmpv); `main.cpp` the window
and the menus. `extract.py` now takes `iTunes.rsrc`, `Localized.rsrc` and `Localizable.strings` from
`iTunes.pkg`. Verified in Xvfb under WindowServer and SystemUIServer with three tagged MP3s: the
library made at the first launch, the window, a song played from a double click, the LCD (title,
artist, the bar, the times, the scrolling title), Pause, the File and Controls menus, resizing down
to 700 wide.

| Item | Source | Status |
|---|---|---|
| The menu bar: MBAR 129's `xMNU`s, their titles from Localizable.strings, keys and modifiers (Shift, Option, Control, no Command), the arrows' glyphs, the submenus, the Option alternates; the Apple menu's items in the application's menu with Services, Hide, Quit; the hidden Commands menu's keys; Play on the space bar | section 1 | Done |
| The commands done: Play / Pause, Next, Previous (the song's start past 3 s), the volume (⌘↑ ⌘↓, Mute), Shuffle, Repeat Off / All / One, Add to Library…, New Playlist, New Playlist from Selection, Delete (from the library or the playlist), Show in Finder (its folder), Show Current Song, the search field (⌥⌘F), Minimize, About | | Done (Get Info, Smart Playlists, the Store, burning, iPods, the equalizer, the visualizer, Cover Flow and the grid: not done) |
| The window: WindowServer's title bar with a unified toolbar of 54, so that its gradient goes on into the band; the band's 54 rows drawn as the last of GradientFrameView's 76 (197 → 150, inactive 233 → 207) with the line of 64 (135); the bottom band 191 → 150 and its line | section 4 | Partial: the title bar is WindowServer's own (Aqua's), not iTunes' gradient and corner pieces (that needs a borderless window that WindowServer still moves and resizes) |
| The places: the transport, the volume and its speakers, the LCD, the search field, the view buttons, the bottom band's buttons, the totals (Txtr 304), the source list (200, at least 150) and the songs (at least 300) | section 4 | Done (the speakers' rows: 0 and 2 by their art) |
| The pictures' states: `stateColumn` and the columns and rows of the table; the buttons' rows (Play / Pause, Shuffle off / on, Repeat off / all / one) | section 5 | Done |
| The LCD: 153 with its caps, the Apple when stopped, the two lines in Txtr 302 centred or scrolling (1.5 s, 1/15 s, a 10-point gap; one point a frame, by eye), the bar (475, 478) and a click or drag on it to move in the song; the times in Lucida Grande 10 in the margins | section 5 | Partial: the lines' places, the second line's content (the artist, else the album), the scrolling's speed and the times' font by eye; the remaining time is cut at the LCD's 200-point width |
| The songs: Name, Time, Artist, Album, Genre, Plays (CWid 9000's widths), rows of 16 in Lucida Grande 11 striped white and 237 243 254, the song playing in bold with ♫, sorting by a click on a header | | Partial: the stripes' colour, the rows' height and the playing mark by eye; no columns chosen by View Options, no resizing of columns |
| The sources: LIBRARY (Music), STORE (iTunes Store, not selectable), PLAYLISTS, with the Source Icons (Small) 441's art | | Partial (no Movies, Podcasts, Radio, devices; no renaming) |
| The library: ~/Music/iTunes/iTunes Music Library.xml in iTunes' plist format (Tracks with Track ID, Name, Artist, Album, Genre, Kind, Size, Total Time, Track Number, Year, Date Added, Bit Rate, Sample Rate, Play Count, Location; Playlists with the Master "Library"); the music folder (XDG_MUSIC_DIR) added at the first launch; files given at launch added | | Done (no `iTunes Library` binary database, no copying into the iTunes Music folder) |
| Playback: libmpv (a song after another in the list's order, or shuffled; Repeat All and One), the volume as the slider's value | | Done |

## Next

- iTunes' own window frame (its title bar's gradient, corner pieces and 13004–13006 buttons):
  WindowServer would need to move, resize and shadow a borderless window.
- The LCD's two text lines' places and what they show in turn (title, artist, album, the times).
- The rest of `FUN_000249e4`: the browser columns (+0xC0 / +0xCC / +0xD8, a third of the width
  each, at least 300 high), Cover Flow, the Store bar.
- The library: `iTunes Library` (binary, `hdfm`) and `iTunes Music Library.xml`.
- The port (an AquaKit program, a player on GStreamer or Qt Multimedia): nothing done.
