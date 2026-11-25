// SPDX-License-Identifier: LGPL-2.1-or-later
#include "snapshots.h"

#include "aqua.h"
#include "assets.h"
#include "frame.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLinearGradient>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimeZone>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

namespace TM {

namespace {

QString configDir()
{
    const QByteArray x = qgetenv("XDG_CONFIG_HOME");
    return (x.isEmpty() ? QDir::homePath() + "/.config" : QString::fromLocal8Bit(x)) + "/snowleopard";
}

QString attribute(const QString &path, const char *key)
{
    char buf[128] = {};
    const ssize_t n = getxattr(QFile::encodeName(path).constData(), key, buf, sizeof buf - 1);
    return n > 0 ? QString::fromLatin1(buf) : QString();
}

// /proc/self/mountinfo's mount points (the octal escapes decoded)
QStringList mountPoints()
{
    QStringList out;
    QFile f("/proc/self/mountinfo");
    if (!f.open(QIODevice::ReadOnly))
        return out;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 5)
            continue;
        QByteArray raw = fields[4], mp;
        for (int i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size()) {
                mp += char(raw.mid(i + 1, 3).toInt(nullptr, 8));
                i += 3;
            } else {
                mp += raw[i];
            }
        }
        out << QFile::decodeName(mp);
    }
    return out;
}

// /proc/self/mountinfo: the longest mount point holding `path` (_BSDVolumePathLength)
QString mountPoint(const QString &path)
{
    QFile f("/proc/self/mountinfo");
    QString best = "/";
    if (!f.open(QIODevice::ReadOnly))
        return best;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 5)
            continue;
        QByteArray raw = fields[4], mp;
        for (int i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size()) {
                mp += char(raw.mid(i + 1, 3).toInt(nullptr, 8));
                i += 3;
            } else {
                mp += raw[i];
            }
        }
        const QString m = QFile::decodeName(mp);
        if (m.size() > best.size() && path.startsWith(m) && (path.size() == m.size() || path[m.size()] == '/'))
            best = m;
    }
    return best;
}

QString byDevice(const char *dir, const QString &path)
{
    struct stat st;
    if (stat(QFile::encodeName(path).constData(), &st) != 0)
        return {};
    const QDir d(dir);
    for (const QString &e : d.entryList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
        struct stat dev;
        if (stat(QFile::encodeName(d.filePath(e)).constData(), &dev) == 0 && dev.st_rdev == st.st_dev) {
            QString name = e;
            // udev escapes as \x20
            for (int i; (i = name.indexOf("\\x")) >= 0 && i + 3 < name.size();)
                name.replace(i, 4, QChar(name.mid(i + 2, 2).toInt(nullptr, 16)));
            return name;
        }
    }
    return {};
}

QFont font(qreal size, bool bold = false) { return Assets::uiFont(size, bold); }

QImage icon(const QString &name, int size)
{
    static QHash<QString, QImage> cache;
    const QString key = name + QString::number(size);
    auto it = cache.find(key);
    if (it != cache.end())
        return *it;
    QString path = name.startsWith('/') ? name : "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/" + name;
    QImage img = Assets::image(path, QSize(size, size));
    cache.insert(key, img);
    return img;
}

QString sizeText(qint64 bytes)
{
    // the Finder's, base 1000
    if (bytes <= 0)
        return "Zero KB";
    const double kb = bytes / 1000.0;
    if (kb < 1000)
        return QString::number(std::max(1.0, std::round(kb))) + " KB";
    const double mb = kb / 1000.0;
    if (mb < 1000)
        return QLocale(QLocale::English).toString(mb, 'f', 1) + " MB";
    return QLocale(QLocale::English).toString(mb / 1000.0, 'f', 2) + " GB";
}

QString dateText(const QDateTime &d)
{
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    const QString time = en.toString(d.time(), "h:mm AP");
    const QDate today = QDate::currentDate();
    if (d.date() == today)
        return "Today, " + time;
    if (d.date() == today.addDays(-1))
        return "Yesterday, " + time;
    return en.toString(d.date(), "MMM d, yyyy") + " " + time;
}

// the list's geometry in a window of `size` (y down)
constexpr int Toolbar = 37;                 // the unified toolbar under the title bar (icon only)
constexpr int Top = Frame::TitleHeight + Toolbar;
constexpr int StatusBar = 22;
constexpr int Header = 17;
constexpr int Row = 19;
constexpr int CellW = 106, CellH = 100;

