// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aqua.h"
#include "stack.h"

#include "assets.h"
#include "x11.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

namespace Stacks {

namespace {

const QString DockRes = "/System/Library/CoreServices/Dock.app/Contents/Resources/";
const QString CoreTypes = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/";

QString configFile() { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/dock.conf"; }

// the Dock's default.plist: persistent-others' tile-data by its "home directory relative" path
QJsonObject defaultTile(const QString &folder)
{
    const QString home = QDir::homePath();
    QString relative = folder == "applications:" ? "/Applications" : folder;
    if (relative.startsWith(home))
        relative = "~" + relative.mid(home.size());
    const QJsonArray others = Assets::plist(DockRes + "English.lproj/default.plist")["persistent-others"].toArray();
    for (const QJsonValue &t : others) {
        const QJsonObject data = t["tile-data"].toObject();
        if (data["home directory relative"].toString() == relative)
            return data;
    }
    return {};
}

QString key(const QString &folder) { return "stacks/" + name(folder).replace(' ', '_'); }

} // namespace

QString name(const QString &folder)
{
    if (folder.startsWith("applications:"))
        return folder == "applications:" ? QStringLiteral("Applications") : QFileInfo(folder.mid(13)).fileName();
    return QFileInfo(folder).fileName();
}

Settings settings(const QString &folder)
{
    const QJsonObject d = defaultTile(folder);
    QSettings q(configFile(), QSettings::IniFormat);
    const QString k = key(folder);
    Settings s;
    s.sort = Sort(qBound(1, q.value(k + "-arrangement", d["arrangement"].toInt(2)).toInt(), 5));
    s.asFolder = q.value(k + "-displayas", d["displayas"].toInt(0)).toInt() == 1;
    s.view = View(qBound(0, q.value(k + "-showas", d["showas"].toInt(0)).toInt(), 3));
    return s;
}

void save(const QString &folder, const Settings &s)
{
    QSettings q(configFile(), QSettings::IniFormat);
    const QString k = key(folder);
    q.setValue(k + "-arrangement", int(s.sort));
    q.setValue(k + "-displayas", s.asFolder ? 1 : 0);
    q.setValue(k + "-showas", int(s.view));
}

// ---- items

QVector<Item> items(const QString &folder, Sort sort)
{
    QVector<Item> out;
    if (folder.startsWith("applications:")) {
        // the Mac applications that a Linux program stands for, and their Utilities folder
        const QString sub = folder.mid(13);   // "" or "/Utilities"
        const QString dir = "/Applications" + sub;
        QSet<QString> standing;   // the Linux programs standing for a Mac application
        for (const QString &bundle : QDir(Assets::path(dir)).entryList({"*.app"}, QDir::Dirs, QDir::Name)) {
            const App a = Apps::forMacBundle(dir + '/' + bundle);
            if (!a.valid())
                continue;
            standing.insert(a.id);
            Item i;
            i.name = bundle.chopped(4);
            i.app = a;
            i.kind = "Application";
            out.append(i);
        }
        if (sub.isEmpty()) {
            // the other Linux applications, beside the Mac ones (as the Mac's /Applications has the
            // ones you installed), with the names and icons of their .desktop entries
            QSet<QString> all = standing;
            for (const QString &bundle : QDir(Assets::path("/Applications/Utilities")).entryList({"*.app"}, QDir::Dirs))
                if (const App a = Apps::forMacBundle("/Applications/Utilities/" + bundle); a.valid())
                    all.insert(a.id);
            all.insert(Apps::finder().id);
            for (const App &a : Apps::all()) {
                if (!a.shown || all.contains(a.id) || a.name.isEmpty())
                    continue;
                Item i;
                i.name = a.name;
                i.app = a;
                i.kind = "Application";
                out.append(i);
            }
        }
        if (sub.isEmpty() && !items("applications:/Utilities", Name).isEmpty()) {
            Item u;
            u.name = "Utilities";
            u.path = "applications:/Utilities";
            u.folder = true;
            u.kind = "Folder";
            out.append(u);
        }
    } else {
        QMimeDatabase mimes;
        for (const QFileInfo &fi : QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            Item i;
            i.name = fi.isDir() ? fi.fileName() : fi.fileName();
            i.path = fi.absoluteFilePath();
            i.folder = fi.isDir();
            // "date added": when the file came into the folder (its birth, else its last change)
            i.created = fi.birthTime().isValid() ? fi.birthTime() : fi.lastModified();
            i.added = fi.birthTime().isValid() ? fi.birthTime() : fi.metadataChangeTime();
            i.modified = fi.lastModified();
            i.kind = fi.isDir() ? QStringLiteral("Folder") : mimes.mimeTypeForFile(fi).comment();
            out.append(i);
        }
    }
    QCollator c;
    c.setNumericMode(true);
    c.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(out.begin(), out.end(), [&](const Item &a, const Item &b) {
        switch (sort) {
        case DateAdded: return a.added > b.added;   // the latest first
        case DateModified: return a.modified > b.modified;
        case DateCreated: return a.created > b.created;
        case Kind: return a.kind != b.kind ? c.compare(a.kind, b.kind) < 0 : c.compare(a.name, b.name) < 0;
        case Name:
        default: return c.compare(a.name, b.name) < 0;
        }
    });
    return out;
}

// ---- icons

QImage folderIcon(const QString &folder, int size)
{
    QString icns = "GenericFolderIcon.icns";
    if (folder == "applications:")
        icns = "ApplicationsFolderIcon.icns";
    else if (folder == "applications:/Utilities")
        icns = "UtilitiesFolder.icns";
    else if (folder == QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
        icns = "DownloadsFolder.icns";
    else if (folder == QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        icns = "DocumentsFolderIcon.icns";
    QImage img = Assets::image(CoreTypes + icns, QSize(size, size));
    return img.isNull() ? QIcon::fromTheme("folder").pixmap(size).toImage() : img;
}

QImage itemIcon(const Item &item, int size)
{
    static QHash<QString, QImage> cache;
    const QString k = (item.app.valid() ? "app:" + item.app.id : item.path) + '@' + QString::number(item.modified.toSecsSinceEpoch()) + '@' +
                      QString::number(size);
    auto it = cache.constFind(k);
    if (it != cache.constEnd())
        return *it;
    QImage img;
    const QString suffix = QFileInfo(item.path).suffix().toLower();
    static const QStringList pictures = {"jpg", "jpeg", "png", "gif", "bmp", "tif", "tiff", "webp"};
    if (item.app.valid()) {
        img = Apps::icon(item.app, size);
    } else if (item.folder) {
        img = folderIcon(item.path, size);
    } else if (pictures.contains(suffix)) {
        // a picture: its thumbnail, as the Finder's icon previews
        QImageReader r(item.path);
        r.setAutoTransform(true);
        const QSize own = r.size();
        if (own.isValid())
            r.setScaledSize(own.scaled(size, size, Qt::KeepAspectRatio));
        const QImage thumb = r.read();
        if (!thumb.isNull()) {
            img = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            const QRect at(QPoint((size - thumb.width()) / 2, (size - thumb.height()) / 2), thumb.size());
            p.fillRect(at.adjusted(-1, -1, 1, 1), QColor(0, 0, 0, 60));
            p.drawImage(at, thumb);
        }
    }
    if (img.isNull()) {
        // the documents' icons of Preview and TextEdit, by their types
        static const QHash<QString, QString> documents = {
            {"pdf", "Preview.app/Contents/Resources/pdf.icns"}, {"ps", "Preview.app/Contents/Resources/ps.icns"},
            {"eps", "Preview.app/Contents/Resources/eps.icns"}, {"psd", "Preview.app/Contents/Resources/psd.icns"},
            {"jpg", "Preview.app/Contents/Resources/jpeg.icns"}, {"jpeg", "Preview.app/Contents/Resources/jpeg.icns"},
            {"png", "Preview.app/Contents/Resources/png.icns"}, {"gif", "Preview.app/Contents/Resources/gif.icns"},
            {"tif", "Preview.app/Contents/Resources/tiff.icns"}, {"tiff", "Preview.app/Contents/Resources/tiff.icns"},
            {"bmp", "Preview.app/Contents/Resources/bmp.icns"}, {"tga", "Preview.app/Contents/Resources/tga.icns"},
            {"ico", "Preview.app/Contents/Resources/ico.icns"}, {"icns", "Preview.app/Contents/Resources/icns.icns"},
            {"vcf", "../System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/VCard.icns"},
            {"txt", "TextEdit.app/Contents/Resources/txt.icns"}, {"md", "TextEdit.app/Contents/Resources/txt.icns"},
            {"log", "TextEdit.app/Contents/Resources/txt.icns"}, {"rtf", "TextEdit.app/Contents/Resources/rtf.icns"},
            {"html", "TextEdit.app/Contents/Resources/html.icns"}, {"htm", "TextEdit.app/Contents/Resources/html.icns"},
            // music: iTunes'; films: QuickTime Player's; fonts: Font Book's; mail: Mail's
            {"mp3", "iTunes.app/Contents/Resources/iTunes-mp3.icns"}, {"mpga", "iTunes.app/Contents/Resources/iTunes-mp3.icns"},
            {"m4a", "iTunes.app/Contents/Resources/iTunes-aac.icns"}, {"aac", "iTunes.app/Contents/Resources/iTunes-aac.icns"},
            {"wav", "iTunes.app/Contents/Resources/iTunes-wav.icns"}, {"aiff", "iTunes.app/Contents/Resources/iTunes-aiff.icns"},
            {"aif", "iTunes.app/Contents/Resources/iTunes-aiff.icns"}, {"flac", "iTunes.app/Contents/Resources/iTunes-generic.icns"},
            {"ogg", "iTunes.app/Contents/Resources/iTunes-generic.icns"}, {"m3u", "iTunes.app/Contents/Resources/iTunes-playlist.icns"},
            {"mp4", "QuickTime Player.app/Contents/Resources/MPEG-4.icns"}, {"m4v", "QuickTime Player.app/Contents/Resources/MPEG-4.icns"},
            {"mov", "QuickTime Player.app/Contents/Resources/QuickTime Movie.icns"}, {"avi", "QuickTime Player.app/Contents/Resources/AVI.icns"},
            {"mpg", "QuickTime Player.app/Contents/Resources/MPEG.icns"}, {"mpeg", "QuickTime Player.app/Contents/Resources/MPEG.icns"},
            {"mkv", "QuickTime Player.app/Contents/Resources/Movie.icns"}, {"webm", "QuickTime Player.app/Contents/Resources/Movie.icns"},
            {"3gp", "QuickTime Player.app/Contents/Resources/3GPP.icns"}, {"dv", "QuickTime Player.app/Contents/Resources/DV.icns"},
            {"ttf", "Font Book.app/Contents/Resources/ttf.icns"}, {"otf", "Font Book.app/Contents/Resources/otf.icns"},
            {"ttc", "Font Book.app/Contents/Resources/ttc.icns"}, {"dfont", "Font Book.app/Contents/Resources/dfont.icns"},
            {"eml", "Mail.app/Contents/Resources/document.icns"}, {"mbox", "Mail.app/Contents/Resources/mbox.icns"},
            {"webarchive", "Safari.app/Contents/Resources/webarchive.icns"}};
        auto d = documents.constFind(suffix);
        // a download that hasn't finished: Safari's icon with its progress
        static const QStringList partial = {"part", "crdownload", "download"};
        if (partial.contains(suffix))
            img = Assets::image("/Applications/Safari.app/Contents/Resources/download5.icns", QSize(size, size));
        if (img.isNull() && d != documents.constEnd())
            img = Assets::image("/Applications/" + *d, QSize(size, size));
        else if (img.isNull() && (suffix == "desktop" || QFileInfo(item.path).isExecutable()))
            img = Assets::image(CoreTypes + "GenericApplicationIcon.icns", QSize(size, size));
    }
    if (img.isNull())
        img = Assets::image(CoreTypes + "GenericDocumentIcon.icns", QSize(size, size));
    if (img.isNull())
        img = QIcon::fromTheme(QMimeDatabase().mimeTypeForFile(item.path).iconName()).pixmap(size).toImage();
    cache.insert(k, img);
    return img;
}

QImage pile(const QString &folder, const Settings &s, int size)
{
    const QVector<Item> list = items(folder, s.sort);
    if (list.isEmpty())
        return folderIcon(folder, size);
    // the first three, the first on top; those under it a little turned and shifted
    QImage out(size, size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
    static const qreal angles[] = {0, -7, 6};
    static const qreal shifts[] = {0, -0.04, 0.05};
    const int n = qMin(3, int(list.size()));
    const int icon = qRound(size * 0.86);
    for (int i = n - 1; i >= 0; --i) {
        const QImage img = itemIcon(list[i], icon);
        p.save();
        p.translate(size / 2.0 + shifts[i] * size, size - icon / 2.0 - i * size * 0.02);
        p.rotate(angles[i]);
        p.drawImage(QRectF(-icon / 2.0, -icon / 2.0, icon, icon), img);
        p.restore();
    }
    return out;
}

void installTemplate()
{
    QSettings q(configFile(), QSettings::IniFormat);
    if (q.value("stacks/template-installed", false).toBool())
        return;
    const QString templ = "/System/Library/User Template/English.lproj/";
    bool any = false;
    for (auto [loc, file] : {std::pair{QStandardPaths::DocumentsLocation, "Documents/About Stacks.pdf"},
                             std::pair{QStandardPaths::DownloadLocation, "Downloads/About Downloads.pdf"}}) {
        const QString source = Assets::path(templ + file), dir = QStandardPaths::writableLocation(loc);
        if (!QFileInfo::exists(source) || dir.isEmpty())
            continue;
        any = true;
        QDir().mkpath(dir);
        const QString target = dir + '/' + QFileInfo(source).fileName();
        if (!QFileInfo::exists(target) && QFile::copy(source, target))
            QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    }
    // once: deleting them keeps them away (without the DVD's files, nothing is marked)
    if (any)
        q.setValue("stacks/template-installed", true);
}

void open(const Item &item)
{
    if (item.app.valid())
        Apps::launch(item.app);
    else if (item.folder && !item.path.startsWith("applications:"))
        Apps::launch(Apps::finder(), {item.path});
    else if (!item.path.isEmpty())
        Apps::openFile(item.path);
}

// ---- the fan and the grid

namespace {

// 10.6's fan, by eye: 48 point icons 54 apart going up from the tile, bending right (the top one some
// 100 points over), fifteen at most; the names on dark lozenges on their left
constexpr int FanIcon = 48, FanPitch = 54, FanMax = 15;
// the grid: 64 point icons in 100 x 98 cells, the names (two lines) under them, eight columns at most
constexpr int GridIcon = 56, CellW = 96, CellH = 94, GridMaxColumns = 8, Header = 34, Footer = 34;
// the panel's art: its pieces 40 points, its body (where the content goes) 16 in from the edges
constexpr int Inset = 18, TipHeight = 14;

QImage piece(const char *name) { return Assets::image(DockRes + QString::fromLatin1(name)); }

qreal ease(qreal t) { return 1 - std::pow(1 - t, 3); }

} // namespace

Popup::Popup(const QString &folder, View view, const QImage &wallpaper, QPoint tileCentre, int tileTop)
    : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_root(folder), m_folder(folder), m_view(view),
      m_wallpaper(wallpaper), m_tileCentre(tileCentre), m_tileTop(tileTop)
{
    setAttribute(Qt::WA_DeleteOnClose);
    if (X11::compositing())
        setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFont(Assets::uiFont(13));
    m_anim = new QTimer(this);
    m_anim->setInterval(16);
    connect(m_anim, &QTimer::timeout, this, [this] {
        m_progress = qMin(1.0, m_progress + 16.0 / (m_view == Fan ? 220 : 160));
        update();
        if (m_progress >= 1)
            m_anim->stop();
    });
    show(folder);
}

void Popup::show(const QString &folder)
{
    m_folder = folder;
    m_items = items(folder, settings(m_root).sort);
    m_hover = -1;
    m_scroll = 0;
    if (m_view == Fan)
        layoutFan();
    else
        layoutGrid();
    m_progress = 0;
    m_anim->start();
    QWidget::show();
    activateWindow();
    setFocus();
    update();
}

int fanCapacity(QPoint tileCentre, int tileTop)
{
    // as many as fit between the tile and the menu bar (22 points, and 8 under it), fifteen at most
    QScreen *s = QGuiApplication::screenAt(tileCentre);
    const int screenTop = s ? s->geometry().top() : 0;
    const int room = tileTop - 10 - FanIcon - (screenTop + 22 + 8);
    return qBound(1, room / FanPitch + 1, FanMax);
}

void Popup::layoutFan()
{
    m_cells.clear();
    const int total = int(m_items.size());
    // more than fit: the last place is "N More in Finder"
    const int capacity = fanCapacity(m_tileCentre, m_tileTop);
    const int shown = total > capacity ? capacity - 1 : total;
    m_more = total - shown;
    // global positions first, then the window around them
    QVector<QPointF> centres;
    QFontMetricsF fm(font());
    qreal left = m_tileCentre.x(), right = m_tileCentre.x(), top = m_tileTop;
    const int n = shown + (m_more ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        const qreal bend = 0.5 * i * i;   // to the right, more the higher
        const QPointF c(m_tileCentre.x() + bend, m_tileTop - 10 - FanIcon / 2.0 - i * FanPitch);
        Cell cell;
        if (i < shown)
            cell.item = m_items[i];
        else
            cell.item.name = Assets::tr("/System/Library/CoreServices/Dock.app", "Localizable", "MORE_IN_FINDER", "%@ More in Finder")
                                 .replace("%@", QString::number(m_more));
        cell.angle = qRadiansToDegrees(qAtan(i / (FanPitch / 1.0)));
        // the lozenge: the name and 10 points either side (260 at most: longer names lose their middle)
        const qreal tw = std::ceil(qMin(260.0, fm.horizontalAdvance(cell.item.name))) + 24;
        cell.icon = QRectF(c.x() - FanIcon / 2.0, c.y() - FanIcon / 2.0, FanIcon, FanIcon);
        cell.label = QRectF(cell.icon.left() - 8 - tw, c.y() - 11, tw, 22);
        left = qMin(left, cell.label.left() - 12);
        right = qMax(right, cell.icon.right() + 16);
        top = qMin(top, cell.icon.top() - 16);
        m_cells.append(cell);
    }
    const QRect screen = QGuiApplication::screenAt(m_tileCentre)->geometry();
    QRect r(QPoint(int(left), int(top)), QPoint(int(right), m_tileTop + 4));
    r = r.intersected(screen);
    setGeometry(r);
    for (Cell &c : m_cells) {
        c.icon.translate(-r.left(), -r.top());
        c.label.translate(-r.left(), -r.top());
        if (c.item.name.isEmpty())
            continue;
        c.image = c.item.path.isEmpty() && !c.item.app.valid()
                      ? piece("openinfinder.png").scaled(FanIcon, FanIcon, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)   // "N More in Finder"
                      : itemIcon(c.item, FanIcon);
    }
}

void Popup::layoutGrid()
{
    m_cells.clear();
    m_more = 0;
    const QRect screen = QGuiApplication::screenAt(m_tileCentre)->geometry();
    const int n = qMax(1, int(m_items.size()));
    // the columns: about square, wider when there are many, as many as fit
    int cols = qBound(1, int(std::ceil(std::sqrt(n * 1.3))), GridMaxColumns);
    cols = qMin(cols, n);
    cols = qMin(cols, (screen.width() - 2 * Inset - 40) / CellW);
    const int rows = (n + cols - 1) / cols;
    const int maxBody = m_tileTop - screen.top() - 40 - Header - Footer - 2 * Inset - TipHeight;
    const int bodyH = qMin(rows * CellH, qMax(CellH, maxBody / CellH * CellH));
    m_contentHeight = rows * CellH;
    // wide enough for its name, and the way back beside it in a subfolder
    const int least = m_folder != m_root ? 2 * 150 + QFontMetrics(Assets::uiFont(13, true)).horizontalAdvance(name(m_folder)) : 200;
    const int w = qMax(cols * CellW + 2 * Inset + 24, least), h = bodyH + Header + Footer + 2 * Inset + TipHeight;
    int x = m_tileCentre.x() - w / 2;
    x = qBound(screen.left() + 4, x, screen.right() - w - 4);
    setGeometry(x, m_tileTop - h + 4, w, h);
    m_tipX = m_tileCentre.x() - x;
    m_panel = QRectF(0, 0, w, h - TipHeight + 6);
    m_body = QRectF((w - cols * CellW) / 2.0, Inset + Header, cols * CellW, bodyH);
    m_backButton = m_folder != m_root ? QRectF(Inset + 8, Inset + 4, 140, 22) : QRectF();
    // the Applications stack's folders aren't the Finder's (the Mac applications stand for Linux programs)
    m_finderButton = m_folder.startsWith("applications:") ? QRectF() : QRectF(w - Inset - 162, m_body.bottom() + 2, 140, 22);   // clear of the corner
    for (int i = 0; i < m_items.size(); ++i) {
        Cell c;
        c.item = m_items[i];
        const QRectF cell(m_body.left() + (i % cols) * CellW, m_body.top() + (i / cols) * CellH, CellW, CellH);
        c.icon = QRectF(cell.center().x() - GridIcon / 2.0, cell.top() + 6, GridIcon, GridIcon);
        c.label = QRectF(cell.left() + 4, c.icon.bottom() + 3, CellW - 8, 30);
        c.image = itemIcon(c.item, GridIcon);
        m_cells.append(c);
    }
}

int Popup::cellAt(QPointF p) const
{
    if (m_view == Grid) {
        if (!m_body.contains(p))
            return -1;
        p.ry() += m_scroll;
    }
    for (int i = 0; i < m_cells.size(); ++i) {
        const Cell &c = m_cells[i];
        if (c.icon.adjusted(-6, -4, 6, 4).contains(p) || (m_view == Fan ? c.label.contains(p) : QRectF(c.icon.left() - 14, c.icon.top(), c.icon.width() + 28, c.label.bottom() - c.icon.top()).contains(p)))
            return i;
    }
    return -1;
}

void Popup::paintEvent(QPaintEvent *)
{
    if (X11::compositing() || m_wallpaper.isNull()) {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        if (m_view == Fan)
            paintFan(p);
        else
            paintGrid(p);
        return;
    }
    // nothing composites: drawn apart, the window shaped to it, over the desktop picture (as the Dock)
    QImage content(size() * devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    content.setDevicePixelRatio(devicePixelRatioF());
    content.fill(Qt::transparent);
    {
        QPainter cp(&content);
        cp.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
        if (m_view == Fan)
            paintFan(cp);
        else
            paintGrid(cp);
    }
    X11::shapeToContent(this, content);
    QPainter p(this);
    const QPoint at = mapToGlobal(QPoint(0, 0)) - screen()->geometry().topLeft();
    p.drawImage(QPoint(0, 0), m_wallpaper, QRect(at, size()));
    p.drawImage(QPoint(0, 0), content);
}

void Popup::paintFan(QPainter &p)
{
    const qreal e = ease(m_progress);
    const QPointF tile = mapFromGlobal(QPoint(m_tileCentre.x(), m_tileTop));
    static const QImage shadow = piece("stackitemshadow.png");
    for (int i = 0; i < m_cells.size(); ++i) {
        const Cell &c = m_cells[i];
        // from the tile to its place, the farther ones later
        const qreal t = qBound(0.0, (e * 1.25) - i * 0.25 / qMax(1, int(m_cells.size())), 1.0);
        const QPointF to = c.icon.center(), from(tile.x(), tile.y() - FanIcon / 2.0);
        const QPointF at = from + (to - from) * t;
        p.save();
        p.setOpacity(t);
        p.translate(at);
        p.rotate(c.angle * t);
        p.translate(-to);
        // the name: white on a dark lozenge (the Dock's item shadow under it)
        if (!shadow.isNull())
            p.drawImage(c.label.adjusted(-10, -10, 10, 10), shadow);
        QPainterPath lozenge;
        lozenge.addRoundedRect(c.label, 11, 11);
        p.fillPath(lozenge, i == m_hover ? QColor(30, 80, 190, 235) : QColor(28, 28, 28, 200));
        p.setPen(Qt::white);
        Aqua::drawText(p, c.label.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignHCenter,
                   QFontMetrics(font()).elidedText(c.item.name, Qt::ElideMiddle, int(c.label.width() - 16)));
        if (!c.image.isNull())
            p.drawImage(c.icon, c.image);
        if (i == m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 50));
            p.drawRoundedRect(c.icon.adjusted(-3, -3, 3, 3), 6, 6);
        }
        p.restore();
    }
}

void Popup::paintGrid(QPainter &p)
{
    p.setOpacity(ease(m_progress));
    // the panel: nine pieces, the tip in the bottom row under the tile
    const QImage tl = piece("stackbackground-tl.png"), tm = piece("stackbackground-tm.png"), tr = piece("stackbackground-tr.png"),
                 l = piece("stackbackground-l.png"), c = piece("stackbackground-c.png"), r = piece("stackbackground-r.png"),
                 bl = piece("stackbackground-bl.png"), bm = piece("stackbackground-bm.png"), br = piece("stackbackground-br.png"),
                 bt = piece("stackbackground-bt.png");
    const int W = width(), H = height();
    if (tl.isNull()) {
        p.setPen(QColor(255, 255, 255, 120));
        p.setBrush(QColor(25, 25, 25, 204));
        p.drawRoundedRect(QRectF(8, 8, W - 16, H - TipHeight - 16), 10, 10);
    } else {
        const int cw = tl.width(), th = tl.height(), bh = bl.height();
        p.drawImage(QRect(0, 0, cw, th), tl);
        p.drawImage(QRect(cw, 0, W - 2 * cw, th), tm);
        p.drawImage(QRect(W - cw, 0, cw, th), tr);
        p.drawImage(QRect(0, th, cw, H - th - bh), l);
        p.drawImage(QRect(cw, th, W - 2 * cw, H - th - bh), c);
        p.drawImage(QRect(W - cw, th, cw, H - th - bh), r);
        p.drawImage(QRect(0, H - bh, cw, bh), bl);
        p.drawImage(QRect(W - cw, H - bh, cw, bh), br);
        const int tx = qBound(cw, m_tipX - bt.width() / 2, W - cw - bt.width());
        p.drawImage(QRect(cw, H - bh, tx - cw, bh), bm);
        p.drawImage(QPoint(tx, H - bh), bt);
        p.drawImage(QRect(tx + bt.width(), H - bh, W - cw - tx - bt.width(), bh), bm);
    }
    // the folder's name over it; back to the folder it came from
    QFont title = Assets::uiFont(13, true);
    p.setFont(title);
    p.setPen(Qt::white);
    Aqua::drawText(p, QRectF(Inset, Inset + 2, W - 2 * Inset, 24), Qt::AlignCenter, name(m_folder));
    p.setFont(font());
    if (!m_backButton.isNull()) {
        p.setPen(QColor(255, 255, 255, 220));
        Aqua::drawText(p, m_backButton, Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8("◀ ") + name(m_root));
    }
    // the items, scrolled inside the body
    p.save();
    p.setClipRect(m_body.adjusted(-12, 0, 12, 0));
    p.translate(0, -m_scroll);
    QFont small = Assets::uiFont(11);
    for (int i = 0; i < m_cells.size(); ++i) {
        const Cell &cell = m_cells[i];
        if (i == m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 45));
            p.drawRoundedRect(QRectF(cell.icon.left() - 14, cell.icon.top() - 4, cell.icon.width() + 28, cell.label.bottom() - cell.icon.top() + 6), 7, 7);
        }
        if (!cell.image.isNull())
            p.drawImage(cell.icon, cell.image);
        p.setFont(small);
        p.setPen(Qt::white);
        const QFontMetrics fm(small);
        // two lines at most, the second elided in its middle
        QString text = cell.item.name, second;
        if (fm.horizontalAdvance(text) > cell.label.width()) {
            int cut = text.size();
            while (cut > 1 && fm.horizontalAdvance(text.left(cut)) > cell.label.width())
                --cut;
            const int space = text.left(cut).lastIndexOf(' ');
            if (space > 0)
                cut = space + 1;
            second = fm.elidedText(text.mid(cut), Qt::ElideMiddle, int(cell.label.width()));
            text = text.left(cut).trimmed();
        }
        Aqua::drawText(p, QRectF(cell.label.left(), cell.label.top(), cell.label.width(), 14), Qt::AlignHCenter | Qt::AlignTop, text);
        if (!second.isEmpty())
            Aqua::drawText(p, QRectF(cell.label.left(), cell.label.top() + 14, cell.label.width(), 14), Qt::AlignHCenter | Qt::AlignTop, second);
    }
    p.restore();
    // the scroller's thumb on the right when it scrolls
    if (m_contentHeight > m_body.height()) {
        const qreal visible = m_body.height() / m_contentHeight;
        const qreal y = m_body.top() + (m_body.height() - m_body.height() * visible) * (m_scroll / (m_contentHeight - m_body.height()));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 90));
        p.drawRoundedRect(QRectF(W - Inset - 10, y, 5, m_body.height() * visible), 2.5, 2.5);
    }
    // Open in Finder: the words and the arrow
    if (m_finderButton.isNull())
        return;
    p.setFont(font());
    const bool hot = m_hover == -2;
    p.setPen(hot ? Qt::white : QColor(255, 255, 255, 200));
    const QString open = Assets::tr("/System/Library/CoreServices/Dock.app", "Localizable", "SHOW_IN_FINDER", "Open in Finder");
    Aqua::drawText(p, m_finderButton.adjusted(0, 0, -24, 0), Qt::AlignVCenter | Qt::AlignRight, open);
    // the Dock's openinfinder art: the curved arrow in its circle
    static const QImage arrow = piece("openinfinder.png").scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(hot ? 1.0 : 0.85);
    // its circle fills some two thirds of the picture: 24 points for a 16 point circle
    p.drawImage(QRectF(m_finderButton.right() - 21, m_finderButton.center().y() - 12, 24, 24), arrow);
    p.setOpacity(1);
}

