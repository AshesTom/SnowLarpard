# Mail (4.x, 10.6): what was recovered

First pass over Snow Leopard's Mail, from your own DVD. No Apple file is in the repository: the
material below lives in `.local/media/re/` (ignored by git) and is rebuilt from the DVD.

## Where the files come from

| File | Source | Notes |
|---|---|---|
| `Mail.app` (complete) | `Mail.pkg` on the DVD's HFS+ partition | `extract.py` only takes the icons and `Info.plist`; the whole package was unpacked into `.local/media/re/mail/` |
| `Mail.x86_64` (4.2 MB) | x86_64 slice of `Mail.app/Contents/MacOS/Mail` (universal x86_64 + i386) | `__TEXT` **in the clear** (no `SG_PROTECTED_VERSION_1`, unlike the Dock and the Finder) |
| `Message.x86_64` (5.1 MB) | x86_64 slice of `Message.framework/Versions/B/Message` (already in the assets) | in the clear |

Mail links against Message.framework (1075), AddressBook, CalendarStore (To Dos), WebKit (reading and
composing), Quartz/QTKit, IMCore/InstantMessage (presence, "Reply With iChat"), DataDetectors,
ExchangeWebServices (private), iLifeMediaBrowser (Photo Browser), Backup (Time Machine) and
SecurityInterface (S/MIME).

## Tools and limits of this pass

- **REA 6.8.0**, resource route (`open_bundle_resources` + `decode_interface_builder`): every nib in
  `English.lproj` (82 documents) decoded into an object, outlet and action graph, exported to
  `.local/media/re/mail/evidence-ib.json`. The bundle had to be split in two (`en1/`, `en2/`):
  the decoder caps a pass at 64 nibs and goes through the localisations alphabetically.
- **REA 6.9.0 with Ghidra 12.2** for the code (sections 4 to 7). REA 6.8.0 refused Ghidra 12.2;
  6.9.0 accepts any 12.x from 12.1 on.
- The Objective-C metadata comes from `llvm-otool -ov` (classes, superclasses, ivars, methods with
  their IMP addresses), turned into JSON: `mail/Mail.classes.json`, `mail/Message.classes.json`.

## 1. Architecture (Objective-C metadata)

| Binary | Classes | Methods |
|---|---|---|
| Mail | 343 | 7 562 |
| Message.framework | 343 | 7 004 |

**The split**: Mail is the interface. Message.framework holds the whole mail engine (accounts,
protocols, local store, rules, MIME), and Mail only draws it.

### Mail: the interface

| Class | Superclass | Role (from its methods) |
|---|---|---|
| `MailApp` | `NSApplication` | the application: termination with handlers, automatic fetch (`autoFetchMail:`, `resetAutoFetchTimer`), the `message:`, `mailto:` and `x-mail-*` URLs, opening `.emlx`/`.mbox` files, the context menus, sync (`syncer`, `mailSyncBundle`) |
| `MessageViewer` | `NSResponder` | the viewer window (300 methods): message transfer with undo, junk mail, search, the list columns, mailbox/list split, the title, per-viewer defaults (`_viewerDefaultsKeyFormats`, `writeDefaultsToArray:`) |
| `SingleMessageViewer` → `MessageBrowser` | `MessageViewer` | a window for one message, then the browser |
| `TableViewManager` | `NSObject` | the message list: cells, bold for unread, colours, dragging, deletion with the next message selected, type-select |
| `MessageMall` | `NSObject` | the model behind the list: the messages of one or more stores, filtering, sorting, **threading** (`_deduceThreadHierarchyForMessages:`, opening and closing threads), all done in the background |
| `MailboxesOutlineViewController`, `MailboxesController`, `MailboxListCell` | | the mailbox sidebar: sections, unread counts, progress per mailbox, small/large icons, drag and drop |
| `MessageContentController`, `MessageWebHTMLView`, `ThreadDisplay` | | the message pane (WebKit) and the thread view |
| `ComposeBackEnd`, `DocumentEditor` → `MailDocumentEditor`, `HeadersEditor`, `ComposeHeaderView`, `WebViewEditor` → `MailWebViewEditor`, `EditingMessageWebView` | | the compose window: model / window / headers / WebKit editor |
| `AddressTextField`, `AddressAttachment`, `AddressAttachmentCell`, `CompletionController` | | address fields with "atoms" and completion |
| `Stationery`, `StationerySelector` | | stationery |
| `NoteDocumentEditor`, `NoteWebViewEditor`, `ToDoListController`, `ToDoAlarmCell` | | Notes and To Dos (10.5–10.6) |
| `AccountSetupAssistant` (`Assistant`), `AccountAutoconfigurator` | | account creation and automatic configuration |
| `*Preferences` (9 classes) | `NSPreferencesModule` | the preference panes: General, Accounts, RSS, Junk Mail, Fonts & Colors, Viewing, Composing, Signatures, Rules |
| `EmailImporter` and its 5 `FilesystemEmailImporter` subclasses | `Importer` | Import Mailboxes |

### Message.framework: the engine

