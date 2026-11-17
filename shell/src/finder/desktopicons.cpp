// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktopicons.h"

#include "dsstore.h"
#include "labels.h"

#include "aqua.h"
#include "assets.h"
#include "x11.h"

#include "foundation.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QWidgetAction>

#include <sys/stat.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <cmath>

namespace {

const QString finderBundle = QStringLiteral("/System/Library/CoreServices/Finder.app");
QString t(const char *key, const char *fallback) { return Assets::tr(finderBundle, "Localizable", key, fallback); }
QString core(const char *key, const char *fallback) { return Assets::tr(finderBundle, "LocalizableCore", key, fallback); }
QString q(const std::string &s) { return QString::fromStdString(s); }
std::string s(const QString &q) { return q.toStdString(); }

QString desktopFolder()
{
    return q(Finder::userDir("DESKTOP", AK::home() + "/Desktop"));
}

QString displayTag()
{
    QString d = qEnvironmentVariable("DISPLAY");
    d.replace('/', '_').replace(':', '_');
    return d;
}

QString runtimeDir()
{
    const QString r = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return r.isEmpty() ? QStringLiteral("/tmp") : r;
}

QString finderWindows()
{
    QDir dir(QCoreApplication::applicationDirPath());
    const QString exe = dir.filePath("FinderWindows");
    return QFileInfo(exe).isExecutable() ? exe : QString();
}

// an AquaKit picture (a cairo surface) as Qt's: drawn into an ARGB32 surface of that size, whose
// bytes are QImage's premultiplied ARGB32
QImage toQImage(const AK::Image &img, int size)
{
    if (img.isNull())
        return {};
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    const AK::Size fit = AK::scaledToFit(img.size(), {double(size), double(size)});
    img.draw(cr, AK::Rect{(size - fit.w) / 2, (size - fit.h) / 2, fit.w, fit.h});
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    QImage out(cairo_image_surface_get_data(surface), size, size, cairo_image_surface_get_stride(surface), QImage::Format_ARGB32_Premultiplied);
    out = out.copy();
    cairo_surface_destroy(surface);
    return out;
}

QColor qcolor(const AK::Color &c) { return QColor::fromRgbF(float(c.r), float(c.g), float(c.b), float(c.a)); }

// the contextual menu's "Label:" and its eight swatches, the first the "none" one
class SwatchRow : public QWidget {
public:
    SwatchRow(int current, QMenu *menu, std::function<void(int)> chosen) : m_current(current), m_menu(menu), m_chosen(std::move(chosen))
    {
        setMouseTracking(true);
        setFixedSize(21 + 8 * 19 + 20, 40);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::black);
        p.drawText(QRect(21, 0, width() - 21, 19), Qt::AlignVCenter | Qt::AlignLeft, core("LB1", "Label:"));
        for (int k = 0; k < 8; ++k) {
            const QRectF r(21 + k * 19 + 0.5, 20.5, 14, 14);
            const int index = k == 0 ? 0 : Finder::Labels::Order[k - 1];
            if (k == m_hover || index == m_current) {
                p.setPen(QColor(0, 0, 0, k == m_hover ? 115 : 64));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(-3, -3, 3, 3));
            }
            if (k == 0) {
                p.setPen(QPen(QColor(0, 0, 0, 140), 1.5));
                p.drawLine(r.topLeft() + QPointF(3.5, 3.5), r.bottomRight() - QPointF(3.5, 3.5));
                p.drawLine(QPointF(r.right() - 3.5, r.top() + 3.5), QPointF(r.left() + 3.5, r.bottom() - 3.5));
                continue;
            }
            const QColor c = qcolor(Finder::Labels::color(index));
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0, c.lighter(125));
            g.setColorAt(1, c);
            p.setBrush(g);
            p.setPen(c.darker(165));
            p.drawRect(r);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int k = at(e->position());
        if (k != m_hover) {
            m_hover = k;
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        const int k = at(e->position());
        if (k < 0)
            return;
        m_chosen(k == 0 ? 0 : Finder::Labels::Order[k - 1]);
        m_menu->close();
    }

private:
    int at(QPointF p) const
    {
        const int k = int((p.x() - 21 + 2) / 19);
        return p.y() >= 17 && p.x() >= 19 && k >= 0 && k < 8 ? k : -1;
    }
    int m_current, m_hover = -1;
    QMenu *m_menu;
    std::function<void(int)> m_chosen;
};

} // namespace

DesktopIcons::DesktopIcons(QWidget *desktop) : QObject(desktop), m_desktop(desktop)
{
    // its socket: FinderWindows ("place:"), the menu bar ("cmd:") while the desktop is in front
    m_server = new QLocalServer(this);
    // abstract (no file), as FinderWindows' own: "snowleopard-desktop-<uid>-<display>"
    const QString name = "snowleopard-desktop-" + QString::number(getuid()) + "-" + displayTag();
    m_server->setSocketOptions(QLocalServer::AbstractNamespaceOption);
    if (m_server->listen(name))
        connect(m_server, &QLocalServer::newConnection, this, &DesktopIcons::incoming);
    // the Desktop folder and the disks change: the icons follow
    auto *poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this] {
        // a cheap signature of what is shown
        QStringList now;
        for (const Finder::Volume &v : Finder::volumes())
            now << q(v.path);
        const QDir d(desktopFolder());
        now << d.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        now << QString::number(QFileInfo(d.absolutePath() + "/.DS_Store").lastModified().toMSecsSinceEpoch());
        static QStringList last;
        if (now != last) {
            last = now;
            reload();
        }
    });
    poll->start(1000);
    reload();
}

void DesktopIcons::incoming()
{
    while (QLocalSocket *c = m_server->nextPendingConnection()) {
        connect(c, &QLocalSocket::readyRead, c, [this, c] {
            while (c->canReadLine()) {
                const QString line = QString::fromUtf8(c->readLine()).trimmed();
                if (line.startsWith("place:")) {
                    // "place:x,y<tab>name..." the items FinderWindows drops on the desktop, placed there
                    QStringList f = line.mid(6).split('\t');
                    const QStringList xy = f.takeFirst().split(',');
                    QPoint at(xy.value(0).toInt(), xy.value(1).toInt());
                    at -= m_desktop->mapToGlobal(QPoint(0, 0));
                    const Metrics m = metrics();
                    for (const QString &name : f) {
                        m_pendingPlaces.append({name, at});
                        at.ry() += m.grid.height();
                    }
                } else if (line.startsWith("cmd:")) {
                    perform(line.mid(4));
                }
            }
        });
        connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
    }
}

// ---- the grid