int sidebarWidth(QSize size) { return std::min(192, size.width() / 4); }

} // namespace

QString displayName(const QString &path)
{
    if (path == QDir::homePath())
        return QFileInfo(path).fileName();
    const QString n = QFileInfo(path).fileName();
    return n.isEmpty() ? QString("Macintosh HD") : n;
}

QString volumeName(const QString &mountPoint)
{
    const QString label = byDevice("/dev/disk/by-label", mountPoint);
    if (!label.isEmpty())
        return label;
    return mountPoint == "/" ? QString("Macintosh HD") : QFileInfo(mountPoint).fileName();
}

QVector<Volume> volumes()
{
    // the Finder's Computer: the startup disk, then the disks the desktop mounted (/run/media, /media, /mnt)
    QVector<Volume> out{{volumeName("/"), "/", Volume::Internal}};
    QSet<QString> seen{"/"};
    for (const QString &mp : mountPoints()) {
        const bool removable = mp.startsWith("/run/media/") || mp.startsWith("/media/");
        if (seen.contains(mp) || !(removable || mp.startsWith("/mnt/")))
            continue;
        if (mp.count('/') > (mp.startsWith("/run/media/") ? 4 : mp.startsWith("/media/") ? 3 : 2))
            continue;   // a mount inside a disk
        seen.insert(mp);
        out.push_back({volumeName(mp), mp, removable ? Volume::Removable : Volume::External});
    }
    return out;
}

QVector<BackupStore> backupStores()
{
    // FUN_100014390: every mounted volume holding Backups.backupdb (lstat), its computers' folders;
    // backupd's BackupPath too when it isn't a mount point of its own
    QStringList disks = mountPoints();
    QSettings st(configDir() + "/timemachine.conf", QSettings::IniFormat);
    const QString configured = st.value("BackupPath").toString();
    if (!configured.isEmpty())
        disks << QDir::cleanPath(configured);
    QVector<BackupStore> out;
    QSet<QString> seen;
    for (const QString &disk : disks) {
        const QString db = (disk == "/" ? QString() : disk) + "/Backups.backupdb";
        struct stat sb;
        if (seen.contains(disk) || lstat(QFile::encodeName(db).constData(), &sb) != 0 || !S_ISDIR(sb.st_mode))
            continue;
        seen.insert(disk);
        const bool removable = disk.startsWith("/run/media/") || disk.startsWith("/media/");
        const bool mounted = mountPoint(disk) == disk;
        for (const QString &computer : QDir(db).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            BackupStore b;
            b.computer = db + "/" + computer;
            b.computerName = computer;
            b.volume = {mounted ? volumeName(disk) : QFileInfo(disk).fileName(), disk,
                        disk == "/" ? Volume::Internal : removable ? Volume::Removable : Volume::External};
            out.push_back(b);
        }
    }
    // TBackupVolumeTableViewController: sorted by displayName, caseSensitiveSystemLocaleCompare:
    std::sort(out.begin(), out.end(), [](const BackupStore &a, const BackupStore &b) {
        return QString::localeAwareCompare(a.computerName + " " + a.volume.name, b.computerName + " " + b.volume.name) < 0;
    });
    return out;
}

QString volumeIconFile(const Volume &v)
{
    // IOStorageFamily.kext's icons, as the Finder's
    const char *name = v.kind == Volume::Internal ? "Internal" : v.kind == Volume::Removable ? "Removable" : "External";
    return QString("/System/Library/Extensions/IOStorageFamily.kext/Contents/Resources/") + name + ".icns";
}

QImage volumeIcon(const Volume &v, int size) { return Assets::image(volumeIconFile(v), QSize(size, size)); }

QString machineFolder()
{
    QSettings s(configDir() + "/timemachine.conf", QSettings::IniFormat);
    QString disk = s.value("BackupPath").toString();
    if (disk.isEmpty())
        disk = s.value("BackupAlias").toString();
    if (disk.isEmpty())
        return {};
    const QString db = disk + "/Backups.backupdb";
    char name[256] = {};
    gethostname(name, sizeof name - 1);
    const QString host = QString::fromLocal8Bit(name).section('.', 0, 0);
    if (QFileInfo(db + "/" + host).isDir())
        return db + "/" + host;
    const QStringList machines = QDir(db).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    return machines.isEmpty() ? QString() : db + "/" + machines.first();
}