| Area | Classes |
|---|---|
| Accounts | `Account` → `MailAccount` → `LocalAccount` (→ `RSSAccount`), `POPAccount`, `RemoteStoreAccount` → `IMAPAccount`, `EWSAccount`; sending: `DeliveryAccount`, `EWSDeliveryAccount` |
| IMAP | `IMAPConnection`, `IMAPConnectionPool`, `IMAPGateway`, `IMAPMailboxSyncEngine`, `IMAPResponse`, the `IMAPClient*Operation` family (~40 classes) |
| POP / SMTP / network | `POP3Connection`, `Connection`, `_NSSocket`, `SASLAuthenticator`, `AuthScheme` |
| Exchange | `EWSGateway`, `EWSMessage`, `EWSRequestOperation` / `EWSResponseOperation` (~25 classes) |
| Local store | `MessageStore` → `LibraryStore`, `NeXTMbox`; `RemoteStore` → `LibraryIMAPStore`; `MailboxUid` (a mailbox's identity), `LibraryUpgrader` |
| Messages | `Message` → `LibraryMessage`, `RemoteMessage` → `EWSMessage`, `MessageThread`; `MimePart`, `MessageAttachment` |
| Rules, search | `MessageRule`, `Criterion` → `MessageCriterion`, `SmartMailboxUnreadCountManager` |
| Other | `ActivityMonitor` / `ActivityAggregate` (the Activity window), `ToDo`, `ToDoAlarm`, `CalendarStoreInterchange`, `RSSInterchange`, `ParentalControlManager`, `Defaults` |

## 2. The interface (nibs)

| Nib | Contents |
|---|---|
| `MailViewer.nib` | the main menu (owner `MailApp`), the Dock menu, `MailboxesController` |
| `MessageViewer.nib` + `MessageViewerContents.nib` | the viewer window and its contents (sidebar, list, message pane) |
| `MessageViewerToDoContents.nib` | the same viewer for To Dos |
| `MessageWindow.nib`, `ThreadView.nib` | one message in its own window, the thread view |
| `WebMessageEditor.nib` | the compose window (317 objects, 110 connections) |
| `NoteEditor.nib`, `EmbeddedNoteEditor.nib` | Notes |
| `*Preferences.nib`, `MailSorter.nib` (Rules), `SignaturePreferences.nib`, `SMTPSettings.nib` | Preferences |
| `AccountSetupAssistant.nib`, `AssistantWindow.nib` | account creation |
| `ActivityPanel.nib`, `ActivityViewEntry.nib`, `ConnectionDoctor.nib` | Window menu |
| `*Banner.nib` (Junk, Load Images, Certificate, Calendar, Parent/Child) | the bars over a message |

**The main menu** (`MailViewer.nib`) was read in full, with each item's action, tag and shortcut:
Mail, File, Edit, View, Mailbox, Message, Format, Window, Help. A few useful points for a port:

- the `selectMailbox:` tags 1–8 (⌘1 to ⌘8) give Inbox, Outbox, Drafts, Sent, Trash, Junk, Notes,
  To Do; `setSpecialMailbox:` 0–3 sets Drafts, Sent, Trash, Junk;
- `sortByTagOfSender:` sorts by a column tag: Subject 2, Date Received 3, Number 4, From 5,
  Attachments 6, To 8, Flags 9, Date Sent 10, Mailbox 11, Color 12, Buddy 13, Size 14;
- `setMessagePriority:` uses 1 (high), 3 (normal), 5 (low), as the `X-Priority` header does;
- the shortcuts include ⇧⌘N Get New Mail, ⇧⌘D Send, ⌘R / ⇧⌘R Reply / Reply All, ⇧⌘F Forward,
  ⇧⌘⌫ Erase Deleted Items, ⌥⌘J Erase Junk Mail, ⇧⌘U Mark as Read, ⇧⌘L Flag, ⇧⌘J Junk.

The complete dump (each nib's objects, outlets and actions, then the menu tree) is in
`.local/media/re/mail/nibs.txt`.

**The toolbar** is not in the nibs: see section 4.

## 3. Data and defaults (Message.framework resources)

- **`tables.sql`**: the schema of `~/Library/Mail/Envelope Index` (SQLite), 70 lines: tables
  `messages`, `subjects`, `addresses`, `recipients`, `threads`, `mailboxes`, `attachments`, `feeds`,
  `ews_folders`, `todos` (+ `alarms`, `calendars`, `todo_notes`, `todos_server_snapshot`,
  `todos_deleted_log`), `associations`, `events`, `properties`, and the `after_delete_message` trigger.
  A Linux port can create the same database and read a Mac's `~/Library/Mail` as it is.
- **`Message.defaults`**: the registration domain (fetch every `PollTime` = 5 minutes, RSS every
  30, `LineLength` 76, `SendFormat` MIME, `MailSortOrder` Date, quoting of replies, Windows-friendly
  attachments...).
- `Colors.plist`, `urlPrefixes.plist`, `ISPAccountsXQuery.xml` (the providers' automatic setup),
  `MailSync` (the sync helper, universal), `Syncer.syncschema`, `RSSFeeds.mom` (Core Data).

## 4. The toolbars (`*.toolbar` files)

`-[MessageViewer(MessageViewerToolbar) setupToolbar]` (Mail 0x10001ba1a) creates a `MailToolbar`
with the identifier `MainWindow`, sets the viewer as its delegate, turns on autosave and
customization, and takes its items from `configureToolbarItems`. The items are **data**: the
bundle's `MainWindow.toolbar`, an old-style (NeXT) plist with a `default set` (identifiers in
order) and a `toolbar array` (the item dictionaries). The other windows do the same with
`MessageBrowser`, `SingleMessageViewer`, `ComposeWindow_NewMessage`,
`ComposeWindow_ReplyOrForward` and `NoteEditor`.

Each item has `identifier` (the action's selector, or a group name), `tag`, `image` (a `TB_*.png`
from the bundle, 43 of them), `title` / `altTitle` / `help` (keys into the strings tables), and
optionally `target` (`MessageViewer`, `DocumentEditor`, `HeadersEditor`, `WebViewEditor`), `segments`
(a segmented group) or `borderless` with `inactiveImage` / `pressedImage`.

`toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar:` (0x10001c0da) does the following:

- the flexible space is AppKit's standard item;
- **tag 100** is the search field: a `MessageViewerSearchField` bound to `searchPhrase`, updated
  continuously, sized by `_minimumSizeForSearchFieldToolbarItem` / `_maximumSize…`, with a menu
  form that calls `searchIndex:`;
- anything else goes through `NSToolbar segmentedOrBorderlessItemForItemIdentifier:withDictionary:`,
  and each segment's target is the viewer when it responds to the selector
  (`configureSegmentedItem:withDictionary:forSegment:`, 0x10001d80d);
- the item's tag is the dictionary's `tag`, and validation goes by these tags (the menu items use
  the same ones: 70 Delete, 71 Reply, 72 Reply All, 73 Forward, 74 New Message, 76 Get Mail...).

The viewer's default set is: Get Mail, flexible space, Delete/Junk (group 300),
Reply/Reply All/Forward (group 301), New Message, Note/To Do (group 305), flexible space, Search.
A port can read the DVD's `.toolbar` files at run time, as Preview reads `CommonToolbar.proplist`
(`shell/src/preview/plist.h` already parses this syntax).

## 5. The message list's columns

The column identifiers are numbers as strings, **the same numbers as the `sortByTagOfSender:` tags**
(`TableViewManager _columnWithIdentifierTag:` compares `identifier.integerValue`). From
`MessageViewerContents.nib`, the default width / minimum / maximum:

| Id | Column | Width | Min | Max | Cell (from `_setupColumnsForTableView`, 0x10000e811) |
|---|---|---|---|---|---|
| 0 | read status (no title) | 40 | — | 300 | rollover cell, centred, action `showFollowupsToMessage:`, header icon |
| 9 | flags | 20 | 20 | 20 | image cell, header icon |
| 15 | thread | 18 | 18 | 18 | rollover cell, action `toggleThread:`, header icon |
| 4 | # (number) | 42 | 25 | 750 | widths computed from sample numbers in the list's font |
| 13 | buddy | 20 | 20 | 20 | image cell, header icon |
| 5 | From | 75 | 75 | 2000 | truncated at the end |
| 8 | To | 75 | 75 | 2000 | truncated at the end |
| 2 | Subject | 100 | 100 | 15000 | truncated at the end |
| 17 | Last Saved | 50 | 10 | — | `DateCell` |
| 10 | Date Sent | 72 | 50 | 2000 | `DateCell`, min = the cell's minimum + 10 |
| 3 | Date Received | 81 | 81 | 2000 | `DateCell`, min = the cell's minimum + 10 |
| 16 | Date Viewed | 97 | 81 | 2000 | `DateCell` |
| 6 | Attachments ("Contents") | 65 | 20 | 10000 | image cell, centred, header icon |
| 14 | Size | 65 | 28 | 10000 | |
| 7 | Rank | 93 | 44 | 1000 | `RankCell` (search relevance) |
| 11 | Mailbox | 100 | 46.3 | 1000 | |
| 18 | Author | 75 | 75 | 2000 | truncated at the end (RSS) |
| 19 | Note | 64 | 10 | — | rollover cell, header icon |

Every column's data cell draws no background (the list draws its own stripes and colours). Which
columns are visible, their order and widths are per-viewer preferences, kept by a shared
`MessageListColumnManager` (`setupColumnManagerFromDictionary:`).

The window (`MessageViewer.nib`): 778 × 482 at first, the mailbox pane 137 px wide on the left.
In `MessageViewerContents.nib`, the list sits above the message (`MessageHeaderView` 45 px high
over a `MessageTextView`), and the mailbox pane ends with a 23 px `MailboxPaneBottomView` (the
+ / action buttons) holding a 15 px `VerticalSplitViewHandle`.

## 6. Threading (`MessageMall`)

`_deduceThreadHierarchyForMessages:firstTime:` (0x100039809), run in the background:

1. Two preferences: `UseReferencesWhenThreading` (off unless set) and `UseSubjectsWhenThreading`
   (on unless set to NO).
2. Each message gets a `MessageThreadingData` (parent, replies, thread). Messages are indexed by a
   digest of their `Message-ID` and, for replies, by a digest of `In-Reply-To` (or, with
   references on, of each `References` entry).
3. The parent is the message whose `Message-ID` matches the `In-Reply-To` (or the first
   `References` entry found). A message that arrives later adopts the replies waiting for it in the
   `In-Reply-To` index.
4. By subject: when `shouldUseSubjectForThreading`, the subject without its Re:/Fwd: prefixes
   keys a table holding the **earliest sent** message with that subject. A later message with no
   parent from the headers hangs under it; an earlier one takes its place and becomes the others'
   parent.
5. Batches of 50 messages per autorelease pool. The work stops as soon as a newer filter series
   has started (the user changed mailbox or search).

## 7. The `.emlx` files (Message.framework)

`+[Library _writeEmlxFile:forMessage:withBodyData:plistData:error:]` (Message 0xc69d7):

- open with `O_WRONLY | O_CREAT | O_TRUNC`, mode 0666; if that fails, create the parent folders
  and try again; turn the cache off (`fcntl F_NOCACHE`);
- write the **body's length in decimal followed by `\n`**, then the **raw RFC 822 message**, then
  the **XML plist** of the metadata, with nothing between them.

Reading (`_messageDataAtPath:`, 0xd0d14) maps the file and finds the message's range from that
prefix. Keys found in the framework's strings, as in the plist: `flags`, `date-sent`, `subject`,
`sender`, `to`, `remote-id`, `original-mailbox`, `partial`.

The `flags` integer (`-[Message messageFlags]`): bits 10–15 are the **number of attachments**
(0x3F and 0x3D mean "not known yet", counted as 0; `numberOfAttachments`, 0x46a15); bits 16–18 are
the **priority** (0 = not known yet: the cached header values are reloaded; `priority`, 0x46922).

The other bits come from `_IMAPMessageFlagsTable` (Message `__DATA,__const` 0x341900), which
`_MFMessageFlagsFromArray` (0x700c5) walks to turn IMAP flags into this integer:

| Bit | Meaning | IMAP flag |
|---|---|---|
| 0x1 | read | `\Seen` |
| 0x2 | deleted | `\Deleted` |
| 0x4 | answered | `\Answered` |
| 0x10 | flagged | `\Flagged` |
| 0x40 | draft | `\Draft` |
| 0x100 | forwarded | `$Forwarded`, `Forwarded` |
| 0x200 | redirected | `Redirected` |
| 0xFC00 | number of attachments (above) | — |
| 0x70000 | priority (above) | — |
| 0x1000000 | junk | `$Junk`, `Junk` |
| 0x2000000 | not junk | `$NotJunk`, `NotJunk` |
| 0x20000000 | junk recorded (already fed to the junk filter) | `JunkRecorded` |
| 0x40000000 | the message's colour applies to its text, not its background (section 10) | — |

The table's third field is a second, separate set of bits for each IMAP flag (`\Seen` 0x20,
`\Deleted` 0x10...), apparently for the server's side; its use is not traced. Bits 0x8, 0x20 and
0x80 are not in the table.

## 8. The folders in `~/Library/Mail` (Message.framework)

- **The root**: `Defaults unresolvedSupportDirectory` (0x30f8) takes the user's Library folder
  (`NSSearchPathForDirectoriesInDomains`, else `~/Library`) and adds `Mail`, then resolves symbolic
  links and aliases. In 10.6 it is `~/Library/Mail` itself: the `V2` level came with 10.7.
- **An account's folder**: `defaultPathNameForAccountWithHostname:username:` (0x18d337) formats
  `%@-%@@%@` with the account type, the user name and the host, so `IMAP-me@mail.example.com`.
  Local mailboxes live in `Mailboxes`, To Dos in `ToDos.mbox`.
- **A mailbox**: the folder names from the account down, joined (`MailboxUid
  _stringByAppendingPathComponentsForAccount:root:prefix:separator:pathExtension:`, 0xf47d), and
  the account type's extension on the last one: `mbox` locally, and `imapmbox`, `ewsmbox`,
  `rssmbox`, `pcmbox`, `caldavmbox` (from the framework's strings) for the others.
- **A message**: `<mailbox>/Messages/` (`_dataDirectoryForMailbox`, 0x8057c) plus
  `_filenameForMessageID` (0x811e6): `<id>.emlx` for a whole message, `<id>.partial.emlx` when
  only part of it was downloaded, `<id>.empty.emlx` for a placeholder. `<id>` is the library message's
  `messageID` (`Library dataPathForMessage:type:`, 0x8199e), presumably its row in `Envelope Index`.
- **Attachments taken out of partial messages**: `<mailbox>/Attachments/<id>/`
  (`attachmentsDirectoryForLibraryID:mailboxID:`, 0xd3cd0).
- The framework's strings also name an `Info.plist` and a `.mboxCache.plist`; where they are
  written is not traced.

## 9. The mailbox sidebar (Mail)

- **Rows**: the mailbox list font's size + 9, at least 20 px (`_setOutlineViewRowHeight`,
  0x10001431d). With large icons, the first level's rows are at least 33 px
  (`outlineView:heightOfRowByItem:`, 0x100013610).
- **The font**: `+[NSFont(MailAdditions) mailboxListFont]` (0x100009dee) takes the font from the
  user's defaults, else the **system font at 11 pt**, so rows are 20 px by default.
- **Sections**: every top-level item is drawn as a source-list group row (`outlineView:isGroupItem:`
  is `_isTopLevelItem:`). Their titles (`outlineView:objectValueForTableColumn:byItem:`,
  0x10002371f): the localized `MAILBOXES_SECTION_TITLE` ("Mailboxes"), `REMINDERS_SECTION_TITLE`
  ("Reminders"), `SMART_MAILBOXES_SECTION_TITLE` ("Smart Mailboxes"); accounts (RSS among them) and
  mailboxes show their `displayName`.
- **Each row** (`_configureCell:forItem:`, 0x100023eeb): no icon on the section rows; small icons
  from the second level down, and on the first level unless Use Large Mailbox Icons is on; grey text
  when the account is offline; a progress indicator while something runs on the mailbox (determinate
  when the activities report a fraction, refreshed every 0.25 s); the RSS feeds' arrow; then the
  unread badge.
- **The icons** (`+[MailboxesController iconForMailboxUid:smallSize:]`, 0x1000247c7): the mailbox's
  type picks a slot in a 32-entry table of image names (Mail `__data` 0x1003e7640):

| Mailbox | Small | Large |
|---|---|---|
| Inbox | `InMailbox` | `InMailboxLarge` |
| Drafts | `DraftsMailbox` | `DraftsMailboxLarge` |
| Sent | `SentMailbox` | `OutMailboxLarge` |
| Outbox | `ToBeDeliveredMailbox` | `ToBeDeliveredMailboxLarge` |
| Trash | `TrashMailbox` | `TrashMailboxLarge` |
| Junk | `JunkMailbox` | `junkmail` |
| Notes | `NotesMailbox` | `NotesMailboxLarge` |
| To Do | `ToDoMailbox` | `ToDoMailboxLarge` |
| RSS feed | `RSSMailbox` | `RSSMailboxLarge` |
| Account (IMAP, POP, Exchange) | `GenericAccount` | `GenericAccountLarge` |
| Any other mailbox | `NormalMailbox` → the Finder's folder icon (`'fldr'`) | the same at 32 px |
| Smart mailbox | `SmartFolderSmall` → the saved-search icon | `SmartFolderLarge`, 32 px |
| "On My Mac" | `LocalAccount` → `NSImageNameComputer` | 32 px |
| MobileMe account | `AOSAccount` → `NSImageNameMobileMe` | 32 px |

  The names are images in Mail's `Resources` (`.png` or `.tiff`), loaded with `imageNamed:`. The
  last four have no file: the code makes them from system icons, at 16 px small and 32 px large
  (which name goes with which system icon is read from the code's order of comparisons and the
  names, not traced one by one). Types 100–107 (probably the
  aggregate mailboxes at the top of MAILBOXES, over all accounts) use the same slots.
- **The unread badge** (`MailboxListCell`):
  - text: the count, localized, in **Helvetica Bold** at the mailbox list font's size, centred;
  - width: the text's width (or "99"'s when the count is under 10) + half its height, rounded up,
    + 1 (`badgeWidth`, 0x100025e9f);
  - shape: a rounded rectangle whose radius is half the text's height (`drawBadgeCenteredInRect:`,
    0x10002756e);
  - colours (`badgeColor` 0x100027873, `badgeTextColor` 0x100026071). Control tint 1 is
    `NSBlueControlTint`, the other branch Graphite:

| State | Badge | Text |
|---|---|---|
| row not selected, window active, Blue | (133, 151, 187) | white |
| row not selected, window active, Graphite | (137, 152, 169) | white |
| row not selected, window inactive | (154, 154, 154) | white |
| row selected | white | focused list: Blue (56, 115, 191), Graphite (97, 117, 139); list not focused: Blue (136, 153, 188), Graphite (148, 162, 178); window inactive: (159, 159, 159) |

## 10. The message list's drawing (Mail)

- **Row height** (`updateTableViewRowHeight`, 0x10000f9a9): the height of a text cell holding
  "Afgy" in the bold font, or in the notes' font if taller.
- **Unread in bold**: `setFont:` (0x10000f689) derives the bold font with `NSFontManager
  convertFont:toHaveTrait:` (bold), when the preference `shouldShowUnreadMessagesInBold` is on.
- **Text colour** (`_colorForMessage:inRow:withCell:`, 0x10003eb62): a deleted message (shown with
  Show Deleted Messages) is **dark grey**; on a selected row, the selection's text colour (white
  when the list has focus); otherwise the message's own colour from the rules when bit
  0x40000000 says it colours the text.
- **Background colour**: a message coloured by a rule with bit 0x40000000 off gets its colour as
  the row's background.
- **Threads** (`tableView:rangesForBackgroundShadingInRange:shadingColors:leftColumnColors:`,
  0x1000282af): in threaded mode with thread highlighting on (and not while searching or focused),
  each thread's rows (the thread line, plus its messages when open) get a band:
  - the thread's colour blended 55 % with white, else `lighterThreadBackgroundColor`;
  - in the left column, the thread's colour itself, else `threadBackgroundColor`.

  Both come from Message.framework's `NSColor(NSColorUtilities)` category, not AppKit:
  `threadBackgroundColor` (0x447de) is the `ThreadBackgroundColor` preference if set (an integer,
  0xRRGGBB, read by `colorWithIntValue:`), else device RGB **(205, 220, 243)**;
  `lighterThreadBackgroundColor` (0x44760) is that colour blended 55 % with white, about
  (233, 239, 250).