DesktopIcons::Metrics DesktopIcons::metrics() const
{
    // TDesktopViewController -calculateGridSize (as iconview.h): the label's width from the width of
    // "0" (cw) and the line height (lh) of the label's font, by the grid spacing
    QFont font("Lucida Grande");
    font.setPixelSize(m_textSize);
    const QFontMetricsF fm(font);
    const double cw = fm.horizontalAdvance('0'), lh = std::ceil(fm.height());
    Metrics m;
    m.pad = std::floor(m_iconSize / 16.0);
    m.box = m_iconSize + 2 * m.pad;
    m.lineHeight = lh;
    if (m_labelOnBottom) {
        const double least = std::max(4 * cw + 18, m.box);
        m.labelWidth = std::floor(least + m_gridSpacing * ((17 * cw + 18) - least) / 100);
        int w = int(m.labelWidth + 1), h = int(m.box + std::floor(2 * lh + 2) + 6);
        m.grid = QSize(w + (w & 1), h + (h & 1));
    } else {
        m.labelWidth = std::floor(std::max(4 * cw + 18, 17 * cw + 18) * (0.5 + m_gridSpacing / 200.0));
        int w = int(m.labelWidth + m.box + 2), h = int(std::max(m.box, 2 * lh) + 4 + m_gridSpacing / 10);
        m.grid = QSize(w + (w & 1), h + (h & 1));
    }
    return m;
}

QRect DesktopIcons::available() const
{
    // -availableWindowPositioningBoundsForDisplay: the screen less the menu bar and the Dock
    QScreen *screen = m_desktop->screen();
    QRect r = screen ? screen->availableGeometry() : m_desktop->rect();
    r.translate(-m_desktop->geometry().topLeft());
    const QRect all = m_desktop->rect();
    if (r == all)
        r.setTop(22);   // no work area published: the menu bar's 22 points
    return r;
}

QPoint DesktopIcons::spot(int column, int row) const
{
    // layout style 2: the grid's origin at the right edge; -anchorAtGridLocation: 4 left of the
    // cell's centre, the icon box 4 under the cell's top
    const Metrics m = metrics();
    const QRect a = available();
    const double cx = a.right() + 1 - (column + 0.5) * m.grid.width();
    if (m_labelOnBottom)
        return QPoint(int(std::round(cx - 4)), int(std::round(a.top() + row * m.grid.height() + 4 + m.box / 2)));
    const double left = a.right() + 1 - (column + 1) * m.grid.width();
    return QPoint(int(std::round(left + 4 + m.box / 2)), int(std::round(a.top() + row * m.grid.height() + m.grid.height() / 2.0 + 4)));
}

void DesktopIcons::reload()
{
    AK::Settings conf("finder");
    m_iconSize = std::clamp(conf.integer("DesktopIconSize", 64), 16, 128);
    m_gridSpacing = std::clamp(conf.integer("DesktopGridSpacing", 54), 1, 100);
    m_textSize = std::clamp(conf.integer("DesktopTextSize", 12), 10, 16);
    m_labelOnBottom = conf.boolean("DesktopLabelOnBottom", true);
    m_iconPreview = conf.boolean("DesktopShowIconPreview", true);
    m_arrangeBy = q(conf.string("DesktopArrangeBy", "none"));

    QStringList selected = selectedPaths();
    m_icons.clear();
    // the disks first (the startup disk at the top), as Preferences > General asks
    for (const Finder::Volume &v : Finder::volumes()) {
        const char *key = v.kind == Finder::Volume::Internal ? "ShowHardDrivesOnDesktop"
                          : v.kind == Finder::Volume::External ? "ShowExternalHardDrivesOnDesktop"
                                                               : "ShowRemovableMediaOnDesktop";
        if (!conf.boolean(key, true))
            continue;
        Icon i;
        i.item = Finder::Item{v.name, v.path, true};
        i.volume = true;
        i.ejectable = v.kind != Finder::Volume::Internal;
        m_icons.append(i);
    }
    if (conf.boolean("ShowMountedServersOnDesktop", false)) {
        const QDir gvfs("/run/user/" + QString::number(getuid()) + "/gvfs");
        for (const QFileInfo &f : gvfs.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            Icon i;
            i.item = Finder::Item{s(f.fileName()), s(f.absoluteFilePath()), true};
            i.volume = true;
            i.ejectable = true;
            m_icons.append(i);
        }
    }
    for (const Finder::Item &item : Finder::list(s(desktopFolder()))) {
        Icon i;
        i.item = item;
        m_icons.append(i);
    }

    // the places kept: the files' Iloc in the Desktop folder's .DS_Store, the disks' in
    // FXDesktopVolumePositions (finder.conf's DesktopVolumePositions: "path|x|y")
    Finder::DSStore::Store store(s(desktopFolder()));
    QHash<QString, QPoint> volumes;
    for (const std::string &e : conf.list("DesktopVolumePositions")) {
        const QStringList f = q(e).split('|');
        if (f.size() == 3)
            volumes[f[0]] = QPoint(f[1].toInt(), f[2].toInt());
    }
    for (Icon &i : m_icons) {
        if (i.volume) {
            if (volumes.contains(q(i.item.path))) {
                i.anchor = volumes.value(q(i.item.path));
                i.placed = true;
            }
        } else if (const Finder::DSStore::Record *r = store.get(i.item.name, "Iloc")) {
            int x, y;
            if (Finder::DSStore::iconLocation(*r, x, y)) {
                i.anchor = QPoint(x, y);
                i.placed = true;
            }
        }
    }
    // the items FinderWindows dropped here: where they were dropped
    QVector<int> placedNow;
    for (auto it = m_pendingPlaces.begin(); it != m_pendingPlaces.end();) {
        bool found = false;
        for (int k = 0; k < m_icons.size(); ++k)
            if (!m_icons[k].volume && q(m_icons[k].item.name) == it->first) {
                m_icons[k].anchor = it->second;
                m_icons[k].placed = true;
                placedNow << k;
                found = true;
            }
        it = found ? m_pendingPlaces.erase(it) : it + 1;
    }
    layout();
    if (!placedNow.isEmpty())
        savePlaces(placedNow);
    m_selection.clear();
    for (int k = 0; k < m_icons.size(); ++k)
        if (selected.contains(q(m_icons[k].item.path)))
            m_selection.insert(k);
    m_images.clear();
    writeState();
    m_desktop->update();
}

