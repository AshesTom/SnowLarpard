# iTunes (8.2.1, 10.6): what was recovered

First pass over the iTunes on Snow Leopard's DVD. No Apple file is in the repository: the material
below lives in `.local/media/re/itunes/` (ignored by git) and is rebuilt from the DVD. The last iTunes
for Snow Leopard, 11.4, is a separate Software Update product for 10.6.8 (not part of the combo):
section 7.

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
| Close / minimize / zoom: `WindowFrameButtonView`, the HIObject class `com.apple.itunes.windowframebuttonview`, kind in parameter `WFBt` (1 close, 2 minimize, 3 zoom; 5, 6, 7 the small ones); pictures **13004 / 13005 / 13006** (small 13014–13016) from 10.5 on, 13000–13002 / 13010–13012 before; accessibility roles AXCloseButton, AXMinimizeButton, AXZoomButton | `FUN_0001c938` | Done |
| The title bar: `GradientWindow.nib`'s document window (the browser window's frame is the same): `WindowTitleBarView` 1000, 22 high across the top; its title `WindowTitleTextView` 1001 at (102, 1, width − 204, 18), TXTS 730 (`Txtr` 13000: Lucida Grande 13, black; its shadow #C4C4C4 one point under, offset from the TXTS's last field; the inactive colours left at 0); the buttons 1050 / 1051 / 1052 at x 8 / 29 / 50, y 4, 14 × 15 (`WFBt` 1 / 2 / 3, 13000–13002 in the nib, 13004–13006 from 10.5). The utility window's: 16 high, TXTS 731 (Txtr 102, 11 points), its buttons 13014–13016 at 5 / 23 / 41, y 1. The buttons share one tracking area, the union of the three (`FUN_0001b9c8`, IDs 0x41A–0x41C): over any of them all three show their rollover column (kind 4: inactive, normal, rollover, pressed) | `GradientWindow.nib`, `WindowTitleBarView` (vtable 0xbf02c8: `FUN_0001d6b6` finds the buttons, `FUN_0001b9c8`, `FUN_0001da10`), the TXTS reader `FUN_00298df4` | Done (the inactive title's colour: TXTS 730 leaves it black at 0, how the view greys it not read) |
| The layout pass (on resize): the content = the window's bounds with top + 54 and bottom − 29; the source list on the left, `+0x13E` wide (at least 150), a 1-point divider, then the main area, whose list keeps at least `ACfg`'s minimum list width (the source list narrows first); the Genius sidebar on the right (its width × a scale at +0x544); a header strip of 27 points over the list for some sources; a 47-point area under the list when shown | `FUN_000249e4` (8 KB) | Partial (the main split read; the browser, Cover Flow, the Store bar and the bottom buttons' rects not yet) |
| The LCD's progress bar (`LCDProgressBarView`, its own rect 15 to 5 points above its bottom, 25 in from each side, at most 240 wide): not the playing LCD's, which has `ITScrubberView` (below) | `LCDProgressBarView` slot 35, `FUN_0005f712` | Done |
| The playing LCD (`LCDPlayerView`, made by `FUN_0004e1b2`): +0x48 the scrubber `ITScrubberView` (`FUN_0004e4d4`, its time mode from the `stim` preference, 1 or 2), +0x4C / +0x50 the two lines (`LCDStaticTextView`, `FUN_0004e71c`), +0x58 / +0x54 the times (UI text views in the LCD's style, right- and left-justified). Its layout (`FUN_0004ee6a`, slot 35), in W × H: t = the width of "-00:00:00" (0x904120) in that style, measured once; the scrubber at (t + 2, H − 15), W − 2(t + 2) wide, 14 high, inset by (w − 240) / 4 on each side past 240; the time played at (its x − t − 2, its y − 1), t × 14; the other time at its right + 2; the first line 2 down, as wide as its text and centred in W − 10 from 5 when it fits, else (5, W − 10); the second at (37, 16), W − 74 wide | `FUN_0004ee6a`, the floats at 0xaaf640–0xaaf6f4 | Done |
| The scrubber's art: PSLD 512 (picture PICT 513, all black; mask PICT 512, 29 × 10 DirectBits, white leaving the picture out): the slider rect (0, 0)–(21, 10) with 7-point fixed ends, the thumb (21, 0)–(29, 10) a diamond, the drag rect 2 in from each end; drawn by `FUN_0027a97a` (the track stretched, the thumb at the value; a 50% pattern in #ACB098 over a loaded part, a stream's) | `FUN_0027a97a`, PSLD 512 | Done (the loaded part not drawn: no streams) |
| The times (`FUN_0004ea50`, `FUN_0004eaf6`): whole seconds; left the time played; right, when the length is known, "-" and the time left (mode 1) or the whole time (mode 2); a click on the right one switches the mode and keeps it in `stim` (`FUN_0013658a`) | | Done |
| What the lines show (`BrowserLCDPlayer`, `FUN_0004d618`, `FUN_0004d90c`, `FUN_0004dcf4`, `FUN_0004df02`): each line a kind range and its current kind: the first 0 to 0 (the name, the track info's +0x8), the second 1 to 5 (1 the artist, +0x408, the CD-Text performer; 2 the album, +0x608; 3 / 4 a stream's two strings; 5 a stream's `name`); a new song puts each line on the first of its kinds that changed; every **10 s** (a timer, 0xa2a348) the second goes to its next non-empty kind, the new text sliding up over the old (+0x25A a point every 1/15 s, the old one going out at the top) | `FUN_00051dfe`, `FUN_00051e3a`, `FUN_0004ebcc`, `FUN_0004f66c` | Done |
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
| The window: its own frame, as GradientFrameView draws it: a borderless window (WindowServer gives it a frame's outline, rounded at the top and the bottom, and its shadow, because it says which bottom corners it has), the gradient's 76 rows (197 → 150, inactive 233 → 207) under the title bar and the band, the bevel 7001 / 7000 / 7002 along the top, the title, the three buttons with their shared rollover; a drag on the bands moves the window, a double click on the title bar puts it in the Dock; the line of 64 (135); the bottom band 191 → 150 and its line | section 4 | Done (the zoom button, iTunes' mini player, does nothing yet; the corners' masks 7003–7006 are the outline's whole pixels, not their antialiasing) |
| The places: the transport, the volume and its speakers, the LCD, the search field, the view buttons, the bottom band's buttons, the totals (Txtr 304), the source list (200, at least 150) and the songs (at least 300) | section 4 | Done (the speakers' rows: 0 and 2 by their art) |
| The pictures' states: `stateColumn` and the columns and rows of the table; the buttons' rows (Play / Pause, Shuffle off / on, Repeat off / all / one) | section 5 | Done |
| The LCD: 153 with its caps, the Apple when stopped; the lines, the scrubber (PSLD 512's PICTs decoded) and the times placed as `FUN_0004ee6a` does; the title, then the artist and the album in turn every 10 s sliding up; scrolling a point every 1/15 s after 1.5 s; the time left or the whole time (`stim`) by a click; a click or drag on the scrubber to move in the song | section 5 | Done (the antialiasing of the text is cairo's, not QuickDraw's) |
| The songs: the status column, Name, Time, Artist, Album, Genre, Plays (STR# 9006's titles, CWid 9000's widths); the header ListHeader 304 (its sorted column's rows 1 / 2, Txtr 9004); rows of the line height + 2 in Txtr 9002, striped by the colour table's 14 and 16, the columns' lines 217, the selection 10 / 19 / 22 in a key list, else 11 / 20 / 23; the song playing: TrackStatusIcons 306's speaker; sorting by a click on a header | `FUN_0040ef86`, the colour table `FUN_00021488` | Done (no columns chosen by View Options, no resizing of columns) |
| The sources: LIBRARY (Music), STORE (iTunes Store, not selectable), PLAYLISTS; rows of 20 on the colour table's 27 (28 inactive), the selection 300 (column 0 key, 1 not), the headers in Txtr 9004 at x 9 (29 / 30, shadow 31), the items' icons 441 at 14n + 19 and text in Txtr 9002 at 14n + 39 (32, or 33 with the shadow 34 / 35 when selected) | `FUN_0002d3b2`, `FUN_0016cb22` | Done (no Movies, Podcasts, Radio, devices; no renaming) |
| The library: ~/Music/iTunes/iTunes Music Library.xml in iTunes' plist format (Tracks with Track ID, Name, Artist, Album, Genre, Kind, Size, Total Time, Track Number, Year, Date Added, Bit Rate, Sample Rate, Play Count, Location; Playlists with the Master "Library"); the music folder (XDG_MUSIC_DIR) added at the first launch; files given at launch added | | Done (no `iTunes Library` binary database, no copying into the iTunes Music folder) |
| Playback: libmpv (a song after another in the list's order, or shuffled; Repeat All and One), the volume as the slider's value | | Done |

## Next

- The mini player (the zoom button): the window down to its title bar and top band.
- The rest of `FUN_000249e4`: the browser columns (+0xC0 / +0xCC / +0xD8, a third of the width
  each, at least 300 high), Cover Flow, the Store bar.
- The library: `iTunes Library` (binary, `hdfm`) and `iTunes Music Library.xml`.

## 7. iTunes 11.4 (Software Update, 10.6.8)

The last iTunes for Snow Leopard. Apple's Snow Leopard catalog (`index-leopard-snowleopard.merged-1`)
still serves two iTunes products: **041-88055, iTunes 10.6.3** (its .dist: 10.6 and later) and
**041-84834, iTunes 11.4** (its .dist refuses below 10.6.8, `ERROR_0`; it replaces 11.3.1 or older,
`SourceVersion` 1140011004000018). 11.4's packages: iTunesX.pkg (192.5 MB, the application),
MobileDevice, CoreFP, CoreADI, iTunesAccess. The material is in `.local/media/re/itunes11/`.

| File | Notes |
|---|---|
| `iTunes.app/Contents/MacOS/iTunes` | universal **i386 + x86_64**; `LSMinimumSystemVersionByArchitecture`: i386 10.6.8, x86_64 10.7, so on Snow Leopard the **i386** slice runs (26.5 MB, sha256 `346a1382…2c76`, stripped: 83 286 functions, 2 426 C++ classes by their RTTI). C++ on its own view layer (`ITView`, `ITWindowFrameView`, `ITViewController`; boost's shared pointers), a little Cocoa (`__OBJC`, the old runtime) |
| `Resources/iTunes.rsrc` | 1 012 `PNG ` (1×), an `aete`, an `icns`: no `TMPL`, `PBUT` or `Txtr` any more. `Images.rsrc` (512) and `Images2.rsrc` (215) are their @2x (ID + 15000 for most) |
| `English.lproj/TextStyles.plist` | the text styles, by the numbers the Txtr resources had (302 the LCD: HelveticaNeue-Medium 11 centred; 9002 the list: HelveticaNeue 12; 9004 its header) |
| `English.lproj/Localizable.strings` | a binary plist (12 086 strings) under hashed keys; `LocalizableStringsMap.plist` gives each key its number, which is **(8.2.1's STR# ID << 16) \| its index** (`9006.001` "Name" is 0x232E0001; 135.006 LIBRARY, 135.009 PLAYLISTS) |
| `English.lproj/MainMenu.nib` | an empty `AMainMenu`: the menus are tables in the program (below) |
| the package's `Scripts/obsolete-files` | 5 453 paths its preinstall (`deleteObsoleteFiles.pl`, `deleteomatic`) deletes: 8.2.1's `Localized.rsrc` and icons among them |

REA 6.9.0 (Ghidra 12.2) on the i386 slice; section 4's tools (`classes.py` for the classes and their
vtables, `all.s`) redone for 11.4; `pictures.txt` (the picture table), `menus11.txt` (the menus).

| Item | Source | Status |
|---|---|---|
| The picture table: built at launch by a static initializer (0xa79b59–0xa9ce1e) at 0x18fdc60, 823 entries of 0x4C bytes: the ID, columns, rows, fixed left / right / top / bottom, flags, the kind (+0x20), the @2x and alternative IDs (+0x24…). Read back from the initializer's stores | `FUN_00a76eb8` (lookup), `FUN_00a77820` (a cell into a rect) | Done (`pictures.txt`) |
| The menus: a table of menus (initializer at 0x934e2a: ID, 4-char tag, title's text number, items, count; 129 iTunes, 131 File, 133 Edit, 135 View, 134 Controls, 141 Store, 138 Window, 140 Help; 160 Commands, 139 Dock and the submenus) and items of 16 bytes {command, submenu, 0, text number}, `DIV0` a separator. The texts carry their keys: `Minimize###M`, `Show in Finder###R#shift`, `Convert…####option` (an Option alternate) | the items at 0x133dba4…, Controls' built at launch at 0x19343b4 | Done |
| The main window's frame: `BrowserWindowFrameView` (the other windows' is `ITGradientFrameView`); its body parts (`_BrowserWindowFrameViewBodyPartView`): the top **5000** (144 × 61, two 72-point columns active and inactive, 4-point ends; its last rows the lines 185 and 123) and the bottom **5001** (144 × 24, its first row 123), column 1 when the window isn't key | `FUN_001cba14`, `FUN_0001d572` | Done |
| The other windows' gradient frame (`ITGradientFrameView`): 13200 / 13204 / 13206 / 13208 / 13209 (1040 wide, two columns); the top rect the title bar plus the top border, the bottom rect the bottom border (+12 with a grow box) | `FUN_007d2956`, `FUN_007d3404`, `FUN_007d208e` | Read |
| The stoplights (`ITWindowControlsView`, `ITStoplightViewController`): 13004–13006 (14 × 16 cells, kind 4, colour and Graphite rows) in a 300 × 23 area, at (7, 5), 7 apart (6 for the small ones) | `FUN_001e8e54`, `FUN_001e97b0` | Done |
| The top part's layout (`BWHeaderController`): the transport at (60, 16); the volume from its right + 10, at most 110 wide, its right 8 before the AirPlay button (5 before the LCD); under 68 the transport moves left to keep 68; the LCD **40 %** of the width, centred; the search field 175 wide, its right at W − 36 (W − 40 with the full-screen button), narrower by the Downloads button (16 or 25, + 5); mirrored right to left | `FUN_001ebd78` | Done (AirPlay, Downloads, Up Next not ported) |
| The transport (`TransportControlView`): Previous **317**, Play **321** (rows Play, Pause, Stop), Next **319**, 30 × 30 cells of kind 5, 13 apart, centred on Play | `FUN_001d2e24` | Done |
| The volume (`VolumeControlView`): the speaker **328** (21 × 21, four rows quiet to loud), the slider (track **350**, two rows, 10-point ends; thumb **352**, kind 3), the loud speaker at the right, the slider between them less 4 | `FUN_001d4018`, `FUN_001d4cc4` | Done |
| The LCD (`LCDBackgroundView`): picture **150** (464 × 60, 4-point ends) through a layer, the Apple **151** centred when nothing plays. `LCDPlayerView`: the artwork at (12, 8), as tall as the LCD less 16, past 360 points; the scrubber (track **160**, thumb **161**) 2 above the bottom between the two times, past 240 inset by a tenth of the excess; the two lines centred above it | `FUN_00022168`, `FUN_0002257e`, `FUN_001dca1c`, `FUN_002b1dae` | Partial (the lines' exact heights read approximately, the artwork not ported) |
| The list's header (`ITListHeaderView`, pictures 9137–9140) and the status icons | | Missing (the port draws a flat header and the volume's speaker) |

The port (`shell/src/itunes`) follows the installed version (`resources.cpp`, Info.plist's version).
With 11.4: the menus from the tables above (their texts and keys from the user's Localizable.strings
through LocalizableStringsMap.plist), the text styles from TextStyles.plist, the pictures by 11.4's
table; the window as above (the top part, the stoplights, the transport, the volume, the LCD, the
search field, the status bar with the totals in 301); the sidebar shown (11.4 hides it by default);
no Up Next, artwork, Store or grid. 8.2.1 stays as it was.

Software Update (`tools/softwareupdate.py`): on 10.6.8, the catalog's iTunesX.pkg products whose
.dist allows the system and that are newer than the installed iTunes; only iTunesX.pkg is downloaded;
no restart: the Software Update app installs it after the download, in its install window.
`extract.py --update` deletes what a package's `Scripts/obsolete-files` lists before laying its files
over, converts binary-plist `.strings`, and takes 11.4's TextStyles.plist, AppConfig.plist and
LocalizableStringsMap.plist. Verified in Xvfb: the scan offers iTunes 11.4 (192.5 MB) on 10.6.8 with
8.2.1 installed; the app installs it (Show Details without the restart line); iTunes then opens in
11.4's window and menus, and plays.

## 8. iTunes 11.4: the nav bar and the iTunes Store

| Item | Source | Status |
|---|---|---|
| The nav bar (`BWNavBarController`; `BWWindowController` puts it between the top part and the body): 29 points, picture 870 (light theme; 880 the dark one), a line 887 under it | `FUN_000ffd88` (870), `FUN_00109564` (887) | Done |
| The library's (`BWLibraryNavBarController`, `FUN_00100874`): the media kind's picture 8251 (24 × 24 cells, rows Music, Movies, TV Shows, Podcasts, Books, Apps, iTunes U, Tones…) at the left; the views (`BWNavBarRadioButtonDataSource`, their names in STR# 163 by kind through the jump table at 0x13664d0: Songs 163.099, Albums 163.100, Artists 163.101, Genres 163.102, Playlists 163.104, Radio 163.106) on 871 (three cells, 11-point ends), 869 (blue) when chosen, text in TextStyles' 310 (`FUN_0045b6b2`: 310, 311 or 1306 by the bar's kind), 3 apart; "iTunes Store" (163.084) on 871 at the right (`FUN_00103c9e`) | | Done (only Songs has its view in the port) |
| The Store's (`BWStoreNavBarController`): back 882, forward 883 (three cells), home 879; the menus its pages give (`iTunes.addProtocol(<plist>)`: navbar ▸ menus, each a title, a URL and items, dividers among them; navbar-selected-menu) on the pills 876, or 886 with their arrow (a drop-down of the items), text 311; "Library" (163.092) at the right | `FUN_00215f7a`, the pictures 876–886 | Done |
| The URL bag: `https://init.itunes.apple.com/bag.xml?ix=5` (a signed plist {bag, certs, signature}, the bag a plist in its data); its `storeFront` the first page; the response's `X-Set-Apple-Store-Front` sent back as `X-Apple-Store-Front` | the strings, `MusicStore` | Done (still served by Apple to iTunes 11.4) |
| The User-Agent: "iTunes/11.4 (Macintosh; OS X " the system ")" and " AppleWebKit/" for the pages | `FUN_0096c93b` | Done |
| The pages' JavaScript object `iTunes` (`StoreiTunesJSObject`): 157 bindings in the tables at 0x227fe8 (methods), 0x229574 (library and playback), 0x229e9e and 0x22a2fc (properties), 0x22a674 (osBuildID, platform "Mac", version "11.4", acceptedLanguages, appIsFrontmost, appBundleName "com.apple.iTunes"); its window (0x22d996: id, data, open, colours, modal, title, style, width, height; 0x22d8d2: close, resizeTo); Storage (setValue, valueForKey, removeValue, removeAllItems) | `jsbindings.txt`, `FUN_00232ce6`, `FUN_0022cb0b`, `FUN_0022e568`, `FUN_00d610aa`, `FUN_0023c262` | Done (what needs an account answers null) |
| The catalog's lookups (`client-api.itunes.apple.com/…/lookup`, the "lockup" and "product-dv" profiles) are signed by the client (`X-JS-SP-TOKEN`, `signStorePlatformRequestData`, obfuscated in the binary) | `FUN_00d6088d` | Not reproduced: see below |

The port: `shell/src/itunes/storeprocess.cpp`, the program `iTunesStore` (built when Qt WebEngine
is), renders the pages off screen as WebKit2's web content process would, with the `iTunes` object
above, the bag's store front, cross-origin requests allowed (WebKit 1 in iTunes had them);
`store.cpp` shows its pictures in the window and hands it the mouse, the wheel and the keyboard;
the nav bar is `main.cpp`'s `NavBar`. The catalog lookups, whose signature is not reproduced, are
answered for this proof of concept from Apple's public lookup API (`itunes.apple.com/lookup`, with
`entity=song` for a product page) made into the StorePlatform shape the pages read: `{results: {id:
{id, kind, name, artistName, url, artwork {url "…/{w}x{h}bb.{f}", width, height}, offers [{price,
priceFormatted, actionText, assets [the preview]}], childrenIds, children}}, version: 2}`. Verified in
Xvfb: the Store's home with its Cover Flow (turning, a click opening the album), the genre pages'
grids (New Albums, Pre-Orders, Top Albums), an album's page with its songs, the menus and their
drop-downs, back and forward. Not done: buying, signing in, previews' playback, the library's
Albums, Artists, Genres, Playlists and Radio views.
