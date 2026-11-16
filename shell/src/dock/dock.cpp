// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dock.h"

#include "aqua.h"
#include "dockmenu.h"
#include "assets.h"
#include "session.h"
#include "stack.h"
#include "x11.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <cmath>
#include <functional>
#include <memory>

static const QString dockRes = QStringLiteral("/System/Library/CoreServices/Dock.app/Contents/Resources/");
static const QString coreTypes = QStringLiteral("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/");

static QString configFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/dock.conf";
}


Dock::Dock(QWidget *parent) : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_X11NetWmWindowTypeDock);
    setMouseTracking(true);
    setAcceptDrops(true);
    if (X11::compositing())
        setAttribute(Qt::WA_TranslucentBackground);
    m_clock.start();
    m_anim.setInterval(16);
    connect(&m_anim, &QTimer::timeout, this, [this] {
        bool busy = false;
        for (Tile &t : m_tiles) {
            if (t.bounceStart >= 0 && m_clock.elapsed() - t.bounceStart > 6000)
                t.bounceStart = -1;   // never came up: stop, as the Mac does
            if (t.bounceStart >= 0 && t.kind == Tile::Folder && m_clock.elapsed() - t.bounceStart > 600)
                t.bounceStart = -1;   // a stack hops once
            busy |= t.bounceStart >= 0;
        }
        // hiding slides
        qreal target = m_autohide && !underMouse() && m_pressed < 0 ? 1 : 0;
        if (!m_autohide)
            target = 0;
        if (!qFuzzyCompare(m_hideOffset + 1, target + 1)) {
            m_hideOffset = target > m_hideOffset ? qMin(target, m_hideOffset + 0.08) : qMax(target, m_hideOffset - 0.08);
            busy = true;
            if (qFuzzyCompare(m_hideOffset + 1, target + 1))
                placeWindow();
        }
        if (!busy)
            m_anim.stop();
        update();
    });
    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(400);
    connect(&m_hideTimer, &QTimer::timeout, this, [this] { m_anim.start(); });

    loadSettings();
    Stacks::installTemplate();   // a new account's PDFs about Stacks, once
    // The settings change from the Apple menu's Dock submenu (SystemUIServer) too.
    QDir().mkpath(QFileInfo(configFile()).path());
    m_watch.addPath(QFileInfo(configFile()).path());
    if (QFileInfo::exists(configFile()))
        m_watch.addPath(configFile());
    auto reload = [this] {
        QTimer::singleShot(100, this, [this] {
            if (QFileInfo::exists(configFile()) && !m_watch.files().contains(configFile()))
                m_watch.addPath(configFile());
            loadSettings();
            if (!X11::compositing())   // the desktop picture under the Dock, as it is now
                m_wallpaper = Assets::wallpaperFor(QGuiApplication::primaryScreen()->geometry().size());
            refreshWindows();
            placeWindow();
        });
    };
    connect(&m_watch, &QFileSystemWatcher::fileChanged, this, reload);
    connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, reload);

    connect(&m_stackWatch, &QFileSystemWatcher::directoryChanged, this, &Dock::foldersChanged);
    m_watcher = new X11::Watcher(this);
    connect(m_watcher, &X11::Watcher::changed, this, &Dock::refreshWindows);
    // The Trash's fullness
    QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/files";
    QDir().mkpath(trash);
    m_watch.addPath(trash);

    if (!X11::compositing())
        m_wallpaper = Assets::wallpaperFor(QGuiApplication::primaryScreen()->geometry().size());
    refreshWindows();
    placeWindow();
}

QString Dock::menuText(const char *key, const char *fallback) const
{
    return Assets::tr("/System/Library/CoreServices/Dock.app", "DockMenus", key, fallback);
}

void Dock::loadSettings()
{
    QSettings s(configFile(), QSettings::IniFormat);
    m_tileSize = qBound(16, s.value("tilesize", 64).toInt(), 128);
    m_largeSize = qBound(m_tileSize, s.value("largesize", 96).toInt(), 128);
    m_magnify = s.value("magnification", false).toBool();
    m_minimizeToApp = s.value("minimize-to-application", false).toBool();
    bool autohide = s.value("autohide", false).toBool();
    if (autohide != m_autohide) {
        m_autohide = autohide;
        m_anim.start();
    }
    if (s.contains("persistent-apps")) {
        m_kept = s.value("persistent-apps").toStringList();
    } else {
        // The first time: the Dock of a new 10.6 account, from the DVD's default.plist
        m_kept.clear();
        QJsonValue def = Assets::plist(dockRes + "English.lproj/default.plist");
        for (const QJsonValue &tile : def["persistent-apps"].toArray()) {
            QString url = tile["tile-data"]["file-data"]["_CFURLString"].toString();
            if (url.startsWith("file://"))
                url = QUrl(url).toLocalFile();
            if (url.endsWith(".app"))
                m_kept << "mac:" + url;
        }
        if (m_kept.isEmpty())
            m_kept = {"mac:/Applications/Mail.app", "mac:/Applications/Safari.app", "mac:/Applications/iTunes.app",
                      "mac:/Applications/Preview.app", "mac:/Applications/System Preferences.app"};
        m_kept.removeAll("mac:/Applications/Dashboard.app");   // Dashboard is for later
        saveApps();
    }
    m_iconCache.clear();
}