void DesktopIcons::layout()
{
    // -[TLayoutGrid nextOpenLocation:]: the free spots column by column from the right, each from the
    // top; an item with its own place takes the spot it is on
    const Metrics m = metrics();
    const QRect a = available();
    const int rows = std::max(1, int(a.height() / m.grid.height()));
    const int columns = std::max(1, int(a.width() / m.grid.width()));
    QSet<int> taken;
    auto cellOf = [&](QPoint p) {
        const int column = std::clamp(int(std::floor((a.right() + 1 - p.x()) / double(m.grid.width()))), 0, columns - 1);
        const int row = std::clamp(int(std::floor((p.y() - a.top()) / double(m.grid.height()))), 0, rows - 1);
        return column * rows + row;
    };
    QVector<int> order;
    for (int k = 0; k < m_icons.size(); ++k)
        order << k;
    const bool kept = m_arrangeBy != "none" && m_arrangeBy != "grid";
    if (kept) {
        // Keep Arranged By: the disks first, then the items in that order
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
            const Icon &a = m_icons[x], &b = m_icons[y];
            if (a.volume != b.volume)
                return a.volume;
            if (m_arrangeBy == "dateModified")
                return Finder::modificationTime(a.item) > Finder::modificationTime(b.item);
            if (m_arrangeBy == "dateCreated")
                return Finder::creationTime(a.item) > Finder::creationTime(b.item);
            if (m_arrangeBy == "size")
                return Finder::sizeOnDisk(a.item) > Finder::sizeOnDisk(b.item);
            if (m_arrangeBy == "kind")
                return Finder::kind(a.item) < Finder::kind(b.item);
            if (m_arrangeBy == "label")
                return Finder::Labels::get(a.item.path) > Finder::Labels::get(b.item.path);
            return AK::finderLess(a.item.name, b.item.name);
        });
    } else {
        for (Icon &i : m_icons)
            if (i.placed) {
                const int c = cellOf(i.anchor);
                if (m_arrangeBy == "grid")
                    i.anchor = spot(c / rows, c % rows);
                taken.insert(c);
            }
    }
    int next = 0;
    for (int k : order) {
        Icon &i = m_icons[k];
        if (i.placed && !kept)
            continue;
        while (taken.contains(next))
            ++next;
        i.anchor = spot(next / rows, next % rows);
        taken.insert(next);
    }
}

QRect DesktopIcons::iconRect(const Icon &i) const
{
    return QRect(i.anchor.x() - m_iconSize / 2, i.anchor.y() - m_iconSize / 2, m_iconSize, m_iconSize);
}

QRect DesktopIcons::labelRect(const Icon &i) const
{
    const Metrics m = metrics();
    const int lines = labelLines(q(i.item.name)).size();
    if (m_labelOnBottom)
        return QRect(int(i.anchor.x() - m.labelWidth / 2), int(i.anchor.y() - m.box / 2 + m.box + 2), int(m.labelWidth), int(lines * m.lineHeight + 2));
    return QRect(int(i.anchor.x() + m.box / 2 + 2), int(i.anchor.y() - lines * m.lineHeight / 2), int(m.labelWidth), int(lines * m.lineHeight + 2));
}

QStringList DesktopIcons::labelLines(const QString &name) const
{
    // two lines at most (one beside small icons), cut at a word, the second shortened in its middle
    QFont font("Lucida Grande");
    font.setPixelSize(m_textSize);
    const QFontMetricsF fm(font);
    const double w = metrics().labelWidth - 6;
    if (fm.horizontalAdvance(name) <= w)
        return {name};
    int cut = 0;
    for (int k = 1; k <= name.size(); ++k) {
        if (fm.horizontalAdvance(name.left(k)) > w)
            break;
        if (k == name.size() || name[k] == ' ' || name[k - 1] == '-' || name[k - 1] == '_')
            cut = k;
    }
    if (cut == 0)
        while (cut < name.size() && fm.horizontalAdvance(name.left(cut + 1)) <= w)
            ++cut;
    const QString first = name.left(cut).trimmed(), rest = name.mid(cut).trimmed();
    if (!m_labelOnBottom && m_iconSize <= 16)
        return {fm.elidedText(name, Qt::ElideMiddle, w)};
    return {first, fm.elidedText(rest, Qt::ElideMiddle, w)};
}

int DesktopIcons::iconAt(QPoint p) const
{
    for (int k = m_icons.size() - 1; k >= 0; --k) {
        const Icon &i = m_icons[k];
        if (iconRect(i).contains(p))
            return k;
        // the label's text, not its whole place
        QFont font("Lucida Grande");
        font.setPixelSize(m_textSize);
        const QFontMetricsF fm(font);
        const QRect lr = labelRect(i);
        const QStringList lines = labelLines(q(i.item.name));
        for (int n = 0; n < lines.size(); ++n) {
            const double w = fm.horizontalAdvance(lines[n]) + 10;
            const QRectF line(m_labelOnBottom ? lr.center().x() - w / 2 : lr.x() - 5, lr.y() + n * metrics().lineHeight, w, metrics().lineHeight);
            if (line.contains(p))
                return k;
        }
    }
    return -1;
}

QImage DesktopIcons::imageFor(const Icon &i)
{
    const QString key = q(i.item.path);
    auto it = m_images.find(key);
    if (it != m_images.end())
        return *it;
    const AK::Image img = i.volume ? [&] {
        for (const Finder::Volume &v : Finder::volumes())
            if (v.path == i.item.path)
                return Finder::volumeIcon(v, m_iconSize);
        return Finder::icon(i.item, m_iconSize, false);
    }()
                                   : Finder::icon(i.item, m_iconSize, m_iconPreview);
    const QImage out = toQImage(img, m_iconSize);
    m_images.insert(key, out);
    return out;
}

// ---- drawing