- **The unread column's minimum width**: 20 px, or in threaded mode the width needed for the
  thread's unread count (`setUpUnreadColumnMinWidth:isThreaded:`, 0x1001c55cb).

## 11. The message pane (Mail)

- **The banner** (`MessageContentController _updateBanner`, 0x10001839e): at most one at a time,
  chosen in this order:
  1. a calendar message (types 6 and 7): `CalendarBanner`;
  2. Parental Controls: a child's message waiting for approval, or a child's permission request
     (`ParentBanner` / `ChildBanner`);
  3. a security error (error codes 0x40B–0x40C, the message having flag 0x8 or 0x800000,
     presumably encrypted and signed): `CertificateBanner`;
  4. junk (junk level 2, junk filtering on): `JunkMailBanner`;
  5. an HTML message whose remote images were blocked (not viewing the source): `LoadImagesBanner`;
  6. otherwise no banner.
- **Placing it** (`_showBannerView:`, 0x1000980ab): the banner from its nib is laid out at 1000 px
  wide, its width-resizable text field fitted, then placed over the top of the content's scroll
  view, which shrinks by the banner's height. It follows the content view's resizing.
- **The headers** (`MessageHeaderDisplay`): an `NSTextView` (`MessageHeaderView`) holding an
  attributed string that the viewing state builds for the chosen detail level (`headerDetailLevel`,
  All Headers ⇧⌘H). Set up in `setUp` (0x10001730e): the user's default font
  (`userFontOfSize:0`), a text container that follows the view's width, a 5 px horizontal inset.
  The sender's photo comes from the Address Book (`imageForMailAddress:`), and addresses are
  attachments (the same atoms as in the compose window). RSS articles get a tiled banner
  (`rss_banner_tile`, `rss_banner_right_cap`).