void Dock::saveApps()
{
    QSettings s(configFile(), QSettings::IniFormat);
    s.setValue("persistent-apps", m_kept);
}

static App appForEntry(const QString &entry)
{
    if (entry.startsWith("mac:"))
        return Apps::forMacBundle(entry.mid(4));
    if (entry.startsWith("app:")) {
        App a = Apps::byId(entry.mid(4));
        return a.valid() ? Apps::forWindow(a.wmClass, a.id, 0) : a;
    }
    return {};
}

void Dock::refreshWindows()
{
    // Who runs: the windows, by application
    QHash<QString, QVector<xcb_window_t>> windows;
    QHash<QString, App> running;
    QHash<xcb_window_t, App> minimized;   // those with a tile of their own
    QStringList order;
    for (xcb_window_t w : X11::clientWindows()) {
        App a = Apps::forWindow(X11::wmClass(w), X11::wmInstance(w), X11::pid(w));
        if (!running.contains(a.id))
            order << a.id;
        running.insert(a.id, a);
        windows[a.id].append(w);
        if (!m_minimizeToApp && X11::minimized(w) && !X11::hidden(w))
            minimized.insert(w, a);
    }
    // the minimized windows keep their places, a new one comes last (by the Trash); a window back
    // out of the Dock takes its picture with it
    for (int i = m_minimized.size() - 1; i >= 0; --i)
        if (!minimized.contains(m_minimized[i])) {
            const QString prefix = "win:" + QString::number(m_minimized[i]) + ':';
            for (auto it = m_iconCache.begin(); it != m_iconCache.end();)
                it = it.key().startsWith(prefix) ? m_iconCache.erase(it) : std::next(it);
            m_minimized.removeAt(i);
        }
    for (xcb_window_t w : X11::clientWindows())
        if (minimized.contains(w) && !m_minimized.contains(w))
            m_minimized.append(w);
    QHash<QString, qint64> bouncing;
    for (const Tile &t : m_tiles)
        if (t.bounceStart >= 0 && !windows.contains(t.app.id))
            bouncing.insert(t.app.id, t.bounceStart);

    QVector<Tile> tiles;
    QSet<QString> shown;
    auto add = [&](Tile t) {
        t.windows = windows.value(t.app.id);
        t.bounceStart = bouncing.value(t.app.id, -1);
        t.label = t.app.name;
        shown.insert(t.app.id);
        tiles.append(t);
    };
    Tile finder;
    finder.app = Apps::finder();
    add(finder);
    for (const QString &entry : m_kept) {
        Tile t;
        t.app = appForEntry(entry);
        t.entry = entry;
        if (t.app.valid() && !shown.contains(t.app.id))
            add(t);
    }
    for (const QString &id : order) {
        if (shown.contains(id))
            continue;
        Tile t;
        t.app = running.value(id);
        add(t);
    }
    Tile sep;
    sep.kind = Tile::Separator;
    tiles.append(sep);
    // the stacks of 10.6's default.plist: Applications, Documents, Downloads
    QStringList folders{"applications:"};
    for (auto loc : {QStandardPaths::DocumentsLocation, QStandardPaths::DownloadLocation}) {
        const QString dir = QStandardPaths::writableLocation(loc);
        if (QDir(dir).exists())
            folders << dir;
    }
    for (const QString &dir : folders) {
        Tile f;
        f.kind = Tile::Folder;
        f.folder = dir;
        f.label = Stacks::name(dir);
        tiles.append(f);
        if (!dir.startsWith("applications:") && !m_stackWatch.directories().contains(dir)) {
            m_stackWatch.addPath(dir);
            m_stackCounts[dir] = int(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size());
        }
    }
    for (xcb_window_t w : std::as_const(m_minimized)) {
        Tile m;
        m.kind = Tile::Window;
        m.window = w;
        m.app = minimized.value(w);
        m.label = X11::title(w);
        tiles.append(m);
    }
    Tile trash;
    trash.kind = Tile::Trash;
    trash.label = "Trash";
    tiles.append(trash);
    bool changedCount = tiles.size() != m_tiles.size();
    m_tiles = tiles;
    if (m_hover >= m_tiles.size())
        m_hover = -1;
    if (changedCount)
        placeWindow();
    layoutTiles();
    publishIconGeometry();
    update();
}