QVector<Snapshot> snapshots(const QString &machine)
{
    QVector<Snapshot> out;
    if (machine.isEmpty())
        return out;
    const QDir d(machine);
    for (const QString &name : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        // _CSBackupPathIsBackupItem's 3: YYYY-MM-DD-HHMMSS ("…-2", from older backupds, kept readable)
        if (name.size() < 17 || name[4] != '-' || name[7] != '-' || name[10] != '-' || (name.size() > 17 && name[17] != '-'))
            continue;
        QDateTime date = QDateTime::fromString(name.left(17), "yyyy-MM-dd-HHmmss");
        if (!date.isValid())
            continue;
        Snapshot s{name, d.filePath(name), date};
        // TSnapshot's fCompletionDate: com.apple.backupd.SnapshotCompletionDate, decimal µs since 1970
        const QString completed = attribute(s.path, "user.com.apple.backupd.SnapshotCompletionDate");
        if (!completed.isEmpty())
            s.date = QDateTime::fromMSecsSinceEpoch(completed.toLongLong() / 1000);
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Snapshot &a, const Snapshot &b) { return a.date > b.date; });
    return out;
}

QString folderInSnapshot(const QString &snapshot, const QString &original, const QString &volumeHint)
{
    const QString mp = mountPoint(original);
    const QString relative = mp == "/" ? original : original.mid(mp.size());
    // _FindChildFolderByMatchingXTAttr: the volume folder with the volume's UUID, else its name
    QString uuid = byDevice("/dev/disk/by-uuid", original).toUpper();
    QStringList volumes;
    if (!volumeHint.isEmpty())
        volumes << volumeHint;
    const QDir d(snapshot);
    for (const QString &v : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!uuid.isEmpty() && attribute(d.filePath(v), "user.com.apple.backupd.SnapshotVolumeUUID") == uuid)
            volumes << v;
    QString label = byDevice("/dev/disk/by-label", original);
    volumes << (label.isEmpty() ? QString("Macintosh HD") : label);
    for (const QString &v : volumes) {
        for (const QString &p : {relative, original}) {   // the Mac's layout, then the older one's
            const QString path = d.filePath(v) + (p == "/" ? QString() : p);
            if (QFileInfo(path).isDir())
                return path;
        }
    }
    return {};
}

namespace {
Entry entryFor(const QFileInfo &fi)
{
    static QMimeDatabase mime;
    Entry e;
    e.name = fi.fileName();
    e.dir = fi.isDir();
    struct stat st;
    if (lstat(QFile::encodeName(fi.filePath()).constData(), &st) == 0)
        e.size = qint64(st.st_blocks) * 512;
    e.modified = fi.lastModified();
    if (e.dir) {
        e.kind = "Folder";
    } else {
        QString c = mime.mimeTypeForFile(fi).comment();
        if (!c.isEmpty())
            c[0] = c[0].toUpper();
        e.kind = c.isEmpty() ? QString("Document") : c;
    }
    return e;
}
} // namespace

QVector<Entry> searchFolder(const QString &root, const QString &text, int limit)
{
    QVector<Entry> out;
    if (root.isEmpty() || text.isEmpty() || !QFileInfo(root).isDir())
        return out;
    const QDir base(root);
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext() && out.size() < limit) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (!fi.fileName().contains(text, Qt::CaseInsensitive))
            continue;
        Entry e = entryFor(fi);
        e.name = base.relativeFilePath(fi.filePath());
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
        const int c = QString::compare(a.name.section('/', -1), b.name.section('/', -1), Qt::CaseInsensitive);
        return c ? c < 0 : a.name < b.name;
    });
    return out;
}

QVector<Entry> listFolder(const QString &path)
{
    QVector<Entry> out;
    static QMimeDatabase mime;
    const QDir d(path);
    for (const QFileInfo &fi : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System, QDir::Name | QDir::IgnoreCase)) {
        Entry e;
        e.name = fi.fileName();
        e.dir = fi.isDir();
        struct stat st;
        if (lstat(QFile::encodeName(fi.filePath()).constData(), &st) == 0)
            e.size = qint64(st.st_blocks) * 512;
        e.modified = fi.lastModified();
        if (e.dir) {
            e.kind = "Folder";
        } else {
            QString c = mime.mimeTypeForFile(fi).comment();
            if (!c.isEmpty())
                c[0] = c[0].toUpper();
            e.kind = c.isEmpty() ? QString("Document") : c;
        }
        out.push_back(e);
    }
    return out;
}