- **`ColorBackgroundView`** (used throughout the viewer nibs, `drawRect:` 0x1000279f2): one
  colour (with the pattern phase fixed to the window), or an image, or rows of colours of a given
  height.

## 12. The compose window (Mail)

- **The header** (`ComposeHeaderView tile`, 0x100058c50): the rows stack downwards in this order,
  To, Cc, Bcc, Reply-To, Subject, then one line shared by Account, Signature, Delivery (the SMTP
  server), with Priority and Security (signing and encryption) at its right edge. The view grows or
  shrinks to the result: 5 px below the last row (6 without the "fat" bottom controls), more while
  the user is customizing the header (the disclosure button + 2 × 7 px).
- **The account line** (`_calculateAccountFrame:deliveryFrame:signatureFrame:`, 0x100059726): the
  pop-ups size themselves to their contents (`sizeToFitWidth`), each capped at its maximum width,
  and share the width that remains; the line ends 17 px before the view's right edge, with room
  kept for Security (about 60 px) and Priority (about 45 px) when they are shown. The exact
  arithmetic between them is not fully traced.
- **Drawing** (`drawRect:`, 0x10006266a): the window's background colour, and a border image
  along the bottom row.

## 13. The address atoms (Mail)

`AddressAttachmentCell` draws an address as a **pill**: a line as thick as the rectangle is high,
with round caps (`_drawAtomPartsWithRect:cellFrame:`, 0x100082f01), first in the border colour,
then 1 px inset in the fill colour. When the text selection reaches the atom's left or right side,
that end is square instead. Colours (`+initialize`, 0x10008135d), device RGB:

| State | Border | Fill |
|---|---|---|
| normal, window active | (164, 189, 236) | (222, 231, 248) |
| normal, window inactive | (215, 215, 215) | (244, 244, 244) |
| "spotlighted" (`isSpotlighted`; when it is set is not traced) | (109, 149, 224) | (187, 206, 241) |
| selected, window active | (89, 139, 236), full | — |
| selected, window inactive | (145, 145, 145), full | — |

Size (`cellSize`, 0x1000818b2): the name's width + 30 px (+ 4 for the left margin unless hidden),
17 px high for atom type 1, 14 px for the others; type 0 (plain text, no pill) gets the name's
width + 15 px, plus the pop-down arrow's width (`PopDownArrow`) when `isOnLine`. The pill's bounds
are the cell inset 4 px on the left and 2 px on the right (`atomBoundsRectForCellFrame:`,
0x10008378c).

**Which headers** (Message.framework, `-[Defaults headersToDisplayFromHeaderKeys:headerDetailLevel:]`
0xb2e3b and `__headersToDisplay` 0xb31f0):

| Level | Headers |
|---|---|
| 0 | none |
| 1 (default) | Resent-From, From, Date, Resent-To, To, Resent-Cc, Cc, Bcc, Subject, Reply-To, the attachments line, and RSS's source name and author |
| 2 (All Headers) | the list above, then every other header of the message, each once |
| 3 (Custom) | the headers among those that match one of the user's patterns (whole-name match); the default list is not added by itself |