void DesktopIcons::paint(QPainter &p, const QRect &dirty)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QFont font("Lucida Grande");
    font.setPixelSize(m_textSize);
    font.setStyleStrategy(QFont::NoSubpixelAntialias);   // -drawLabelInContext: no font smoothing
    p.setFont(font);
    const QFontMetricsF fm(font);
    const Metrics m = metrics();
    const QPoint offset = m_drag == Moving ? m_dragAt - m_pressAt : QPoint();
    for (int k = 0; k < m_icons.size(); ++k) {
        const Icon &i = m_icons[k];
        const bool selected = m_selection.contains(k);
        const QPoint shift = selected ? offset : QPoint();
        const QRect ir = iconRect(i).translated(shift);
        const QRect lr = labelRect(i).translated(shift);
        if (!dirty.intersects(ir.united(lr).adjusted(-8, -8, 8, 8)))
            continue;
        if (selected) {
            // the icon's selection: a dark rounded square around its box
            const QRectF box(i.anchor.x() + shift.x() - m.box / 2 - 2, i.anchor.y() + shift.y() - m.box / 2 - 2, m.box + 4, m.box + 4);
            p.setPen(QPen(QColor(255, 255, 255, 110), 1));
            p.setBrush(QColor(0, 0, 0, 90));
            p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        }
        p.drawImage(ir, imageFor(i));
        if (k == m_renaming)
            continue;
        const QStringList lines = labelLines(q(i.item.name));
        const int label = i.volume ? 0 : Finder::Labels::get(i.item.path);
        for (int n = 0; n < lines.size(); ++n) {
            const double w = std::ceil(fm.horizontalAdvance(lines[n])) + 10;
            const QRectF line(m_labelOnBottom ? std::round(lr.center().x() - w / 2 + 0.5) : lr.x() - 5, lr.y() + n * m.lineHeight, w, m.lineHeight);
            if (selected || label) {
                // the label's band: the selection's colour, or the label's (TDesktopIcon
                // -drawSingleLineLabelBackground:...hiliteColor:selected:labelIndex:)
                QColor band = selected ? (m_active ? QColor(56, 117, 215) : QColor(0, 0, 0, 120)) : qcolor(Finder::Labels::color(label));
                if (selected || !label) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(band);
                } else {
                    QLinearGradient g(line.topLeft(), line.bottomLeft());
                    g.setColorAt(0, band.lighter(118));
                    g.setColorAt(1, band);
                    p.setPen(Qt::NoPen);
                    p.setBrush(g);
                }
                p.drawRoundedRect(line, m.lineHeight / 2, m.lineHeight / 2);
            }
            const QRectF text = line.adjusted(5, 0, -5, 0);
            if (!selected && !label) {
                // white on the picture, with its shadow: dark, one point down, a little blurred
                p.setPen(QColor(0, 0, 0, 70));
                for (QPointF d : {QPointF(-1, 1), QPointF(1, 1), QPointF(0, 2), QPointF(0, 0)})
                    p.drawText(text.translated(d), Qt::AlignCenter, lines[n]);
                p.setPen(QColor(0, 0, 0, 200));
                p.drawText(text.translated(0, 1), Qt::AlignCenter, lines[n]);
                p.setPen(Qt::white);
            } else {
                p.setPen(selected ? Qt::white : Qt::black);
            }
            p.drawText(text, Qt::AlignCenter, lines[n]);
        }
    }
    if (m_drag == Marquee) {
        const QRect r = QRect(m_pressAt, m_dragAt).normalized();
        p.setPen(QColor(255, 255, 255, 200));
        p.setBrush(QColor(255, 255, 255, 50));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.drawRect(r);
    }
    p.restore();
}

void DesktopIcons::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (active)
        writeState();
    m_desktop->update();
}

// ---- the mouse

void DesktopIcons::select(int i, bool extend)
{
    if (!extend)
        m_selection.clear();
    if (i >= 0) {
        if (extend && m_selection.contains(i))
            m_selection.remove(i);
        else
            m_selection.insert(i);
        m_anchor = i;
    }
    writeState();
    m_desktop->update();
}

bool DesktopIcons::mousePress(QPoint at, Qt::MouseButton button, Qt::KeyboardModifiers mods, int clicks)
{
    if (m_rename)
        endRename(true);
    const int i = iconAt(at);
    const bool extend = mods & (Qt::ShiftModifier | Qt::MetaModifier);
    if (button == Qt::RightButton || (button == Qt::LeftButton && (mods & Qt::ControlModifier))) {
        if (i >= 0 && !m_selection.contains(i))
            select(i, false);
        else if (i < 0)
            select(-1, false);
        showMenu(at, m_desktop->mapToGlobal(at));
        return true;
    }
    if (button != Qt::LeftButton)
        return false;
    m_pressAt = m_dragAt = at;
    if (i >= 0) {
        if (clicks == 2 && !extend) {
            open(selectedItems().isEmpty() ? QVector<Finder::Item>{m_icons[i].item} : selectedItems());
            return true;
        }
        if (extend || !m_selection.contains(i))
            select(i, extend);
        m_drag = m_selection.contains(i) ? Moving : None;
        return true;
    }
    if (!extend)
        select(-1, false);
    m_drag = Marquee;
    m_marqueeBase = m_selection;
    return i >= 0;
}

void DesktopIcons::mouseMove(QPoint at)
{
    if (m_drag == None)
        return;
    m_dragAt = at;
    if (m_drag == Marquee) {
        const QRect r = QRect(m_pressAt, at).normalized();
        m_selection = m_marqueeBase;
        for (int k = 0; k < m_icons.size(); ++k)
            if (r.intersects(iconRect(m_icons[k])) || r.intersects(labelRect(m_icons[k])))
                m_selection.insert(k);
    }
    m_desktop->update();
}

void DesktopIcons::mouseRelease(QPoint at, Qt::KeyboardModifiers mods)
{
    const Drag drag = m_drag;
    m_drag = None;
    if (drag == Moving && (at - m_pressAt).manhattanLength() > 3) {
        const QPoint global = m_desktop->mapToGlobal(at);
        // over another window (a Finder window): FinderWindows takes them there
        xcb_connection_t *c = X11::conn();
        xcb_window_t under = 0;
        if (c) {
            if (auto *r = xcb_query_pointer_reply(c, xcb_query_pointer(c, X11::root()), nullptr)) {
                under = r->child;
                free(r);
            }
        }
        if (under && under != xcb_window_t(m_desktop->winId())) {
            QString line = QString("drop:%1,%2,%3").arg(global.x()).arg(global.y()).arg(mods & Qt::AltModifier ? 1 : 0);
            for (const QString &p : selectedPaths())
                line += "\t" + p;
            send(line);
            m_desktop->update();
            return;
        }
        // on a folder of the desktop: into it (FinderWindows moves them, its Copy window shown)
        const int target = iconAt(at);
        if (target >= 0 && !m_selection.contains(target) && m_icons[target].item.folder) {
            QString line = QString("items:%1\t%2").arg(mods & Qt::AltModifier ? "transferCopy" : "transfer", q(m_icons[target].item.path));
            for (const QString &p : selectedPaths())
                line += "\t" + p;
            send(line);
            m_desktop->update();
            return;
        }
        // moved on the desktop: their new places kept (no longer kept arranged)
        const QPoint by = at - m_pressAt;
        QVector<int> moved;
        for (int k : m_selection) {
            m_icons[k].anchor += by;
            m_icons[k].placed = true;
            moved << k;
        }
        if (m_arrangeBy != "none" && m_arrangeBy != "grid") {
            m_arrangeBy = "none";
            AK::Settings("finder").set("DesktopArrangeBy", std::string("none"));
            for (Icon &i : m_icons)
                if (!i.placed)
                    i.placed = true;
            QVector<int> all;
            for (int k = 0; k < m_icons.size(); ++k)
                all << k;
            moved = all;
        }
        savePlaces(moved);
        layout();
    } else if (drag == Moving) {
        // a click on the name of the item selected alone: renaming after a moment
        const int i = iconAt(at);
        if (i >= 0 && m_selection.size() == 1 && m_selection.contains(i) && labelRect(m_icons[i]).contains(at) && m_active)
            QTimer::singleShot(700, this, [this, i] {
                if (m_drag == None && m_selection.size() == 1 && m_selection.contains(i))
                    startRename();
            });
    }
    m_desktop->update();
}