// A tile's icon on the screen, as laid out now
QRect Dock::tileRect(const Tile &t) const
{
    const qreal floorH = qRound(m_tileSize * 0.82);
    const qreal baseline = height() - floorH * 0.32;
    const QPoint topLeft = mapToGlobal(QPoint(qRound(t.x - t.size / 2), qRound(baseline - t.size)));
    return QRect(topLeft, QSize(qRound(t.size), qRound(t.size)));
}

// Where each window goes when it's minimized and comes from when it's back (_NET_WM_ICON_GEOMETRY,
// read by WindowServer for the genie): its own tile, or its application's. Written every time the
// tiles change, even unchanged: the genie waits for it
void Dock::publishIconGeometry()
{
    QHash<xcb_window_t, QRect> where;
    for (const Tile &t : std::as_const(m_tiles))
        if (t.kind == Tile::Application)
            for (xcb_window_t w : t.windows)
                where.insert(w, tileRect(t));
    for (const Tile &t : std::as_const(m_tiles))   // a minimized one's own tile over its application's
        if (t.kind == Tile::Window)
            where.insert(t.window, tileRect(t));
    for (auto it = where.cbegin(); it != where.cend(); ++it)
        X11::setIconGeometry(it.key(), it.value());
    xcb_flush(X11::conn());
}