QString signature(const QVector<Entry> &entries)
{
    QString s;
    for (const Entry &e : entries)
        s += e.name + '\t' + QString::number(e.size) + '\t' + QString::number(e.modified.toSecsSinceEpoch()) + '\n';
    return s;
}

QRect searchFieldRect(QSize size)
{
    return size.width() > 420 ? QRect(size.width() - 171, Frame::TitleHeight + 7 + 1, 161, 21) : QRect();
}

QRect itemRect(QSize size, int item, bool icons)
{
    const int x = sidebarWidth(size) + 1;
    if (icons) {
        // icon view: 64-point icons in cells 106 wide, 100 high, from the top left
        const int perRow = std::max(1, (size.width() - x - 10) / CellW);
        return QRect(x + 10 + (item % perRow) * CellW, Top + 10 + (item / perRow) * CellH, CellW, CellH);
    }
    return QRect(x, Top + Header + item * Row, size.width() - x, Row);
}

QImage restoreImage(const Entry &e, QSize size, int item, bool icons, QRect *frame)
{
    // restoreImageForNode:outRect: the item's cell (its icon and name) over nothing, in its frame
    const QRect cell = itemRect(size, item, icons);
    const QFont f = font(12);
    const QFontMetrics fm(f);
    const QImage ic = icon(!e.icon.isEmpty() ? e.icon : e.dir ? "GenericFolderIcon.icns" : "GenericDocumentIcon.icns", icons ? 64 : 16);
    QRect r, iconRect, textRect;
    QString label = e.name.section('/', -1);
    if (icons) {
        label = fm.elidedText(label, Qt::ElideMiddle, cell.width() - 6);
        const int tw = fm.horizontalAdvance(label) + 10;
        iconRect = QRect(cell.center().x() - 32, cell.y() + 4, 64, 64);
        textRect = QRect(cell.center().x() - tw / 2, iconRect.bottom() + 6, tw, 16);
    } else {
        iconRect = QRect(cell.x() + 18, cell.y() + 1, 16, 16);
        textRect = QRect(cell.x() + 38, cell.y(), fm.horizontalAdvance(label) + 4, Row);
    }
    r = iconRect.united(textRect);
    QImage img(r.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.translate(-r.topLeft());
    p.drawImage(iconRect, ic);
    p.setFont(f);
    p.setPen(Qt::black);
    Aqua::drawText(p, QRectF(textRect), (icons ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter), label);
    p.end();
    if (frame)
        *frame = r;
    return img;
}

int itemAt(QSize size, QPoint p, int count, bool icons)
{
    if (p.x() <= sidebarWidth(size) || p.y() < Top + (icons ? 0 : Header) || p.y() >= size.height() - StatusBar)
        return -1;
    for (int i = 0; i < count; ++i)
        if (itemRect(size, i, icons).contains(p))
            return i;
    return -1;
}

QImage paintWindow(const WindowPicture &w, QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath shape;   // the top corners rounded (4), the bottom ones as a textured window's
    shape.addRoundedRect(QRectF(0, 0, size.width(), size.height()), 4, 4);
    p.setClipPath(shape);

    // the title bar and the unified toolbar
    for (int y = 0; y < Top; ++y) {
        p.setPen(Frame::unifiedToolbarColor(y, Top, true));
        p.drawLine(0, y, size.width(), y);
    }
    Frame::TitleBar t;
    t.width = size.width();
    t.title = w.title;
    t.active = true;
    t.hasToolbar = true;
    t.unifiedToolbar = Toolbar;
    t.proxyIcon = icon(w.titleIcon.isEmpty() ? QString("GenericFolderIcon.icns") : w.titleIcon, 16);
    p.drawImage(0, 0, Frame::paintTitleBar(t));
    p.setPen(QColor(64, 64, 64));
    p.drawLine(0, Top - 1, size.width(), Top - 1);

    // the toolbar's controls: back and forward, the views, the search field
    auto segmented = [&](QRect r, int segments, int selected) {
        QLinearGradient g(r.topLeft(), r.bottomLeft());
        g.setColorAt(0, QColor(251, 251, 251));
        g.setColorAt(1, QColor(210, 210, 210));
        p.setPen(QColor(110, 110, 110));
        p.setBrush(g);
        p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3.5, 3.5);
        const double sw = double(r.width()) / segments;
        for (int i = 0; i < segments; ++i) {
            const QRectF seg(r.x() + i * sw, r.y(), sw, r.height());
            if (i == selected) {
                p.save();
                p.setClipRect(seg);
                p.setBrush(QColor(140, 140, 140));
                p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3.5, 3.5);
                p.restore();
            }
            if (i)
                p.drawLine(QPointF(seg.x() + 0.5, r.y() + 1), QPointF(seg.x() + 0.5, r.bottom()));
        }
    };
    const int ty = Frame::TitleHeight + 7;
    segmented(QRect(8, ty, 56, 23), 2, -1);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(60, 60, 60));
    p.drawPolygon(QPolygonF({QPointF(22, ty + 11.5), QPointF(30, ty + 7), QPointF(30, ty + 16)}));
    p.drawPolygon(QPolygonF({QPointF(50, ty + 11.5), QPointF(42, ty + 7), QPointF(42, ty + 16)}));
    segmented(QRect(76, ty, 112, 23), 4, w.icons ? 0 : 1);
    p.setPen(QColor(50, 50, 50));
    for (int i = 0; i < 3; ++i)   // the list view's glyph
        p.drawLine(76 + 28 + 9, ty + 8 + i * 3, 76 + 28 + 19, ty + 8 + i * 3);
    p.drawRect(76 + 9, ty + 7, 4, 4);   // the icon view's
    p.drawRect(76 + 15, ty + 7, 4, 4);
    p.drawRect(76 + 9, ty + 13, 4, 4);
    p.drawRect(76 + 15, ty + 13, 4, 4);
    if (const QRect field = searchFieldRect(size); !field.isEmpty()) {
        const QRectF search = QRectF(field).adjusted(0.5, 0.5, -0.5, -0.5);
        if (w.searchFocus) {   // the keyboard focus ring
            p.setPen(QPen(QColor(110, 160, 230, 200), 3));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(search.adjusted(-1, -1, 1, 1), 11, 11);
        }
        p.setPen(QColor(130, 130, 130));
        p.setBrush(Qt::white);
        p.drawRoundedRect(search, 10, 10);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(search.x() + 7, search.y() + 5, 7, 7));
        p.drawLine(QPointF(search.x() + 13, search.y() + 11.5), QPointF(search.x() + 16, search.y() + 14.5));
        if (!w.search.isEmpty()) {
            p.setFont(font(12));
            p.setPen(Qt::black);
            Aqua::drawText(p, QRectF(search.x() + 21, search.y(), search.width() - 40, search.height()), Qt::AlignLeft | Qt::AlignVCenter,
                           QFontMetrics(font(12)).elidedText(w.search, Qt::ElideRight, int(search.width()) - 40));
            // the cancel button: a grey disc with a white cross
            const QPointF c(search.right() - 10, search.center().y());
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(165, 165, 165));
            p.drawEllipse(c, 6.5, 6.5);
            p.setPen(QPen(Qt::white, 1.5));
            p.drawLine(c + QPointF(-2.5, -2.5), c + QPointF(2.5, 2.5));
            p.drawLine(c + QPointF(-2.5, 2.5), c + QPointF(2.5, -2.5));
        }
    }

    // the sidebar
    const int sw = sidebarWidth(size);
    const QRect body(0, Top, size.width(), size.height() - Top - StatusBar);
    p.fillRect(QRect(0, Top, sw, body.height()), QColor(220, 226, 232));
    p.fillRect(QRect(sw, Top, 1, body.height()), QColor(165, 165, 165));
    p.setRenderHint(QPainter::Antialiasing, false);
    int y = Top + 8;
    auto header = [&](const QString &s) {
        p.setFont(font(11, true));
        p.setPen(QColor(255, 255, 255, 160));
        Aqua::drawText(p, QRectF(10, y + 1, sw - 12, 16), Qt::AlignLeft | Qt::AlignVCenter, s);
        p.setPen(QColor(113, 126, 140));
        Aqua::drawText(p, QRectF(10, y, sw - 12, 16), Qt::AlignLeft | Qt::AlignVCenter, s);
        y += 20;
    };
    auto item = [&](const QString &iconName, const QString &s) {
        p.drawImage(19, y, icon(iconName, 16));
        p.setFont(font(12));
        p.setPen(Qt::black);
        Aqua::drawText(p, QRectF(40, y, sw - 44, 16), Qt::AlignLeft | Qt::AlignVCenter, s);
        y += 20;
    };
    header("DEVICES");
    item("/System/Library/Extensions/IOStorageFamily.kext/Contents/Resources/Internal.icns", "Macintosh HD");
    y += 6;
    header("PLACES");
    item("DesktopFolderIcon.icns", "Desktop");
    item("HomeFolderIcon.icns", QFileInfo(w.home).fileName());
    item("ApplicationsFolderIcon.icns", "Applications");
    item("DocumentsFolderIcon.icns", "Documents");

    const int lx = sw + 1, lw = size.width() - lx;
    if (w.icons) {
        // icon view: white, the icons in their grid, the selected one on a grey rounded backdrop and
        // its name on the selection colour
        p.fillRect(QRect(lx, Top, lw, body.height()), Qt::white);
        p.setClipRect(QRect(lx, Top, lw, body.height()));
        for (int i = 0; i < w.entries.size(); ++i) {
            const Entry &e = w.entries[i];
            const QRect cell = itemRect(size, i, true);
            if (cell.top() > body.bottom())
                break;
            const bool sel = w.selected.contains(e.name);
            const QRect ir(cell.center().x() - 32, cell.y() + 4, 64, 64);
            if (sel) {
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 40));
                p.drawRoundedRect(QRectF(ir).adjusted(-4, -4, 4, 4), 5, 5);
            }
            p.drawImage(ir, icon(!e.icon.isEmpty() ? e.icon : e.dir ? "GenericFolderIcon.icns" : "GenericDocumentIcon.icns", 64));
            p.setFont(font(12));
            const QFontMetrics fm(p.font());
            const QString label = fm.elidedText(e.name.section('/', -1), Qt::ElideMiddle, cell.width() - 6);
            const int tw = fm.horizontalAdvance(label) + 10;
            const QRect lr(cell.center().x() - tw / 2, ir.bottom() + 6, tw, 16);
            if (sel) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(56, 117, 215));
                p.drawRoundedRect(QRectF(lr), 8, 8);
                p.setRenderHint(QPainter::Antialiasing, false);
            }
            p.setPen(sel ? Qt::white : Qt::black);
            Aqua::drawText(p, QRectF(lr), Qt::AlignCenter, label);
        }
        p.setClipPath(shape);
    } else {
    // the list: its header, the rows alternating white and blue (NSTableView's alternating colours)
    p.fillRect(QRect(lx, Top + Header, lw, body.height() - Header), Qt::white);
    for (int r = 0; Top + Header + r * Row < body.bottom(); ++r)
        if (r % 2)
            p.fillRect(QRect(lx, Top + Header + r * Row, lw, Row), QColor(237, 243, 254));
    QLinearGradient hg(0, Top, 0, Top + Header);
    hg.setColorAt(0, QColor(255, 255, 255));
    hg.setColorAt(1, QColor(221, 221, 221));
    p.fillRect(QRect(lx, Top, lw, Header), hg);
    p.fillRect(QRect(lx, Top + Header - 1, lw, 1), QColor(160, 160, 160));
    const int kindW = std::min(130, lw / 5), sizeW = std::min(70, lw / 7), dateW = std::min(170, lw / 4);
    const int nameW = lw - kindW - sizeW - dateW;
    const int cols[4] = {lx, lx + nameW, lx + nameW + dateW, lx + nameW + dateW + sizeW};
    const char *titles[4] = {"Name", "Date Modified", "Size", "Kind"};
    p.setFont(font(11));
    for (int c = 0; c < 4; ++c) {
        if (c)
            p.fillRect(QRect(cols[c], Top + 2, 1, Header - 4), QColor(180, 180, 180));
        p.setPen(Qt::black);
        Aqua::drawText(p, QRectF(cols[c] + 6, Top, (c < 3 ? cols[c + 1] : size.width()) - cols[c] - 10, Header),
                       (c == 2 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, titles[c]);
    }
    p.setClipRect(QRect(lx, Top + Header, lw, body.bottom() - Top - Header + 1));
    for (int i = 0; i < w.entries.size(); ++i) {
        const Entry &e = w.entries[i];
        const QRect r = itemRect(size, i, false);
        if (r.top() > body.bottom())
            break;
        const bool sel = w.selected.contains(e.name);
        if (sel) {
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0, QColor(94, 148, 227));
            g.setColorAt(1, QColor(43, 103, 207));
            p.fillRect(r, g);
        }
        if (e.dir) {   // the disclosure triangle
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QColor(255, 255, 255) : QColor(110, 110, 110));
            const double cx = r.x() + 9, cy = r.y() + Row / 2.0;
            p.drawPolygon(QPolygonF({QPointF(cx - 2.5, cy - 4.5), QPointF(cx + 3.5, cy), QPointF(cx - 2.5, cy + 4.5)}));
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        p.drawImage(r.x() + 18, r.y() + 1, icon(!e.icon.isEmpty() ? e.icon : e.dir ? "GenericFolderIcon.icns" : "GenericDocumentIcon.icns", 16));
        p.setFont(font(12));
        p.setPen(sel ? Qt::white : Qt::black);
        Aqua::drawText(p, QRectF(r.x() + 38, r.y(), nameW - 44, Row), Qt::AlignLeft | Qt::AlignVCenter, e.name.section('/', -1));
        p.setPen(sel ? Qt::white : QColor(80, 80, 80));
        Aqua::drawText(p, QRectF(cols[1] + 6, r.y(), dateW - 10, Row), Qt::AlignLeft | Qt::AlignVCenter, dateText(e.modified));
        Aqua::drawText(p, QRectF(cols[2] + 4, r.y(), sizeW - 10, Row), Qt::AlignRight | Qt::AlignVCenter,
                       e.dir ? QString("--") : sizeText(e.size));
        Aqua::drawText(p, QRectF(cols[3] + 6, r.y(), size.width() - cols[3] - 10, Row), Qt::AlignLeft | Qt::AlignVCenter, e.kind);
    }
    p.setClipPath(shape);
    }

    // the status bar (a textured window's bottom)
    const QRect sb(0, size.height() - StatusBar, size.width(), StatusBar);
    QLinearGradient sg(sb.topLeft(), sb.bottomLeft());
    sg.setColorAt(0, QColor(204, 204, 204));
    sg.setColorAt(1, QColor(167, 167, 167));
    p.fillRect(sb, sg);
    p.fillRect(QRect(0, sb.y(), size.width(), 1), QColor(81, 81, 81));
    p.setFont(font(11));
    QString status = w.entries.size() == 1 ? QString("1 item") : QString::number(w.entries.size()) + " items";
    if (w.available >= 0)
        status += ", " + sizeText(w.available) + " available";
    p.setPen(QColor(255, 255, 255, 120));
    Aqua::drawText(p, QRectF(sb).translated(0, 1), Qt::AlignCenter, status);
    p.setPen(QColor(40, 40, 40));
    Aqua::drawText(p, QRectF(sb), Qt::AlignCenter, status);
    p.end();
    return img;
}