void DesktopIcons::savePlaces(const QVector<int> &moved)
{
    Finder::DSStore::Store store(s(desktopFolder()));
    AK::Settings conf("finder");
    std::vector<std::string> volumes = conf.list("DesktopVolumePositions");
    bool volumesChanged = false;
    for (int k : moved) {
        const Icon &i = m_icons[k];
        if (i.volume) {
            const std::string prefix = i.item.path + "|";
            volumes.erase(std::remove_if(volumes.begin(), volumes.end(), [&](const std::string &e) { return e.rfind(prefix, 0) == 0; }),
                          volumes.end());
            volumes.push_back(prefix + std::to_string(i.anchor.x()) + "|" + std::to_string(i.anchor.y()));
            volumesChanged = true;
        } else {
            store.set(Finder::DSStore::iconLocation(i.item.name, i.anchor.x(), i.anchor.y()));
        }
    }
    store.save();
    if (volumesChanged)
        conf.set("DesktopVolumePositions", volumes);
}

// ---- the commands

QVector<Finder::Item> DesktopIcons::selectedItems() const
{
    QVector<Finder::Item> out;
    QList<int> sorted = m_selection.values();
    std::sort(sorted.begin(), sorted.end());
    for (int k : sorted)
        if (k < m_icons.size())
            out << m_icons[k].item;
    return out;
}

QStringList DesktopIcons::selectedPaths() const
{
    QStringList out;
    for (const Finder::Item &i : selectedItems())
        out << q(i.path);
    return out;
}

void DesktopIcons::send(const QString &line)
{
    // to the Finder's windows' process (started if it isn't running)
    const QString exe = finderWindows();
    if (exe.isEmpty())
        return;
    if (line.startsWith("cmd:"))
        QProcess::startDetached(exe, {"--command", line.mid(4)});
    else
        QProcess::startDetached(exe, {"--line", line});
}

void DesktopIcons::open(const QVector<Finder::Item> &items)
{
    QStringList paths;
    for (const Finder::Item &i : items)
        paths << q(i.path);
    if (!paths.isEmpty())
        send("items:cmdOpen:\t" + paths.join('\t'));
}

void DesktopIcons::newFolder(QPoint at)
{
    // "untitled folder", "untitled folder 2"... where the desktop was clicked, its name ready to type
    QDir desktop(desktopFolder());
    desktop.mkpath(".");
    const QString base = t("N2", "untitled folder");
    QString name = base;
    for (int n = 2; desktop.exists(name); ++n)
        name = QStringLiteral("%1 %2").arg(base).arg(n);
    if (!desktop.mkdir(name))
        return;
    // its place: where the desktop was clicked, else the grid's next one (both kept)
    reload();
    for (int k = 0; k < m_icons.size(); ++k)
        if (q(m_icons[k].item.name) == name && !m_icons[k].volume) {
            if (at.x() >= 0)
                m_icons[k].anchor = at;
            m_icons[k].placed = true;
            savePlaces({k});
        }
    for (int k = 0; k < m_icons.size(); ++k)
        if (q(m_icons[k].item.name) == name && !m_icons[k].volume) {
            select(k, false);
            startRename();
        }
}

void DesktopIcons::setLabel(int index)
{
    for (const Finder::Item &i : selectedItems())
        Finder::Labels::set(i.path, index);
    m_desktop->update();
}

void DesktopIcons::cleanUp()
{
    // Clean Up: each icon to the nearest free spot of the grid
    const Metrics m = metrics();
    const QRect a = available();
    const int rows = std::max(1, int(a.height() / m.grid.height()));
    const int columns = std::max(1, int(a.width() / m.grid.width()));
    QSet<int> taken;
    QVector<int> all;
    for (int k = 0; k < m_icons.size(); ++k) {
        Icon &i = m_icons[k];
        int best = -1;
        double bestD = 1e18;
        for (int c = 0; c < rows * columns; ++c) {
            if (taken.contains(c))
                continue;
            const QPoint sp = spot(c / rows, c % rows);
            const double d = std::pow(sp.x() - i.anchor.x(), 2) + std::pow(sp.y() - i.anchor.y(), 2);
            if (d < bestD) {
                bestD = d;
                best = c;
            }
        }
        if (best < 0)
            continue;
        taken.insert(best);
        i.anchor = spot(best / rows, best % rows);
        i.placed = true;
        all << k;
    }
    savePlaces(all);
    m_desktop->update();
}

void DesktopIcons::arrangeBy(const QString &by)
{
    // the desktop's Arrange By keeps it arranged (DesktopViewSettings' arrangeBy)
    m_arrangeBy = by;
    AK::Settings("finder").set("DesktopArrangeBy", s(by));
    for (Icon &i : m_icons)
        i.placed = by == "none" ? i.placed : false;
    layout();
    if (by == "none") {
        QVector<int> all;
        for (int k = 0; k < m_icons.size(); ++k) {
            m_icons[k].placed = true;
            all << k;
        }
        savePlaces(all);
    }
    m_desktop->update();
}