`-[Defaults headerDetailLevel]` (0x46669) maps the stored preference string to these numbers, 1
when it is missing or unknown. One header name is always left out of levels 2 and 3 (a constant
compared with `isEqualToString:`, not read yet).

## 14. Tests and renders

`tools/mail/` holds an implementation of the formats above and checks of it. Like the rest of
the shell, it reads your DVD's files at run time and contains none of them.

- `mailformats.py`: the old-style plist parser (toolbars, `.strings`), `.emlx` reading and
  writing, the flags integer (with the IMAP table), the `~/Library/Mail` paths, the header lists.
- `test_mailformats.py` (`python3 -m unittest tools/mail/test_mailformats.py`): 15 tests. The
  rules themselves (an `.emlx` round trip, flags, paths, header levels, parser errors), then, when
  Mail.app is unpacked, Mail's own files: all six `.toolbar` files parse and every default item
  resolves; every image they name exists; every label is in `Localizable.strings`; the sidebar's
  icon files exist, and the four system-drawn names have none.
- `render_check.py`: draws the toolbar, the sidebar, the list (with a thread), the address atoms
  and the badges in every state, from the recovered numbers and the DVD's images and Lucida
  Grande, into `.local/media/re/mail/render/` (`mail-render-check.png` puts them on one sheet).
  What the code did not give (the toolbar's bezels and gradient, the source list's background and
  group rows, the table header, the unread dot) is drawn approximately and hatched in red.

What the renders showed: the toolbar's segmented groups need each segment as wide as its label
(the first try, with fixed 32 px segments, overlapped the labels); the list's row comes out at
16 px for a 12 pt list font, so the message list font's default size is the next thing to confirm.

## 15. The account setup assistant ("Welcome to Mail")

**The window** (`AssistantWindow.nib`, owner `AssistantManager`): 578 × 416, titled by the
assistant (`windowTitle`). Buttons at the bottom: help (round, 21 × 23 at 20, 17), Cancel
(98 × 32 at 136, 12), Go Back (108 × 32 at 348, 12, `back:`), Continue (108 × 32 at 456, 12,
`forward:`, key equivalent Return), all Lucida Grande 13. Behind the pages, a
`TransparentImageView` (548 × 355 at 20, 55) holding an `NSBox` (422 × 346 at 119, 5) whose content
view hosts the current assistant's view.

- **The stamp and the veil** (`TransparentImageView`, `awakeFromNib` 0x10010be0f, `drawRect:`
  0x10010f9d7): the image `Mail_Large` (309 × 412) from Mail's resources, aligned top-left and not
  scaled, so its bottom is cut; then a white fill at 75 % over the box's bounds, inset 3 px left and
  4 px at the bottom, 6 px narrower and lower, with an 8 px corner radius.
- **Showing it** (`_showInWindow:` 0x10010f532): as a sheet over a window, or alone (frame
  autosaved as `SetupAssistant`); for the first launch's types (1, 4, 5) as an application-modal
  window.

**The pages** (`AccountSetupAssistant.nib`): one `NSTabView` (420 × 344, bound to `selectedTab`)
of 8 tabs: Welcome, Welcome with User Name, Incoming Server, Incoming Mail Security, Outgoing
Server, Outgoing Mail Security, Summary, Automatic Setup. The first page: a title in Lucida Grande
Bold 14 (12, 314) and a description in 13 pt (29, 231, 362 × 75), both bound (`firstPaneTitle`,
`firstPaneDescription`, from `Assistant.strings` `TITLE_FOR_WELCOME` / `DESCRIPTION_FOR_WELCOME`,
or `..._ADD_ACCOUNT` when adding one); right-aligned labels and three 197 × 22 fields at x 147
(name y 201, email y 174, password y 147, a secure field); an "invalid" icon next to the email
(352, 176) bound to `isEmailAddressValid`; a spinner and a caution icon (36, 67) and a status text
(60, 10, 340 × 73) for errors. The email field has the focus at first
(`_firstResponderForBasicInformation`).

**The flow** (`AccountSetupAssistant`):
- Going forward validates the page (`_validateTab:` 0x1001105a1): an empty email, host, user or SMTP
  server shows the matching message (`NO_EMAIL_MESSAGE`, `NO_HOST_MESSAGE`, `NO_USER_MESSAGE`,
  `NO_SMTP_MESSAGE`; `NO_OWA_MESSAGE` for Exchange) next to the caution icon; an account that
  already exists is refused with its name; otherwise the next step runs: automatic configuration,
  or checking the incoming then the outgoing server.
- Automatic setup (`_updateAutomaticSetupFromEmailAddress` 0x10010ef5c): offered when the address's
  domain is gmail.com, yahoo.com or aol.com (`_canUseAutomaticSetupForDomain:` 0x1001103c2), or when
  the locally installed ISP settings (`ISPAccountsXQuery.xml` in Message.framework) know the
  domain. Option held down at Continue forces the manual pages.