bool Dock::trashFull() const
{
    QDir trash(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash/files");
    return !trash.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
}

QImage Dock::iconFor(const Tile &t, int size)
{
    QString key;
    switch (t.kind) {
    case Tile::Application: key = "app:" + t.app.id; break;
    case Tile::Folder: key = "folder:" + t.folder + ':' + QString::number(m_stackStamp); break;
    case Tile::Trash: key = trashFull() ? "trash:full" : "trash:empty"; break;
    case Tile::Window: key = "win:" + QString::number(t.window); break;
    case Tile::Separator: return {};
    }
    key += ':' + QString::number(size);
    auto it = m_iconCache.constFind(key);
    if (it != m_iconCache.constEnd())
        return *it;
    QImage img;
    if (t.kind == Tile::Application) {
        img = Apps::icon(t.app, size);
    } else if (t.kind == Tile::Window) {
        // 10.6's DOCKWindowTile: the window's picture in the tile, its application's icon over its
        // bottom right corner, 48 of the tile's 128, 2 from the edges
        img = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        const QImage shot = X11::preview(t.window);
        if (!shot.isNull()) {
            const QSizeF fit = QSizeF(shot.size()).scaled(size, size, Qt::KeepAspectRatio);
            p.drawImage(QRectF((size - fit.width()) / 2, (size - fit.height()) / 2, fit.width(), fit.height()), shot);
        }
        const int badge = int(std::ceil(size * 48 / 128.0)), margin = qMax(1, size * 2 / 128);
        const QImage icon = Apps::icon(t.app, badge);
        if (!icon.isNull())
            p.drawImage(QRect(size - badge - margin, size - badge - margin, badge, badge), icon);
    } else if (t.kind == Tile::Trash) {
        img = Assets::image(dockRes + (key.startsWith("trash:full") ? "trashfull.png" : "trashempty.png"), QSize(size, size));
    } else {
        // a stack: the pile of its first items, or its folder ("Display as Folder")
        const Stacks::Settings st = Stacks::settings(t.folder);
        img = st.asFolder ? Stacks::folderIcon(t.folder, size) : Stacks::pile(t.folder, st, size);
    }
    if (img.isNull()) {
        img = QIcon::fromTheme(t.kind == Tile::Trash ? "user-trash" : "folder").pixmap(size).toImage();
    }
    m_iconCache.insert(key, img);
    return img;
}

// ---- geometry

int Dock::tileAt(const QPointF &p) const
{
    for (int i = 0; i < m_tiles.size(); ++i) {
        const Tile &t = m_tiles[i];
        qreal half = t.size / 2 + qMax(2, m_tileSize / 24) / 2.0;
        if (p.x() >= t.x - half && p.x() < t.x + half && p.y() >= height() - t.size - m_tileSize * 0.5 - 4)
            return t.kind == Tile::Separator ? -1 : i;
    }
    return -1;
}

void Dock::layoutTiles()
{
    const qreal gap = qMax(2, m_tileSize / 24);
    const qreal range = 3.0 * (m_tileSize + gap);   // magnification reaches three icons each side
    // the sizes first, against the unmagnified positions
    qreal total = 0;
    QVector<qreal> base;
    for (const Tile &t : m_tiles) {
        qreal w = t.kind == Tile::Separator ? m_tileSize * 0.5 : m_tileSize;
        base.append(total + w / 2);
        total += w + gap;
    }
    qreal x0 = (width() - total + gap) / 2;
    qreal grown = 0;
    for (int i = 0; i < m_tiles.size(); ++i) {
        Tile &t = m_tiles[i];
        qreal s = t.kind == Tile::Separator ? m_tileSize * 0.5 : m_tileSize;
        if (m_magnify && m_mouseX >= 0 && t.kind != Tile::Separator) {
            qreal d = std::abs(m_mouseX - (x0 + base[i]));
            if (d < range)
                s += (m_largeSize - m_tileSize) * std::cos(d / range * M_PI / 2);
        }
        t.size = s;
        grown += s + gap;
    }
    // laid from the centre: the Dock grows on both sides
    qreal x = (width() - grown + gap) / 2;
    for (Tile &t : m_tiles) {
        t.x = x + t.size / 2;
        x += t.size + gap;
    }
}

void Dock::placeWindow()
{
    QRect screen = QGuiApplication::primaryScreen()->geometry();
    // room for the magnified icons, the bounce and the names over them
    int h = qMax(m_largeSize, m_tileSize) + m_tileSize + 40;
    setGeometry(screen.x(), screen.bottom() - h + 1, screen.width(), h);
    layoutTiles();
    winId();
    int floorH = qRound(m_tileSize * 0.82);
    X11::setDock(this, 0, m_autohide ? 0 : m_tileSize + floorH / 3 + 4);
    // Clicks go through the empty part, unless the pointer is in (names, magnification)
    if (m_tiles.isEmpty())
        return;
    qreal left = m_tiles.first().x - m_tiles.first().size - floorH, right = m_tiles.last().x + m_tiles.last().size + floorH;
    int band = m_tileSize + floorH / 3 + 4;
    const QRegion strip(0, h - 2, width(), 2), shelf(int(left), h - band - 4, int(right - left), band + 4);
    if (!m_wallpaper.isNull()) {
        // no compositor: the paint shapes the window to the Dock (what isn't drawn shows the windows
        // under it); hidden, its edge keeps the pointer
        m_keep = m_autohide && m_hideOffset > 0.99 ? strip : QRegion();
        update();
    } else if (m_autohide && m_hideOffset > 0.99)
        setMask(strip);
    else if (underMouse() || m_pressed >= 0)
        clearMask();
    else
        setMask(shelf);
}

// ---- drawing

void Dock::paintEvent(QPaintEvent *)
{
    if (m_wallpaper.isNull()) {
        QPainter p(this);
        paintDock(p);
        return;
    }
    // nothing composites: the Dock drawn apart, the window shaped to it (windows show around it, not
    // the desktop picture's rectangle), then over the desktop picture
    QImage content(size() * devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    content.setDevicePixelRatio(devicePixelRatioF());
    content.fill(Qt::transparent);
    {
        QPainter cp(&content);
        paintDock(cp);
    }
    X11::shapeToContent(this, content, m_keep);
    QPainter p(this);
    const QPoint origin = geometry().topLeft() - QGuiApplication::primaryScreen()->geometry().topLeft();
    p.drawImage(QPoint(0, 0), m_wallpaper, QRect(origin, size()));
    p.drawImage(QPoint(0, 0), content);
}

void Dock::paintDock(QPainter &p)
{
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const qreal dpr = devicePixelRatioF();
    if (m_tiles.isEmpty())
        return;
    const qreal shift = m_hideOffset * (m_tileSize * 1.6 + 12);
    p.translate(0, shift);

    const qreal H = height();
    // Proportions measured on 10.6's Dock: the floor is 0.82 of an icon deep, its back edge a tenth of an
    // icon past the outer icons, its front edge three quarters of its depth further out.
    const qreal floorH = qRound(m_tileSize * 0.82);
    const qreal margin = m_tileSize * 0.1;
    const qreal inset = floorH * 0.75;
    qreal backLeft = m_tiles.first().x - m_tiles.first().size / 2 - margin;
    qreal backRight = m_tiles.last().x + m_tiles.last().size / 2 + margin;
    qreal left = backLeft - inset, right = backRight + inset;
    QPolygonF floor({QPointF(backLeft, H - floorH), QPointF(backRight, H - floorH), QPointF(right, H), QPointF(left, H)});
    m_floor = floor.boundingRect();

    // The glass floor: the DVD's scurve texture mapped on the trapezoid
    QString texture = m_tileSize <= 40 ? "scurve-sm.png" : m_tileSize <= 64 ? "scurve-m.png" : m_tileSize <= 96 ? "scurve-l.png" : "scurve-xl.png";
    static QHash<QString, QImage> textures;
    if (!textures.contains(texture))
        textures.insert(texture, Assets::image(dockRes + texture));
    const QImage &tex = textures[texture];
    if (!tex.isNull()) {
        QTransform map;
        if (QTransform::quadToQuad(QPolygonF(QRectF(tex.rect())), floor, map)) {
            p.save();
            p.setTransform(map, true);
            p.drawImage(QPointF(0, 0), tex);
            p.restore();
        }
    } else {
        QLinearGradient g(0, H - floorH, 0, H);
        g.setColorAt(0, QColor(230, 230, 235, 200));
        g.setColorAt(1, QColor(150, 150, 160, 220));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPolygon(floor);
    }
    // its edges: a bright back rim, the front line
    p.setPen(QPen(QColor(255, 255, 255, 150), 1));
    p.drawLine(floor[0], floor[1]);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1));
    p.drawLine(floor[1], floor[2]);
    p.drawLine(floor[3], floor[0]);
    static QImage front = Assets::image(dockRes + "frontline.png");
    if (!front.isNull())
        p.drawImage(QRectF(left, H - 3, right - left, 3), front);
    else
        p.fillRect(QRectF(left, H - 2, right - left, 2), QColor(255, 255, 255, 120));

    const qreal baseline = H - floorH * 0.32;   // where the icons stand
    static QImage indicator = Assets::image(dockRes + "indicator_medium.png");
    static QImage indicatorSmall = Assets::image(dockRes + "indicator_small.png");
    static QImage indicatorLarge = Assets::image(dockRes + "indicator_large.png");
    static QImage separator = Assets::image(dockRes + "separator.png");
    for (int i = 0; i < m_tiles.size(); ++i) {
        Tile &t = m_tiles[i];
        if (t.kind == Tile::Separator) {
            QRectF r(t.x - t.size / 2, H - floorH, t.size, floorH);
            if (!separator.isNull())
                p.drawImage(r, separator);
            else
                p.fillRect(QRectF(t.x - 0.5, r.top(), 1, r.height()), QColor(255, 255, 255, 150));
            continue;
        }
        qreal bounce = 0;
        if (t.bounceStart >= 0) {
            qreal phase = (m_clock.elapsed() - t.bounceStart) / 600.0;
            bounce = std::abs(std::sin(phase * M_PI)) * m_tileSize * 0.6;
        }
        int px = qCeil(t.size * dpr);
        QImage icon = iconFor(t, qMax(m_largeSize, m_tileSize) * int(std::ceil(dpr)));
        QRectF ir(t.x - t.size / 2, baseline - t.size - bounce, t.size, t.size);
        if (i == m_dragging)
            p.setOpacity(0.5);
        if (!icon.isNull()) {
            // the reflection on the floor, faded
            p.save();
            p.setClipRegion(QRegion(floor.toPolygon()));
            p.setOpacity(0.22 * p.opacity());
            p.translate(0, baseline * 2 + 2 * bounce);
            p.scale(1, -1);
            p.drawImage(ir, icon);
            p.restore();
            p.drawImage(ir, icon);
        }
        Q_UNUSED(px);
        // pressed: darker
        if (i == m_pressed && m_dragging < 0 && !icon.isNull()) {
            QImage dark = icon.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter dp(&dark);
            dp.setCompositionMode(QPainter::CompositionMode_SourceAtop);
            dp.fillRect(dark.rect(), QColor(0, 0, 0, 110));
            dp.end();
            p.drawImage(ir, dark);
        }
        p.setOpacity(1);
        // the light under running applications
        // (the Finder always runs on the Mac: its light is always on)
        if (t.kind == Tile::Application && (!t.windows.isEmpty() || t.app.id == "finder")) {
            const QImage &ind = m_tileSize >= 60 ? indicatorLarge : m_tileSize >= 40 ? indicator : indicatorSmall;
            if (!ind.isNull()) {
                QSizeF s = ind.size();
                p.drawImage(QPointF(t.x - s.width() / 2, H - s.height()), ind);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(140, 200, 255));
                p.drawEllipse(QPointF(t.x, H - 4), 2.5, 2.5);
            }
        }
    }

    // The name over the icon under the pointer
    if (m_hover >= 0 && m_dragging < 0 && m_pressed < 0 && m_hideOffset < 0.01) {
        const Tile &t = m_tiles[m_hover];
        QFont f = Assets::uiFont(14);
        QFontMetricsF fm(f);
        qreal w = fm.horizontalAdvance(t.label) + 24, h = fm.height() + 6;
        QRectF label(t.x - w / 2, baseline - t.size - h - 10, w, h);
        label.moveLeft(qBound(4.0, label.left(), width() - w - 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 40));
        p.drawRoundedRect(label.translated(0, 1.5), h / 2, h / 2);
        p.setBrush(QColor(40, 40, 40, 200));
        p.drawRoundedRect(label, h / 2, h / 2);
        p.setFont(f);
        p.setPen(Qt::white);
        Aqua::drawText(p, label, Qt::AlignCenter, t.label);
    }
}

