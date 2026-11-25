// SPDX-License-Identifier: LGPL-2.1-or-later
// Time Machine's side of the Finder (docs/timemachine-host.md): the backup database the shell's
// backupd writes, its snapshots and their dates, a folder mapped into a snapshot as CarbonCore's
// _CSBackupFindMatchingSnapshotItemByPath does, and the pictures of the Finder's window showing that
// folder in each snapshot (TBackupSnapshotWindowController's offscreen window, section 5).
#pragma once

#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

namespace TM {

struct Snapshot {
    QString name;        // "2009-10-22-153045"
    QString path;        // the snapshot folder
    QDateTime date;      // SnapshotCompletionDate, else the name's
};

// timemachine.conf's BackupPath: <disk>/Backups.backupdb/<computer> (_CSBackupSessionCopyBackupDirectory)
QString machineFolder();
// its snapshots, newest first (BUCopySnapshotArray's order is oldest first; the stack's newest first)
QVector<Snapshot> snapshots(const QString &machine);
// <snapshot>/<volume folder>/<path relative to the volume's mount point>, the volume folder found by
// its SnapshotVolumeUUID attribute, else by name; the older layout (the absolute path) as well.
// Empty when the snapshot doesn't hold it.
// volumeHint: the volume folder to try first (a volume entered from the Computer of another disk)
QString folderInSnapshot(const QString &snapshot, const QString &original, const QString &volumeHint = QString());

// The Finder's Computer (TNode's computer node, "file:///?Computer"): the startup disk, then the disks
// mounted under /run/media, /media and /mnt
struct Volume {
    QString name, path;
    enum Kind { Internal, External, Removable } kind = Internal;
};
QVector<Volume> volumes();
QString volumeName(const QString &mountPoint);
QString volumeIconFile(const Volume &v);
QImage volumeIcon(const Volume &v, int size);
// Browse Other Time Machine Disks' rows: every <volume>/Backups.backupdb/<computer> on the mounted disks
struct BackupStore {
    QString computer;       // the computer's backup folder
    QString computerName;
    Volume volume;
};
QVector<BackupStore> backupStores();

struct Entry {
    QString name;
    bool dir = false;
    qint64 size = 0;        // its physical size (blocks)
    QDateTime modified;
    QString kind;
    QString icon;           // an icon file of its own (the Computer's volumes), else the folder's or document's
};
QVector<Entry> listFolder(const QString &path);
// The Finder's search field inside Time Machine (docs/timemachine-host.md 10): the items under `root`
// whose name contains `text` (the field's name search, no hidden items), a file walk standing for the
// query in that snapshot (_kMDItemTimeMachinePath = "<scope>/*"); each Entry's name its path relative
// to root, sorted by their names; at most `limit`
QVector<Entry> searchFolder(const QString &root, const QString &text, int limit = 1000);
// a signature of the folder's items (names, sizes, dates): "smart" navigation's changes
QString signature(const QVector<Entry> &entries);

// The Finder's window in list or icon view, as a picture: title bar and unified toolbar, sidebar, the list,
// the status bar
struct WindowPicture {
    QString title;
    QVector<Entry> entries;
    QSet<QString> selected;
    qint64 available = -1;
    QString home;           // for the sidebar
    bool icons = false;     // icon view (else list view)
    QString titleIcon;      // the proxy icon's file, else the folder's
    QString search;         // the search field's text (a search's results: the entries' names relative paths)
    bool searchFocus = false;
};
// the toolbar's search field in the picture (y down), empty when the window is too narrow for it
QRect searchFieldRect(QSize size);
QImage paintWindow(const WindowPicture &w, QSize size);
// the items in the picture (y down): item i's rectangle; -1 from itemAt outside them
QRect itemRect(QSize size, int item, bool icons);
int itemAt(QSize size, QPoint p, int count, bool icons);
// restoreImageForNode:outRect: item's icon and name over nothing, its frame in the picture (y down)
QImage restoreImage(const Entry &e, QSize size, int item, bool icons, QRect *frame);
// An unavailable snapshot (invalidTargetImage): Invalid*.png's 9-slice frame filled with 0.149 grey at 0.85
QImage invalidPicture(QSize size);

QString displayName(const QString &path);

} // namespace TM