bool DesktopIcons::perform(const QString &cmd)
{
    const QStringList paths = selectedPaths();
    const QString list = paths.join('\t');
    bool anyVolume = false;
    for (int k : m_selection)
        anyVolume = anyVolume || m_icons[k].volume;
    if (cmd == "cmdOpen:" || cmd == "cmdOpenWithDownArrow:") {
        open(selectedItems());
    } else if (cmd == "cmdShowStaticInfo:") {
        send("items:cmdShowStaticInfo:\t" + (paths.isEmpty() ? desktopFolder() : list));
    } else if (cmd == "cmdQuickLook:") {
        if (!paths.isEmpty())
            send("items:cmdQuickLook:\t" + list);
    } else if (cmd == "cmdMoveToTrash:") {
        if (!paths.isEmpty() && !anyVolume)
            send("items:cmdMoveToTrash:\t" + list);
    } else if (cmd == "cmdDuplicate:" || cmd == "cmdArchive:" || cmd == "copy:" || cmd == "cmdShowOriginal:") {
        if (!paths.isEmpty())
            send("items:" + cmd + "\t" + list);
    } else if (cmd == "paste:") {
        send("items:paste:\t" + desktopFolder());
    } else if (cmd == "undo:") {
        send("items:undo:");
    } else if (cmd == "cmdMakeAlias:") {
        // "name alias" beside the original, on the desktop
        for (const Finder::Item &i : selectedItems()) {
            QString name = t("N3", "^0 alias").replace("^0", q(i.name));
            const QString base = name;
            for (int n = 2; QFileInfo::exists(desktopFolder() + "/" + name); ++n)
                name = QStringLiteral("%1 %2").arg(base).arg(n);
            QFile::link(q(i.path), desktopFolder() + "/" + name);
        }
        reload();
    } else if (cmd == "cmdEject:") {
        for (int k : m_selection)
            if (m_icons[k].ejectable)
                QProcess::startDetached("gio", {"mount", "-e", q(m_icons[k].item.path)});
        QTimer::singleShot(1500, this, &DesktopIcons::reload);
    } else if (cmd == "selectAll:") {
        for (int k = 0; k < m_icons.size(); ++k)
            m_selection.insert(k);
        m_desktop->update();
    } else if (cmd == "cmdDeselectAll:") {
        select(-1, false);
    } else if (cmd == "cmdNewFolder:" || cmd == "cmdNewFolderAtMouseLocation:") {
        newFolder(cmd == "cmdNewFolderAtMouseLocation:" ? m_menuAt : QPoint(-1, -1));
    } else if (cmd == "cmdCleanUp:") {
        cleanUp();
    } else if (cmd.startsWith("cmdArrangeBy") || cmd.startsWith("cmdKeepArrangedBy")) {
        static const QHash<QString, QString> by = {{"Name:", "name"}, {"ModDate:", "dateModified"}, {"CreationDate:", "dateCreated"},
                                                    {"PhysicalSize:", "size"}, {"Kind:", "kind"}, {"Label:", "label"}, {"None:", "none"}};
        const QString what = cmd.mid(cmd.startsWith("cmdArrangeBy") ? 12 : 17);
        if (by.contains(what))
            arrangeBy(by.value(what));
    } else if (cmd.startsWith("cmdSetLabel") && cmd.size() > 11 && cmd[11].isDigit()) {
        setLabel(cmd[11].digitValue());
    } else if (cmd == "cmdToggleViewOptions:") {
        send("items:cmdDesktopViewOptions:");
    } else if (cmd == "cmdRename:") {
        startRename();
    } else if (cmd == "cmdChangeDesktopBackground:") {
        const QString prefs = QCoreApplication::applicationDirPath() + "/../../../System Preferences.app/Contents/MacOS/System Preferences";
        QProcess::startDetached(QFileInfo(prefs).absoluteFilePath(), {"--pane", "desktop"});
    } else {
        // the others (New Finder Window, Go...): the Finder's windows
        send("cmd:" + cmd);
    }
    writeState();
    return true;
}

// the desktop's state for the menu bar (as FinderWindows writes its front window's)
void DesktopIcons::writeState()
{
    if (!m_active)
        return;
    const bool any = !m_selection.isEmpty();
    bool volume = false, ejectable = true, alias = false;
    for (int k : m_selection) {
        volume = volume || m_icons[k].volume;
        ejectable = ejectable && m_icons[k].ejectable;
        struct stat st;
        alias = m_selection.size() == 1 && lstat(m_icons[k].item.path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
    }
    QStringList enabled = {"cmdNewFileViewer:", "cmdNewFolder:", "cmdFindFile:", "cmdNewSmartFolder:", "paste:", "selectAll:", "undo:",
                           "cmdViewAsIcons:", "cmdCleanUp:", "cmdArrange:", "cmdArrangeByName:", "cmdArrangeByModDate:",
                           "cmdArrangeByCreationDate:", "cmdArrangeByPhysicalSize:", "cmdArrangeByKind:", "cmdArrangeByLabel:",
                           "cmdKeepArrangedBy:", "cmdToggleViewOptions:", "cmdGoToComputer:", "cmdGoHome:", "cmdGoToDesktop:",
                           "cmdGoToApplications:", "cmdGoToDocuments:", "cmdGoTo:", "cmdConnectTo:", "cmdPreferences:",
                           "cmdEmptyTrash:", "cmdShowStaticInfo:"};
    if (any)
        enabled << "cmdOpen:" << "cmdQuickLook:" << "copy:" << "cmdDuplicate:" << "cmdMakeAlias:" << "cmdArchive:"
                << "cmdSetLabel:" << "cmdAddToSidebar:";
    if (any && !volume)
        enabled << "cmdMoveToTrash:";
    if (any && ejectable && volume)
        enabled << "cmdEject:";
    if (alias)
        enabled << "cmdShowOriginal:";
    QString out = "pid " + QString::number(getpid()) + "\n";
    for (const QString &e : enabled)
        out += "E " + e + "\n";
    const QVector<Finder::Item> sel = selectedItems();
    if (sel.size() == 1) {
        out += "T cmdArchive:\t" + t("N168", "Compress “^0”").replace("^0", q(sel.front().name)) + "\n";
        out += "T cmdQuickLook:\t" + core("N169.18", "Quick Look “^0”").replace("^0", q(sel.front().name)) + "\n";
        out += "T copy:\t" + core("N47", "Copy “^0”").replace("^0", q(sel.front().name)) + "\n";
        out += "T cmdEject:\t" + core("N57", "Eject “^0”").replace("^0", q(sel.front().name)) + "\n";
        out += "L " + QString::number(Finder::Labels::get(sel.front().path)) + "\n";
    }
    out += "C cmdViewAsIcons:\n";
    if (m_arrangeBy != "none" && m_arrangeBy != "grid") {
        static const QHash<QString, QString> keep = {{"name", "Name"}, {"dateModified", "ModDate"}, {"dateCreated", "CreationDate"},
                                                      {"size", "PhysicalSize"}, {"kind", "Kind"}, {"label", "Label"}};
        out += "C cmdKeepArrangedBy" + keep.value(m_arrangeBy) + ":\n";
    }
    QFile f(runtimeDir() + "/snowleopard-desktop-" + QString::number(getuid()) + "-" + displayTag() + ".state");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(out.toUtf8());
}

// ---- the keyboard (the menu bar's key equivalents, Super as Command)

bool DesktopIcons::keyPress(int key, Qt::KeyboardModifiers mods, const QString &)
{
    if (m_rename)
        return false;
    const bool cmd = mods & Qt::MetaModifier, shift = mods & Qt::ShiftModifier, opt = mods & Qt::AltModifier;
    if (cmd && key == Qt::Key_Backspace && !shift)
        return perform("cmdMoveToTrash:");
    if (cmd && shift && key == Qt::Key_Backspace)
        return perform("cmdEmptyTrash:");
    if (cmd && shift && key == Qt::Key_N)
        return perform("cmdNewFolder:");
    if (cmd && !shift && !opt) {
        switch (key) {
        case Qt::Key_O:
        case Qt::Key_Down: return perform("cmdOpen:");
        case Qt::Key_I: return perform("cmdShowStaticInfo:");
        case Qt::Key_D: return perform("cmdDuplicate:");
        case Qt::Key_L: return perform("cmdMakeAlias:");
        case Qt::Key_R: return perform("cmdShowOriginal:");
        case Qt::Key_Y: return perform("cmdQuickLook:");
        case Qt::Key_C: return perform("copy:");
        case Qt::Key_V: return perform("paste:");
        case Qt::Key_A: return perform("selectAll:");
        case Qt::Key_Z: return perform("undo:");
        case Qt::Key_E: return perform("cmdEject:");
        case Qt::Key_J: return perform("cmdToggleViewOptions:");
        case Qt::Key_N: return perform("cmdNewFileViewer:");
        default: break;
        }
    }
    if (cmd) {
        // the Go menu's and the others: the Finder's windows have them
        static const QHash<int, QString> go = {{Qt::Key_C, "cmdGoToComputer:"}, {Qt::Key_H, "cmdGoHome:"}, {Qt::Key_D, "cmdGoToDesktop:"},
                                               {Qt::Key_A, "cmdGoToApplications:"}, {Qt::Key_O, "cmdGoToDocuments:"},
                                               {Qt::Key_G, "cmdGoTo:"}};
        if (shift && go.contains(key))
            return perform(go.value(key));
        if (!shift && key == Qt::Key_K)
            return perform("cmdConnectTo:");
        if (!shift && key == Qt::Key_Comma)
            return perform("cmdPreferences:");
        return false;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        startRename();
        return true;
    }
    if (key == Qt::Key_Space)
        return perform("cmdQuickLook:");
    // the arrows: the nearest icon that way
    if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down) {
        if (m_icons.isEmpty())
            return true;
        if (m_anchor < 0 || m_anchor >= m_icons.size()) {
            select(0, false);
            return true;
        }
        const QPoint from = m_icons[m_anchor].anchor;
        int best = -1;
        double bestD = 1e18;
        for (int k = 0; k < m_icons.size(); ++k) {
            const QPoint d = m_icons[k].anchor - from;
            const bool ahead = key == Qt::Key_Left ? d.x() < -2 : key == Qt::Key_Right ? d.x() > 2 : key == Qt::Key_Up ? d.y() < -2 : d.y() > 2;
            if (!ahead)
                continue;
            const double along = key == Qt::Key_Left || key == Qt::Key_Right ? std::abs(d.x()) : std::abs(d.y());
            const double across = key == Qt::Key_Left || key == Qt::Key_Right ? std::abs(d.y()) : std::abs(d.x());
            const double score = along + 3 * across;
            if (score < bestD) {
                bestD = score;
                best = k;
            }
        }
        if (best >= 0)
            select(best, false);
        return true;
    }
    // type-select
    return false;
}