// ---- the pointer

void Dock::enterEvent(QEnterEvent *)
{
    m_hideTimer.stop();
    clearMask();   // the names and the magnified icons go over the band
    if (m_autohide)
        m_anim.start();
}

void Dock::leaveEvent(QEvent *)
{
    m_mouseX = -1;
    m_hover = -1;
    layoutTiles();
    if (m_autohide)
        m_hideTimer.start();
    placeWindow();
    update();
}

void Dock::mouseMoveEvent(QMouseEvent *e)
{
    QPointF pos = e->position();
    if (m_pressed >= 0 && m_dragging < 0 && (pos - m_pressPos).manhattanLength() > 8 &&
        m_tiles[m_pressed].kind == Tile::Application && m_pressed > 0)
        m_dragging = m_pressed;   // the Finder stays first
    if (m_dragging >= 0) {
        // Reordering: the dragged icon takes the place under the pointer, among the applications
        int target = tileAt(QPointF(pos.x(), height() - 2));
        if (target > 0 && target != m_dragging && m_tiles[target].kind == Tile::Application) {
            m_tiles.move(m_dragging, target);
            m_dragging = target;
            m_pressed = target;
        }
        layoutTiles();
        update();
        return;
    }
    if (pos.y() < height() - qMax(m_largeSize, m_tileSize) - m_tileSize * 0.6) {
        m_mouseX = -1;
        m_hover = -1;
    } else {
        m_mouseX = pos.x();
        layoutTiles();
        m_hover = tileAt(pos);
    }
    update();
}