QImage invalidPicture(QSize size)
{
    const QString r = "/System/Library/PrivateFrameworks/Backup.framework/Versions/A/Resources/Invalid";
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.fillRect(QRect(11, 11, size.width() - 22, size.height() - 22), QColor::fromRgbF(0.149, 0.149, 0.149, 0.85));
    const QImage tl = Assets::image(r + "TopLeft.png"), tr = Assets::image(r + "TopRight.png"), bl = Assets::image(r + "BottomLeft.png"),
                 br = Assets::image(r + "BottomRight.png"), top = Assets::image(r + "Top.png"), bottom = Assets::image(r + "Bottom.png"),
                 left = Assets::image(r + "Left.png"), right = Assets::image(r + "Right.png");
    const int W = size.width(), H = size.height();
    p.drawImage(QRect(tl.width(), 0, W - tl.width() - tr.width(), top.height()), top);
    p.drawImage(QRect(bl.width(), H - bottom.height(), W - bl.width() - br.width(), bottom.height()), bottom);
    p.drawImage(QRect(0, tl.height(), left.width(), H - tl.height() - bl.height()), left);
    p.drawImage(QRect(W - right.width(), tr.height(), right.width(), H - tr.height() - br.height()), right);
    p.drawImage(0, 0, tl);
    p.drawImage(W - tr.width(), 0, tr);
    p.drawImage(0, H - bl.height(), bl);
    p.drawImage(W - br.width(), H - br.height(), br);
    return img;
}

} // namespace TM