- The pages visited are a stack: `_pushTab:` (0x10011315f) enables Go Back, `_popTab` (0x10011fbbd)
  returns to the previous one and disables Go Back when the stack is empty.
- `_updateTab:` (0x1001133c5) prepares the Incoming Server page (Address Book and iCal options) and
  the Automatic Setup summary (the "Automatically set up account" checkbox, shown only with the
  `DisplayAutomaticSetupCheckbox` preference; the contacts and calendars options for Exchange).

**Render and comparison**: `tools/mail/render_assistant.py` draws the first page from these frames
with the DVD's stamp, strings and Lucida Grande; `--compare SCREENSHOT` crops a 10.6 screenshot's
window, scales it to the nib's size and puts screenshot, render and difference side by side. Against
a 10.6 screenshot, the stamp, veil, box, title, description, labels, fields, focus and buttons all
fall within 1–2 px. The first try showed two defects, now fixed: the paragraphs need the Mac's
17 px line height at 13 pt (not cairo's 16), and an open path drew lines between the buttons. What
still differs is AppKit's (approximated here): the title bar, the buttons' and fields' bezels, the
help button, the window's grey, and text antialiasing.

## 16. Our Mail.app (`shell/src/mail`)

Built on AquaKit, from the sections above:

| File | What it does |
|---|---|
| `main.cpp` | The Welcome assistant: `AssistantWindow.nib` and `AccountSetupAssistant.nib` instantiated by AquaKit's `Nib`, the stamp and veil of §15, the pages pushed and popped as `_pushTab:` / `_popTab` do. The automatic setup guesses the servers (the three domains of §15, else `imap.`/`smtp.` + the domain) and falls back to the manual pages with the error. Then the viewer (§4 toolbar, §9 sidebar and badges, §5 list, §11 headers at level 1), and the compose window (Send from `ComposeWindow_NewMessage.toolbar`). |
| `store.cpp` | §7–8: the `.emlx` files (length line, message, plist with `flags`, `date-sent`, `remote-id`, `sender`, `subject`), the `<type>-<user>@<host>/<mailbox>.imapmbox/Messages/<uid>.emlx` layout, the flags from `_IMAPMessageFlagsTable`; the accounts in `~/.config/snowleopard/mail.conf`, the passwords in the keyring (libsecret, schema `org.snowleopard.Mail`). |
| `net.cpp` | IMAP and SMTP with libcurl on a worker thread; the results come back on AquaKit's run loop. Fetching a message marks it read on the server, so `\Seen` is taken off again when it wasn't there; a sent message is appended to the server's Sent mailbox; Delete is `UID MOVE` to Trash (or COPY, `\Deleted`, EXPUNGE). TLS is required, except on localhost. |
| `mime.cpp` | Headers (folded lines, RFC 2047 words), multipart, quoted-printable, base64, charsets by iconv, HTML made plain, attachments' names, dates. |
| `textview.cpp` | The compose window's text: lines wrapped at words, the caret, typing and the arrows. |

**Testing.** `tools/mail/fakemail.py` is a local IMAP4rev1 and SMTP server (what curl sends, in
memory, seeded with plain, multipart quoted-printable and Latin-1 HTML messages). Mail was driven
in Xvfb through XTEST with a throwaway HOME, end to end: the assistant (a wrong server, the manual
pages, the right one), the sync (3 messages, `\Seen` kept as it was on the server), opening them
(read on the server and here, the badge going down), sending (SMTP, then APPEND to Sent, the message
coming back to the inbox), Delete (UID MOVE to Trash) and the shortcuts. Found and fixed on the way:
- curl hands a custom command's untagged answers to the program only when they are named like the
  command, and not FETCH's: the flags are read from curl's debug callback (`CURLINFO_HEADER_IN`);
- AquaKit's `TextField` left its caret's timer running after it was destroyed: closing a window
  while one of its fields had the keyboard crashed the program (all AquaKit programs);
- AquaKit gives no text for a key with ⌘ or ⌃: the shortcuts read the keysym.

Gaps found in AquaKit: no ⌘A (select all) in `TextField`, no attributed text in `TableView` (the
unread column is a "●", not the bold rows of §10).

## Next

| Item | Needs |
|---|---|
| The assistant's other pages (Incoming / Outgoing Server, Security, Summary), rendered from the same nib | `render_assistant.py`, tabs 2–8 |
| The message list font's default (name and size), to confirm the 16 px row | `+[NSFont(MailAdditions) messageListFont]` (0x10000f60e) |
| The rest of the `.emlx` plist's keys, and how `Envelope Index` rows are written | `Library insertOrUpdateMessages:…`, the plist writer |
| The headers' fonts and colours, and the header name always left out | `MessageHeaders attributedStringShowingHeaderDetailLevel:…` (0x1a85df) |
| The attachments' view under the headers | `HeaderAttachmentsView` |
| The toolbar's and source list's look | CoreUI (already reversed for the Finder: `shell/src/aquakit`) |