void Dock::mousePressEvent(QMouseEvent *e)
{
    int i = tileAt(e->position());
    if (i < 0)
        return;
    if (e->button() == Qt::RightButton || (e->button() == Qt::LeftButton && e->modifiers() & Qt::ControlModifier)) {
        showMenu(i, e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    m_pressed = i;
    m_pressPos = e->position();
    // Held down: the menu, as the Mac does after a moment
    QTimer::singleShot(700, this, [this, i] {
        if (m_pressed == i && m_dragging < 0) {
            m_pressed = -1;
            showMenu(i, QCursor::pos());
        }
    });
    update();
}

void Dock::mouseReleaseEvent(QMouseEvent *e)
{
    int pressed = m_pressed;
    m_pressed = -1;
    if (m_dragging >= 0) {
        Tile t = m_tiles[m_dragging];
        m_dragging = -1;
        bool out = e->position().y() < height() - qMax(m_largeSize, m_tileSize) - m_tileSize;
        // the new order of the kept applications; dragged out of the Dock: no longer kept
        QStringList kept;
        for (const Tile &k : m_tiles)
            if (k.kind == Tile::Application && !k.entry.isEmpty() && !(out && k.entry == t.entry))
                kept << k.entry;
        if (!out && t.entry.isEmpty() && t.app.id != "finder") {
            // a running application dropped among the kept ones: kept from now on
            kept.clear();
            for (const Tile &k : m_tiles) {
                if (k.kind != Tile::Application || k.app.id == "finder")
                    continue;
                if (!k.entry.isEmpty())
                    kept << k.entry;
                else if (k.app.id == t.app.id)
                    kept << (t.app.macBundle.isEmpty() ? "app:" + t.app.id : "mac:" + t.app.macBundle);
            }
        }
        m_kept = kept;
        saveApps();
        refreshWindows();
        return;
    }
    update();
    if (pressed >= 0 && pressed == tileAt(e->position()))
        activate(m_tiles[pressed]);
}

void Dock::activate(Tile &t)
{
    App finder = Apps::finder();
    switch (t.kind) {
    case Tile::Folder:
        showStack(int(&t - m_tiles.data()));
        return;
    case Tile::Trash:
        Apps::launch(finder, {"trash:///"});
        return;
    case Tile::Separator:
        return;
    case Tile::Window:   // out of the Dock
        X11::activate(t.window);
        return;
    case Tile::Application:
        break;
    }
    if (t.windows.isEmpty()) {
        if (t.app.id == "finder")
            Apps::launch(finder, {QDir::homePath()});
        else if (!Apps::launch(t.app))
            return;
        t.bounceStart = m_clock.elapsed();
        m_anim.start();
        return;
    }
    // Running: its windows come forward (the minimized ones too when none is shown)
    bool allMinimized = true;
    for (xcb_window_t w : t.windows)
        allMinimized &= X11::minimized(w);
    for (xcb_window_t w : t.windows)
        if (allMinimized || !X11::minimized(w))
            X11::activate(w);
}

// ---- stacks

void Dock::showStack(int index)
{
    if (index < 0 || index >= m_tiles.size())
        return;
    // a second click on its tile closes it
    if (m_stack) {
        const bool same = m_stackTile == index;
        m_stack->close();
        if (same)
            return;
    }
    const Tile &t = m_tiles[index];
    const Stacks::Settings st = Stacks::settings(t.folder);
    Stacks::View view = st.view;
    const QPoint centre = mapToGlobal(QPoint(int(t.x), height() - int(t.size / 2)));
    const int top = mapToGlobal(QPoint(0, height() - int(t.size) - m_tileSize / 6)).y();
    if (view == Stacks::Automatic)   // a fan while it fits (a dozen at most), a grid for more
        view = Stacks::items(t.folder, st.sort).size() <= qMin(12, Stacks::fanCapacity(centre, top)) ? Stacks::Fan : Stacks::Grid;
    if (view == Stacks::List) {
        // the list: a Dock menu of the items, a folder's items in its submenu
        auto *menu = new DockMenu(m_wallpaper, this);
        // its submenus filled when they open: the function outlives this call
        auto fill = std::make_shared<std::function<void(QMenu *, const QString &)>>();
        std::weak_ptr<std::function<void(QMenu *, const QString &)>> weak = fill;
        *fill = [weak, st](QMenu *m, const QString &folder) {
            for (const Stacks::Item &item : Stacks::items(folder, st.sort)) {
                const QIcon icon(QPixmap::fromImage(Stacks::itemIcon(item, 16)));
                if (item.folder) {
                    QMenu *sub = static_cast<DockMenu *>(m)->addDockMenu(item.name);
                    sub->setIcon(icon);
                    const QString path = item.path;
                    auto self = weak.lock();
                    QObject::connect(sub, &QMenu::aboutToShow, sub, [sub, path, self] {
                        if (sub->isEmpty())
                            (*self)(sub, path);
                    });
                } else {
                    m->addAction(icon, item.name, [item] { Stacks::open(item); });
                }
            }
        };
        (*fill)(menu, t.folder);
        if (!t.folder.startsWith("applications:")) {
            menu->addSeparator();
            const QString folder = t.folder;
            menu->addAction(Assets::tr("/System/Library/CoreServices/Dock.app", "Localizable", "SHOW_IN_FINDER", "Open in Finder"),
                            [folder] { Apps::launch(Apps::finder(), {folder}); });
        }
        menu->popupAbove(centre.x(), top);
        return;
    }
    auto *popup = new Stacks::Popup(t.folder, view, m_wallpaper, centre, top);
    m_stack = popup;
    m_stackTile = index;
}

void Dock::stackMenu(DockMenu *menu, const Tile &t)
{
    // Sort by, Display as, View content as (DockMenus.strings); Open "Downloads"
    const QString folder = t.folder;
    const Stacks::Settings st = Stacks::settings(folder);
    auto change = [this, folder](std::function<void(Stacks::Settings &)> f) {
        Stacks::Settings s = Stacks::settings(folder);
        f(s);
        Stacks::save(folder, s);
        ++m_stackStamp;
        update();
    };
    QMenu *sort = menu->addDockMenu(menuText("SORT_BY", "Sort by"));
    const std::pair<Stacks::Sort, const char *> sorts[] = {{Stacks::Name, "NAME"}, {Stacks::DateAdded, "DATE_ADDED"},
                                                           {Stacks::DateModified, "DATE_MODIFIED"}, {Stacks::DateCreated, "DATE_CREATED"},
                                                           {Stacks::Kind, "KIND"}};
    static const char *const sortNames[] = {"", "Name", "Date Added", "Date Modified", "Date Created", "Kind"};
    for (auto [value, k] : sorts) {
        QAction *a = sort->addAction(menuText(k, sortNames[value]), [change, value] { change([value](Stacks::Settings &s) { s.sort = value; }); });
        a->setCheckable(true);
        a->setChecked(st.sort == value);
    }
    QMenu *display = menu->addDockMenu(menuText("DISPLAY_AS", "Display as"));
    for (bool asFolder : {true, false}) {
        QAction *a = display->addAction(menuText(asFolder ? "DISPLAY_AS_FOLDER" : "DISPLAY_AS_STACK", asFolder ? "Folder" : "Stack"),
                                        [change, asFolder] { change([asFolder](Stacks::Settings &s) { s.asFolder = asFolder; }); });
        a->setCheckable(true);
        a->setChecked(st.asFolder == asFolder);
    }
    QMenu *view = menu->addDockMenu(menuText("SHOW_AS", "View content as"));
    const std::pair<Stacks::View, const char *> views[] = {{Stacks::Fan, "FAN"}, {Stacks::Grid, "GRID"}, {Stacks::List, "LIST"},
                                                           {Stacks::Automatic, "AUTOMATIC"}};
    static const char *const viewNames[] = {"Automatic", "Fan", "Grid", "List"};
    for (auto [value, k] : views) {
        if (value == Stacks::Automatic)
            view->addSeparator();
        QAction *a = view->addAction(menuText(k, viewNames[value]), [change, value] { change([value](Stacks::Settings &s) { s.view = value; }); });
        a->setCheckable(true);
        a->setChecked(st.view == value);
    }
    menu->addSeparator();
    if (!folder.startsWith("applications:"))
        menu->addAction(menuText("OPEN_FILENAME", "Open \u201c%@\u201d").replace("%@", t.label),
                        [folder] { Apps::launch(Apps::finder(), {folder}); });
}

void Dock::foldersChanged(const QString &path)
{
    // the pile made again; Downloads hops once when a file comes into it (10.6's)
    const int count = int(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size());
    const bool more = count > m_stackCounts.value(path, count);
    m_stackCounts[path] = count;
    ++m_stackStamp;
    if (more && path == QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
        for (Tile &t : m_tiles)
            if (t.kind == Tile::Folder && t.folder == path) {
                t.bounceStart = m_clock.elapsed();
                m_anim.start();
            }
    update();
}

void Dock::showMenu(int index, const QPoint &global)
{
    if (index < 0 || index >= m_tiles.size())
        return;
    Tile t = m_tiles[index];
    if (t.kind == Tile::Window) {
        // a minimized window's tile: its application's menu (10.6's DOCKWindowTile createMenu:)
        auto app = std::find_if(m_tiles.cbegin(), m_tiles.cend(), [&](const Tile &a) { return a.kind == Tile::Application && a.app.id == t.app.id; });
        if (app == m_tiles.cend())
            return;
        t = *app;
    }
    auto *menu = new DockMenu(m_wallpaper, this);
    App finder = Apps::finder();
    if (t.kind == Tile::Trash) {
        menu->addAction(menuText("OPEN", "Open"), [finder] { Apps::launch(finder, {"trash:///"}); });
        menu->addSeparator();
        menu->addAction(menuText("EMPTY_TRASH", "Empty Trash"), [] { Session::emptyTrash(); })->setEnabled(trashFull());
    } else if (t.kind == Tile::Folder) {
        stackMenu(menu, t);
    } else {
        // its windows: the active one checked, a diamond before the minimized ones (as 10.6)
        const xcb_window_t active = X11::activeWindow();
        for (xcb_window_t w : t.windows) {
            const bool minimized = X11::minimized(w) && !X11::hidden(w);
            QAction *a = menu->addAction((minimized ? QStringLiteral("\u25C6 ") : QString()) + X11::title(w), [w] { X11::activate(w); });
            if (w == active) {
                a->setCheckable(true);
                a->setChecked(true);
            }
        }
        if (!t.windows.isEmpty())
            menu->addSeparator();
        if (t.app.id != "finder") {
            QMenu *options = menu->addDockMenu(menuText("OPTIONS", "Options"));
            if (!t.entry.isEmpty()) {
                options->addAction(menuText("REMOVE_FROM_DOCK", "Remove from Dock"), [this, t] {
                    m_kept.removeAll(t.entry);
                    saveApps();
                    refreshWindows();
                });
            } else {
                options->addAction(menuText("KEEP_IN_DOCK", "Keep in Dock"), [this, t] {
                    m_kept << (t.app.macBundle.isEmpty() ? "app:" + t.app.id : "mac:" + t.app.macBundle);
                    saveApps();
                    refreshWindows();
                });
            }
            options->addAction(menuText("OPEN_AT_LOGIN", "Open at Login"))->setEnabled(false);
            options->addAction(menuText("SHOW_IN_FINDER", "Show in Finder"))->setEnabled(false);
            menu->addSeparator();
        }
        if (t.windows.isEmpty()) {
            menu->addAction(menuText("OPEN", "Open"), [this, index] {
                if (index < m_tiles.size())
                    activate(m_tiles[index]);
            });
        } else {
            menu->addAction(menuText("HIDE", "Hide"), [t] { Session::hideApp(t.app); });
            if (t.app.id != "finder")
                menu->addAction(menuText("QUIT", "Quit"), [t] { Session::quitApp(t.app); });
        }
    }
    Q_UNUSED(global);
    const Tile &tile = m_tiles[index];
    const qreal floorH = qRound(m_tileSize * 0.82);
    // the tip a dozen pixels over the icon, as on 10.6
    int iconTop = int(height() - floorH * 0.32 - tile.size) - 12;
    menu->popupAbove(mapToGlobal(QPoint(int(tile.x), 0)).x(), mapToGlobal(QPoint(0, iconTop)).y());
}

// ---- dropping files: on an application (opened with it) or on the Trash

void Dock::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void Dock::dragMoveEvent(QDragMoveEvent *e)
{
    m_mouseX = e->position().x();
    layoutTiles();
    m_hover = tileAt(e->position());
    bool ok = m_hover >= 0 && (m_tiles[m_hover].kind == Tile::Application || m_tiles[m_hover].kind == Tile::Trash);
    ok ? e->acceptProposedAction() : e->ignore();
    update();
}

void Dock::dropEvent(QDropEvent *e)
{
    int i = tileAt(e->position());
    if (i < 0)
        return;
    QStringList files;
    for (const QUrl &u : e->mimeData()->urls())
        files << (u.isLocalFile() ? u.toLocalFile() : u.toString());
    if (m_tiles[i].kind == Tile::Trash) {
        QProcess::startDetached("gio", QStringList{"trash"} + files);
    } else if (m_tiles[i].kind == Tile::Application) {
        Apps::launch(m_tiles[i].app, files);
        if (m_tiles[i].windows.isEmpty()) {
            m_tiles[i].bounceStart = m_clock.elapsed();
            m_anim.start();
        }
    }
    e->acceptProposedAction();
}