void Popup::mouseMoveEvent(QMouseEvent *e)
{
    int hit = cellAt(e->position());
    if (hit < 0 && m_view == Grid && (m_finderButton.contains(e->position()) || m_backButton.contains(e->position())))
        hit = m_finderButton.contains(e->position()) ? -2 : -3;
    if (hit != m_hover) {
        m_hover = hit;
        update();
    }
}

void Popup::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

void Popup::mouseReleaseEvent(QMouseEvent *e)
{
    if (!rect().contains(e->position().toPoint())) {
        close();
        return;
    }
    if (m_view == Grid && m_finderButton.contains(e->position())) {
        open(Item{name(m_folder), m_folder.startsWith("applications:") ? QString() : m_folder, {}, true, {}, {}, {}, {}});
        close();
        return;
    }
    if (m_view == Grid && m_backButton.contains(e->position())) {
        show(m_root);
        return;
    }
    const int i = cellAt(e->position());
    if (i < 0)
        return;
    const Item item = m_cells[i].item;
    if (m_view == Fan && item.path.isEmpty() && !item.app.valid()) {
        // "N More in Finder"
        if (!m_folder.startsWith("applications:"))
            Apps::launch(Apps::finder(), {m_folder});
        close();
        return;
    }
    // a folder in the grid: its items in the same grid (10.6's)
    if (m_view == Grid && item.folder) {
        show(item.path);
        return;
    }
    open(item);
    close();
}

void Popup::wheelEvent(QWheelEvent *e)
{
    if (m_view != Grid)
        return;
    m_scroll = qBound(0.0, m_scroll - e->angleDelta().y() / 2.0, qMax(0.0, m_contentHeight - m_body.height()));
    QMouseEvent move(QEvent::MouseMove, e->position(), e->globalPosition(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    mouseMoveEvent(&move);
    update();
}

void Popup::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
        close();
    else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && m_hover >= 0) {
        open(m_cells[m_hover].item);
        close();
    } else
        QWidget::keyPressEvent(e);
}

} // namespace Stacks