// ---- renaming in place

void DesktopIcons::startRename()
{
    if (m_selection.size() != 1 || m_rename)
        return;
    const int k = *m_selection.begin();
    if (m_icons[k].volume)
        return;   // a disk's name is its label: Get Info's
    const Icon &i = m_icons[k];
    m_renaming = k;
    auto *field = new QLineEdit(m_desktop);
    QFont font("Lucida Grande");
    font.setPixelSize(m_textSize);
    field->setFont(font);
    field->setFrame(false);
    field->setStyleSheet("QLineEdit { background: white; color: black; border: 1px solid rgba(0,0,0,115); padding: 0 2px; }");
    field->setAlignment(m_labelOnBottom ? Qt::AlignCenter : Qt::AlignLeft);
    const QString name = q(i.item.name);
    field->setText(name);
    const QFontMetricsF fm(font);
    const int w = std::max(int(metrics().labelWidth), int(fm.horizontalAdvance(name)) + 16);
    const QRect lr = labelRect(i);
    field->setGeometry(m_labelOnBottom ? lr.center().x() - w / 2 : lr.x(), lr.y(), w, int(metrics().lineHeight) + 4);
    const int dot = name.lastIndexOf('.');
    field->setSelection(0, !i.item.folder && dot > 0 ? dot : name.size());
    field->installEventFilter(this);
    m_rename = field;
    field->show();
    field->setFocus();
    m_desktop->update();
}

void DesktopIcons::endRename(bool commit)
{
    if (!m_rename)
        return;
    QLineEdit *field = m_rename;
    m_rename = nullptr;
    const int k = m_renaming;
    m_renaming = -1;
    QString name = field->text();
    field->hide();
    field->deleteLater();
    m_desktop->setFocus();
    if (!commit || k < 0 || k >= m_icons.size())
        return m_desktop->update();
    const Icon i = m_icons[k];
    name.replace('/', ':');
    if (name.isEmpty() || name == q(i.item.name))
        return m_desktop->update();
    if (name.startsWith('.')) {
        QMessageBox::warning(m_desktop, "Finder",
                             t("RN4", "You can’t use a name that begins with a dot “.”, because these names are reserved for the system. Please choose another name."));
        return m_desktop->update();
    }
    const QString to = desktopFolder() + "/" + name;
    if (QFileInfo::exists(to) && name.compare(q(i.item.name), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(m_desktop, "Finder", t("RN17", "The name “^0” is already taken. Please choose a different name.").replace("^0", name));
        return m_desktop->update();
    }
    // the extension changed: asked first (FXEnableExtensionChangeWarning)
    const QString before = QFileInfo(q(i.item.name)).suffix(), after = QFileInfo(name).suffix();
    if (!i.item.folder && before != after && !before.isEmpty() && !after.isEmpty() &&
        AK::Settings("finder").boolean("FXEnableExtensionChangeWarning", true)) {
        QMessageBox box(m_desktop);
        box.setText(t("RN12", "Are you sure you want to change the extension from “^0” to “^1”?").replace("^0", "." + before).replace("^1", "." + after));
        box.setInformativeText(t("RN13", "If you make this change, your document may open in a different application."));
        QPushButton *keep = box.addButton(t("RN20", "Keep ^0").replace("^0", "." + before), QMessageBox::RejectRole);
        box.addButton(t("RN19", "Use ^0").replace("^0", "." + after), QMessageBox::AcceptRole);
        box.setDefaultButton(keep);
        box.exec();
        if (box.clickedButton() == static_cast<QAbstractButton *>(keep))
            return m_desktop->update();
    }
    if (!QFile::rename(q(i.item.path), to))
        return m_desktop->update();
    // its place goes with it
    Finder::DSStore::Store store(s(desktopFolder()));
    for (const char *code : {"Iloc", "cmmt"})
        if (const Finder::DSStore::Record *r = store.get(i.item.name, code)) {
            Finder::DSStore::Record moved = *r;
            moved.name = s(name);
            store.remove(i.item.name, code);
            store.set(moved);
        }
    store.save();
    Finder::Labels::forget(i.item.path);
    reload();
    for (int n = 0; n < m_icons.size(); ++n)
        if (q(m_icons[n].item.path) == to)
            select(n, false);
}

bool DesktopIcons::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_rename) {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            QTimer::singleShot(0, this, [this] { endRename(false); });
            return true;
        }
        // Return renames here (QLineEdit would pass it on to the desktop, which would rename again)
        if (e->type() == QEvent::KeyPress &&
            (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Return || static_cast<QKeyEvent *>(e)->key() == Qt::Key_Enter)) {
            QTimer::singleShot(0, this, [this] { endRename(true); });
            return true;
        }
        if (e->type() == QEvent::FocusOut)
            QTimer::singleShot(0, this, [this] { endRename(true); });
    }
    return QObject::eventFilter(o, e);
}

