# Time Machine's host side: who shows the browser, how snapshots get into it

Mac OS X 10.6 (Finder 10.6, `Finder_FE-665`; Time Machine.app 1.1; Dock 10.6). Every claim cites a
function or an address of the binary named. The Backup.framework side (the scene, stars, timeline,
the `BU*` internals) is in `docs/timemachine-reverse.md` / `.local/media/re/backup/` by the other
agent; this file covers what the host does and what it hands to the framework.

## 0. Sources and tools

| What | Where |
|---|---|
| Time Machine.app's launcher, thin x86_64 | `.local/media/re/finder/tmapp/TimeMachineApp.x86_64`, `tmapp.dis` (llvm-objdump; 139 lines, the whole program) |
| The Finder's x86_64 slice, decrypted (`tools/deprotect.py`, 935 pages) | `.local/media/re/finder/Finder.x86_64` (`Finder.enc` the encrypted thin slice) |
| Its full decompile (REA `decompile-all`, Ghidra 12.2, same parameters as `mx-TimeMachine/dec.log`) | `.local/media/re/finder/dec/functions/<addr>.c`, checkpoint `finder/ckpt` |
| Its Objective-C metadata, method → IMP map | `finder/objc.txt` (`llvm-objdump --objc-meta-data`), `finder/methods.tsv` (`finder/mkmethods.py`) |
| Its annotated disassembly (selectors, cfstrings, literals named) | `finder/finder.dis` (`llvm-objdump --macho -d`); `finder/fn.py ADDR` prints one function, `finder/d.sh ADDR` one decompiled function |
| CarbonCore's `_CSBackup*` path helpers (in the clear) | `.local/media/re/finder/cc-dec/functions/` (REA, `--name _CSBackup`; checkpoint `finder/cc-ckpt`) |
| The Dock (already decrypted and decompiled) | `.local/media/re/dock/dec/functions/` |
| The Finder's strings | `Finder.app/Contents/Resources/English.lproj/Localizable.strings` (UTF-16), the nibs `BackupVolumeBrowserWindow.nib`, `ProgressRenameConflictView.nib`, `ProgressReplaceView.nib`, `BackupProgressWindow.nib` |

Naming used below: `FUN_x` is a stripped C++ function of the Finder (named from its `Backup.mm`
trace strings when it has some: every TBackup function logs through `FUN_1000131a2(file, line,
"_FXTracePeabodyStartup", fmt, ...)`, so the source line numbers are known). Globals: `DAT_10042c158`
the Finder's well-known nodes (`+0x40` Home, `+0x60` Computer, `+0x70` Desktop: from
`cmdGoHome:`, `cmdGoToComputer:` 0x100097c04, `TDesktopWindowController initializeDesktop`).
Node predicates: `FUN_10001a7ba(node, 0x10000)` "is a query (search) node", `FUN_10003c7fa(node)`
(flag 0x4000) "is the Computer" (they pick the "?Search" and "?Computer" URLs, §6).

## 1. The launcher: /Applications/Time Machine.app (com.apple.backup.launcher)

`Info.plist`: CFBundleIdentifier `com.apple.backup.launcher`, CFBundleSignature `dlet`,
**LSUIElement 1** (no Dock icon of its own, no menu bar), LSMinimumSystemVersion 10.5. No Cocoa:
it links ApplicationServices, CoreFoundation, CoreServices, libSystem only. The whole program:

- `start` (0x100000cbc) → `main` (0x100000d23) → `exit(main())`.
- `main(argc, argv)` (0x100000d23): `cmd = argc > 1 ? atoi(argv[1]) : default`; a jump table at
  0x100000d50 (5 entries) for 0..4, anything else (or no argument) the default:
  - **0 / default** (0x100000d64): post `com.apple.peabody.show`.
  - **1** (0x100000d6d): `url = CFURLCreateWithFileSystemPath(NULL,
    "/System/Library/PreferencePanes/TimeMachine.prefPane", kCFURLPOSIXPathStyle, false)`;
    `LSOpenCFURLRef(url, NULL)`; `CFRelease(url)`.
  - **2** (0x100000d9b): post `com.apple.peabody.backupnow`.
  - **3** (0x100000da4 → 0x100000cf8): `if (_CSBackupServerIsActive() && (p =
    _CSBackupGetSharedServerProxy())) _CSBackupServerProxySendMessage(p, 'canc', 0, 0)` (Stop
    Backing Up: the same 'canc' message as the menu extra's `stopBackingUp:`, section 31).
  - **4** (0x100000dad): post `com.apple.peabody.browseOtherDisks`.
  - Posting (0x100000db4): `CFNotificationCenterPostNotification(
    CFNotificationCenterGetDistributedCenter(), name, NULL, NULL, deliverImmediately = true)`.
  - returns 0. Nothing else: no event loop, no window, the process exits at once.
- Who passes the number: the **Dock**. `-[DOCKFileTile createMenu:options:]` (Dock 0x1000355d8)
  reads the bundle's `Resources/Menu.plist` (an array of {command, name}) and
  `English.lproj/Menu.strings`, builds the items (`FUN_1000332e3`, the `command` key's integer
  becomes the item's command; the item's property 'ouch'/'APP ' marks it as a docklet item); for
  the bundle id `com.apple.backup.launcher` only (CFStringCompare at 0x1000355d8+): it removes
  "Stop Backing Up" (3) when backupd isn't running, "Back Up Now" (2) when it is
  (`FUN_1000453b5`: `_CSBackupServerCopyStatus()`'s `kCSBackupdKeyIsRunning == kCFBooleanTrue`),
  and retitles item 1000 (the Dock's "Open") with Menu.strings' `"Enter Time Machine"`.
  A chosen docklet item → `-[DOCKFileTile doCustomMenuCommand:withAction:andModifiers:]`
  (0x100044347, action 2) → a dispatch block `FUN_100044548`: resolves the tile's alias, takes the
  bundle's executable (`_CFBundleCopyExecutableURLInDirectory`) and launches it with
  `argv = [ [NSString stringWithFormat:@"%d", command] ]` through `_LSOpenApplicationURL`
  ("couldnotlaunch" logged on failure). A click on the tile launches it with no argument → 0 →
  `com.apple.peabody.show`.
- Menu.plist: 1 "Open Time Machine Preferences...", 2 "Back Up Now", 3 "Stop Backing Up", 4
  "Browse Other Time Machine Disks..."; Menu.strings maps them to the "…" forms and has "Enter
  Time Machine".

## 2. Who receives com.apple.peabody.show: the Finder

- `com.apple.peabody.*` appears in only four binaries of the whole system: Time Machine.app,
  TimeMachine.menu, Search.bundle (Spotlight's menu: "show" only) and Backup.framework (the
  constants `kInvokeTimeMachineNotification` = "com.apple.peabody.show", `kBackupNow` =
  "com.apple.peabody.backupnow", `kBackupBrowseOtherDisks` = "com.apple.peabody.browseOtherDisks",
  plus "About to post com.apple.peabody.show": the framework posts it too). The decrypted Dock,
  backupd, loginwindow, mds don't contain it; the Finder's protected `__TEXT` hid it until decrypted.
- Binaries with an `LC_LOAD_DYLIB` of `/System/Library/PrivateFrameworks/Backup.framework/Versions/A/Backup`
  in `.local/media/re/root` (llvm-otool -L): **Finder**, TimeMachine.menu, AddressBook.framework and
  AddressBookManager.app (Mail and iPhoto, the other Time Machine hosts, aren't on the DVD).
  SystemMigration only names it in a plist.
- **`TBackup::Initialize`** (Finder 0x100012af0, Backup.mm line 538 "Completed TBackup::Initialize()"):
  clears the "entering" flag `DAT_10042b980` and adds four distributed observers (suspension
  behaviour 4 = DeliverImmediately):

| Notification | Object | Callback |
|---|---|---|
| `com.apple.backup.BackupRequestHandshakeNotification` | `com.apple.backup.BackupObject` | `FUN_1001da7a3` (§15) |
| `com.apple.peabody.backupnow` | any | `FUN_1001d99d3` → `TBackup::BackupNow` `FUN_1001d99a0`: `_CSBackupBackUpNow(_CSBackupGetDefaultBackupSession())` |
| `com.apple.peabody.show` | any | **`FUN_1001db9cd`** |
| `com.apple.peabody.browseOtherDisks` | any | **`FUN_1001da6dc`** |

  then `TBackup::SetSnapshotsRoot` (`FUN_100012bea`/`FUN_100012d08`). `TBackup::Terminate`
  (`FUN_1001da0ca`) removes them.
- `FUN_1001db9cd` (show): returns if the Finder's UI mode (`FUN_10000b888`, the TUIPolicyManager
  mode at `+0x4a8`: 'full', 'simp' for Simple Finder (InterfaceLevel = "Simple"), **'peab'** while in
  Time Machine) is already 'peab'; if `DAT_10042b980` (isEntering) is set: "Not trying to start TM
  because already entering UI" (line 186); else sets it and calls
  **`TBackup::EnterBackupWithDefaultStore`** 0x1001db972 (line 175). C++ exceptions are caught.
- `FUN_1001da6dc` (browseOtherDisks): the same guards ("Not trying Browse Other Disks because
  already entering UI", line 227), then **`TBackup::ChooseAndEnterBackupStore`** 0x1001d973c (§14).
- The Finder's own `-[TApplicationController cmdEnterTimeMachine:]` (0x10019532b) and
  `cmdBrowseOtherTimeMachineDisks:` (0x100195335) jump straight to those two; no nib binds them.

## 3. Entering: the sequence

1. **`TBackup::EnterBackupWithDefaultStore`** (0x1001db972, line 601): `GetFrontProcess(&psn)` (the
   application in front when Enter was chosen), then `TBackup::EnterBackupSession(
   _CSBackupGetDefaultBackupSession(), &psn)`.
2. **`TBackup::EnterBackupSession`** (0x1001db8e4, line 686): stores the session as the *browse
   session* (`FUN_1001dec08(&DAT_10042c600, session)`), runs **`TBackup::PreflightShowBackup`**
   (0x1001db08c) and if it passes, `TBackupStoreMountingTask::MakeStoreAvailable(psn)` (0x10014120c,
   line 695).
3. **`PreflightShowBackup`** (0x1001db08c) returns false, and:
   - no browse session: "GetBrowseSession() == NULL" (line 1183);
   - Simple Finder ('simp'): alert LA31 "You don’t have permission to open this application." (line 1190);
   - a modal window in front (`UAlerts::FrontWindowIsModal`, `FUN_10002d817`): beeps/brings it forward (line 1199);
   - the session not configured (`TBackup::IsBackupSessionConfigured` `FUN_1001db032`: the
     snapshots root `FUN_100012dd0` exists or its error isn't -6581): the Time Machine icon
     ('mill'/'peab' icon ref) on an alert (`FUN_1000d8e4c`: message, informative text, buttons 1..3):
     - no Time Machine disk at all among the mounted ones (`FUN_10012bf0e` = number of computer
       backups found, §14, is 0): "You haven’t selected a location for Time Machine backups." (BU1) /
       "To select a location for backups, set up Time Machine." (BU2), buttons "Set Up Time Machine"
       (BU3), "Cancel" (AL1); Set Up opens TimeMachine.prefPane (`FUN_1001c51cc`), line 1263, 1295;
     - else (some disk has backups): BU1 / "You can set up a new backup disk in Time Machine
       preferences, or use a previous backup disk." (BU32), buttons "Use Previous Disk…" (BU33, 1),
       "Cancel" (AL1, 2), "Time Machine Preferences…" (BU34, 3): 1 shows the chooser (§14), 3 the
       preference pane (line 1226).
   - on every false return the isEntering flag is cleared 4 s later (`FUN_10000fc10(4.0, FUN_1001d9224)`).
4. **`TBackupStoreMountingTask`** (`MakeStoreAvailable` 0x10014120c makes one, 0x208 bytes, ctor
   0x100140c6c, "TBackupStoreMountingTask created"; a second request only brings its progress
   window forward): a thread task (`FUN_1001415fa`, body `FUN_100140a20`) that mounts the backup
   store (network / sparse bundle; `FXCheckSlowTimeMachineMount` 0x1001417b6) and finds the
   **snapshots root** = `TBackup::SnapshotsRootNode` (0x100012dd0):
   `_CSBackupSessionCopyBackupDirectory(session, inMount, &err)` — the machine folder
   `<disk>/Backups.backupdb/<computer>` — as a node (`NodeCopyFromPathName` fallback, the
   "dumping more info" traces at lines 2319-2363). Its progress shows after 1 s, named "backup
   volume" (MN24). Its destructor (0x100140ec6, lines 174-206): if it wasn't cancelled and found a
   root, and the mount took 60 s or more (`< 60000000` µs) or the default
   `FXAlwaysAskToStartTimeMachine` is set, asks "Do you still want to start Time Machine?" (MN22;
   informative "Backup volume is now available." MN23 when it was slow); then
   **`TBackup::StartTimeMachine(root, &psn, flag)`** (0x1001dd868). Otherwise isEntering is cleared.
5. **`TBackup::StartTimeMachine`** (0x1001dd868, lines 649-671): clears the pending restore
   (`DAT_10042c700`), Preflight again, stores the snapshots root (`FUN_1001d9694` → `DAT_10042c6c8`,
   under the mutex `DAT_10042c620`), then `FUN_1001d995c(psn)`: `SetFrontProcess(psn)` and "is
   that process the Finder itself?":
   - **not the Finder** → **`TBackup::StartTimeMachineForOutsideClient`** (0x1001d97d6, lines
     731-760): sends the Apple event **'peab'/'BROW'** to that process (`AECreateDesc('psn ')`,
     `AESend(..., kAEWaitReply, kAEDefaultTimeout)`); if the reply has no error ('errn' absent or
     0) the front application (Mail, iPhoto, Address Book: those link Backup.framework) hosts Time
     Machine itself and the Finder stops. If the event fails (an application that doesn't support
     it), the Finder hosts it:
   - **the Finder** (or the fallback) → **`TBackup::StartTimeMachineForFinder(flag)`** (0x1001dcde6).

So the host is always the Finder unless the front application answers 'peab'/'BROW'.

### StartTimeMachineForFinder (0x1001dcde6, lines 772-1059)

1. `FUN_100174bc6(sidebar, true)`: the sidebar model enters its Time Machine state (it makes the
   Computer item visible, `kLSSharedFileListVolumesComputerVisible`, if it was hidden, `+0x218`).
   The saved frame `DAT_10042c680..698` = NSZeroRect.
2. Posts the **local** notification `TimeMachineWillStart` (CFNotificationCenterGetLocalCenter);
   observers: `-[TQLPreviewViewController timeMachineWillStart:]` (0x1001b5c47: `stopPlaying`) and
   `-[TFlowViewController(TimeMachine) timeMachineWillStart:]` (0x1002049d1:
   `stopPlayingInlinePreview`): Quick Look and Cover Flow stop playing media.
3. The largest browser size: the main display's window-positioning bounds (`FUN_100038751` →
   `FUN_100017feb(…, CGMainDisplayID())`) **less 480 wide and 370 high**.
4. The default target: the **Desktop** folder (`DAT_10042c158+0x70`) if it's a valid Time Machine
   target, else **Computer** (+0x60). Valid (`FUN_1001dace6(node, flag)`): a search is valid if its
   scope node is; the Computer is; nodes failing `FUN_100045820` (node-flag tests) aren't; else
   `flag ? _CSBackupItemGetsBackedUp(defaultSession, url) : _CSBackupWasItemEverBackedUp(defaultSession,
   url)` (the flag is the mounting task's byte `+0x60`, 0 when entering from the menu).
5. **The window**: `[[TGlobalWindowController globalWindowController]
   windowForTimeMachineWithDefaultTarget: default]` (0x10017aafb) = the **frontmost Finder browser
   window excluding the desktop** (`frontmostBrowserWindowControllerExcludingDesktop`), else a new
   window `createWindowWithTarget: default` (so with no window open, Time Machine shows the
   Desktop folder). Kept in **`DAT_10042c660`** (the "entry window"); made key and front if it
   isn't main.
6. The target: with the default session, the window's `target`; browsing another disk, Computer.
   If not valid: Home (+0x40), then Computer. The entry window's original target path is saved in
   `DAT_10042c6b0` (a node vector, `FUN_100108b0a`).
7. The entry window's frame is saved (`DAT_10042c680`) and, if larger than the maximum, shrunk to
   `min(w, screenW − 480) × min(h, screenH − 370)`, **centred on its old frame** (x += round((w −
   w′)/2), y += round((h − h′)/2)) by an `NSViewAnimation` of 0.5 s (blocking mode 0); then
   `DAT_10042b978 = 1` (restore the frame on exit).
8. The selection (`FUN_100039eb8(nodeBrowser, …)`) is kept. Posts the local `TimeMachineDidEndResizing`.
9. **Screen updates off** (`FUN_1000199cc`: `CGSDisableUpdate(cid)`).
10. A **new** `TBackupBrowserWindowController` `initWithState: [entry windowState] targetPath:
    path(target) timeMachineTarget: TTimeMachineTarget(empty node, empty TSnapshot)` (0x100199e44)
    → **`DAT_10042b970`** (the Time Machine browser: a real, live Finder window with the entry
    window's view style, sidebar, toolbar, frame). The **entry window is hidden**:
    `_hideAttachedWindows:YES findKey:NO` (its sheets/drawers) and `orderOut:`.
11. `setBackupLastUserSelection:` the selection; `CGSSetWindowTags(cid, browserWindowNumber, 0x8)`
    (`FUN_1001016a7`; 0x8 is the no-shadow tag in the usual CGSPrivate naming, not checked in
    WindowServer).
12. **`_BUInitializeTimeMachineHostProcess()`** and the callbacks (all with the browser
    controller as the refcon):

| Registration | Finder callback | §  |
|---|---|---|
| `BURegisterActivateSnapshot` | `FUN_1001dbef4(controller, CFURLRef snapshotURL)` | 4 |
| `BURegisterDeactivateSnapshot` | `FUN_1001d9d76(controller, CFURLRef)` | 4 |
| `BURegisterTimeMachineDismissed` | `FUN_1001daab0(controller)` | 11 |
| `BURegisterTimeMachineRestore` | `FUN_1001dc572(controller, ?, CFURLRef dest)` | 9 |
| `BURegisterRequestRestoreImages` | `FUN_1001d9362(controller)` → `[controller restoreImages]` | 9 |
| `BURegisterUpdateEntryWindow` | `FUN_1001da368(controller)` → returns a window number | 11 |
| `BURegisterNavigateForwardOrBackward` (refcon 0) | `FUN_1001daa0e(?, int index, bool backward)` → int | 10 |
| `BURegisterRequestSnapshotImage` (refcon: the image renderer) | `FUN_1001bd8e4(renderer, CFURLRef)` | 5 |

    the last one by `FUN_1001bce1e(windowState)` → `FUN_1001bcc8c` (the renderer singleton
    `FUN_100010476`, mutex at `+0x60`, state `+0x14a`). Then `drawCompletelyIntoBackBuffer`.
13. The start URL: `FUN_1001dfa1e(target)` (§6) → `CFURLCreateWithString` →
    **`BUStartTimeMachine(browserWindowNumber, targetURL, 0)`** (line 1054; "started? %s" line 1059).
14. Started: UI mode **'peab'** (`FUN_1001bb650('peab', 0)`; it reads the default `_FXPeabodyMenuBar`
    but ignores it). Not started: the image renderer stops (`FUN_1001bce7a`), the browser
    `closeWindow`, `DAT_10042b970 = 0`.
15. `CGSReenableUpdate(cid)`: the swap of the entry window for the browser is never seen.

## 4. The live window: activating and deactivating snapshots

There is **one live window**, `TBackupBrowserWindowController` (`DAT_10042b970`), retargeted from
snapshot to snapshot; the other windows of the stack are pictures (§5).

- **Activate** `FUN_1001dbef4(controller, url)`: for a folder target, url → node
  (`FUN_10017585a`, `FUN_10004dee4`); the URL "file:///?Computer" → the Computer node; then
  **`-[TBackupBrowserWindowController backupTargetActivate: node]`** (0x10019aa99). For a search,
  the query's scope is set to the snapshot (`FUN_100180ab6(query, path of
  BUCopyCurrentTargetSnapshotURL())`) and the query node itself is activated. Last:
  **`BUActivatedSnapshot(windowNumber, url)`** (the window is ready; the framework shows it).
- `backupTargetActivate:` (0x10019aa99): stop watching scrolls; map the user's last selection into
  the new folder (`mapBackupSelectionToContainer:outSelection:` 0x10019b440: for each selected item
  its namesake under the new target, else the nearest ancestor that exists there); `setSnapshotForNode:`
  (0x10019a5db: a `TTimeMachineTarget` for the current snapshot, from
  `BUCopyCurrentTargetSnapshotURL()`); `setTargetPath:… rebuildView:YES retarget:NO`; reselect;
  `BUTimeMachineSetRestoreAllowed(selectionCount != 0)` (`FUN_1001d9654`; in column view always 1);
  restore the scroll position (`_backupLastScrollPosition`); `resetHistory`; `makeKeyAndOrderFront`.
- **Deactivate** `FUN_1001d9d76`: `backupTargetDeactivate:` (0x100199a03: empty), the window
  `orderOut:` unless a retarget is under way (`DAT_10042b97c > 0`), then
  `BUDeactivatedSnapshot(windowNumber, url)`.
- The browser window (`initWithState:…` 0x100199e44): **zoom (button 2) and minimize (button 1)
  disabled**; close button and `cancel:` (Escape) → `BUTimeMachineAction(1)` (0x100199f3e,
  0x100199f2f: leave Time Machine); `cmdToggleToolbar:` does nothing (0x100199a09);
  `configureWindowCloseButton` empty; no proxy-icon drag (0x100199a0f returns NO);
  `shouldStaggerWindowPositions` NO; `fetchWindowProxyNode` maps to the original
  (`originalNodeForNode:`); `isBackupBrowser` YES, `isSnapshotImageBrowser` NO.
- The **"Now"** entry is no special case in the Finder: it is the original (live) URL handed to
  `BUStartTimeMachine`; activating it targets the live folder like any snapshot (the initial
  `TTimeMachineTarget` has an empty snapshot node). The Finder's "Now", "Today (Now)", "Restore All",
  "Highlight Changes", "Backup Timeline"… strings (BU10-BU30) aren't referenced by its code
  (10.5 leftovers): the framework draws those.

## 5. The other snapshots: pictures rendered offscreen

- `FUN_1001bd8e4(renderer, url)` (the RequestSnapshotImage callback): a search URL is ignored;
  "?Computer" → the Computer; else url → node; queued (`FUN_1001bd872` → `FUN_1001bd7fe`: a task
  on the renderer's queue `+0xa0`, node list `+0x158`).
- The task `FUN_1001bd616`, still wanted: **`FUN_1001bd00c(renderer, node)`** renders, then
  `_BUUpdateSnapshotImage(CGImageRef, url of the node)` (the URL rebuilt from the node; the
  Computer as "file:///?Computer").
- `FUN_1001bd00c`: a **`TBackupSnapshotWindowController`** (one, kept at `+0x120`; created
  `initWithState: savedWindowState targetPath: path(node) timeMachineTarget: TTimeMachineTarget(node,
  snapshot of BUCopyCurrentTargetSnapshotURL)`, later retargeted `setTargetPath:withViewStyle:rebuildView:`
  + `setSnapshot:` + `restoreBrowserViewState:` the window state's "BrowserViewState"), its window
  `_setHasActiveAppearance:YES`, `display`, **framed exactly as the live browser's window**
  (`setFrame:display:NO animate:NO`), `drawCompletelyIntoBackBuffer`, then
  `CGWindowContextCreate(CGSDefaultConnectionForThread(), its windowNumber)` +
  `CGWindowContextCreateImage` = the picture (the window never comes on screen). The main window is
  redisplayed afterwards. `TBackupSnapshotWindowController` (0x10019b9be…0x10019b9ef):
  `isSnapshotImageBrowser` YES, `refreshBackupSnapshots` nothing.
- So each stacked window is a picture of the same folder inside that snapshot, same view, same
  size, same scroll/view state, active appearance; the front one is live.
- Refresh: `-refreshBackupSnapshots` (0x10019a097): new window state to the renderer, redraw,
  `BUUpdateGenericSnapshotImage(liveWindowNumber)` (the live window as the stand-in picture for
  snapshots not yet rendered), then `FUN_1001d9644`: drop the queue and
  `BUInvalidateAllSnapshotImages()`. Triggered by scrolling (`scrollContentChanged:` 0x100199d01,
  only while key, **2.0 s** later `delayedScrollContentChanged:`; the watched views: the scroll
  view's clip view, in column view every column's), and by a search's content change
  (`viewContentChanged` 0x10019a6f9).
- On exit the renderer stops (`FUN_1001bce7a` → `FUN_1001bcc48`: closes its window).

## 6. How a snapshot's folder is mapped

- `TTimeMachineTarget` (ObjC type string in `initWithState:targetPath:timeMachineTarget:`):
  `{bool; TFENode node; TUniString urlString; TSnapshot {TFENode fSnapshot; TDate fCompletionDate;
  TDate fStartDate}; TRef<CFTypeRef>}`; made by `FUN_1001dfd2c(node, snapshot)`, its URL string by
  `FUN_1001dfa1e(node)`:
  - the Computer → **"file:///?Computer"** (string at 0x100259b8c);
  - a search → the search scope's URL + **"?Search"** (0x100259ba5), or "file:///?Search";
  - else the node's file URL.
  `TSnapshot` (`FUN_1001e03a4(snapshotNode)`): **`fStartDate`** from the xattr
  `com.apple.backupd.SnapshotStartDate`, **`fCompletionDate`** from `com.apple.backupd.SnapshotCompletionDate`
  (`kCompletionDateXAttrKey`) on the snapshot folder; `FUN_1001e0002`/`FUN_1001dfeea`: `getxattr`
  of up to 100 bytes, **decimal ASCII microseconds since 1970** (`atoll`), → CFAbsoluteTime
  (`/1e6 − kCFAbsoluteTimeIntervalSince1970`). backupd writes them in `-[Snapshot
  stampAsCompleteWithType:]` (backupd 0x100026fcf: `gettimeofday` → sec·1e6 + usec as a string,
  NUL included in the size; also `kCSBackupSnapshotTypeXAttrKey`).
- The snapshots root: `_CSBackupSessionCopyBackupDirectory(session)` = `<disk>/Backups.backupdb/<computer>`
  (§3.4). The list of snapshots and their dates for the timeline are Backup.framework's
  (`BUCopySnapshotArray()`: dictionaries `SnapshotURL`, `SnapshotDate`, `SnapshotIsValid`); the
  Finder never reads "Latest".
- Original → snapshot: `-targetForOriginalNode:` (0x10019a876) → `FUN_1001df438(snapshot, node)` →
  CarbonCore **`_CSBackupFindMatchingSnapshotItemByPath(snapshotPath, originalPath, out)`** (0xd299b):
  the original's volume (mount-point prefix `_BSDVolumePathLength`, `_VolumeNameAndCopyUUID`; without
  one, "/Volumes/<name>/…" parsed) → `…WithHints` (0xd27ff): the snapshot's child folder whose
  xattr **`com.apple.backupd.SnapshotVolumeUUID`** equals the volume's UUID
  (`_FindChildFolderByMatchingXTAttr`), else the folder named like the volume; result
  `<snapshot>/<volume folder>/<path relative to the volume's mount point>` (1025 bytes max).
- Snapshot → original: `-originalNodeForNode:` (0x10019a23d) → `FUN_1001e0124` → `FUN_1001df244`:
  `_CSBackupPathIsBackupItem` = 3 (a snapshot root) → the Computer ("?Computer" string); 4 →
  CarbonCore **`_CSBackupFindMatchingOriginalItemByPath(path, out)`** (0xd164d): after
  "Backups.backupdb/" skip `<computer>/<snapshot>/`, take `<volume>/`; the mounted volume whose UUID
  matches that folder's (`_CopyMatchingVolumeUUID`, `getfsstat64`) gives its mount point, else
  "/Volumes/<volume>"; then the rest of the path appended. −50 (paramErr) if not a snapshot path.
- **`_CSBackupPathIsBackupItem(len, UniChar *path)`** (CarbonCore 0x475b1), used by
  `-isNodeSnapshotRoot:` (0x10019aa1b, == 3) and the mapping: the path must start with "/";
  an optional "Volumes/<name>/"; then "Backups.backupdb" (else **0**, not a backup item); ending
  there **1**; then "/<computer>", ending inside it or right after its "/" **2**; the next
  component must be at least 17 characters with '-' at its offsets 4, 7 and 10 (YYYY-MM-DD-HHMMSS)
  and end (or be followed by '/') at offset 17, else **5** (another item of the computer folder:
  "Latest", "….inProgress", a "…-2" name); the snapshot folder itself (optionally with a final '/')
  **3**; anything inside it **4**.
- `-isValidTargetForSnapshot:` (0x10019a772): searches and the Computer always; with a snapshot,
  the node must be inside it (`FUN_10017885a`); without one (Now), a backed-up kind of node.

## 7. Navigating inside Time Machine

- Opening a folder in 'peab' mode always retargets the front (Time Machine) window, never a new
  one (`-[TGlobalWindowController targetSelectOrCreateWindowWithOptions:inTarget:]` 0x10003b900).
- `-setTargetPath:withViewStyle:rebuildView:retarget:` (0x10019a312; the 3-argument form passes
  retarget:YES): retarget NO (from the activate callback): the view style kept, column view by
  `setColumnViewTargetPathMatchingBackupPath:rebuildView:` (0x10019ad17: the column path rebuilt
  inside the snapshot, `getPathSegmentMatchingPathNode:rootedAt:usingPathNode:`). Retarget YES
  (the user opened a folder): super, then unless `FUN_100020222(target)`: `DAT_10042b97c++`, redraw,
  `BUUpdateGenericSnapshotImage(windowNumber)`, **`BUChangeTimeMachineTarget(url of the new
  target)`** (`FUN_1001dc1f6`, which also resets the search date tracker `FUN_1001d9cce`),
  `DAT_10042b97c--`: the framework recomputes the timeline for the new folder.
- `browserSelectionChanged` (0x100199fea): remembers the user's selection
  (`_backupLastUserSelection`, unless the selection is being reset by a retarget) and
  `BUTimeMachineSetRestoreAllowed(selectedCountInFocus != 0)`.
- **NavigateForwardOrBackward** `FUN_1001daa0e(?, index, backward)`: only for a **search**
  (`DAT_10042c6a0`, the search tracker, else returns −1 = let the framework do it):
  `BUCopySnapshotArray()` (−2 if none) and `FUN_1001df5f6` (backward) / `FUN_1001df812`
  (forward): from the snapshot at `index`, the next snapshot whose `SnapshotDate` crosses one of
  the dates where results appear or disappear (the tracker's two sorted date vectors at `+0x10` and
  `+0x28`, filled from the query's `kCSBackupItemOldestSnapshotMDKey` / `NewestSnapshotMDKey`
  values, §10); invalid entries (`SnapshotIsValid` false or no date) count as dateless
  (`FUN_1001df1b8`). Returns the index to go to.

## 8. Get Info, Quick Look and the menus in Time Machine

- UI mode 'peab' (34 sites test `FUN_10000b888() == 'peab'`): the contextual menu
  (`buildContextMenu:…` 0x1000b6042), Go commands (`commonValidateGoToForNode:` 0x10003a0e7),
  Quick Look full screen (`quickLookShouldOpenFullscreenWithEvent:` 0x100086bb3), Get Info
  (`showInfoWindowForInfoNodes:browserWindowController:` 0x1001a61e0 — Info windows get the Time
  Machine level, `FUN_1001d9448`: `setLevel: _BUGetTimeMachineWindowLevel()`;
  `-positionWindow` 0x1001a78f3 places them 4 further right and a menu bar's height lower), the
  path menu (`shouldEnablePathMenuItem:forNode:` 0x100196709), sharing, privileges…
- Contextual commands in the browser (`TBrowserViewController(Actions)`):
  **"Restore “^0” to…" / "Restore ^0 Items to…"** (BU35, BU35.1; `validateRestoreTo:`
  0x1000b7105: only in the backup browser, up to 10 items, each `FUN_100176642`),
  `cmdRestoreTo:` (0x1001ec1ee) → `FUN_1001dc83e(chooseLocation = 1)` (§9);
  "Delete Backup" (N169.22, `cmdDeleteBackupSnapshot:` 0x1001ed146: the current snapshot folder),
  "Delete All Backups of “^0”" / "… of ^0 Items" (N169.21, N169.21.1, `cmdDeleteAllBackups:`
  0x1001ed1a0); confirmations DA4/DA6 ("Are you sure you want to permanently remove …"); before
  deleting `BUAboutToDeleteSnapshots(urls)` (`FUN_1001dd93a`); while the deletion's progress runs
  the browser goes to level `CGWindowLevelForKey(kCGStatusWindowLevelKey) + 2` and
  `_BUUnlockFrontProcess()` (task ctor 0x100137954 → `FUN_1001d9bca(1)`), back to
  `_BUGetTimeMachineWindowLevel()` + `_BULockFrontProcess()` after (dtors 0x100137864, 0x1001378d0),
  both gated by the default `_FXTimeMachineLockFrontProcess` (default YES).

## 9. Restore, Restore All, Restore to…, the conflicts

- Before the exit animation the framework asks for the items' pictures:
  **`-[TBrowserViewController restoreImages]`** (0x1001ed68a): for every visible *selected* node,
  `restoreImageForNode:outRect:` (0x1001ebd3f; the icon view's own 0x1001e96b5): the cell drawn into
  an NSImage of its frame (icon and title), the frame converted to window coordinates; an array of
  `{kNodeRestoreImagesKey: image, kNodeRestoreRectKey: NSValue rect}` (the flying icons).
- **`UpdateEntryWindow`** `FUN_1001da368(browser)` (also before the exit): the entry window
  (`DAT_10042c660`) takes the browser's size (`setFrame:display:animate:NO`, and the saved frame's
  size updated, `DAT_10042b978 = 0`), its target = the browser's target (searches: the scope),
  the selection mapped (`mapBackupSelectionToContainer:`), the scroll position; drawn into its
  back buffer; **returns its window number** (the window the framework flies back into). It
  also keeps **`nodesToRestore`** in `DAT_10042c6d0`.
- **`-nodesToRestore`** (0x10019b263): the selection (column view: the last column's); **if
  nothing is selected, every item of the target folder** — that is "Restore All" (the framework
  titles the button). Filtered by `FUN_1000b88ac`.
- **Restore** `FUN_1001dc572(browser, ?, destURL)`: the first node's parent (`FUN_100014974`) +
  "/" → `_CSBackupFindMatchingOriginalItemByPath` → its original location as the destination
  (else the URL given), then **`FUN_1001dc2e4(choose = 0, dest, nodes, parent)`**: a *pending
  restore* (`DAT_10042c700` pending, `DAT_10042c701` "choose a folder", `DAT_10042c708` the original
  path, `DAT_10042c710` the destination URL string, `DAT_10042c730` the nodes):
  - several items without a common parent (`FUN_100095bae`), or the destination being the
    Computer → choose a folder;
  - the destination missing → its original path kept (`BUCopyCurrentTargetOriginalPath`).
- **Restore to…** (`FUN_1001dc83e(1)`, from the menu): `nodesToRestore`, a pending restore with
  "choose", then `BUTimeMachineAction(1)` (leave).
- After the dismissal (§11) the pending restore runs **`FUN_1001ddace`** (deferred, `FUN_1001decfe`):
  - "choose" → `FUN_1001da1ea`: `TChooseFolderController` with "Select a folder for the restored
    items" (BU16), `run`, the chosen folder;
  - the destination exists → go;
  - it doesn't: an alert (Time Machine icon): "The enclosing folder for “^0” no longer exists at
    its original location. You can recreate the enclosing folder, or choose a new location for the
    restore." (BU37, one item) / the plural BU38; buttons **"Recreate Enclosing Folders"** (BU40, 1),
    **"Choose Location…"** (BU39, 2), **"Cancel"** (BU9, 3). Recreate: `FUN_1001dc8b9` makes each
    missing folder of the path (`FSCreateDirectoryUnicode`), Choose Location if that fails.
  - go → **`FUN_1001dae04(destNode, nodes)`**: the entry window retargeted to the destination
    folder, then **`FUN_100191294(dest, nodes, props, client, …, 'ask ', …, 6, restore = 1)`**: the
    Finder's copy task (`FUN_1000c1ac8`, 0x1f8 bytes) with its *restore* flag; an unwritable drop
    box asks NE55 / NE56 first. So the restore is an ordinary Finder copy (Copy window, progress)
    from the snapshot to the original folder.
- **Conflicts** (`FUN_100131e8c`): an item already there with the restore flags (0x800000, or
  0x2000000 for a system item) shows view kind **5 = `ProgressRenameConflictView`** (399×100):
  **"Keep Original"** (left, resolution 1 = skip), **"Keep Both"** (middle, 8), **"Replace"**
  (right, 2), "Apply to All" (`-[TTaskErrorOrConflictViewController setResolutionsForKind:]`
  0x100143beb); its text (`FUN_10013059e`): "The location to which you’re restoring “^0” already
  contains an item with the same name. Do you want to replace it with the one you’re restoring?"
  (PE1.1), "An item named “^0” with extension “^1” already exists in this location. Do you want to
  replace it with the one you’re restoring?" (PE32.1), with administrator wording PE68.1 / PE70.1,
  and for an item Mac OS X requires PE1.2 ("Do you want to replace your current copy of “^0” with
  the one you’re restoring? “^0” is required by Mac OS X. …"). (An ordinary copy's conflict is kind 6,
  ProgressReplaceView: Don't Replace 1 / Stop 5 / Replace 2.)
- **Keep Both**: the restore task's namer is `FUN_1001396ad` (`FUN_1000c286c`, chosen by the restore
  flag; the copy task's is `FUN_10013984f`): `TUniqueNamer` (`FUN_100175b42`) with the suffix
  **" (original)"** (CP6) instead of " copy" — one of the two items is renamed "name (original).ext"
  (by 10.6's behaviour the one already in place; the call site isn't traced).
- Some items unrestorable: "Some items can’t be restored.\nDo you want to continue?" (N37, 0x1001128ed).

## 10. Spotlight inside Time Machine

- The Finder's search field works in the browser: the target is a query node, its URL
  "<scope>?Search". The query (`FUN_100128896`): the search's RawQuery + **`&& (_kMDItemTimeMachinePath
  = "<original scope path>/*")`**, value-list attributes `kCSBackupItemOldestSnapshotMDKey` and
  `kCSBackupItemNewestSnapshotMDKey`, `_MDQueryCreateFromDictionary`, search scope =
  `<backup disk>/Backups.backupdb` (`FUN_100128274`: the snapshots root's volume + kBackupsTopLevelFolder).
  Each result carries the oldest and newest snapshot holding it: the tracker `DAT_10042c6a0`
  (`FUN_10012906e`, rebuilt when the query changes, `FUN_100128e9c`) and the navigation of §7.
- On activation the query's scope moves into that snapshot (§4); `-updateSearchTarget:`
  (0x10019a933) sets it to the current snapshot URL's path; the snapshots' pictures of a search are
  the framework's generic ones (§5).

## 11. Leaving Time Machine: how the session returns

**`TimeMachineDismissed`** `FUN_1001daab0(browser)`:
1. Sidebar out of its Time Machine state (`FUN_100174bc6(…, false)`); image renderer stopped.
2. The four main callbacks unregistered (`BURegister…(browser, NULL)`).
3. The **entry window shown again**: `makeKeyAndOrderFront`, `_hideAttachedWindows:NO` (its sheets back).
4. `FUN_1001d9482`: screen updates off, **UI mode 'full'**, updates on. `FUN_1001d94d0`: every Get
   Info window back to the normal level (`CGWindowLevelForKey(kCGNormalWindowLevelKey)`).
5. `-[TBackupBrowserWindowController backupDismissed]` (0x100199f85): the node browser cleared,
   `DAT_10042b970 = 0`, `closeWindow`.
6. Unless the saved frame is empty: the entry window back to its **original target** (if one was
   saved, `setTargetPath:withViewStyle:`) and, if it had been shrunk (`DAT_10042b978`), its
   **original frame through the animator** (`[[window animator] setFrame:display:YES]`).
7. A pending restore (§9) runs after (`FUN_1001decfe(0, FUN_1001ddace, copy)`); the pending state cleared.

Leaving is always the framework's decision: the browser's Cancel/close/Escape and Restore to… only
call `BUTimeMachineAction(1)`.

## 12. The rest of the screen

- The Finder's part: the entry window hidden while the browser stands in for it; Quick Look and
  Cover Flow stop playing; Get Info windows lifted to the Time Machine level; the Finder's other
  windows and the desktop window untouched (the framework covers them); while a deletion's progress
  shows, the front-process lock is lifted. Menu bar: the Finder reads `_FXPeabodyMenuBar` but does
  nothing with it; its menus validate in 'peab' mode.
- Everything else is Backup.framework's (its imports, for the other agent's notes):
  `CGDisplayCapture`/`CGDisplayCaptureWithOptions`/`CGDisplayRelease` (the displays captured),
  `CGSCaptureWindowsContentsToRect`, `CGSGetOnScreenWindowList`/`CGSGetWindowBounds`/`CGSGetWindowLevel`
  (pictures of the screen's windows), `SetSystemUIMode` (menu bar and Dock hidden),
  `CoreDockGetAutoHideEnabled`, `CoreDockDisableExposeKeysIfNecessary` (Exposé keys off),
  `_kLSPermittedFrontASNsArrayKey` (`_BULockFrontProcess`: only the host may come forward),
  `CGDisplay…TransferByFormula` / `CGDisplayRestoreColorSyncSettings` (the fade),
  `CVDisplayLink*` (the animation), `_BUGetTimeMachineWindowLevel`.

## 13. Browse Other Time Machine Disks: the chooser

- `TBackup::ChooseAndEnterBackupStore` (0x1001d973c, line 585): `GetFrontProcess(&psn)`;
  `[[TBackupVolumeBrowserWindowController backupVolumeBrowserWindowController]` (0x10012ad9f, a
  singleton `DAT_10042d628`, `initWithWindowNibName:@"BackupVolumeBrowserWindow"`)
  `setEntryPSN: psn]`, `showWindow:`, `FUN_10009bdba` (Finder to front).
- `BackupVolumeBrowserWindow.nib`: window "**Choose Time Machine Disk to Browse**", content 372×374
  (min 336×352); bold text "These disks contain Time Machine backups. Select the disk you want to use
  with Time Machine:" (23,303 332×51); a bordered table (20,68 332×208) with one column "vol";
  buttons "**Use Selected Disk**" (207,12 151×32, Return, `cmdOK:`, `_okBtn`) and "**Cancel**"
  (102,12 105×32, Escape, `cmdCancel:`).
- `windowDidLoad` (0x10012b7a5): OK enabled only with a selection; the nib's columns removed;
  `TBackupVolumeTableViewController` (init 0x10012b2af: sorted by `displayName`,
  `caseSensitiveSystemLocaleCompare:`, `refresh`). `windowShouldZoom:toFrame:` (0x10012b456) sizes
  the window to the rows.
- The rows (`-refresh` 0x10012bdda): every **computer backup folder** (`<volume>/Backups.backupdb/<computer>`)
  on the mounted volumes — the list at `FUN_1000133c4()+0x80`, filled by the volume watcher
  (`FUN_100013dfa`: a child appearing under a volume's Backups.backupdb; `FUN_100014390` tests
  `<mount point>/Backups.backupdb`, "/Backups.backupdb" on the boot volume, by `lstat`), less those
  failing `FUN_100045820`; kept current by `computerBackupAdded:` / `computerBackupDeleted:`
  (`FUN_10012b712`). Each `TBackupVolumeTableViewItem` (`initWithNode:` 0x10012bb3a):
  **displayName "“^0” on “^1”"** (BU41: the computer folder's name, its volume's name), icon the
  volume's, drawn at 32×32 (`tableView:willDisplayCell:…` 0x10012af34). Double-click = OK.
- `cmdOK:` (0x10012baa6): `close`; with a selection: isEntering set ("Entering, setting isEntering
  to true", BackupVolumeBrowser.mm line 896), **`TBackup::EnterBackupWithStore(computerFolder,
  entryPSN)`** (0x1001dbb6a): path → `CFURLCreateWithFileSystemPath(…, POSIX, isDir)` →
  `TBackup::CreateSessionForURL` (0x1001d93d4): `_CSBackupCreateBackupSession(NULL,
  CFSTR("com.apple.finder"))` + `_CSBackupSetBackupSessionTarget(session, url)` →
  `EnterBackupSession(session, &psn)` (§3: Preflight, mount, StartTimeMachine; browsing another
  disk the start target is the Computer, §3.6). `cmdCancel:` (0x10012b679) only closes ("Canceling,
  isEntering: %s").
- The same chooser opens from "Use Previous Disk…" (Preflight, §3.3) and from the "disk can't be
  found" alert of `TBackup::BackupStoreNotAvailable` (0x1001de02a, line 1097: "The current Time
  Machine backup disk can‘t be found." BU31 / BU32; "Time Machine Preferences…" BU34 = 1, Cancel 2,
  "Use Previous Disk…" BU33 = 3; without any other disk: "Your Time Machine backup disk can’t be
  found." BU2.1 with "Set Up Time Machine" BU3 / Cancel, or "An unexpected error occurred (error code
  ^0)." NE2 for a mount error).

## 14. The handshake

`FUN_1001da7a3` (on `com.apple.backup.BackupRequestHandshakeNotification`, object
`com.apple.backup.BackupObject`): replies `com.apple.backup.BackupHandshakeNotification` (same
object, distributed) with `{ConnectionID: the snapshots root's URL string, pid: getpid(),
MaxWindowSize: CGRect {0, 0, 1000, 700}}` (line 114, "About to post … w. BackupObject, pid: %d").

## 15. Host API used (as the Finder calls it)

| Call | Arguments (from the Finder's call sites) |
|---|---|
| `__BUInitializeTimeMachineHostProcess()` | — |
| `BUStartTimeMachine(int windowNumber, CFURLRef target, 0)` → Boolean | called at 0x1001dd726 |
| `BUActivatedSnapshot(int wid, CFURLRef)`, `BUDeactivatedSnapshot(int wid, CFURLRef)` | §4 |
| `_BUUpdateSnapshotImage(CGImageRef, CFURLRef)`, `BUUpdateGenericSnapshotImage(int wid)`, `BUInvalidateAllSnapshotImages()` | §5 |
| `BUChangeTimeMachineTarget(CFURLRef)` | §7 |
| `BUTimeMachineSetRestoreAllowed(Boolean)`, `BUTimeMachineAction(1)` (leave) | §4, §11 |
| `BUCopyCurrentTargetSnapshotURL()`, `BUCopyCurrentTargetOriginalPath(CFURLRef)`, `BUCopySnapshotArray()` | §4, §9, §7 |
| `BUAboutToDeleteSnapshots(CFArrayRef urls)` | §8 |
| `_BUGetTimeMachineWindowLevel()`, `_BULockFrontProcess()`, `_BUUnlockFrontProcess()` | §8 |
| The eight `BURegister…(refcon, callback)` | §3 step 12 |

## 16. The shell's backupd and what the Finder expects

The shell's port (`shell/src/backupd/main.cpp` `backup()`): `<BackupPath>/Backups.backupdb/<hostname
up to the first dot>/<YYYY-MM-DD-HHMMSS>/<label of the volume holding $HOME, else "Macintosh HD">/<$HOME
as an absolute path>/…`, made as `<date>.inProgress/<UUID>` then renamed, "Latest" a relative symlink.

| The Finder / CarbonCore expects | The port | To do for the host |
|---|---|---|
| `Backups.backupdb` at a volume's root for the chooser (§13); the default disk from the session (`_CSBackupSessionCopyBackupDirectory`) | under BackupPath, any folder | the default store = timemachine.conf's BackupPath; the chooser may list `Backups.backupdb/*` at mount points plus BackupPath |
| `<snapshot>/<volume folder>/<path relative to that volume's mount point>`; the volume folder found by xattr `com.apple.backupd.SnapshotVolumeUUID`, else by name | the volume label, then the **absolute** $HOME path | equal when $HOME is on "/"; with /home a separate mount, the Mac rule would give `<label>/thomas`: the host must use the port's rule (absolute path) or the port the Mac's — pick one; no UUID xattr written (name matching only) |
| snapshot names exactly 17 characters "YYYY-MM-DD-HHMMSS" (`_CSBackupPathIsBackupItem` 3/4) | the same, but "-2", "-3"… on collisions | such snapshots would be type 5 (not a snapshot) for CarbonCore's rule |
| dates from xattrs `com.apple.backupd.SnapshotStartDate` / `…SnapshotCompletionDate` (decimal µs since 1970) | not written | write `user.com.apple.backupd.SnapshotCompletionDate`/`StartDate` (Linux `user.` namespace), or take the date from the folder name |
| "Latest" | written | unused by the Finder (the framework's snapshot list decides) |
| the restore = a Finder copy from the snapshot to the original folder | snapshots are plain folders with hard-linked files | a Linux copy works; restored files are new inodes (not links) |

Since done in the port (docs/reverse-status.md sections 37 and 39): backupd writes the Mac's layout
(the path relative to the mount point), 17-character names only, and the three attributes as
`user.com.apple.backupd.*`; Time Machine.app reads the BackupPath store and both layouts.

## 17. Not resolved

- What `FUN_100045820` tests exactly (node flags 0x40004, 0x40000, 0x8c1000000 and a parent's
  0x800000: probably "not on a local, backed-up volume"); which disks the chooser leaves out.
- `FUN_100020222(target)` in `setTargetPath:…retarget:` (when a retarget doesn't tell the framework).
- Which of the two items "Keep Both" renames " (original)" (the namer is known, not its call site).
- The second argument of the Restore callback (unused by the Finder) and `BUTimeMachineAction`'s
  other values; the navigation callback's first argument.
- The mount task's thread body beyond `FUN_10002a17e` (network and sparse-bundle stores), and its
  progress window's look.
- How Mail/iPhoto/Address Book answer 'peab'/'BROW' (Address Book links Backup.framework; not read).
- The Finder's "Back Up Selected Folder"/"Time Machine Backup" progress window
  (`TBackupProgressWindowController`, `BackupProgressWindow.nib`, BU22-BU46) is a separate feature,
  not read beyond its strings.
