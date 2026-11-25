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
  the `TMPL` templates Apple left in the files), `ditl.py` (a dialog's DITL items with their CNTLs). Outputs: `menus.txt` (every `xMNU`),
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
| The layout pass (on resize): the content = the window's bounds with top + 54 and bottom − 29; the source list on the left, `+0x13E` wide (at least 150), a 1-point divider, then the main area, whose list keeps at least `ACfg`'s minimum list width (the source list narrows first); the Genius sidebar on the right (its width × a scale at +0x544); a header strip of 27 points over the list for some sources; a 47-point area under the list when shown | `FUN_000249e4` (8 KB) | Partial (the main split, the browser, Cover Flow (+0x152 = 2: its height the preference +0x593a, at least 300, at most what leaves the songs 120, the songs right under it) and the Store bar (the Store source, kind 2: the main area's top 27 points, the pages under it) read; the bottom bar's buttons are not placed here (their places: the table below); the browser's columns when more than three (+0x15C, eight at most, +0x153 of them) each an equal share of the width; the LCD 200 + 2/3 of the width past 700, 46 high) |
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
| The mini player (`FUN_00149a4a`, the zoom button and Window ▸ Zoom): the flag +0x172; the window's bounds kept per mode (+0x176 full, +0x180 mini, the mini one 200 wide at first: `FUN_0000fe66` at 0xff71, or the saved window's left, top and width); its limits through kEventWindowGetMinimumSize / GetMaximumSize (the window's vtable 0xbc45a8, slots 0x18c / 0x190, `FUN_00144158`, `FUN_00144124`, `FUN_001440ca`): mini 65 high, 65 to 450 wide; full at least the preference's minimum, unbounded; the controls at their mini rects (above). At a narrow width the LCD's layout (`FUN_0004ee6a`) hides nothing: the scrubber's rect goes empty, the times stay where it puts them | `FUN_00149a4a`, `FUN_001440ca` | Done (the window's attributes it changes, `FUN_0041760a`, not read; the times' overlap not checked against a screenshot) |
| The commands done: Play / Pause, Next, Previous (the song's start past 3 s), the volume (⌘↑ ⌘↓, Mute), Shuffle, Repeat Off / All / One, Add to Library…, New Playlist, New Playlist from Selection, Delete (from the library or the playlist), Show in Finder (its folder), Show Current Song, the search field (⌥⌘F; the words it filters by, its menu, the Store's search: section 28), Minimize, About; Show Browser (⌘B), File ▸ Rating, Get Info (⌘I), Window ▸ Equalizer (⌥⌘2): section 9; as Grid (⌥⌘4): section 10; New / Edit Smart Playlist: section 11; as Cover Flow (⌥⌘5): section 12; View Options (⌘J): section 13; Preferences (⌘,): section 14; Show Duplicates: section 18; Advanced ▸ Open Audio Stream (⌘U): section 19; Select All, Select None and the songs chosen together: section 20; New Playlist Folder: section 22; Library ▸ Import Playlist, Export Playlist, Export Library: section 27; the iTunes Store: section 15; Show Visualizer (⌘T): section 16; Advanced ▸ Subscribe to Podcast…, the LIBRARY's sources, movies and audiobooks: section 29 | | Done (burning and iPods: not done) |
| The window: its own frame, as GradientFrameView draws it: a borderless window (WindowServer gives it a frame's outline, rounded at the top and the bottom, and its shadow, because it says which bottom corners it has), the gradient's 76 rows (197 → 150, inactive 233 → 207) under the title bar and the band, the bevel 7001 / 7000 / 7002 along the top, the title, the three buttons with their shared rollover; a drag on the bands moves the window, a double click on the title bar puts it in the Dock; the line of 64 (135); the bottom band 191 → 150 and its line | section 4 | Done (the corners' masks 7003–7006 are the outline's whole pixels, not their antialiasing) |
| The places: the transport, the volume and its speakers, the LCD, the search field, the view buttons, the bottom band's buttons, the totals (Txtr 304), the source list (200, at least 150) and the songs (at least 300) | section 4 | Done (the speakers' rows: 0 and 2 by their art) |
| The pictures' states: `stateColumn` and the columns and rows of the table; the buttons' rows (Play / Pause, Shuffle off / on, Repeat off / all / one) | section 5 | Done |
| The LCD: 153 with its caps, the Apple when stopped; the lines, the scrubber (PSLD 512's PICTs decoded) and the times placed as `FUN_0004ee6a` does; the title, then the artist and the album in turn every 10 s sliding up; scrolling a point every 1/15 s after 1.5 s; the time left or the whole time (`stim`) by a click; a click or drag on the scrubber to move in the song | section 5 | Done (the antialiasing of the text is cairo's, not QuickDraw's) |
| The songs: the status column, Name, Time, Artist, Album, Genre, Plays (STR# 9006's titles, CWid 9000's widths); the header ListHeader 304 (its sorted column's rows 1 / 2, Txtr 9004); rows of the line height + 2 in Txtr 9002, striped by the colour table's 14 and 16, the columns' lines 217, the selection 10 / 19 / 22 in a key list, else 11 / 20 / 23; the song playing: TrackStatusIcons 306's speaker; sorting by a click on a header | `FUN_0040ef86`, the colour table `FUN_00021488` | Done (the columns: View Options, section 13; no resizing of columns) |
| The sources: LIBRARY (Music), STORE (iTunes Store, not selectable), PLAYLISTS; rows of 20 on the colour table's 27 (28 inactive), the selection 300 (column 0 key, 1 not), the headers in Txtr 9004 at x 9 (29 / 30, shadow 31), the items' icons 441 at 14n + 19 and text in Txtr 9002 at 14n + 39 (32, or 33 with the shadow 34 / 35 when selected) | `FUN_0002d3b2`, `FUN_0016cb22` | Done (the LIBRARY's Movies, TV Shows, Podcasts, Audiobooks, Applications, Radio, Ringtones: section 29; devices not done; renaming: section 26) |
| The library: ~/Music/iTunes/iTunes Music Library.xml in iTunes' plist format (Tracks with Track ID, Name, Artist, Album, Genre, Kind, Size, Total Time, Track Number, Year, Date Added, Bit Rate, Sample Rate, Play Count, Location; Playlists with the Master "Library"; the media kinds' keys and the distinguished playlists: section 29); the music folder (XDG_MUSIC_DIR) added at the first launch; files given at launch added | | Done (no `iTunes Library` binary database, no copying into the iTunes Music folder) |
| Playback: libmpv (a song after another in the list's order, or shuffled; Repeat All and One), the volume as the slider's value | | Done |

## Next

- The rest of `FUN_000249e4`: Cover Flow, the Genius sidebar, the Store bar.
- Cover Flow's scroller and full screen (section 12); the grid's own cells (section 10); the smart playlists' own
  blobs (section 11), View Options (DITL 144), the Preferences (DITL 134 and its panes
  256–263), the columns' widths dragged.
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
| The LCD (`LCDBackgroundView`): picture **150** (464 × 60, 4-point ends) through a layer, the Apple **151** centred when nothing plays. `LCDPlayerView`: the artwork at (12, 8), as tall as the LCD less 16, past 360 points; the scrubber (track **160**, thumb **161**) 2 above the bottom between the two times, past 240 inset by a tenth of the excess; the two lines centred above it | `FUN_00022168`, `FUN_0002257e`, `FUN_001dca1c`, `FUN_002b1dae` | Partial (the lines' exact heights read approximately) |
| The LCD's artwork (`FUN_002b1dae`): its view at (12, 8), (H − 16) square; shown when the song has artwork (+0xE2B) and the LCD is wider than 360, the lines' and the scrubber's area then beginning at its right edge, else at 12 | `FUN_002b1dae` | Done (the song's own picture from its tags, TagLib's PICTURE) |
| The songs' pictures and styles (`FUN_00e5675c`, by the text size: small or large): the header's style 9004, the rows' 9002 (9003 large), the header picture **304** (**313** right to left), the status icons **8455**, the checkboxes 795, the stars 528–547, the HD/CC badges 714–718, the iCloud states 8450; the header view at least 22 high (26 large) | `FUN_00e5675c` | Done (header, status icons) |
| The header (`FUN_00024686`, called by the list's drawing `FUN_00023ec8`): each column's cell of 304 (40 × 23, 2 and 16 fixed), its row the column's sort indicator (1 ▼, 2 ▲), its column by `FUN_000200da`'s kind 3 (state 0 normal → 1, 1 pressed → 2, 4 inactive window → 0; 11.4's state tables at 0x134cd44, not 8.2.1's); the title in 9004, 5 in, 5 from the right or 16 when sorted; columns 0x82 and 0x7D draw an icon instead | `FUN_00024686`, `FUN_000200da` | Done (which of rows 1 and 2 is ascending assumed: 2) |
| The status icons (`FUN_00026da7`): 8455 (20 × 20, two columns: the second on a highlighted row), its row by the track's status through the table at 0x137277c: 2 → 1 (paused, the speaker), 3 → 0 (playing, its waves), 14 → 2 (✓), 15 → 3 (⊗), 4 / 8 / 13 → 5 (!), 9 → 7, 10 → 6 | `FUN_00026da7` | Done (playing and paused) |
| The edge shadows 9137–9140 (8 × 15, left and right, light and dark: `0x833e11`) | | Read, not ported |

The port (`shell/src/itunes`) follows the installed version (`resources.cpp`, Info.plist's version).
With 11.4: the menus from the tables above (their texts and keys from the user's Localizable.strings
through LocalizableStringsMap.plist), the text styles from TextStyles.plist, the pictures by 11.4's
table; the window as above (the top part, the stoplights, the transport, the volume, the LCD, the
search field, the status bar with the totals in 301); the sidebar shown (11.4 hides it by default);
the songs' header and status icons, the LCD's artwork, the Store (section 8), the MiniPlayer, Up Next and the grids (section 17) since. 8.2.1 stays as it was.

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

## 9. iTunes 8.2.1: the browser, the ratings, Get Info, the Equalizer

| Item | Source | Status |
|---|---|---|
| The browser (View ▸ Show Browser `SBro`, ⌘B): three `ColumnDisplay`s over the songs. Their rects (+0xC0 Genre, +0xCC Artist, +0xD8 Album, +0xB4 the whole) from the list's: as tall as the browser's height (the browser object's +0x452; at first half the list's room less 10, `FUN_000683fa`'s default 0.5; at least 80; the songs keep 120 under it, `FUN_0013cbbe`); with the genre shown (the preference's +0x25B) the thirds left, middle, right, else Artist and Album halves. The songs start 10 points lower: the splitter, pictures 313 (stretched) and 312 (its grip) loaded with the window at 0x10744 / 0x10e71 | `FUN_000249e4` at 0x25454–0x25640, `FUN_0013cbbe`, `FUN_000683fa` | Done (the splitter's drag by eye) |
| A `ColumnDisplay` (`FUN_0040e0ac`) is the songs' list engine (`FUN_002fa50e`) with one column, its header in Txtr 9004 (0x232C); its first row "All (N Genres / Artists / Albums)" (STR# 166.001–166.006); a choice filters the columns right of it and the songs | `FUN_0040e0ac` | Done (no Compilations row, no Composer column) |
| The user rating: PBUT 528 (unselected), 529 (selected, editable), 531 (selected without the keyboard), 68 × 121, eleven rows of 11 (a half star each, 0 to 100 by 10); CWid 9000's field 25, 79 wide (minimum 79). A click on the selected row's stars, File ▸ Rating (`SRT0`–`SRT5`, xMNU 200): the library's `Rating` (20 a star) | `templated.txt`, `CWid 9000` | Done (the place of the stars in the cell by eye: 5 in) |
| Get Info (`geti`): DLOG 135 (580 × 512), DITL 135: OK, Cancel, Previous, Next, the tabs CNTL 1358 at (20, 15) 540 × 441 with tab# 1358's labels (Summary, Info, Video, Sorting, Options, Lyrics, Artwork). The Info pane DITL 271: Name, Artist / Year, Album Artist / Track Number of, Album / Disc Number of, Grouping / BPM, Composer, Comments, Genre with its popup (CNTL 2710), Part of a compilation. Sorting DITL 276, Options DITL 283 (Volume Adjustment, Equalizer Preset, Remember position, Skip when shuffling, Gapless album) | `ditl.py` (DITL, CNTL, tab# decoded) | Partial: Info written to the file's tags (TagLib) and the library; Lyrics the tags'; Artwork shown; Summary laid out by eye (its pane is drawn by code not read); Video (DITL 275), Sorting (DITL 276: the Sort fields as the tags' TITLESORT, ARTISTSORT, ALBUMARTISTSORT, ALBUMSORT, COMPOSERSORT, SHOWSORT) and Options (DITL 272, the single song's; 283 is several songs') built from their DITLs, what the library doesn't keep disabled; Comments one line (the DITL's field is 64 high) |
| The Equalizer (`eqwn`, ⌥⌘2): EQWindow.nib, a gradient utility window (16-point title bar, TXTS 731, buttons 13014–13016 18 apart); On (EQcb 5000, `EQon`, TXTS 500) at (36, 10); the presets' popup (EQpu 5001, PBUT 786, TXTS 510) at (123, 6), its menu xMNU 513 then the presets (`FUN_00155694`); the user pane at (11, 32): the preamp (EQsl 110) at 15, the ten bands (EQsl 100–109) from 101, 31 apart, 25 × 127 (track SlTr 3210, 10 kept at each end; thumb SlTh 3212, three cells of kind 3); their names (TXTS 501) and the scale (TXTS 502) | `EQWindow.nib`, `FUN_00156142` | Done (the window's zoom, `FUN_00155bde`, not done; Make Preset and Edit List: section 18) |
| The sliders: `UISlider` (`com.apple.itunes.uislider`, `FUN_0001ff14`; SlTr / SlTh / SlTo read by `FUN_00053822`), vertical when taller than wide: the thumb at (len − thumb) − (v − min)(len − thumb) / (max − min) (`FUN_0002c454`), a drag changing the value by the move × (max − min) / (len − thumb) (`FUN_004691d6`); the value in hundredths of a dB, ±1200 | `FUN_0002c454`, `FUN_004691d6` | Done |
| The commands (`FUN_0015680c`): `EQsc` a slider moved puts the popup on Manual (`FUN_001557e4` with 4, `FUN_000bd84a`); `EQlc` a click on a band's name sets it to 0, on "0 dB" (Labl 12) all eleven; `EQon`; `zoom` | `FUN_0015680c` | Done (zoom not done) |
| The built-in presets: the table at 0xa2b520 (`FUN_000a6d48`), 22 entries of 0x30 bytes: +0 the name (STR# 600, its index 1–22: Acoustic … Vocal Booster), +4 the preamp (0 in all), +6 the ten bands, +0x1A ten more shorts (all 0), in hundredths of a dB (Bass Booster 550, 425, 350, 250, 125, 0…) | `FUN_000a6d48` | Done |
| The sound: libmpv's audio filters, FFmpeg's `volume` for the preamp and a peaking `equalizer` an octave wide at each of the ten frequencies | | Done (iTunes' own filter not read) |

## 10. iTunes 8.2.1: the grid

View ▸ as Grid (`thmv`, ⌥⌘4) and the middle view button: `FUN_0014c7ce` with 0x12 sets the window's
display mode (+0x593C) to 4. The grid is `TrackGroupDisplay` (`com.apple.itunes.trackgroupdisplay`,
vtable 0xbc5168; its accessibility handler a `GroupBrowserAccessibilityEventHandler`, as the Store's
`StoreBrowseDisplay`) under a `BrowseBarView` (`com.apple.itunes.browsebarview`, vtable 0xbef5c8).

| Item | Source | Status |
|---|---|---|
| The browse bar: Placards.nib's "BrowseBar", a user pane 29 high (`BBBt` 1000); picture 564 (20 × 29) stretched across; the grouping's bevel buttons (`BBBt` 3, 4, 8, the command `BBBP`, TXTS 720: Txtr 720, Lucida Grande 11, white, centred) 83 × 21, 4 down, the first on 552, the middle on 554, the last on 553 (`biid`; two columns, kind 2; two rows, the second the chosen grouping); their titles STR# 250.001–250.004 (Albums, Artists, Genres, Composers). The size group (`BBBt` 18, bound to the right, 153 × 30): 560 (9 × 10, `BBZS`), the slider (13, 7, 123 × 16; SlTr 556 with 4 kept at its ends, SlTh 558, SlTo 2; value at first 50), 562 (13 × 14, `BBZL`) | `Placards.nib`, `FUN_00584afa` (`BBZS` → 0, `BBZL` → 1, `BBBP` → the 'Brws' 'GrpC' event) | Done (the buttons centred and the size group 20 from the right: by eye, the pane's own layout code not read) |
| The size: the slider's value / 100 (`FUN_0005b120`); the window's handler of 'Brws' 'PSCh' (`FUN_00150974`) gives the grid the scale 0.5 + 1.5 × that (its vtable slot 0x140) and keeps it per grouping (`FUN_000ee868`, ten 0x44-byte records from +0x188) | `FUN_00150974`, `FUN_000ee868` | Done (one size for all the groupings here) |
| The covers' frames: PBUT 578 'browse item no select (small)' (126 × 126, 12 kept at each side: the shadow), 570 'browse item select (small)' (22 kept: the blue ring); 572 the ring without the keyboard; 574 / 576 / 580 the large ones (276, 40 or 19 kept); 582–595 the artists' stacks; 596 the light theme's | `templated.txt` | Partial: 578 and 570 only |
| The covers: the song's own picture (the group's first that has one) filled into the square | | Done (no artwork downloaded, no stacks for the artists) |
| Not read: the covers' base size (100 points at scale 1 here), their spacing, the titles' style and colours, the background (the bar's dark grey continued), what a click on a cover does beyond the selection (a double click plays its songs here) | `TrackGroupDisplay`'s cells are drawn by the list engine: `FUN_002fb2da` draws each visible cell through the callback at its +0x90, the cell's rect from `FUN_002f8cb8` (the engine's default cell size at +0x10 / +0x12, or the row's or column's own); the engine made by `FUN_002fa50e` (its 4th argument the default cell size: height low, width high), TrackGroupDisplay's at 0x1b6940–0x1b6bf5 from its layout object (+0x55D8, `FUN_001be712`: the insets at +0xA, the cell at +0x18 / +0x1C, as many columns as fit the width less the insets, `FUN_001bc2d2`, `FUN_001bc2b6`); that object's cells (87 × 260 or 128 × 250, a cover 100 × 75 and the text beside it) are a row's, not the grid's covers as the port draws them: which view the grid is in 8.2.1 left open | By eye |

## 11. iTunes 8.2.1: smart playlists

File ▸ New Smart Playlist… (`nspl`, ⌥⌘N) and Edit Smart Playlist (`esmp`).

| Item | Source | Status |
|---|---|---|
| The window: SmartPlaylist.nib (656 × 229 with one rule): Match (4001) with all / any (4056, MENU 303) and "of the following rules:"; the rules' group box (4054) at (22, 47), 610 wide; the nib's `singleRuleRow` (610 × 36): the field (4014, 10, 10, 162 × 20), the operator (4016, 180, 10, 157 × 20), the value's pane (4058, 345, 4, 192 × 32), − and + (PBUT 138: 32 × 32 cells, disabled / normal / pressed, − above +) at 541 and 571; the criterion views (text; text range with "to"; `betweenCriterionView` a number and MENU 309 days / weeks / months; size "MB", bit rate "kbps", sample rate "Hz"; `userRatingCriterionView` the stars, PBUT 531); the pane 4053 under the rules: Limit to, its number and unit (MENU 304), "selected by" and the order (MENU 305), Match only checked items, Live updating, Cancel, OK | `SmartPlaylist.nib`, `Localized.rsrc`'s MENU 301–309 | Done (the field popup offers the fields the library keeps: 20 of the nib's 38; Match only checked items disabled, the songs have no check boxes yet; the rows 36 apart by the nib's row pane) |
| The operators: MENU 301 text (contains, does not contain, is, is not, starts with, ends with), 302 numeric (is, is not, is greater than, is less than, is in the range), 308 date (… is after, is before, is in the last, is not in the last, is in the range), 306 boolean, 307 enumerated | MENU 301–308 | Done (306 and 307: their fields not offered) |
| The codes: the search field builds rules the same way (its code at 0x1060e7 calls `FUN_0037c820(list, operator, field, value)`): 0x01000002 "contains" on the fields 2 Name, 3 Album, 4 Artist, 0x47 Album Artist, 0x12 Composer, 0x4E–0x52 the sort fields (section 28) | `FUN_0037c820`'s callers at 0x1060e7… | Read; the other operators' codes and fields' numbers are the format's as known (not read here) |
| The rules in memory: an `SLst` object (`FUN_0037b110`: 0x50 bytes, 'SLst', a reference count, flags at +0xC, the rule count at +0x10, the rules at +0x4C, 0x14 bytes each: field, operator, a sub-list flag, the value). Serialized (`FUN_0037c336`) as a 0x88-byte header ('SLst', 1, 1, the count at +8, two flag bytes at +0xE / +0xF) then per rule a 0x38-byte header (field, operator, sub-list byte, +0x34 the data's length) and its data (a string, a nested list, or a 0x44-byte numeric record whose first 12 words are swapped as 64-bit values, `FUN_0037c268`); the XML writes those bytes as "Smart Criteria" next to "Smart Info" (0xa9f13) | `FUN_0037b110`, `FUN_0037c336`, `FUN_0037c268` | Partial: the port keeps the rules under its own readable "Smart Rules" key, not iTunes' blobs (the numeric record's and Smart Info's fields not read) |
| The rules applied: all or any, then the limit by minutes, hours, MB, GB or items in the chosen order; live updating applies them each time the playlist is shown, else they are applied on OK | | Done (no play dates kept: "recently played" orders by the play count) |
| The source list: a smart playlist with 441's row 16 (the gear) | `441` | Done |

## 12. iTunes 8.2.1: Cover Flow

View ▸ as Cover Flow (`covv`, ⌥⌘5) and the third view button. Cover Flow is `AlbumCoverDisplay`
(`com.apple.itunes.albumcoverdisplay`, vtable 0xbd0fa8) drawing through its own OpenGL canvas (AGL);
the `RendererOGL` at 0xac3dec (`FUN_0054d32c`, `FUN_0054d530`) is the visualizer's, not Cover Flow's.

| Item | Source | Status |
|---|---|---|
| The canvas: `FUN_003f0f08` sets a perspective of 50° vertically, the view's width / height, near 1, far 20; `FUN_003f1262` places a picture by the view's points (x and y mapped to −1…1), a depth (z = −(d × 19 / scale + 1), the canvas's scale at +0x24), turned about its own vertical middle (`glRotatef` x, y, z); the camera (`gluLookAt`) from two points of the view (+0x44…+0x58), the horizon at +0x150 / 100 and +0x154 / 100 of the height | `FUN_003f0f08`, `FUN_003f1262` | Read |
| The layout (`FUN_00403798`, set on resize): the cover a square 2/3 of the view's height (19/30 in a smaller mode, flag 1 of +0x50); 8 covers each side (+0x8); the middle cover at the view's middle (+0x38 = width / 2); a side cover's rect starting a quarter of a cover from the middle one's middle (+0x1C = cover / 4), the next ones 1/7 of a cover apart (+0x18 = 6/7 of a cover: the overlap), past the eighth a quarter of that (+0x20); the sides turned 60° (+0x24 outer, +0x28 inner) and at depth 60 (+0x2C), the middle cover at 30 (+0x30); an opacity 0.8 (+0x58; 0.9 / 0.2 or 0.95 / 0.7 in other modes) | `FUN_00403798`, its zones in `FUN_00403c40` (0 far left, 1 left, 2 middle, 3 right, 4 far right), `FUN_00405e7c` (angles), `FUN_00405e26` (depths), `FUN_00406188` (a cover's place) | Done |
| The drawing (`FUN_004065aa`): the covers sorted (`FUN_0040641e`, by distance), each placed by `FUN_003f1262` with its x, angle, depth (moved back 900 × (1 − fade) while fading) and opacity; its reflection under it, mirrored, its opacity from 0.4 at the cover's foot to 0.05 (0.5 or 0.4 to 0.001 in the other mode, +0x9D) | `FUN_004065aa` | Done |
| The port: cairo, no GL: each cover drawn column by column, each column's height by its depth's perspective for a 50° field of view, the vanishing point at the view's middle; the covers glide to their places a sixth of the way each 1/60 s; ← and →, the scroll wheel, a click on a cover; a double click or Return plays the album; the list under it (55% of the room for Cover Flow, by eye) follows and leads the chosen album | | Partial: the depth units by eye (the difference 30 taken as half a cover), the cover's size scaled to leave the titles room, the titles' style and the black background by eye; the HUD (section 12's table, below) done |
| The HUD (`FUN_00408c7e`, AlbumCoverDisplay's controls): the scroller (`sbar`, PSCR 2022, or 2023 with +0x87 off: picture 2022 or 2024 through mask 2023; the track 52 × 16, 26 and 25 kept at its ends; the thumb 26 × 16 at 52 in the art, 8 kept at its ends, moving 20 in from each end) 16 high, its foot 23 from the view's (57 in full screen); its two insets the larger of 110 and what the buttons beside it take; in the window the full screen button (`full`, PBUT 2020 'HUD fullscreen out', 34 × 24, its second row pressed) at the right, 23 in; in full screen (+0x50 flag 1) Previous (PBUT 2030 'HUD FF Rewind', 31 × 31, column 2), Play / Pause (2028, 37 × 37), Next (2030, column 3) from 57 at the left, 5 apart, and the volume (`volu`); a 4-point splitter (PBUT 2024) at the foot | `FUN_00408c7e`, `FUN_0042c47e`, the PSCR and PBUT templates | Done (the scroller, the full screen button and View ▸ Full Screen; the thumb one album's share, by eye; in full screen the insets 201 and no transport or volume drawn) |
| PICT art: PSCR's pictures are DirectBitsRect with four planes (alpha first) and their masks PackBitsRect (opcode 0x0098: an indexed PixMap and its colour table) | iTunes.rsrc | Done (`resources.cpp` decodes both) |

## 13. iTunes 8.2.1: View Options

View ▸ View Options… (`VOpt`, ⌘J).

| Item | Source | Status |
|---|---|---|
| The dialog: DLOG 144 (420 × 436), DITL 144: the source's icon (a user item, 20, 9, 20 × 20) and its name (44, 12, 221 × 13), "Show Columns" (20, 39), 48 check boxes 120 × 20 in three columns of 16 from (30, 60), 120 apart and 20 down, their titles set by the code, Cancel (248, 396) and OK (330, 396) | `ditl.py 144` | Done (the boxes the library's fields fill: 19, by title; Name always shown) |
| The columns: a column is a field, the numbers CWid 9000 lists with each one's minimum and default width and its justification, the same numbers as the smart playlists' rules (2 Name 200, 3 Album 125, 4 Artist 125, 5 Bit Rate 80 right, 6 Sample Rate 91 right, 7 Year 50, 8 Genre 80, 9 Kind 100, 11 Track # 70 right, 12 Size 60 right, 13 Time 59 right, 14 Comment 250, 16 Date Added 125, 18 Composer 125, 22 Play Count 82 right, 24 Disc # 60 right, 25 My Rating 79, 35 BPM 50 right, 39 Grouping 125, 71 Album Artist 125); their titles STR# 9006 | `CWid 9000`, STR# 9006 | Done |
| The list: the status column, then the columns chosen in their order (new ones at the end), kept in the preferences; sorted by the column's field (numbers by value; Artist by artist, album, disc and track; Album by album, disc and track; Genre by genre, artist, album) | | Done (by eye: the sorts' secondary keys; no horizontal scrolling yet when the columns are wider than the list; columns not dragged or resized) |

## 14. iTunes 8.2.1: the Preferences

iTunes ▸ Preferences… (`pref`, ⌘,).

| Item | Source | Status |
|---|---|---|
| The window: DLOG / DITL 134 (544 wide): the toolbar (a user item, 0, 0, 544 × 56), a line (0, 76), the help button (CNTL 1340), Cancel and OK 39 from the bottom; as tall as the chosen pane's DLOG | `ditl.py 134` | Done |
| The toolbar: STR# 178 (General, Playback, Sharing, Store, Advanced, Parental, Apple TV, Devices; Podcasts STR# 128.005), the icons iTunes.rsrc's `icns` (32 × 32 `il32` / `l8mk`): 300 General, 309 Podcasts, 301 Playback, 302 Sharing, 306 Store, 303 Apple TV, 307 Devices, 304 Advanced; Parental the lock PBUT 142 (33-point cells, kind 1); the chosen item on PBUT 660 'Toolbar Selection' (55 × 56, a PICT and its mask) | `Localized.rsrc`, `iTunes.rsrc` | Done (the order by eye; 660 not decoded: a darker rounded square) |
| The panes: General 256, Podcasts 254, Playback 257, Sharing 261, Store 262, Parental 313, Apple TV 255, Devices 263, Advanced 260 (its Importing 258 and Burning 259 are tab# 1379's tabs, not shown); each built from its DITL: static text, edit text, buttons, check boxes, radio buttons, and the CNTLs (`CNTL`: rect, value, visible, max, min, procID, refCon, title): 144 / 145 separators, 401–415 popups on the MENU their minimum names (1341 Source Text, 1350 List Text: Small / Large; 1353 When you insert a CD…), 48–51 sliders | `dialogItems()`, `classicMenu()` in `resources.cpp` | Done (no fonts per item: dftb not read; the user items drawn by code are left out) |
| What the port acts on: General's Library Name and List Text (Txtr 9002 List Contents (Small) or 9003 (Large) for the songs), Playback's Sound Check (libmpv's ReplayGain), Advanced's Keep MiniPlayer on top (the mini player floats); the rest shown disabled | | Done (crossfading, the sound enhancer, sharing, the Store, parental control, devices, the music folder's organizing and copying: not done) |

## 15. iTunes 8.2.1: the iTunes Store

The iTunes Store source (under STORE) shows the Store in the songs' place, under the Store bar.

| Item | Source | Status |
|---|---|---|
| The Store bar: MusicStoreBar.nib, a pane 448 × 28 on PBUT 612 (1 × 27, stretched); Back (34001, PBUT 600: three 27 × 19 cells, kind 3; `Msgb`) at (7, 4), Forward (34002, 602, 26 wide; `Msgf`) at 34, Home (34003, 626, 32 wide; `Msgh`) at 67, bound to the left; the breadcrumb (34004, 644: 3 points kept at each end, TXTS 600) and the account button (34005, 606: 11 kept at each end, TXTS 600, `Msvc`) 146 wide 7 from the right, bound to the right | `MusicStoreBar.nib` (its HILayoutInfo) | Done (the breadcrumb not shown: the pages give none; Sign In does nothing) |
| Store ▸ Back, Forward, Home (`Msgb` ⌘[, `Msgf` ⌘], `Msgh` ⇧⌘H) | `xMNU` 141 | Done |
| The pages: 8.2.1's own Store was WebKit on Apple's 2009 store, gone; the port shows the same pages as 11.4's (iTunesStore, section 8) | | Partial: the place of the bar over the pages by the iTunes 8 window as known, `FUN_000249e4`'s Store bar rect not read |

## 16. iTunes 8.2.1: the visualizer (Magnetosphere)

View ▸ Show Visualizer (`vtog`, ⌘T). iTunes 8's visualizer is Magnetosphere (`MagnetosphereApplet`),
its scene at 0x53f000–0x54e000 drawn through `RendererOGL` (the global at 0xac3dec, `FUN_0054d32c` its
perspective, `FUN_0054d530` its turns); a frame is `FUN_0053fe00`.

| Item | Source | Status |
|---|---|---|
| The sound: the spectrum's 256 bins, the two channels averaged (`FUN_0054dada`), the least taken off (`FUN_0054e014`); the level the bins' mean, each times the gain and at most 256; a beat above three times the threshold (the threshold then the level, the kick 1, × 0.975 a frame), else the threshold easing to 10 by 0.15 of the difference; the gain ±0.005 a frame toward a mean level of 20, from 0.25 to 4 (`FUN_0054de3a`) | `FUN_0054dada`, `FUN_0054de3a` | Done (the spectrum: libmpv's RMS level, spectral centroid and flux, FFmpeg's `astats` and `aspectralstats` read from `af-metadata`, stand in for the 512 bins iTunes' player gave: 128 bins about the centroid, their mean the level) |
| The bands (`FUN_0054e308`): each its bins' sum and its history's mean, a beat past its threshold and 16 − 2 × its number, the threshold × 0.99 after 25 quiet frames; their beats (`FUN_0054dc56`): bands 0 and 1 one time in five swing the camera, band 2 one in five swings it and one in two new colours, band 3 flips a flag of the particles' motion, band 4 trails long (opacity 1) or short (0.2) | `FUN_0054e308`, `FUN_0054dc56` | Done (the five bands each a fifth of the 128 bins; band 3's flag taken as the swirl's: by eye) |
| The camera (`FUN_0054c430`): 60° (× 1.2 in one mode), near 10, far 5000, at (sin(phase) × 0.7 + 0.85) × its base, the phase moving 0.006 a frame on beats, two turns; the base 300 to 500 (400 to 500 in the fountain mode) and 5 a magnet (`FUN_0054be34`, `FUN_0054be68`, `FUN_0054c58e`) | `FUN_0054c430` | Done |
| The background (`FUN_00540758`): red, green, blue easing to their targets by 0.045, 0.035, 0.025 of the difference a frame | `FUN_00540758` | Done (the targets' colours by eye) |
| The scene (`FUN_000087d8`): the magnets' container (scene 100 across, 3 magnets), the particles' (525), the renderer, the spectrum, the palette; a new scene (`FUN_00543d80`): the next preset, new palette colours (`FUN_0054082a`), one time in three the noise field with the particles' push 0.2 (else 1), one in three the swirl, one in seven a flag; the fountain preset (+0xF) gives the particles lives of 20 to 40, the damping 0.89, the field × 1.5, the magnets × 1.25 | `FUN_000087d8`, `FUN_00543d80`, `FUN_00544e52` | Done (the presets' table, filled at run time, not read: the preset flags +1 trails, +5 links on, the others off; the new scene at band 2's new colours, by eye) |
| The magnets (0xDC-byte records, `FUN_00541250`): their number 1 to 7 (`FUN_00542702`), one a frame added 1 from a random other (the first in the middle) or the last made to shrink 30 to 50 frames and go (`FUN_0054252c`, `FUN_0054111a`); a final radius 25 to 100, the radius 0.1 of it at first, easing by a tenth a frame; a pulse, the level of band (index % 3 + 1) × 3 + 0.1, falling back by 0.35; the strength (pulse × 0.25 + 10) × radius / final radius (`FUN_00541834`); apart from each other by the other's strength / d² × 10 × 0.25 along their difference (`FUN_005422fe`); pulled by −0.01 × their place (`FUN_005411ee`); the speed × 0.95 a frame, at most 2 (`FUN_00541710`) | `FUN_00542448` and its callees | Done (the magnets not drawn; `FUN_00542210` draws something with the preset's +6, not read) |
| The particles (0x5D4-byte records, `FUN_00545a02`): 25 to 600, one a frame added or taken, born in a cube ±5 about the middle, still; each on a bin (its index % 128), its trail 20 to 35 long (growing one a frame, `FUN_00544fc2`), widths sin((1 − i / (n − 1))⁴ × π) × 5, its top speed 6 to 8, its colour easing at 0.01 to 0.05 a frame to the palette's (`FUN_00544b2a`); its charge −(bin × 0.0666 + 0.5), its bin's peak falling by 0.15, its size (bin / 15 + bin % 15 / 15) / 2 (`FUN_00544bea`); its energy falling by a tenth a frame | `FUN_00547598`, `FUN_00545a02`, `FUN_00544bea`, `FUN_00544fc2` | Done (when the energy is raised not read: here its bin + 3 when the bin passes its peak) |
| The forces (`FUN_005459a0`): each magnet's (`FUN_005415f8`) along the particle's difference from it × charge / d² × strength / (strength / 10), turned about inside the magnet's radius, all added and made 2 long, then doubled; the nearest magnet the one whose half radius is nearest; with the field (`FUN_00544ee8`) two angles from Perlin noise of x/100, y/100 and x/100, z/100 (× 2π) give (cos a × 0.5, −sin b × 0.5, sin a × 0.5); with the swirl (`FUN_00544d30`) −sin²((100 + y) / 200 × 15.3) about the nearest magnet's vertical | `FUN_005459a0`, `FUN_005415f8`, `FUN_00544ee8`, `FUN_00544d30` | Done (the noise a smooth value noise, not the table at 0xC00680) |
| Apart two by two (`FUN_0054784a`): each the other's charge / d² × −1 × the push × 0.5 (× 3 in the orbit preset, twice on one magnet) along their difference; the links: a particle's nearest other 20 to 50 away, either's energy past 12 and a random 0 to 50 under the other's less 12, at most 200, no two alike (`FUN_0054779a`) | `FUN_0054784a`, `FUN_0054779a` | Done |
| The motion (`FUN_005453b4`): the speed × 0.975 a frame (`FUN_00544e52`, `DAT_00b7b36c`), + the swirl and the field in time, + the magnets' at once, + the push in time, at most the top speed in time; in the orbit preset (+0xC) a particle within 1.25 radii of its magnet holds on its sphere | `FUN_005453b4` | Done (the orbit preset not ported) |
| The links (100-byte records, `FUN_005408bc`, `FUN_0054096a`, `FUN_00546f62`): grown 3 to 15 to 25 by a tenth of the rest a frame, done near the end; their ends bowed out of their particles' magnets by grown / length × 20 to 70; drawn as GL_LINES: a to b, b's bowed end to a, b's bowed end to a's, each end in its particle's colour, (1 − grown)² × 0.95 opaque | `FUN_0054096a`, `FUN_00546f62` | Done (drawn by cairo, the particles glowing points added up and their trails one stroke each, not GL) |
| View ▸ Full Screen (`full`, ⌘F, in Localized.rsrc's menus) with the visualizer on: Magnetosphere over the whole screen, a click or a key ends it (the port: a full-screen window). View ▸ Visualizer (xMNU 204 `VISS`: a divider and Options… `VisO` in the resource; iTunes puts the visualizers above it: iTunes Visualizer 5800.001, iTunes Classic Visualizer 4000.001). Magnetosphere's keys (5801.001–016): ? help, M change mode, P change palette, I track info, C auto-cycle (on at first), F freeze, N nebula, L camera lock. Done: the submenu with the iTunes Visualizer checked; the keys (the help as 5801's pairs, the song's name at the bottom left a few seconds when it begins or on I, M a new scene, P new colours, C and F stopping the scenes' changes, N the noise field, L the camera still: what they do by their names). Not done: the Classic Visualizer (G-Force's configs: 4002 delta fields, 4003 wave shapes, 4004 colour maps by name only, their definitions compiled in) and Options… | | |

## 17. iTunes 11.4: the MiniPlayer, Up Next, the library's grids

| Item | Source | Status |
|---|---|---|
| The MiniPlayer (Window ▸ Switch to MiniPlayer ⌥⌘M `smin`, MiniPlayer ⌥⌘3 `minp`): a window of its own (MiniPlayerWindowController); MiniPlayerFrameView (`FUN_00a0ae3a`, vtable 0x1778578; built by `FUN_00a0af9e`, drawn by `FUN_00a0c95c`): picture 8100 (300 × 48), 8101 when the window isn't key, stretched; its content 5 from the left, 4 from the top | `FUN_00a0af9e`, `FUN_00a0c95c` | Done |
| MiniPlayerController (vtable 0x17781b8; `FUN_009f87d6`, 28 KB, builds its views with ITView's layout constraints: `FUN_00976440` left, `FUN_0097672c` top, `FUN_009765b6` right, `FUN_00976a18` bottom, `FUN_00976b8e` left and right, `FUN_00978c36` one under another, `FUN_00979bc4` width = m × height, `FUN_00979d36` the height): the artwork (8117 asked for 38 × 38; 8125 when there is none, 8116 its frame) pinned left, top and bottom, square; the window 48 high; the area right of it at least 220 wide (priority 1000), 240 by preference (740); the buttons 9, 8 and 10 from its right. `FUN_00a062da` sets the pictures by theme (light 81xx, dark 81Ex): Play 8112 (rows Play, Pause, Stop), Previous 8113, Next 8111, close 8110, volume 8123 (four rows), AirPlay 8124, Up Next 8114, search 8118, expand 8160 | `FUN_009f87d6`, `FUN_00a062da`, the constraint helpers at 0x976000–0x97a000 | Partial: the buttons' places inside the area (the transport centred, the volume at its left, Up Next at its right, close and expand at its top corners), the lines' styles and the controls shown under the pointer by eye; the volume a click through its four levels (11.4 opens a slider) |
| Up Next (View ▸ Show Up Next ⌥⌘U `PQsh`, Controls ▸ Clear `PQcl`; PlayQueuePopover 0xbb6f78, PlayQueueHeaderView, PlayQueueCell, PlayQueueMsgCellView, at 0xba9000–0xbbc000): the header 8216 (512 × 31), its title 3801.002 "Up Next" in TextStyles' 1411, History 8314 (18 × 18, the clock plain and on), Clear 3801.035 on 8259 (69 × 22, 5-point ends) in 1409; the songs 50 high, the artwork 40, the name in 1401, the artist in 1402, the buttons 8452 (play), 8454 (add), 8459 (remove) under the pointer; nothing to come: 3801.010 in 1410; the popover 400 wide, at most 500 high | the code at 0xba9000–0xbbc000, TextStyles.plist | Partial: a window in place of the popover; the places inside the header and rows by eye; no History; the songs the queue's after the one playing |
| The library's grids (the nav bar's Albums, Artists, Genres: AlbumsGridViewController, ItemGridWithDetailsViewController): the item size by the size setting (`FUN_0004a6ee`): 1 → 128, 25 → 138, 50 → 148, 75 → 158, else 168 wide, 14 more high; laid out by ITGridViewFillTilingAugmenter (`FUN_0004a7b4`, `FUN_007e49ce`): as many columns as fit, each cell then wider by an equal whole share of what is left, and taller by that × height / width; the cells AlbumCoverCell (LabeledEmphasizableItemCell), their lines in 1001 (HelveticaNeue-Medium 13) and 1002 (HelveticaNeue 12) | `FUN_0004a6ee`, `FUN_007e49ce`, `FUN_0004a5a4` | Partial: the cover's place in its cell (10 in, the lines under it) and the selection's ring by eye; no details drawer under a chosen album |

## 18. iTunes 8.2.1: Show Duplicates, the Equalizer's presets

REA 6.10.1 (Ghidra 12.2) again on `iTunes.i386` (sha256 `6361e4ff…82df28`), all 30 310 functions
decompiled (`decompile-all`, a Ghidra checkpoint kept so that later reads restore the analysis).

| Item | Source | Status |
|---|---|---|
| File ▸ Show Duplicates (`SDup`; the Option alternate Show Exact Duplicates): the window's command (`FUN_00150918` finds the window, then `FUN_0014eae4`) toggles the playlist's flag (+0x182 bit 8); the menu reads "Show All" while it is set (`FUN_00140f50`, `kMenuItemShowAll`) | `FUN_00140f50`, `FUN_0003c024` | Done |
| The duplicates (`FUN_00138cea`): the list sorted by Name while they are shown (`FUN_0045386e` with field 2, the sort before kept at +0x10c and put back by Show All); each song compared with those after it: a different media kind (audio or video, `FUN_001827a0`'s flags 0xE62) or, exact, a different track number, track count, disc number or disc count (+0xAA, +0xAC, +0xB0) passes over it; a different Name (`FUN_00174928`) or Artist (`FUN_00174f52`) ends the run; exact, the Album (`FUN_001749fc`) too. A pair found marks both (0x200000 in the item's flags), and the marked ones are what the list shows. Exact is the Option key (`FUN_003ae488`: the modifiers' bit 11). The getters' fields: the player's notification (`FUN_000bee7e`: Name, Artist, Album, Track Number at +0xAA…) | `FUN_00138cea`, `FUN_000bee7e` | Done (the compare's flags, `FUN_0016f88c`: 0x19 or 0x1D with 0x20, taken as the case ignored) |
| The Equalizer's popup commands (`FUN_00155310`): `mpre` Make Preset…, `edeq` Edit List…, `sleq` a preset | `FUN_00155310` | Done |
| The presets: a list at +0x1a9c4 after a head node; a new one inserted after the last whose name sorts before it, the case ignored (`FUN_000a5d60`) | `FUN_000a5d60`, `FUN_000a63f4` | Done (kept in the port's settings, `EQPresets`) |
| Make Preset / Rename (`FUN_000a79e0`): Get String (DLOG 10002) titled STR# 177.009 "Make Preset" or 177.011 "Rename", the prompt 177.010, the field the current preset's name (+0x1ecc); a name taken asks 177.006 (Yes / No, STR# 23987.001 / .002) and that preset is deleted without asking; the new preset takes the sliders' values and is chosen (`FUN_000a770a`) | `FUN_000a79e0`, `FUN_000a770a` | Done ("Do not warn me again", 23987.020 kept at +0x9f0, not shown: the port's alerts have no box) |
| Edit List (`FUN_000a7d12`): DLOG 148 "Edit Presets" (350 × 284), the List Manager's list in user item 2 (20, 10, 200 × 254; inset 1, its scroll bar 15; one row at a time, flags 0x8C), Done 1, Rename… 3, Delete 4, the last two enabled with a row chosen (`FUN_000a639c`) | `FUN_000a7d12`, `FUN_000a639c` | Done |
| Delete (`FUN_000a78b0`): asks 177.004 (Delete / Cancel, 23987.015 / .004; its "Do not warn me again" at +0x9ed); the chosen preset deleted goes Manual with its values (`FUN_000a783c`); then 177.008 about the songs set to it (`FUN_000a5fb8` clears their preset) | `FUN_000a78b0`, `FUN_000a783c`, `FUN_000a5fb8` | Done (177.005 and 177.008 not asked: the port's songs have no preset of their own) |

## 19. iTunes 8.2.1: Open Audio Stream

Advanced ▸ Open Audio Stream… (`ourl`, ⌘U; enabled unless +0x109bf). The material is now kept in
`.local/media/re/itunes/` (`iTunes.i386`, `all.s`, `dec/`, and the Ghidra checkpoint REA restores).

| Item | Source | Status |
|---|---|---|
| The command (`FUN_000b24d6`): the URL asked (`FUN_000b20dc`), then opened (`FUN_000b21c4` with flags 3) | `FUN_000b24d6` | Done |
| The dialog (`FUN_000b20dc` → `FUN_000f795e` → `FUN_002f055e`): DLOG 10003 Get Long String (10002 without the flag 1), titled STR# 130.001 "Open Audio Stream", or 130.002 "Edit URL" when it starts with a text, its prompt (item 3) 130.003 "URL:", the field (item 4, 320 × 64), OK 1 the default, Cancel 2; the text trimmed (`FUN_0028fcfc`) | `FUN_002f055e` | Done |
| The URL's parts (`FUN_00261b94`, 0x3C bytes): the scheme (before ":/"), user and password (before "@"), the host, the port, the path after its "/", its last component before a "?" (the file name), its extension | `FUN_00261b94` | Done |
| By scheme (`FUN_000b21c4`): `itms:` / `itmss:` (`FUN_000fa2f4`) the Store's page (`FUN_0022cf82`), but an `.m3u` / `.pls` there made `http` and played; `pcast:` / `itpc:` made `http` and a podcast subscribed to (`FUN_003cd11c`); others a stream. A URL whose extension is a media, picture, playlist or page one (`FUN_00262270`: mp3, m4a, m4p, m4v, m4b, mp4, mov, m3u, m3u8, mpg, mpeg, 3gp, 3gpp, pdf, pls, jpg, jpeg, tiff, gif, wma, wmv, htm, html) goes to `FUN_003cd11c` too; feeds (xml, rss, atom; `pcast:`, `itpc:`, `feed:`) are told apart there | `FUN_000b21c4`, `FUN_00262270` | Done for streams and the Store (an https page in the Store source); podcasts not ported (the port says 131.001 "The URL could not be opened."); the URLs with a media extension are played as streams by libmpv |
| The stream's track (`FUN_000e29c6` → `FUN_0017fd64`): the one with that URL already in the library (`FUN_00179164`), else a new 'HTTP' track named by the URL's file name, else its path, else the whole URL (255 at most); added to the library and played (`FUN_000c30b8`) | `FUN_0017fd64` | Done (Kind "MPEG audio stream", 3101.003, until the format is read: not read; Track Type URL and the URL as Location in the library's XML) |

## 20. iTunes 8.2.1: the songs' selection

The list engine (`FUN_002fa50e`) is iTunes' own List Manager: its click (`FUN_002fc392`) is `LClick`'s,
the selection flags at +0x2C (`FUN_002f79be`) its `selFlags`.

| Item | Source | Status |
|---|---|---|
| The songs' list (TrackDisplay, `tdis`): flags 8, `lNoRect` (0x90, `lOnlyOne` and `lNoExtend`, when it is made with 0x100) | `FUN_001b65da` | Done (the browser's columns kept to one row) |
| The click (`FUN_002fbfd6`): ⌘ the modifiers' bit 8 (`FUN_0025376e`), ⇧ bit 9 (`FUN_003ae4be`). ⌘ toggles the row (its sense, `FUN_002fb9a8`). ⇧ with `lNoRect` adds (+0x8 and +0xB set: the rows outside the range kept): the range from the anchor to the row, the anchor the first chosen row when the row is past it, else the last (the bounds of the chosen cells), the last click (+0x48 / +0x4C) when the chosen rows are not one block, the row itself with none chosen; the anchor becomes the last click. A plain click on a row not chosen clears the rest (`FUN_002f8858`) and chooses it; on a row chosen, chooses it and keeps the others | `FUN_002fbfd6`, `FUN_002fb726` | Done (a narrowing to the row at the mouse's release, as later lists do, not found in iTunes 8's code: not done) |
| The drag (`FUN_002fc392`, `FUN_002fb726`): the cells between the anchor and the pointer take the click's sense, those outside cleared unless +0xB; the scrolling at the edges (0.2 s a step, faster over 0.8 s) | `FUN_002fc392` | Not ported (songs are not dragged in the port) |
| Edit ▸ Select All (`sall`: `FUN_002fb238`, every cell) and Select None (`SNon`: `FUN_002fba90`), each telling the window (`FUN_001a6f5c`: the 'tdsc' notification, the accessibility's AXSelectedRowsChanged) | `FUN_001b0ec2` | Done (Select All selects the search field's text when it has the keyboard) |
| The commands on the songs chosen: Delete, New Playlist from Selection (the songs in the list's order), File ▸ Rating | | Done (Get Info with several songs: section 21) |

## 21. iTunes 8.2.1: Get Info with several songs

| Item | Source | Status |
|---|---|---|
| File ▸ Get Info with more than one item (`FUN_000e9042`; one only of a device's checked songs counts as one): first STR# 9008.001 "Are you sure you want to edit information for multiple items?", Yes / Cancel (23987.001 / .004) and "Do not ask me again" (23987.019, `FUN_0029e1e4`), kept at +0x2AF | `FUN_000e9042` | Done (the box not shown: the port's alerts have none) |
| The common values (`FUN_000fac26`, its mask 0x79E037DD): each field's bit cleared at the first song whose value differs (the strings compared by `FUN_0028ee44`); `FUN_000fb47a` the second record (0x23A4 bytes, the options) | `FUN_000fac26` | Done (a field shows the value all the songs share, else nothing) |
| Multiple Item Information (`FUN_0015f516`): DLOG 136 (598 × 471), OK 1 the default, Cancel 2, the tabs CNTL 1390 (tab# 1390: Info, Video, Sorting, Options) at (20, 15), 558 × 400; the panes DITL 280 (Info), 281, 282, 283 | `FUN_0015f516`, `FUN_0015852a` | Done (Video, Sorting and Options shown from their DITLs, disabled) |
| DITL 280: a box before each field (Artist, Year, Album Artist, Track Number of, Album, Disc Number of, Grouping, BPM, Composer, Comments, Genre with its popup CNTL 1369, Artwork's well CNTL 1371, Rating's user item); a field typed in boxes itself | DITL 280 | Done (the box ticked on typing: the pane's handler not found, by the dialog's use; the artwork not written) |
| Applied (`FUN_000e9042` → `FUN_0018e2ac` with the boxes' mask, flags 0x201): each song gets the boxed fields; a track or disc number (or its count) boxed but empty keeps each song's own (+0xAA, +0xAC, +0xAE, +0xB0 put back from the song); a progress (130.018) over many | `FUN_000e9042`, `FUN_0018e2ac` | Done (the tags written with TagLib, the rating in the library; no progress window) |

## 22. iTunes 8.2.1: playlist folders

| Item | Source | Status |
|---|---|---|
| File ▸ New Playlist Folder (`nfld`, `FUN_0014ec46`): its parent the playlist chosen when it is a folder (+0x128 bit 2), else that playlist's parent (+0xC), in the same library; made (`FUN_002e60e2` → `FUN_002e5f90`), put in the parent (`FUN_002e670a`), chosen in the sources (`FUN_00166d0a`, with an animation from the row, `FUN_00166b90`) | `FUN_0014ec46` | Done (the animation not done) |
| The folder (`FUN_002e5f90`): named STR# 9000.003 "untitled folder", else 9000.004 "untitled folder ^1" with the first number free (`FUN_000dbb94`); its flag 2 at +0x128; its icon kind 0x14 (+0x4B6): 441's row 20 | `FUN_002e5f90` | Done |
| New Playlist and New Smart Playlist (`FUN_0013c60c`) take the same parent | `FUN_0013c60c` | Done |
| The sources' rows (`FUN_0002d3b2`, the small icons): an item of level n its triangle at 14n + 5 (when it has one: the row's flag 0x2000000), its icon at 14n + 19, its name at 14n + 39 (the large icons: 16n + 3, 16n + 17, 16n + 39); the triangle PBUT 314 "TrackDisclosure" (PNG 314, 10 × 10 cells, three columns, rows 0 closed, 1 turning, 2 open, 3–5 the same white for a chosen row) | `FUN_0002d3b2`, `templated` PBUT 314 | Done (the column by state by eye: 1 normal, 2 pressed; the turning row not animated) |
| A folder's songs: those of the playlists in it and in its folders, each once | | Done (by iTunes' use: the code that gathers them not read) |
| The library's XML: Playlist Persistent ID, Parent Persistent ID, Folder | | Done (the folders open kept in the port's settings, `OpenFolders`) |
| Moving playlists into folders: a drag in the sources | | Done: section 23 |

## 23. iTunes 8.2.1: dragging playlists into folders

| Item | Source | Status |
|---|---|---|
| The sources' drag (`FUN_00169d0a`, its state at +0x2D9 from `FUN_00166758`): over a folder, 4, when the playlist dragged is of the same library, not a kind kept out (+0xEC bit 0x20, `FUN_000dbeda`, kinds 0x13, 0x16, 0x1A), not the folder itself nor one of its parents (`FUN_000db944`), and not already in it; under the rows, 3, when the playlist dragged is in a folder; over a playlist, songs dropped on it (`FUN_000e8f26`) | `FUN_00166758` | Done (a playlist dropped on a playlist: below) |
| The drop (`FUN_000bc4ea` → `FUN_000dd8c8`, the position {parent, after}): refused across libraries and into itself; out of its old folder (`FUN_002e6620`) and into the new one (`FUN_002e670a`: the folder's rule "Playlist is" its Persistent ID, field 0x28, added by `FUN_0037ce5e`, and the folder's flag 4 up its parents) | `FUN_000dd8c8`, `FUN_002e670a` | Done (the new folder opened to show it) |
| The drop's mark: the folder ringed, or the whole list | | By eye (2 points in colour 10) |
| The order of the playlists in a folder | | Done: section 24 |

## 24. iTunes 8.2.1: the playlists' order in the sources

| Item | Source | Status |
|---|---|---|
| Each row's group (`FUN_00168254`, the row's short at +0x20): 0x81 Purchased (kind 0xC), 0x82 Party Shuffle (0x16), 0x84 a folder (+0x128 bits 1 and 2), 0x85 or 0x86 a smart playlist (+0x12F set or not), 0x87 Genius (0x19), 0x88 a playlist; the Library's kinds 2 to 5 their own (4, 6, 3…) | `FUN_00168254` | Done for folders, smart playlists and playlists (the others not in the port; +0x12F not read: 0x86 taken) |
| The sort (`FUN_0028a1d2` with `FUN_00166e76`): two rows compared at the first level their parents differ (+0x654 / +0x658, the paths from the top), there by `FUN_00166d96`: the group, then the name (`FUN_0028efca` with 0x1B), then the playlist's number (+0x24) | `FUN_00166e76`, `FUN_00166d96` | Done (the library's order for the last) |
| The names' compare (`FUN_0028ec76`): flag 2 leading spaces and controls skipped, 1 the case ignored; letters past 0x7A, or digits with 0x10, go to `UCCompareTextDefault` (`FUN_002957e4`): 1 → 0xE (case, width, composition), 8 → 0x10 (diacritics), 0x10 → 0x38000 (digits as numbers) | `FUN_0028ec76`, `FUN_002957e4` | Done (accents: Latin-1's letters folded; other scripts compared by code point) |

## 25. iTunes 8.2.1: deleting a playlist

| Item | Source | Status |
|---|---|---|
| Delete in the sources (`FUN_001677bc` → `FUN_00165bec`, then AXSelectedRowsChanged), Edit ▸ Delete with the sources' keyboard | `FUN_001677bc` | Done |
| The question: STR# 135.002 "Are you sure you want to delete the playlist “^1”?"; with ⌥ (the modifiers' bit 11, `FUN_003ae4b0`; not for a folder) 135.003 "… and all contents from your iTunes library?"; a folder 135.004 with 135.005 "All folders and playlists inside this folder will also be deleted."; Delete / Cancel (23987.015 / .004), "Do not ask me again" (23987.019) kept at +0x9DC (a playlist), +0xC09 (with ⌥), +0x300B (a folder) | `FUN_00165bec` | Done (the box not shown) |
| Not asked: with ⌘ (bit 8, `FUN_0025376e`); a playlist with no songs that is not smart (+0x128 & 3 ≠ 1, and `FUN_00453c96`); a folder with nothing in it (+0x1C) | `FUN_00165bec` | Done |
| The deletion: with ⌥ the playlist's songs out of the library first (`FUN_00454edc`, `FUN_000ea56a`); the playlist, a folder with all it holds (`FUN_000dfeb0`, recursive), out of its folder (`FUN_002e6620`) | `FUN_000dfeb0` | Done (the row chosen after it by eye: the one that took its place, else the one above; the ⌥ deletion not tested) |

## 26. iTunes 8.2.1: renaming a playlist

| Item | Source | Status |
|---|---|---|
| The rename (`FUN_00166b90`): with accessibility on (`FUN_003d756a`) Get String (`FUN_000ed5f6`: STR# 158.014 "Name Playlist", 158.013 "Enter a name for the playlist:"), else an editor in place (`FUN_0058bdd6`, `FUN_0058b80c`: a field over the row's name, armed by a timer of the delay given, in ticks / 60) | `FUN_00166b90` | Done (the in-place editor; the field over the name at 14n + 39, in the list's text style) |
| Started: a new playlist, folder or playlist from selection (`FUN_00166d0a`, 1 tick, after `FUN_0013c60c`, `FUN_0014ec46`, `FUN_001399bc`); a click on the row already chosen, after the double-click time (`FUN_0016e5fe`, `GetDblTime`), a second click cancelling it; Enter (char 3) or ⌘-Return (`FUN_0016b9ea` with `FUN_003ae408`, the ⌘ taken off), at once | `FUN_00166d0a`, `FUN_0016e5fe`, `FUN_0016b9ea` | Done (the double-click time 0.5 s) |
| Kept: the playlist's name set (`FUN_000dbc2e`), the row moving to its place by the new name | `FUN_000dbc2e` | Done (Return or Enter keeps it, Escape cancels, a click elsewhere in the sources keeps it; an empty name keeps the old: by eye) |

## 27. iTunes 8.2.1: importing and exporting playlists

| Item | Source | Status |
|---|---|---|
| File ▸ Library ▸ Export Playlist… (`expr`, `FUN_0014ec46` → `FUN_000a8ef2`): a save panel with STR# 149's types, Plain Text (format 1, 'TEXT' .txt), Unicode Text (2, or 4 with ⌥; 'utxt' .txt), XML (3; 'TEXT' .xml), and for a podcast list OPML (8, .opml); the type chosen kept (+0xC01); written by `FUN_000add4a` → `FUN_000ada92` (a temporary "iTunes Export" file swapped in) → `FUN_000ab536` | `FUN_000a8ef2`, `FUN_000ada92` | Done (the desktop's panel has no type popup: .xml XML, else Unicode Text; Plain Text and OPML not offered) |
| Text (`FUN_000aa206`): STR# 132's columns in this order, each when its field is in the list (Name 2, Artist 4, Composer 0x12, Album 3, Grouping 0x27, Genre 8, Size 0xC, Time 0xD, Disc Number and Count 0x18, Track Number and Count 0xB, Year 7, Date Modified 0xA, Date Added 0x10, Bit Rate 5, Sample Rate 6, Volume Adjustment 0x4A, Kind 9, Equalizer 0x11, Comments 0xE, Play Count 0x16, Last Played 0x17, Skip Count 0x44, Last Skipped 0x45, My Rating 0x19, Location 0xF); a tab between cells (`FUN_000a9f4e`), control characters made spaces; numbers `%d` and empty for 0 (`FUN_000aa192`), the size `%llu`, dates the short local date and time (`FUN_000aa116` → `FUN_00294440`), Location the file's path (`FUN_002556d2`) or a stream's URL; Unicode: a BOM, UTF-16 in the machine's order (2: 0x0009, little-endian on Intel) or the other (4) | `FUN_000aa206`, `FUN_000a9f4e` | Done (Time in seconds and the line's end, a CR, as iTunes' exports have them: the register not followed; Location a POSIX path, not the Mac's HFS one; Date Modified, Volume Adjustment, Equalizer, Last Played, Skip Count, Last Skipped left empty: not kept) |
| XML (`FUN_000ab536`, formats 3 and 6): the library's plist (Major Version 1, Minor Version 1, Application Version 8.2.1, Features…) with the songs marked and the playlist; Export Library… (`expl`) the whole of it | `FUN_000ab536` | Done |
| Import Playlist… (`impt`, `FUN_000a9198`: an open panel titled 149.005 "Import"); the file's kind by its extension (`FUN_000fc1d6`): pls 4, m3u 8, m3u8 0x80000, opml 0x4000, …, else 1 (an XML or a text, by its content); read by `FUN_00103dc6` | `FUN_000fc1d6`, `FUN_00103dc6` | Done for M3U, M3U8, PLS, XML and text (OPML not done) |
| M3U (`FUN_000d9fa2`): `#EXTM3U` first makes it extended; then `#EXTINF:…,title` names the next entry; each line not starting with `#` a file or URL (`FUN_000d99b4`); M3U8 in UTF-8 | `FUN_000d9fa2` | Done (relative paths from the playlist's folder; a name given to a stream's new track only) |
| PLS (`FUN_000d9cf2`): `FileN=` starts an entry (the one before added), `TitleN=` names it, other lines passed over | `FUN_000d9cf2` | Done |
| The imported playlist: named by the file (its XML's playlists by theirs) | | Done (by iTunes' use) |
| Over a playlist (`FUN_000e8f26`: the message 'plda' to it, `FUN_000e6bd4` → `FUN_000e8b6c`; from iTunes itself `FUN_000e894e`): taken when it takes songs (its info's flag 8: not a smart playlist, not a folder), is not the playlist dragged, and `FUN_000e051a` can add them (not kind 0x15); the songs added at its end, those already there kept (8.2.1 asks nothing about duplicates) | `FUN_000e894e`, `FUN_000e051a` | Done (the row ringed; files and URLs dragged from elsewhere, `FUN_000e8b6c`'s other half, not done) |
| Songs dragged from the list onto the sources (the list's click, `FUN_002fc392`, starting the drag): the same test, `FUN_000e894e`: a playlist that takes songs (not smart, not a folder) and is not the list's own; the songs added at its end (`FUN_000e051a`) | `FUN_000e894e` | Done (the row ringed; dropped below the rows, where iTunes' state is 3 with the library's master as the target, `FUN_000dad86`: not done; no reordering inside a playlist) |
| Songs moved inside a playlist (the list's drag, `FUN_001a8cae`): only when the playlist is sorted by its own order, field 1 (`FUN_001a40d6` → `FUN_000dcb80`; not for kind 0x12, nor Party Shuffle while it plays from its own songs); the place the row under the pointer, the next one past its middle (`FUN_002f8cb8`); the edges scroll after 16 ticks, a row each 2 | `FUN_001a8cae`, `FUN_001a40d6` | Done (the first column's header sorts by the playlist's order, field 1; the insertion line by eye; the edges' scrolling not done) |

## 28. iTunes 8.2.1: the search field

The field at the top right (+0x454 in the window, section 4) filters the songs as they are typed:
the window's filter (`FUN_0013d708`) is the search's rules, made from the field's text by
`FUN_00105e24` with the scope chosen (+0x45c), and those of the Browser (+0x58) and the grid, all
of them matched together; the Browser's columns and the grid are built from the songs the search
leaves. With the iTunes Store shown, the field searches the Store instead.

| Item | Source | Status |
|---|---|---|
| The words: the text (255 UTF-16 units at most, `FUN_0028e734`) without its leading spaces (`FUN_0028e8b4`), cut at the characters of class 8 (`FUN_0028e80a`, the spaces) | `FUN_00105e24` | Done (spaces, tabs and line ends) |
| The rules: a list matching all (`FUN_0037b110(0)`) of one list per word matching any (`FUN_0037b110(1)`) of 0x01000002 "contains" on the scope's fields: a song stays when each word is in one of its fields, case ignored | `FUN_00105e24`, `FUN_0037bb7c` | Done (case folded for ASCII, Latin-1 and Latin Extended-A; diacritics not ignored: iTunes' comparison not read) |
| The fields, by scope: All (`All `) Name 2, Sort Name 0x4E, Album 3, Sort Album 0x4F, Artist 4, Sort Artist 0x50, Album Artist 0x47, Sort Album Artist 0x51, Composer 0x12, Sort Composer 0x52; then each of Genre 8, Kind 9, Grouping 0x27, Comments 0x0E, 0x11, Category 0x37, Description 0x36, Show 0x3E (and Sort Show 0x53) only when its column is in the list's (`FUN_004542fe`, `FUN_001722d8`), every one of them (`FUN_00172256`) for an iPod's lists and some of the library's; and 0x38. Artist (`Art `, `Auth`): 4, 0x50, 0x47, 0x51. Album (`Alb `): 3, 0x4F. Composer (`Comp`): 0x12, 0x52. Song (`Song`, `Name`, `Titl`): 2, 0x4E. Show (`Seri`): 0x3E, 0x53 | `FUN_00105e24` | Done for Name, Album, Artist, Album Artist, Composer, and Genre, Kind, Grouping, Comments by their columns (the sort fields, 0x11, 0x37, 0x36, 0x3E, 0x38: not kept by the library) |
| A word of stars, in All: also the songs rated that many stars (`FUN_0037cb1c(list, 0x20, 0x19, n × 20)`) | `FUN_00105e24` | Done |
| The magnifier's menu (`xMNU` 515, "Search"; `FUN_00145dc0`, for the library only, +100 = 1): its items by the source's distinguished kind (+0xE8): Music (4) All, Artist, Album, Composer, Song; Movies, Rented Movies and Podcasts (2, 7, 10: 7 is Rented Movies, STR# 9000.042, by `FUN_002e68aa`) All, Title; TV Shows (3) All, Title, Show; Audiobooks (5) All, Title, Author; the others All, Artist, Album, Name. Their text `kSearchMenuItemAll` and STR# 210 (8 Artist, 9 Album, 10 Composer, 11 Song, 22 Name, 23 Author, 24 Title, 25 Show); the scope chosen checked, the first when it is not among them (the scope stays the window's) | `FUN_00145dc0` | Done (Movies, TV Shows, Podcasts, Audiobooks too: section 29; the arrow beside the glass, AquaKit's `onSearchMenu`: drawn by eye) |
| Cleared by its cancel button or Escape: every song back | AquaKit's NSSearchField | Done |
| The label under the field (+0x458, `FUN_0013d0d0`, shown by `FUN_0013cfe0`): with the field empty STR# 210 by scope, 1 Search, 15 Search Artist, 16 Search Album, 17 Search Composer, 18 Search Name, 19 Search Song, 21 Search Title, 26 Search Authors, 29 Search Shows; in the Store the page's search_info title, else 14 "Search iTunes Store"; while searching, STR# 163.017 "^1 of ^2", the songs shown (+0x474) of the library's | `FUN_0013d0d0`, `FUN_0013cfe0` | Not done: its place is the window nib's (not extracted), sized to its text, at most the field's width |
| The Store's search (`FUN_00232dd8`): the page's request with "term" goes to the URL bag's `search` (`https://search.itunes.apple.com/WebObjects/MZStore.woa/wa/search`), `submit=edit` added; search hints from the page's search_info `search_hints_url` (`FUN_00142b22`) | `FUN_00232dd8`, `FUN_00142b22` | Done on Return: `iTunesStore`'s "search" command loads the bag's `search` with term and submit=edit, which Apple still answers with its results page (Songs, Albums…); the hints and the per-source search history (+0x5b8, `FUN_00142908`) not done |
| Going between the library and the Store | | The field emptied, the menu the library's only: by the code's two uses, the history not followed |

Verified in Xvfb (a library of 14 songs, typed through XTEST): "Michael Jackson" leaves the six songs
by Michael Jackson or "Jackson, Michael" (The Jackson 5, Janet Jackson and Michael Bublé gone), the
Browser's columns and the grid by album following; "temperton" finds Thriller by its composer, and
none with the scope Artist; the cancel button and Escape bring back the 14; in the iTunes Store,
Return shows the Store's "Showing results for “Michael Jackson”". The field now keeps its caret in
view while the text is longer than it (AquaKit's TextField).

## 29. iTunes 8.2.1: Movies, TV Shows, Podcasts, Audiobooks, Radio

LIBRARY's other sources. Each one is a distinguished playlist of the library: a smart playlist
on the track's media kind that `FUN_002e68aa` makes. The exception is the radio tuner, which is a
source of its own ('tune'). The General pane's Show boxes are the library's hide flags. A podcast
is a feed whose episodes are tracks. The port is in `librarysources.cpp`, `media.cpp`,
`podcasts.cpp`, `podcastui.cpp`, `video.cpp` and `radio.cpp`, with hooks in `main.cpp`.

| Item | Source | Status |
|---|---|---|
| The sources' rows (`FUN_00168254`): a distinguished playlist of the library (`FUN_002e616c`: +0x128 bit 1, a kind in the mask 0x40000FC) gets its group by kind (+0xE8): 2 Movies 4, 3 TV Shows 6, 4 Music 3, 5 Audiobooks 8, 6 Ringtones 0xB, 7 Rented Movies 5, 0x1A 0x83; the Podcasts (10) 7, Applications (0x12) 9, the radio tuner (source 'tune') 10. The rows sorted by group (`FUN_00166d96`). So the LIBRARY reads Music, Movies, TV Shows, Podcasts, Audiobooks, Applications, Radio, Ringtones | `FUN_00168254`, `FUN_002e616c`, `FUN_000dc5be` | Done (Rented Movies, Genius and iTunes DJ: not done) |
| Their names: STR# 9000 .036 Music, .034 Movies, .035 TV Shows, .030 Podcasts, .037 Audiobooks, .039 Applications, .041 Ringtones, .042 Rented Movies; 154.001 Radio. Their icons: Source Icons 441, rows 0 Music, 1 Movies, 2 TV Shows, 3 Podcasts, 4 Radio, 5 Audiobooks, 27 Applications, 24 Ringtones | `FUN_002e68aa` (`FUN_002940f4(0x23280022…)`), PBUT 441 | Done (the rows of 441: matched by eye to the pictures) |
| Hidden or shown (`FUN_000dc5be`, byte 0 of its answer): flags of the library object for Movies +0x5932, TV Shows +0x5933, Audiobooks +0x5934, Applications +0x5935, Ringtones +0xF2D8, Podcasts +0x1099D, Radio +0x1099E (iTunes DJ +0x2A85; Genius `FUN_005790fe`). These are DITL 256's boxes 4 Movies, 5 TV Shows, 6 Podcasts, 7 Audiobooks, 8 Applications, 9 Radio, 10 iTunes DJ, 11 Ringtones, 23 Genius. Adding a track of a kind turns its source back on (`FUN_002045c4`, `FUN_001517f8`): Movies (kind & 0x8006 = 2), TV Shows (& 0x8044 = 0x40), Audiobooks (& 0x800C = 8), Ringtones 0x4000, Applications 0x61000. Podcasts and Radio are not turned back on this way. With the podcasts hidden, Subscribe to Podcast is disabled (`FUN_000b5314`) and a pcast link asks 9008.018 (`FUN_003cc45e`) | `FUN_000dc5be`, `FUN_002045c4`, `FUN_001517f8` | Done (General's boxes 4–9 and 11. The defaults were not found, because the preferences' loader was not read: Movies, TV Shows, Podcasts and Radio start on, as on a fresh 8.2.1; Audiobooks, Applications and Ringtones start off, then come on with their first track. iTunes DJ and Genius not done) |
| The media kind (`FUN_0017b984`, cached by `FUN_00181e2e`). The tags' kind byte (MP4's `stik`): 1, 3, 4 music 1; 2 audiobook 8; 5 0x10; 6 music video 0x20; 7, 8 podcast 4; 9 movie 2; 10 TV show 0x40; 11 0x80; 14 ringtone 0x4000. If the byte gives none, a track with video (+0x3C bit 0x8000) is a movie unless 0xE62 is set. Otherwise the file type decides: 'M4B ', 'AA  ', 'AAX ' an audiobook; 'M4R ' a ringtone; 'PDF ' a booklet. The podcast flag (0x10000000) adds 4. Anything else is music | `FUN_0017b984` | Done (MP4's `stik` and `pcst` through TagLib; video from the file's `hdlr` 'vide' track, its size from `tkhd`) |
| Each source's songs (`FUN_002e68aa`, a rule on field 0x3C): Music has any of 0x1121B1; Movies (& 0x8006) = 2; TV Shows (& 0x8044) = 0x40; Audiobooks (& 0x800C) = 8; Ringtones 0x4000; Rented Movies (& 0x8002) = 0x8002. Podcasts are the podcast playlist's (`FUN_003c9ed2`, kind 10) | `FUN_002e68aa` | Done (Music leaves the podcasts out: theirs is 4 alone) |
| Their columns (`FUN_0045386e`: the field, then 1 on the sorted one): Movies Name (sorted), Time, Genre, Year, My Rating, Description (0x36); TV Shows Name, Time, Show 0x3E (sorted), Season 0x3F, Description; Audiobooks Name, Time, Artist (sorted), My Rating; Ringtones Name (sorted), Time, Artist, Album. Podcasts (`FUN_003c21fa`): Name, Time, Release Date 0x22 (sorted, descending: +7), Description (125 wider). CWid 9000 gives Description 250, Category 80, Release Date 94; Show, Season and Episode ID are not in it | `FUN_002e68aa`, `FUN_003c21fa`, CWid 9000 | Done (View Options keeps each source's columns. Show 125 and Season 60 by eye. The Podcasts' Name column is titled "Podcast", 9006.032, and the Radio's "Stream", 141.001, by eye) |
| The search menu (section 28) by kind: Movies, Rented Movies, Podcasts All, Title; TV Shows All, Title, Show (`Seri`); Audiobooks All, Title, Author | `FUN_00145dc0` | Done |
| The plist (the strings of `FUN_000ab536`'s keys): Has Video, Video Width, Video Height, Movie, Music Video, TV Show, Podcast, Unplayed, Series, Season, Episode, Episode Order, Description, Long Description, Category, Release Date, Podcast URL, External GUID, Remember Bookmark, Bookmark Time. The distinguished playlists carry Distinguished Kind and their own key (Movies, TV Shows, Podcasts, Audiobooks true) | iTunes' strings | Done (they are written again on each save from the media kinds, with their Persistent IDs kept. Audiobooks and ringtones have no key of their own, so the port adds "Media Kind", as it adds "Smart Rules") |
| Kinds: STR# 3301 (QuickTime's) .004 MPEG-4 video file, .001 QuickTime movie file; m4b an AAC audio file | `Localizable.strings` | Done |
| Advanced ▸ Subscribe to Podcast… (`pcsb`, `FUN_000b390c`): `FUN_000b20dc`'s URL box (130.034 "Subscribe to Podcast", 130.003 "URL:"), with the last URL kept (+0x1AA4C); then `FUN_003cd11c(app, url, len, 3, 0)` | `FUN_000b390c`, `FUN_000b20dc` | Done (pcast://, itpc:// and feed:// are read as http://) |
| The feed (the parser's strings): RSS's channel, item, title, description, pubDate, enclosure (url, length, type), guid, category; the iTunes namespace (its four spellings) author, summary, subtitle, duration; Atom's entry, published, link rel="enclosure"; the entities amp, copy, hellip, mdash, nbsp, ndash, reg, trade | iTunes' strings | Done (new-feed-url, keywords, explicit, image, block and OPML not done. An episode's Album is the podcast, Artist its author, Genre "Podcast": by iTunes' use) |
| The Podcasts playlist (`FUN_003c9ed2`): kind 10, named 9000.030, with its columns (`FUN_003c21fa`) | `FUN_003c9ed2` | Done |
| The list: each podcast is a row with its triangle (PBUT 314) and its episodes under it when open. GET ALL on a podcast with episodes not downloaded and GET on such an episode come from Localized.rsrc's PBUT 672 "Get All Podcasts" (50 × 13) and 674 "Get Podcast" (31 × 13), their PICTs through their masks. The episodes not played have TrackStatusIcons 306's blue dot (column 12); one downloading has its arrows (column 7) | PBUT 314, 672, 674, 306 | Partial (the cells' places, the grey of an episode not downloaded and the podcast row's date and description are by eye; the list's own drawing for podcasts was not read) |
| The placard under the list (Placards.nib, window 216): HIView 3220 on PBUT 1034, 46 high. Unsubscribe 3221 (`PCSB`, rounded bevel 88 × 22 at 20, 12, TXTS 301), Settings… 3222 (`PCSe`, 80 × 22 at 118), Report a Concern 3223/3224 (`PCRC`), Podcast Directory 3225/3226 (`STPD`, TXTS 544, link arrow PBUT 650), Refresh 3227 (`PCRB`, 76 × 22 at 500). The last ones are bound to the right. `FUN_0013b678` titles 3221 163.081 "Unsubscribe", shows the Directory for the library (0x65) and Report a Concern for the Store's podcasts | Placards.nib, `FUN_0013b678` | Done (Unsubscribe / Subscribe, Refresh for every podcast, the Directory opening the iTunes Store. Settings… is disabled: the podcast's own settings were not done. Report a Concern not done) |
| The Podcasts pane (DITL 254): Check for new episodes (MENU 1374: every hour, day, week, manually), When new episodes are available (1375: download all, the most recent one, nothing), Keep (1376: all, all unplayed, the most recent, the last 2, 3, 4, 5, 10) | DITL 254 | Done (the defaults, every day / the most recent one / all, and Keep deleting only the files the port put in Podcasts/: by eye) |
| The episodes' files: iTunes Music/Podcasts/<the podcast>/<the enclosure's file name> | | Done (the folder is iTunes', the code not read; fetched with curl, User-Agent "iTunes/8.2.1"; a feed that cannot be read gives 8015.004) |
| Playing a movie: Playback pane (DITL 257) "Play Movies and TV Shows" and "Play Music Videos" (MENU 1373: in artwork viewer, in the iTunes window, in a separate window, full screen, full screen (with visuals)) | DITL 257, MENU 1373 | Partial (libmpv's software renderer draws into a view: in the iTunes window in the list's place, in a window of its own as large as the movie, or full screen. A double click switches to full screen and back; Escape stops. There is no artwork viewer in the port, so the iTunes window is used for it; "with visuals" plays without them. The default, the iTunes window, and the controls over the picture were not read) |
| Audiobooks, podcasts, movies and TV shows remember their position: Remember Bookmark, Bookmark Time | the plist's keys | Done (kept on pause, stop, another song and quitting; played from there next time, from the start once played through. Which kinds remember by default: by eye) |
| The radio tuner (`FUN_000bbbbc`): "GET http://pri.kts-af.net/xml/index.xml?" over HTTP/1.0, with "sid=%1&" when it has a session. A genre's streams use `FUN_000bc368`'s "tuning_id=%1&show_field%5B%5D=description&…long_description…num_users…max_users". The answer is parsed with expat (`FUN_000bb846`): kb:menu_record (menu_id, name, shoutcast_count, pass_count), kb:station_record (station, station_url_record/url, bandwidth_kbps, status_code, description, long_description, num_users, max_users); kb:error, kb:error_no. The genres are cached in the preferences ('TCat', "Radio Tuner Categories": `FUN_000f1a7c`, `FUN_000f1e88`), and a failed refresh keeps the old list (`FUN_000f2308`) | `FUN_000bbbbc`, `FUN_000bc368`, `FUN_000bb846`, `FUN_000f2308` | Partial (the service now answers Apple's 404, so the port shows iTunes' 154.010 "An error occurred while contacting the radio tuning service…" over an empty list each time Radio is chosen. The genre and stream rows, their triangles, Stream / Bit Rate / Comments, and a stream played without entering the library are written to the reversed format but untested. No cache of the genres; 154.009's question not asked) |

Verified in Xvfb (display :73, its own HOME and D-Bus; WindowServer, SystemUIServer). The test
library had an MP3, an H.264 movie (.m4v), an MP4 with `stik` 10 and a show, season, episode and
description, and an .m4b, all made with ffmpeg. The first launch showed LIBRARY: Music, Movies, TV
Shows, Podcasts, Audiobooks (turned on by the .m4b) and Radio, with their icons. Movies listed the
movie with Name, Time, Genre, Year, Rating, Description. TV Shows listed the episode with Name,
Time, Show (sorted), Season 1, Description. Music held only the MP3. The plist had Has Video, Video
Width 320 / Height 240, Movie, TV Show, Series, Season, Episode, and the distinguished playlists
with Distinguished Kind 4, 2, 3, 10, 5.

Subscribe to Podcast, given an RSS feed served on 127.0.0.1, showed "The Test Cast" open with its
three episodes. The newest was downloaded into iTunes Music/Podcasts/The Test Cast/. The other
two were grey with GET, the podcast row had GET ALL, and all had blue dots. Release Date was
sorted descending and the descriptions came from itunes:subtitle / the HTML description. The
placard was under the list. GET downloaded episode 2; the triangle closed and opened the podcast;
playing an episode took its dot away.

The movie played in the list's place, its aspect kept, and the list came back at its end. With
"in a separate window" the TV episode played in a 320 × 180 window titled "Pilot". The audiobook
was paused at 0:03 and played again from 0:04 (Bookmark Time 3797 in the plist). Radio showed
the 154.010 alert.