// ---- the contextual menu (TContextMenu's table: the items', or the desktop's background's)

void DesktopIcons::showMenu(QPoint at, QPoint global)
{
    m_menuAt = at;
    auto *menu = new QMenu(m_desktop);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto add = [&](const QString &title, const QString &cmd, bool enabled = true) {
        menu->addAction(title, [this, cmd] { perform(cmd); })->setEnabled(enabled);
    };
    const QVector<Finder::Item> sel = selectedItems();
    if (!sel.isEmpty()) {
        bool volume = false, ejectable = true;
        for (int k : m_selection) {
            volume = volume || m_icons[k].volume;
            ejectable = ejectable && m_icons[k].ejectable;
        }
        const QString one = sel.size() == 1 ? q(sel.front().name) : QString();
        add(t("N151", "Open"), "cmdOpen:");
        if (sel.size() == 1 && !sel.front().folder) {
            // Open With ▸: the desktop's applications for the type, the default first
            QMenu *with = menu->addMenu(t("N152", "Open With"));
            const QString path = q(sel.front().path);
            QProcess mime;
            mime.start("xdg-mime", {"query", "filetype", path});
            mime.waitForFinished(2000);
            QString type = QString::fromUtf8(mime.readAllStandardOutput()).trimmed();
            auto defaultFor = [](const QString &t) {
                QProcess def;
                def.start("xdg-mime", {"query", "default", t});
                def.waitForFinished(3000);
                return QString::fromUtf8(def.readAllStandardOutput()).trimmed();
            };
            QString defaultId = defaultFor(type);
            if (defaultId.isEmpty() && type.startsWith("text/") && type != "text/plain") {
                type = "text/plain";   // its parent's
                defaultId = defaultFor(type);
            }
            QStringList ids;
            if (!defaultId.isEmpty())
                ids << defaultId;
            for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation)) {
                QFile cache(dir + "/mimeinfo.cache");
                if (!cache.open(QIODevice::ReadOnly))
                    continue;
                for (const QByteArray &l : cache.readAll().split('\n'))
                    if (l.startsWith(type.toUtf8() + "="))
                        for (const QByteArray &id : l.mid(type.size() + 1).split(';'))
                            if (!id.isEmpty() && !ids.contains(QString::fromUtf8(id)))
                                ids << QString::fromUtf8(id);
            }
            for (const QString &id : ids) {
                const QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id);
                if (file.isEmpty())
                    continue;
                QSettings entry(file, QSettings::IniFormat);
                QString name = entry.value("Desktop Entry/Name", id).toString();
                if (id == defaultId)
                    name += " (default)";
                with->addAction(name, [file, path] { QProcess::startDetached("gio", {"launch", file, path}); });
            }
            with->setEnabled(!with->isEmpty());
            Aqua::prepareMenu(with);
        }
        menu->addSeparator();
        if (volume)
            add(sel.size() == 1 ? core("N57", "Eject “^0”").replace("^0", one) : core("N58", "Eject"), "cmdEject:", ejectable);
        else
            add(t("N153", "Move to Trash"), "cmdMoveToTrash:");
        menu->addSeparator();
        add(t("N165", "Get Info"), "cmdShowStaticInfo:");
        if (!volume) {
            add(sel.size() == 1 ? t("N168", "Compress “^0”").replace("^0", one) : t("N169", "Compress ^0 Items").replace("^0", QString::number(sel.size())),
                "cmdArchive:");
            add(t("N154", "Duplicate"), "cmdDuplicate:");
        }
        add(t("N155", "Make Alias"), "cmdMakeAlias:");
        add(sel.size() == 1 ? core("N169.18", "Quick Look “^0”").replace("^0", one)
                            : t("N169.19", "Quick Look ^0 Items").replace("^0", QString::number(sel.size())),
            "cmdQuickLook:");
        menu->addSeparator();
        add(sel.size() == 1 ? core("N47", "Copy “^0”").replace("^0", one) : t("N48", "Copy ^0 Items").replace("^0", QString::number(sel.size())),
            "copy:");
        if (!volume) {
            // Label: the swatches (TLabelViewForMenu)
            menu->addSeparator();
            int current = Finder::Labels::get(sel.front().path);
            auto *action = new QWidgetAction(menu);
            action->setDefaultWidget(new SwatchRow(current, menu, [this](int index) { setLabel(index); }));
            menu->addAction(action);
        }
    } else {
        add(t("N156", "New Folder"), "cmdNewFolderAtMouseLocation:");
        menu->addSeparator();
        add(t("N165", "Get Info"), "cmdShowStaticInfo:");
        menu->addSeparator();
        add(t("N164", "Change Desktop Background…"), "cmdChangeDesktopBackground:");
        menu->addSeparator();
        add(t("N169.3", "Clean Up"), "cmdCleanUp:", m_arrangeBy == "none" || m_arrangeBy == "grid");
        QMenu *arrange = menu->addMenu(t("N148", "Arrange By"));
        static const char *const by[][2] = {{"None", "none"}, {"Name", "name"}, {"Date Modified", "dateModified"}, {"Date Created", "dateCreated"},
                                            {"Size", "size"}, {"Kind", "kind"}, {"Label", "label"}};
        for (const auto &b : by) {
            QAction *a = arrange->addAction(QString::fromLatin1(b[0]), [this, b] { arrangeBy(QString::fromLatin1(b[1])); });
            a->setCheckable(true);
            a->setChecked(m_arrangeBy == b[1] || (QByteArray(b[1]) == "none" && m_arrangeBy == "grid"));
        }
        Aqua::prepareMenu(arrange);
        add(core("N35", "Show View Options"), "cmdToggleViewOptions:");
    }
    Aqua::prepareMenu(menu);
    menu->popup(global);
}
