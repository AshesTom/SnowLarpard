// SPDX-License-Identifier: LGPL-2.1-or-later
// /Applications/Time Machine.app: the launcher (com.apple.backup.launcher, docs/timemachine-host.md
// section 1) and the browser it starts, "Enter Time Machine": Backup.framework's TBackupView
// (docs/timemachine-reverse.md) with the Finder's part of it (docs/timemachine-host.md) in one
// full-screen program:
//
//   - the backdrop (starfield.cpp): MPStarField's stars, sun and gas, VortexGL's clouds and twirl;
//   - the entry (showHideRevealAnimation:YES): the desktop sliding down off the screen, the front
//     Finder window flying to the front slot, the stack unfolding from behind it, the galaxy rising,
//     1.5 s on (0.4, 0, 0.6, 1); the exit the reverse; Shift makes every animation 6 times slower;
//   - the stack of ten slots (calculateLayerBoundsAtNormalizedZ:…), their shading, each window a
//     picture of the Finder's window showing the folder inside that snapshot (snapshots.cpp);
//   - the flights between snapshots (promoteDemoteSnapshot:): 0.5·(log2 n + 1) s, the stars rushing;
//   - the toolbar (Cancel, the date, Restore All, Restore), the Previous and Next arrows, the timeline
//     on the right with its magnification;
//   - Restore and Restore All: the items copied back to the original folder after the exit, a
//     conflict asking Keep Original, Keep Both (" (original)") or Replace.
//
// The launcher's numbers (the Dock's menu): none or 0 enter Time Machine, 1 opens Time Machine's
// preferences, 2 backs up now, 3 stops the backup, 4 (or --browse-other-disks) browses other disks
// (the chooser, then the chosen backups with the Computer as target). --folder <path> chooses the
// folder; otherwise the Finder's front window's (its state file), else the Desktop.
#include "snapshots.h"
#include "starfield.h"

#include "aqua.h"
#include "assets.h"
#include "x11.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QGridLayout>
#include <QLabel>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLFunctions_2_1>
#include <QOpenGLWindow>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QListWidget>
#include <QMenu>
#include <QTimer>

#include <xcb/xcb.h>
#ifdef SNOWLEOPARD_WAYLAND
#include "windowserverclient.h"
#endif

#include <signal.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

using namespace TM;

namespace {

const QString Backup = "/System/Library/PrivateFrameworks/Backup.framework";
const QString Resources = Backup + "/Versions/A/Resources/";
const QString FinderBundle = "/System/Library/CoreServices/Finder.app";

// Backup.framework's strings (a framework's Resources, not a bundle's Contents/Resources)
QString bu(const char *key, const char *fallback)
{
    const QString s = Assets::plist(Resources + "English.lproj/Localizable.strings")[QLatin1String(key)].toString();
    return s.isEmpty() ? QString(fallback) : s;
}
QString finder(const char *key, const char *fallback) { return Assets::tr(FinderBundle, "Localizable", key, fallback); }

// The Finder's Computer as a target (its URL "file:///?Computer"): browsing another disk starts there
const QString ComputerTarget = "computer:";

QString appsDir() { return QDir(QCoreApplication::applicationDirPath() + "/../../..").canonicalPath(); }

void openPreferences()
{
    QProcess::startDetached(appsDir() + "/System Preferences.app/Contents/MacOS/System Preferences", {"--pane", "com.apple.prefs.backup"});
}

QString configDir()
{
    const QByteArray x = qgetenv("XDG_CONFIG_HOME");
    return (x.isEmpty() ? QDir::homePath() + "/.config" : QString::fromLocal8Bit(x)) + "/snowleopard";
}

QString desktopDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return d.isEmpty() ? QDir::homePath() + "/Desktop" : d;
}

// the Finder's front window, from the state file FinderWindows writes for the menu bar
struct FinderFront {
    QString folder;
    QSize size;
    QStringList selection;
    bool icons = false;
    uint pid = 0;
};
FinderFront finderFront()
{
    FinderFront f;
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    QString d = QString::fromLocal8Bit(qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && !qgetenv("WAYLAND_DISPLAY").isEmpty()
                                           ? qgetenv("WAYLAND_DISPLAY")
                                           : qgetenv("DISPLAY"));
    d.replace('/', '_');
    QFile file((runtime.isEmpty() ? QString("/tmp") : QString::fromLocal8Bit(runtime)) + "/snowleopard-finder-" + QString::number(getuid()) + "-" +
               d + ".state");
    if (!file.open(QIODevice::ReadOnly))
        return f;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw);
        if (line.startsWith("pid "))
            f.pid = line.mid(4).toUInt();
        else if (line.startsWith("F "))
            f.folder = line.mid(2);
        else if (line.startsWith("W ")) {
            const QStringList wh = line.mid(2).split(' ');
            if (wh.size() == 2)
                f.size = QSize(wh[0].toInt(), wh[1].toInt() + 22);   // the content, and the title bar
        } else if (line.startsWith("I ")) {
            f.selection << QFileInfo(line.mid(2)).fileName();
        } else if (line == "C cmdViewAsIcons:") {
            f.icons = true;
        }
    }
    if (f.pid && kill(pid_t(f.pid), 0) != 0)
        return {};
    return f;
}

// the Finder window's frame on the screen (y down), title bar included: through X11, or on Wayland
// the window server's window events (sl_window_server_v1's frames)
QRect finderWindowRect(uint pid, const QString &title)
{
#ifdef SNOWLEOPARD_WAYLAND
    if (X11::wayland()) {
        QRect found;
        for (const WindowServerClient::WindowInfo &w : WindowServerClient::windows()) {
            if (w.appId != "Finder" || (pid && w.pid != pid) ||
                (w.flags & (WindowServerClient::Minimized | WindowServerClient::Hidden)))
                continue;
            const QRect r(w.x, w.y, w.width, w.height);
            if (QString::fromStdString(w.title) == title || found.isNull())
                found = r;
        }
        return found;
    }
#endif
    if (X11::wayland() || !X11::conn())
        return {};
    QRect found;
    for (xcb_window_t w : X11::clientWindows()) {
        if (X11::wmClass(w) != "Finder" || (pid && X11::pid(w) != pid) || X11::minimized(w) || X11::hidden(w))
            continue;
        xcb_connection_t *c = X11::conn();
        auto *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), nullptr);
        auto *t = xcb_translate_coordinates_reply(c, xcb_translate_coordinates(c, w, X11::root(), 0, 0), nullptr);
        if (g && t) {
            const QRect r(t->dst_x, t->dst_y - 22, g->width, g->height + 22);
            if (X11::title(w) == title || found.isNull())
                found = r;
        }
        free(g);
        free(t);
    }
    return found;
}

struct Anim {
    double start = 0, duration = 0, from = 0, to = 0;
    double x1 = 0.4, y1 = 0, x2 = 0.6, y2 = 1;
    bool running(double now) const { return duration > 0 && now < start + duration; }
    double value(double now) const
    {
        if (duration <= 0 || now >= start + duration)
            return to;
        if (now <= start)
            return from;
        return from + (to - from) * bezier(x1, y1, x2, y2, (now - start) / duration);
    }
};

// the stack's spacing curve (0x3bf3b)
const Spline &spacing()
{
    static const Spline s{{0, 0}, {0.7932, 0.6033}, {0.9211, 0.82}, {1, 1}};
    return s;
}

// calculateShadingFilterColorForSnapshotIndex: white at the front, dark and bluish in the middle
QColor shading(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const double e = t < 0.5 ? 2 * t : t <= 0.7 ? 1 : 1 - (t - 0.7) / 0.3;
    const double i = 0.3 * (t < 0.37 ? t / 0.34 : t <= 0.7 ? 1 : t <= 0.9 ? 1 - (t - 0.7) / 0.2 : 0);
    const double k = 1 - 0.8 * e;
    return QColor::fromRgbF(float(std::clamp(k * (1 - 0.2 * i), 0.0, 1.0)), float(std::clamp(k * (1 + i), 0.0, 1.0)),
                            float(std::clamp(k * (1 + 0.75 * i), 0.0, 1.0)));
}

struct Layer {
    QString folder;            // the folder inside this snapshot (or the live one)
    QDateTime date;            // invalid for Now
    QVector<Entry> entries;
    QString signature;
    bool valid = false;
    QImage picture;
    GLuint texture = 0;
};

struct TimelineItem {
    enum Kind { Placeholder, Month, Label, Snapshot, Current } kind;
    QString text;
    int layer = -1;            // the stack's index of a snapshot or Now
    // a TTimelineItemCluster's member: its cluster (the first member's index), its place and their number
    int cluster = -1, member = 0, members = 1;
};

struct Button {
    QRectF frame;              // on the screen, y up
    QString title;
    bool enabled = true;
};

class TimeMachine : public QOpenGLWindow {
public:
    TimeMachine(const QString &machine, const QString &target, QSize windowSize, QRect liveRect, const QImage &desktop, const QStringList &selection,
                bool icons)
        : m_target(target), m_desktop(desktop), m_icons(icons)
    {
        for (const QString &s : selection)
            m_selection.insert(s);
        QSurfaceFormat fmt;
        fmt.setVersion(2, 1);
        fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
        fmt.setSwapInterval(1);
        setFormat(fmt);
        QScreen *screen = QGuiApplication::primaryScreen();
        m_view = screen->geometry().size();
        setGeometry(screen->geometry());
        setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setTitle("Time Machine");
        // StartTimeMachineForFinder: the window at most the screen less 480 x 370, centred on its old frame
        QSize b = windowSize.isValid() ? windowSize : QSize(770, 383 + 59);
        const QSize max(m_view.width() - 480, m_view.height() - 370);
        const QSize shrunk(std::min(b.width(), max.width()), std::min(b.height(), max.height()));
        m_B = shrunk;
        // the front slot: centred, 140 points lower; the horizon 140 above the screen's top
        m_front = QRectF(std::round((m_view.width() - m_B.width()) / 2.0), std::round((m_view.height() - (m_B.height() + 140)) / 2.0),
                         m_B.width(), m_B.height());
        if (liveRect.isNull()) {
            m_live = m_front;
        } else {
            // y up, the shrunk window centred on the old frame
            const QRectF old(liveRect.x(), m_view.height() - liveRect.bottom() - 1, liveRect.width(), liveRect.height());
            m_live = QRectF(old.x() + std::round((old.width() - m_B.width()) / 2.0), old.y() + std::round((old.height() - m_B.height()) / 2.0),
                            m_B.width(), m_B.height());
            // the window isn't in the desktop's picture (the entering window is left out): the
            // desktop picture shows under it
            QPainter p(&m_desktop);
            const QImage wall = Assets::wallpaperFor(m_desktop.size());
            const QRect hole = liveRect.adjusted(-24, -14, 24, 34).intersected(m_desktop.rect());
            p.drawImage(hole.topLeft(), wall.copy(hole));
        }
        m_machine = machine;
        m_snapshots = snapshots(m_machine);
        retarget(m_target);
        m_clock.start();
        connect(&m_timer, &QTimer::timeout, this, [this] { update(); });
        m_searchTimer.setSingleShot(true);
        m_searchTimer.setInterval(500);
        connect(&m_searchTimer, &QTimer::timeout, this, [this] { applySearch(); });
        m_timer.start(16);
    }

    std::function<void(const QString &from, const QStringList &names, const QString &to)> onRestore;

protected:
    void initializeGL() override
    {
        m_f = new QOpenGLFunctions_2_1;
        m_f->initializeOpenGLFunctions();
        m_stars = std::make_unique<StarField>(m_f);
        m_desktopTex = uploadTexture(m_f, m_desktop);
        const char *const art[] = {"Left", "LeftDown", "Right", "RightDown", "Center", "CenterDown"};
        for (const char *a : art)
            m_art[a] = Assets::image(Resources + a + ".png");
        for (const char *a : {"PreviousButtonUp", "PreviousButtonDown", "NextButtonUp", "NextButtonDown"})
            m_arrowTex[a] = uploadTexture(m_f, Assets::image(Resources + a + ".png"));
        m_arrowSize["Previous"] = QSize(144, 48);
        m_arrowSize["Next"] = QSize(144, 55);
        m_tick[0] = Assets::image(Resources + "TickCap.png");
        m_tick[1] = Assets::image(Resources + "TickFill.png");
        startEntry();
    }

    void paintGL() override
    {
        const double now = time();
        advance(now);
        const int w = m_view.width(), h = m_view.height();
        m_stars->draw(now, w, h, m_galaxy.value(now), m_zOffset.value(now));

        // the layers: the view's points, y up (updateMatrices)
        m_f->glViewport(0, 0, w, h);
        m_f->glMatrixMode(GL_PROJECTION);
        m_f->glLoadIdentity();
        m_f->glOrtho(0, w, 0, h, -1, 1);
        m_f->glMatrixMode(GL_MODELVIEW);
        m_f->glLoadIdentity();
        m_f->glEnable(GL_TEXTURE_2D);
        m_f->glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        m_f->glEnable(GL_BLEND);
        m_f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied

        const double e = revealAmount(now);   // 0: the desktop, 1: in Time Machine
        const double top = m_top.value(now);
        // the stack, farthest first
        std::vector<std::pair<double, int>> order;
        for (int k = 0; k < m_layers.size(); ++k) {
            const double slot = k - top;
            if (slot > -1.0 && slot < 10.0)
                order.push_back({slot, k});
        }
        std::sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first > b.first; });
        const bool revealing = m_phase != Browsing;
        for (auto [slot, k] : order) {
            if (revealing && slot < 0.5)   // the window layer stands for the front one
                continue;
            QRectF r = slotRect(slot, false);
            if (revealing)
                r = lerp(slotRect(slot, true), r, e);
            const double t = std::clamp(slot, 0.0, 9.0) / 9.0;
            double opacity = slot < 0 ? 1 + slot : slot > 9 ? 10 - slot : 1;
            const QColor c = shading(t);
            drawTexture(layerTexture(k), r, opacity, c);
        }
        // the restored items' layers (fRestoreLayers, showRestoreAnimationStartingAt:withDuration:):
        // over the front window where they were, during the flight to Now, opacity 1, 1, 0 at 0, 0.99,
        // 1 of n·p + 2 s; hidden when the exit starts
        if (m_phase == Browsing)
            for (const RestoreLayer &r : m_restoreLayers) {
                const double u = (now - m_restoreStart) / m_restoreDuration;
                const double opacity = u < 0.99 ? 1 : u < 1 ? (1 - u) / 0.01 : 0;
                if (opacity > 0)
                    drawTexture(r.texture, r.frame, opacity);
            }
        // the controls
        if (m_toolbarDirty)
            rebuildToolbar();
        drawTexture(m_toolbarTex, QRectF(0, 10, w, 44), 1);
        for (const char *name : {"Previous", "Next"}) {
            const Button &b = m_arrows[name];
            const QString key = QString(name) + (m_pressed == name ? "ButtonDown" : "ButtonUp");
            drawTexture(m_arrowTex[key], b.frame, b.enabled ? 1 : 0.5);
        }
        if (m_timelineDirty || m_hover.running(now) || m_hover.to > 0)
            rebuildTimeline(now);
        drawTexture(m_timelineTex, QRectF(w - 400, 0, 400, h - 50), 1);

        // the reveal layer (the desktop) and the window layer, over everything while entering or leaving
        if (revealing) {
            drawTexture(m_desktopTex, QRectF(0, -e * h, w, h), 1);
            const int windowLayer = m_phase == Exiting ? 0 : m_current;
            drawTexture(layerTexture(windowLayer), lerp(m_live, m_front, e), 1);
        }
    }

    void mouseMoveEvent(QMouseEvent *ev) override
    {
        m_mouse = ev->position();
        const bool over = m_phase == Browsing && m_mouse.x() >= m_view.width() - 54 && m_mouse.y() <= m_view.height() - 50;
        const double now = time();
        if (over != (m_hover.to > 0)) {
            // the hover amount 0 → 1 in 225 ms
            const double v = m_hover.value(now);
            m_hover = Anim{now, 0.225 * std::fabs((over ? 1 : 0) - v), v, over ? 1.0 : 0.0, 0, 0, 1, 1};
        }
        m_timelineDirty = true;
    }

    void mousePressEvent(QMouseEvent *ev) override
    {
        if (m_phase != Browsing)
            return;
        const QPointF p = ev->position();
        const QPointF up(p.x(), m_view.height() - p.y());
        // the buttons
        for (Button *b : {&m_cancel, &m_restoreAll, &m_restore}) {
            if (b->frame.adjusted(23, 0, -23, 0).contains(up) || (b == &m_cancel && b->frame.contains(up)) ||
                (b == &m_restore && b->frame.contains(up))) {
                m_pressed = b->title;
                m_toolbarDirty = true;
                return;
            }
        }
        for (const char *name : {"Previous", "Next"})
            if (m_arrows[name].frame.contains(up) && m_arrows[name].enabled) {
                m_pressed = name;
                return;
            }
        // the timeline's ticks
        if (p.x() >= m_view.width() - 54 && p.y() <= m_view.height() - 50) {
            const int item = timelineItemAt(p.y());
            if (item >= 0 && m_items[item].layer >= 0)
                gotoSnapshot(m_items[item].layer, ev->modifiers());
            return;
        }
        // the front window: its rows
        const double topY = m_view.height() - (m_front.y() + m_front.height());
        const QPoint local(int(p.x() - m_front.x()), int(p.y() - topY));
        if (QRectF(QPointF(0, 0), m_front.size()).contains(local)) {
            Layer &l = m_layers[m_current];
            // the search field: typing goes there; its cancel button clears the search
            const QRect field = searchFieldRect(m_B);
            if (field.contains(local)) {
                if (!m_search.isEmpty() && local.x() >= field.right() - 18) {
                    m_search.clear();
                    applySearch();
                }
                m_searchFocus = true;
                selectionChanged();
                return;
            }
            if (m_searchFocus) {
                m_searchFocus = false;
                selectionChanged();
            }
            const int row = itemAt(m_B, local, int(l.entries.size()), iconsView());
            if (ev->button() == Qt::RightButton) {
                // the contextual menu on the item clicked, selected if it wasn't
                if (row >= 0 && !m_selection.contains(l.entries[row].name)) {
                    m_selection = {l.entries[row].name};
                    selectionChanged();
                }
                if (row >= 0)
                    contextMenu(ev->globalPosition().toPoint());
                return;
            }
            if (row < 0) {
                m_selection.clear();
            } else {
                const QString name = l.entries[row].name;
                if (ev->type() == QEvent::MouseButtonDblClick && l.entries[row].dir) {
                    if (m_target == ComputerTarget) {
                        // a volume of the Computer: the mounted one of that name, else (another
                        // computer's disk) the startup disk's place with that volume folder
                        m_volumeHint = name;
                        retarget(m_computerPaths.value(name, "/"));
                    } else {
                        // BUChangeTimeMachineTarget (a search's result: its folder, the search left)
                        m_search.clear();
                        m_searchScope.clear();
                        retarget(m_target + "/" + name);
                    }
                    return;
                }
                if (ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
                    if (!m_selection.remove(name))
                        m_selection.insert(name);
                } else {
                    m_selection = {name};
                }
            }
            selectionChanged();
            return;
        }
        // a window of the stack: go to it
        const double top = m_top.value(time());
        for (int k = m_current + 1; k < m_layers.size() && k - top < 10; ++k) {
            if (slotRect(k - top, false).contains(up)) {
                gotoSnapshot(k, ev->modifiers());
                return;
            }
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *ev) override { mousePressEvent(ev); }

    void mouseReleaseEvent(QMouseEvent *ev) override
    {
        const QPointF up(ev->position().x(), m_view.height() - ev->position().y());
        const QString pressed = m_pressed;
        m_pressed.clear();
        m_toolbarDirty = true;
        if (pressed.isEmpty() || m_phase != Browsing)
            return;
        if (pressed == "Previous" || pressed == "Next") {
            if (m_arrows[pressed].frame.contains(up))
                navigate(pressed == "Previous", ev->modifiers());
            return;
        }
        for (Button *b : {&m_cancel, &m_restoreAll, &m_restore})
            if (b->title == pressed && b->frame.contains(up) && b->enabled) {
                if (b == &m_cancel)
                    startExit();
                else
                    restore(b == &m_restoreAll);
            }
    }

    void wheelEvent(QWheelEvent *ev) override
    {
        if (m_phase == Browsing && ev->angleDelta().y())
            gotoSnapshot(std::clamp(m_current + (ev->angleDelta().y() > 0 ? 1 : -1), 0, int(m_layers.size()) - 1), ev->modifiers());
    }

    void keyPressEvent(QKeyEvent *ev) override
    {
        if (m_phase != Browsing)
            return;
        const bool command = ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
        if (m_searchFocus && !command) {
            // the search field's editing: the search runs after a pause in the typing, at once on Return
            switch (ev->key()) {
            case Qt::Key_Escape:
                if (m_search.isEmpty()) {
                    m_searchFocus = false;
                } else {
                    m_search.clear();
                    applySearch();
                }
                selectionChanged();
                return;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                m_searchTimer.stop();
                applySearch();
                return;
            case Qt::Key_Backspace:
                m_search.chop(1);
                m_searchTimer.start();
                selectionChanged();
                return;
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Tab:
                break;
            default:
                if (!ev->text().isEmpty() && ev->text()[0].isPrint()) {
                    m_search += ev->text();
                    m_searchTimer.start();
                    selectionChanged();
                    return;
                }
                break;
            }
        }
        switch (ev->key()) {
        case Qt::Key_Escape:   // the browser's cancel: → BUTimeMachineAction(1)
            startExit();
            break;
        case Qt::Key_Space:    // Quick Look (the space bar, ⌘Y)
            quickLook();
            break;
        case Qt::Key_Y:
            if (command)
                quickLook();
            break;
        case Qt::Key_I:        // Get Info
            if (command)
                getInfo();
            break;
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (command) {
                navigate(ev->key() == Qt::Key_Up, ev->modifiers() & ~(Qt::ControlModifier | Qt::MetaModifier));
            } else {
                const Layer &l = m_layers[m_current];
                if (l.entries.isEmpty())
                    break;
                int row = -1;
                for (int i = 0; i < l.entries.size(); ++i)
                    if (m_selection.contains(l.entries[i].name))
                        row = ev->key() == Qt::Key_Up && row >= 0 ? row : i;
                row = row < 0 ? 0 : std::clamp(row + (ev->key() == Qt::Key_Up ? -1 : 1), 0, int(l.entries.size()) - 1);
                m_selection = {l.entries[row].name};
                selectionChanged();
            }
            break;
        default:
            break;
        }
    }

private:
    enum Phase { Entering, Browsing, Exiting, Done };

    double time() const { return m_clock.nsecsElapsed() / 1e9; }
    static double slow() { return QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier ? 6.0 : 1.0; }   // FXEnableSlowAnimation
    static QRectF lerp(const QRectF &a, const QRectF &b, double t)
    {
        return QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t, a.width() + (b.width() - a.width()) * t,
                      a.height() + (b.height() - a.height()) * t);
    }

    // calculateLayerBoundsAtNormalizedZ:withBaseBounds:withTargetY: for slot i (1…9) of the stack based
    // on `base`, its horizon at `horizon`
    QRectF slotAt(int i, const QRectF &base, double horizon) const
    {
        if (i <= 0)
            return base;
        const double z = spacing()(std::min(i, 9) / 9.0);
        const double s = base.width() / 80.0, D = 1 + (s - 1) * z;
        const QPointF moved(base.x() + z * s * (m_view.width() / 2.0 - base.center().x()), base.y() + z * s * (horizon - (base.y() + base.height())));
        const QPointF c(base.center().x(), base.y() + base.height());
        return QRectF((moved - c) / D + c, base.size() / D);
    }
    // a fractional slot: between its two slots (the keyframes); in front (−1) larger and fading
    QRectF slotRect(double slot, bool collapsed) const
    {
        const QRectF base = collapsed ? m_live : m_front;
        const double horizon = collapsed ? 0 : m_view.height() + 140;
        if (slot < 0) {
            const QRectF f = base;
            const QRectF big(f.center().x() - f.width() * 0.65, f.y() - f.height() * 0.45, f.width() * 1.3, f.height() * 1.3);
            return lerp(base, big, std::min(-slot, 1.0));
        }
        const int lo = int(std::floor(std::min(slot, 9.0))), hi = std::min(lo + 1, 9);
        return lerp(slotAt(lo, base, horizon), slotAt(hi, base, horizon), std::min(slot, 9.0) - lo);
    }

    // updateAnimationEnd: the galaxy rises to the top of the farthest visible window
    double galaxyTarget(int top) const
    {
        const int slot = std::clamp(int(m_layers.size()) - top - 1, 0, 9);
        const QRectF r = slotRect(slot, false);
        return std::round(r.y() + r.height());
    }

    double revealAmount(double now) const
    {
        if (m_phase == Browsing || m_phase == Done)
            return 1;
        const double u = m_revealDuration > 0 ? std::clamp((now - m_revealStart) / m_revealDuration, 0.0, 1.0) : 1;
        const double e = bezier(0.4, 0, 0.6, 1, u);
        return m_phase == Entering ? e : 1 - e;
    }

    void startEntry()
    {
        const double now = time();
        m_phase = Entering;
        m_revealStart = now;
        m_revealDuration = 1.5 * slow();   // _FXBackupRevealDuration_ms
        // triggerSunriseAnimationFromY:toY:duration:, eased (0.42, 0, 0.58, 1)
        m_galaxy = Anim{now, m_revealDuration, 0, galaxyTarget(m_current), 0.42, 0, 0.58, 1};
        m_top = Anim{now, 0, double(m_current), double(m_current)};
    }

    void startExit()
    {
        if (m_phase != Browsing)
            return;
        if (m_finderUsed)
            finderCommand("exit:");   // the Quick Look panel closed, the Info windows back to their level
        const double now = time();
        m_phase = Exiting;
        m_revealStart = now;
        m_revealDuration = 1.5 * slow();
        // setSunAnimationStart: the galaxy sets
        m_galaxy = Anim{now, m_revealDuration, m_galaxy.value(now), 0, 0.42, 0, 0.58, 1};
        m_hover = Anim{now, 0, 0, 0};
        m_timelineDirty = true;
    }

    void advance(double now)
    {
        autoScrollTimeline(now);
        if ((m_phase == Entering || m_phase == Exiting) && now >= m_revealStart + m_revealDuration) {
            if (m_phase == Entering) {
                m_phase = Browsing;   // revealInAnimationComplete:
            } else {
                m_phase = Done;       // revealOutAnimationComplete:
                QTimer::singleShot(0, this, [this] { finish(); });
            }
        }
        if (m_phase == Browsing && !m_top.running(now) && m_afterFlight) {
            auto f = std::move(m_afterFlight);
            m_afterFlight = nullptr;
            f();
        }
        if (m_phase == Browsing && !m_top.running(now) && m_requested >= 0) {   // fRequestedSnapshot
            const int r = m_requested;
            m_requested = -1;
            gotoSnapshot(r, {});
        }
    }

    void finish()
    {
        hide();
        if (onRestore && !m_pendingNames.isEmpty())
            onRestore(m_pendingFrom, m_pendingNames, m_pendingTo);
        QCoreApplication::quit();
    }

    // promoteDemoteSnapshot: n = min(|target − top|, 10), the whole flight 0.5·(log2 n + 1) s
    void gotoSnapshot(int target, Qt::KeyboardModifiers)
    {
        target = std::clamp(target, 0, int(m_layers.size()) - 1);
        const double now = time();
        if (m_top.running(now)) {
            m_requested = target;
            return;
        }
        if (target == m_current)
            return;
        const int n = std::min(std::abs(target - m_current), 10);
        const double d = n / (std::log2(double(n)) + 1);
        const double duration = n * (0.5 / d) * slow();
        m_top = Anim{now, duration, double(m_current), double(target)};
        // triggerSnapshotWarp: the stars' z offset ±n/2, eased (0.42, 0, 0.58, 1); older: towards the viewer
        const double z = m_zOffset.value(now);
        m_zOffset = Anim{now, duration, z, z + (target > m_current ? 0.5 : -0.5) * n, 0.42, 0, 0.58, 1};
        m_galaxy = Anim{now, duration, m_galaxy.value(now), galaxyTarget(target), 0.42, 0, 0.58, 1};
        m_current = target;
        m_toolbarDirty = m_timelineDirty = true;
        followQuickLook();
    }

    // previousPressed: / nextPressed: smart navigation by default (the next snapshot where the folder
    // changed), the adjacent one with ⌘
    void navigate(bool older, Qt::KeyboardModifiers mods)
    {
        const int step = older ? 1 : -1;
        int k = m_current + step;
        if (!(mods & (Qt::ControlModifier | Qt::MetaModifier))) {
            const QString here = m_layers[m_current].signature;
            for (int i = k; i >= 0 && i < m_layers.size(); i += step)
                if (m_layers[i].valid && m_layers[i].signature != here) {
                    k = i;
                    break;
                }
        }
        if (k >= 0 && k < m_layers.size())
            gotoSnapshot(k, mods);
    }

    void retarget(const QString &target)
    {
        m_target = target;
        for (Layer &l : m_layers)
            if (l.texture && m_f)
                m_f->glDeleteTextures(1, &l.texture);
        m_layers.clear();
        Layer now;
        now.folder = target;
        now.valid = target == ComputerTarget || QFileInfo(target).isDir();
        m_layers.push_back(now);
        for (const Snapshot &s : m_snapshots) {
            Layer l;
            // the Computer inside a snapshot: its volumes' folders
            l.folder = target == ComputerTarget ? s.path : folderInSnapshot(s.path, target, m_volumeHint);
            l.valid = !l.folder.isEmpty();
            l.date = s.date;
            m_layers.push_back(l);
        }
        for (Layer &l : m_layers) {
            if (l.valid && l.folder == ComputerTarget) {
                for (const Volume &v : volumes()) {
                    Entry e;
                    e.name = v.name;
                    e.dir = true;
                    e.kind = "Volume";
                    e.icon = volumeIconFile(v);
                    m_computerPaths[v.name] = v.path;
                    l.entries.push_back(e);
                }
            } else if (l.valid && !m_search.isEmpty()) {
                // the query in this snapshot: its scope moved into it (-updateSearchTarget:)
                l.entries = searchFolder(l.folder, m_search);
            } else if (l.valid) {
                l.entries = listFolder(l.folder);
                if (target == ComputerTarget)
                    for (Entry &e : l.entries) {
                        e.kind = "Volume";
                        e.icon = volumeIconFile(Volume{e.name, QString(), Volume::Internal});
                    }
            }
            l.signature = signature(l.entries);
            if (!m_search.isEmpty()) {
                // NavigateForwardOrBackward in a search: where results appear or disappear
                l.signature.clear();
                for (const Entry &e : l.entries)
                    l.signature += e.name + '\n';
            }
        }
        // the selection mapped into the new folder (mapBackupSelectionToContainer:)
        QSet<QString> kept;
        for (const Entry &e : m_layers[std::min(m_current, int(m_layers.size()) - 1)].entries)
            if (m_selection.contains(e.name))
                kept.insert(e.name);
        m_selection = kept;
        m_current = std::min(m_current, int(m_layers.size()) - 1);
        m_top = Anim{0, 0, double(m_current), double(m_current)};
        m_toolbarDirty = m_timelineDirty = true;
        buildTimelineItems();
        if (m_phase == Browsing)
            m_galaxy = Anim{time(), 0.5, m_galaxy.value(time()), galaxyTarget(m_current), 0.42, 0, 0.58, 1};
    }

    void selectionChanged()
    {
        for (Layer &l : m_layers)
            l.picture = QImage();
        m_toolbarDirty = true;
        followQuickLook();
    }

    // The search field's query (host 10): the target becomes the search of the folder (the Computer is
    // not searched), its results in every snapshot; an empty field goes back to the folder
    void applySearch()
    {
        if (m_search.isEmpty()) {
            if (!m_searchScope.isEmpty()) {
                const QString scope = m_searchScope;
                m_searchScope.clear();
                m_selection.clear();
                retarget(scope);
            }
            return;
        }
        if (m_target == ComputerTarget)
            return;
        if (m_searchScope.isEmpty())
            m_searchScope = m_target;
        m_selection.clear();
        retarget(m_searchScope);
    }

    // The front window's selected items in its snapshot (getQuickLookNodes:, the info nodes): the
    // Finder's Quick Look and Get Info show them (docs/timemachine-host.md 8)
    QStringList selectedPaths() const
    {
        QStringList out;
        if (m_layers.isEmpty() || !m_layers[m_current].valid)
            return out;
        const Layer &l = m_layers[m_current];
        for (const Entry &e : l.entries)
            if (m_selection.contains(e.name))
                out << (l.folder == ComputerTarget ? m_computerPaths.value(e.name) : l.folder + "/" + e.name);
        out.removeAll(QString());
        return out;
    }
    // the Finder's commands in the 'peab' UI mode, over its socket (FinderWindows --line "items:…")
    static void finderCommand(const QString &command, const QStringList &paths = {})
    {
        QProcess::startDetached(appsDir() + "/Finder.app/Contents/MacOS/FinderWindows",
                                {"--line", "items:peab:" + command + (paths.isEmpty() ? QString() : "\t" + paths.join('\t'))});
    }
    void getInfo()
    {
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty()) {
            finderCommand("cmdShowStaticInfo:", paths);
            m_finderUsed = true;
        }
    }
    void quickLook()
    {
        // quickLookShouldOpenFullscreenWithEvent: NO in Time Machine: the panel, never full screen
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty() || m_quickLook) {
            finderCommand("cmdQuickLook:", paths);
            m_quickLook = !m_quickLook;   // the Finder toggles the panel
            m_quickLookPaths = paths;
            m_finderUsed = true;
        }
    }
    // the panel follows the selection, and the snapshot shown (-updatePreviewPanel)
    void followQuickLook()
    {
        if (m_quickLook && m_phase == Browsing) {
            const QStringList paths = selectedPaths();
            if (!paths.isEmpty() && paths != m_quickLookPaths)
                finderCommand("follow:", paths);
            m_quickLookPaths = paths;
        }
    }
    // buildContextMenu:… in the 'peab' mode: the selection's Get Info and Quick Look
    void contextMenu(const QPoint &global)
    {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty())
            return;
        QMenu menu;
        menu.addAction(finder("N165", "Get Info"), [this] { getInfo(); });
        const QString one = QFileInfo(paths.first()).fileName();
        menu.addAction(paths.size() == 1 ? Assets::tr(FinderBundle, "LocalizableCore", "N169.18", "Quick Look “^0”").replace("^0", one)
                                         : finder("N169.19", "Quick Look ^0 Items").replace("^0", QString::number(paths.size())),
                       [this] {
                           if (!m_quickLook)
                               quickLook();
                       });
        menu.exec(global);
    }

    GLuint layerTexture(int k)
    {
        Layer &l = m_layers[k];
        if (l.picture.isNull()) {
            if (l.texture)
                m_f->glDeleteTextures(1, &l.texture);
            if (l.valid) {
                WindowPicture w;
                w.title = m_target == ComputerTarget ? QSysInfo::machineHostName().section('.', 0, 0) : displayName(m_target);
                w.entries = l.entries;
                w.selected = m_selection;
                w.home = QDir::homePath();
                w.available = l.folder == ComputerTarget ? -1 : QStorageInfoAvailable(l.folder);
                w.icons = iconsView();
                w.search = m_search;
                w.searchFocus = m_searchFocus;
                if (!m_searchScope.isEmpty() && !m_search.isEmpty())
                    w.title = finder("FF31", "Searching “^0”").replace("^0", displayName(m_searchScope));
                if (m_target == ComputerTarget)
                    w.titleIcon = "com.apple.imac-aluminum-24.icns";   // the computer's model icon, as the Finder's
                l.picture = paintWindow(w, m_B);
            } else {
                l.picture = invalidPicture(m_B);
            }
            l.texture = uploadTexture(m_f, l.picture);
        }
        return l.texture;
    }

    static qint64 QStorageInfoAvailable(const QString &path);

    void drawTexture(GLuint tex, const QRectF &r, double opacity, const QColor &c = Qt::white)
    {
        if (!tex || opacity <= 0)
            return;
        m_f->glBindTexture(GL_TEXTURE_2D, tex);
        m_f->glColor4d(c.redF() * opacity, c.greenF() * opacity, c.blueF() * opacity, opacity);
        m_f->glBegin(GL_QUADS);
        m_f->glTexCoord2d(0, 1);
        m_f->glVertex2d(r.left(), r.top());
        m_f->glTexCoord2d(1, 1);
        m_f->glVertex2d(r.right(), r.top());
        m_f->glTexCoord2d(1, 0);
        m_f->glVertex2d(r.right(), r.bottom());
        m_f->glTexCoord2d(0, 0);
        m_f->glVertex2d(r.left(), r.bottom());
        m_f->glEnd();
    }

    // ---- the toolbar (TMToolbarPushButton, TMDateText)

    static QFont helvetica(double size)
    {
        QFont f;
        f.setFamilies({"Helvetica Neue", "Helvetica", "sans-serif"});
        f.setPixelSize(int(size));
        f.setBold(true);
        return f;
    }
    // optimalSize: the title in HelveticaNeue-Bold 12 scaled by 1.75
    static double optimalWidth(const QString &title)
    {
        return std::round(QFontMetricsF(helvetica(12)).horizontalAdvance(title) * 1.75 + 92 + 20);
    }

    // dividerAngleForPosition:withEndCapAngle: (48°): the perspective's slant at x
    double dividerWidth(double x) const
    {
        const double W = m_view.width();
        x = std::clamp(x, 23.0, W - 23);
        const double a = std::atan2(((W - 20) / 2 * std::tan(48 * M_PI / 180) - 22), (W / 2 - x));
        return std::round(38 / std::tan(a));
    }

    QString dateTitle() const
    {
        if (m_current == 0)
            return bu("BU20", "Today (Now)");
        const QDateTime d = m_layers[m_current].date;
        const QLocale en(QLocale::English, QLocale::UnitedStates);
        const QString time = en.toString(d.time(), "h:mm AP");
        const QDate today = QDate::currentDate();
        QStringList formats;
        if (d.date() == today)
            formats << bu("U18", "Today at ^0").replace("^0", time);
        else if (d.date() == today.addDays(-1))
            formats << bu("U19", "Yesterday at ^0").replace("^0", time);
        else
            formats << en.toString(d.date(), "dddd, MMMM d, yyyy") + " " + time << en.toString(d.date(), "MMMM d, yyyy") + " " + time
                    << en.toString(d.date(), "MMM d, yyyy") + " " + time;
        formats << en.toString(d.date(), "M/d/yy") + " " + time;
        const double width = m_date.frame.width();
        for (const QString &f : formats)
            if (QFontMetricsF(helvetica(15)).horizontalAdvance(f) * 1.75 + 112 + 2 * 44 * 0.7071 < width)
                return f;
        return formats.last();
    }

    void layoutToolbar()
    {
        const double W = m_view.width();
        m_cancel.title = bu("BU9", "Cancel");
        m_restore.title = bu("BU12", "Restore");
        m_restoreAll.title = bu("BU11", "Restore All");
        m_cancel.frame = QRectF(10, 10, optimalWidth(m_cancel.title), 44);
        const double rw = optimalWidth(m_restore.title);
        m_restore.frame = QRectF(W - 10 - rw, 10, rw, 44);
        const double aw = optimalWidth(m_restoreAll.title);
        m_restoreAll.frame = QRectF(m_restore.frame.x() + 46 - aw, 10, aw, 44);
        m_date.frame = QRectF(m_cancel.frame.right() - 46, 10, m_restoreAll.frame.x() + 46 - (m_cancel.frame.right() - 46), 44);
        // updateButtonsEnableStateForTargetIndex: Restore and Restore All for a snapshot other than Now
        // (the Computer's volumes aren't restored)
        const bool snapshot = m_current > 0 && m_layers[m_current].valid && m_target != ComputerTarget;
        m_restoreAll.enabled = snapshot && !m_layers[m_current].entries.isEmpty();
        bool any = false;
        for (const Entry &e : m_layers[m_current].entries)
            any |= m_selection.contains(e.name);
        m_restore.enabled = snapshot && any;
        // the arrows beside the front window (bigArrowsLocationForWindowBounds:)
        m_arrows["Previous"].frame = QRectF(m_front.right() + 40, m_front.y(), 144, 48);
        m_arrows["Next"].frame = QRectF(m_front.right() + 40, m_front.y() - 55, 144, 55);
        m_arrows["Previous"].enabled = m_current + 1 < m_layers.size();
        m_arrows["Next"].enabled = m_current > 0;
    }

    void rebuildToolbar()
    {
        m_toolbarDirty = false;
        layoutToolbar();
        const int W = m_view.width();
        QImage img(W, 44, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        // drawBackground: the caps at the ends, the centre stretched between (the "Down" art while pressed)
        auto art = [&](const char *name, bool down) { return m_art[QString(name) + (down ? "Down" : "")]; };
        auto pressed = [&](const Button &b) { return !m_pressed.isEmpty() && m_pressed == b.title; };
        auto centre = [&](double x0, double x1, bool down) { p.drawImage(QRectF(x0, 0, x1 - x0, 44), art("Center", down)); };
        centre(m_cancel.frame.x() + 51, m_restore.frame.right() - 51, false);
        for (const Button *b : {&m_cancel, &m_restoreAll, &m_restore})
            if (pressed(*b))
                centre(std::max(b->frame.x() + 23, m_cancel.frame.x() + 51), std::min(b->frame.right() - 23, m_restore.frame.right() - 51), true);
        p.drawImage(QPointF(m_cancel.frame.x(), 0), art("Left", pressed(m_cancel)));
        p.drawImage(QPointF(m_restore.frame.right() - 51, 0), art("Right", pressed(m_restore)));
        // the dividers, 23 points inside each joining edge, slanted as the bar's perspective
        for (double x : {m_cancel.frame.right() - 23, m_date.frame.right() - 23, m_restoreAll.frame.right() - 23}) {
            const double dw = dividerWidth(x);
            auto line = [&](double dx, double y0, double y1, double slant, double alpha) {
                p.setPen(QPen(QColor(255, 255, 255, int(alpha * 255)), 1));
                p.drawLine(QPointF(x + dx + 0.5, y0), QPointF(x + dx + 0.5 + slant, y1));
            };
            p.setRenderHint(QPainter::Antialiasing);
            line(0, 38, 0, dw, 0.3);
            line(0, 38, 44, 0, 0.4);
            line(1, 38, 44, 0, 0.15);
            line(1, 38, 0, dw, 0.1);
        }
        // the labels: drawn in perspective (each corner along the bar's slant), a shadow 1 point lower,
        // a faint copy 5 points up
        auto label = [&](const QRectF &frame, const QString &text, double size, bool enabled, double left, double right) {
            const QFont font = helvetica(size);
            const QFontMetricsF fm(font);
            const double tw = fm.horizontalAdvance(text) * 1.75, th = fm.height() * 1.18;
            QImage t(int(std::ceil(tw)) + 4, int(std::ceil(th)) + 4, QImage::Format_ARGB32_Premultiplied);
            const QRectF box = frame.adjusted(left, 0, -right, 0);
            for (int pass = 0; pass < 2; ++pass) {
                t.fill(Qt::transparent);
                QPainter tp(&t);
                tp.setRenderHint(QPainter::TextAntialiasing);
                tp.scale(1.75, 1.18);
                tp.setFont(font);
                tp.setPen(pass == 0 ? QColor(0, 0, 0) : enabled ? QColor(255, 255, 255) : QColor(128, 128, 128));
                tp.drawText(QPointF(1, fm.ascent() + 1), text);
                tp.end();
                const double cx = box.center().x(), base = 44 - 8.6 - (pass == 0 ? -1 : 0);
                const double x0 = cx - tw / 2, x1 = cx + tw / 2, y0 = base - th, y1 = base;
                auto slant = [&](double x, double y) { return QPointF(x + (44 - 6 - y) / 38 * dividerWidth(x) * 0.5, y); };
                QTransform tr;
                if (QTransform::quadToQuad(QPolygonF(QRectF(0, 0, t.width(), t.height())),
                                           QPolygonF({slant(x0, y0), slant(x1, y0), slant(x1, y1), slant(x0, y1)}), tr)) {
                    p.save();
                    p.setTransform(tr);
                    p.setOpacity(0.05);
                    p.drawImage(QPointF(0, -5 * t.height() / th), t);
                    p.setOpacity(1);
                    p.drawImage(QPointF(0, 0), t);
                    p.restore();
                }
            }
        };
        label(m_cancel.frame, m_cancel.title, 12, m_cancel.enabled, 23 - 13, 23);
        label(m_restoreAll.frame, m_restoreAll.title, 12, m_restoreAll.enabled, 23, 23);
        label(m_restore.frame, m_restore.title, 12, m_restore.enabled, 23, 23 + 10);
        // the date, centred on the screen
        const QString date = dateTitle();
        const double dw = m_date.frame.width();
        label(QRectF(W / 2.0 - dw / 2, 0, dw, 44), date, 15, true, 0, 0);
        p.end();
        if (m_toolbarTex)
            m_f->glDeleteTextures(1, &m_toolbarTex);
        m_toolbarTex = uploadTexture(m_f, img);
    }

    // ---- the timeline (TMTimeline, TTimelineControl)

    void buildTimelineItems()
    {
        m_items.clear();
        const QLocale en(QLocale::English, QLocale::UnitedStates);
        const QDate today = QDate::currentDate();
        QDate month;
        int year = 0;
        bool yesterday = false, todayDone = false;
        // oldest → newest, then Now at the bottom
        for (int k = int(m_layers.size()) - 1; k >= 1; --k) {
            const QDate d = m_layers[k].date.date();
            if (!month.isValid() || d.year() != month.year() || d.month() != month.month()) {
                month = d;
                QString text = en.monthName(d.month());
                if (d.year() != year && (year != 0 || d.year() != today.year()))
                    text += " " + QString::number(d.year());
                year = d.year();
                m_items.push_back({TimelineItem::Month, text, -1});
            }
            if (d == today.addDays(-1) && !yesterday) {
                yesterday = true;
                m_items.push_back({TimelineItem::Label, bu("BU18", "Yesterday"), -1});
            }
            if (d == today && !todayDone) {
                todayDone = true;
                m_items.push_back({TimelineItem::Label, bu("BU17", "Today"), -1});
            }
            m_items.push_back({TimelineItem::Snapshot, QString(), k});
        }
        m_items.push_back({TimelineItem::Current, bu("BU19", "Now"), 0});
        // placeholders for the months before the oldest snapshot, while the column has room
        const int capacity = int((m_view.height() - 50 - 60) / 16);
        QDate before = m_layers.size() > 1 ? m_layers.back().date.date() : today;
        before = QDate(before.year(), before.month(), 1);
        while (int(m_items.size()) < capacity && m_items.size() < 400) {
            before = before.addMonths(-1);
            m_items.insert(m_items.begin(), {TimelineItem::Placeholder, QString(), -1});
        }
        clusterTimeline();
        m_timelineDirty = true;
    }

    // FUN_0004a37c: when the items' gap ((H − 2N) / N, FUN_00049046) is under 4 points, the runs of
    // items after each month or label (up to the next one or Now) are cut to at most k items, the
    // largest k up to 31 FUN_00049046 allows, by TTimelineItemClusters (FUN_0004a03c)
    void clusterTimeline()
    {
        const double colH = m_view.height() - 50 - 60;
        const int n = int(m_items.size());
        auto anchor = [](const TimelineItem &it) { return it.kind == TimelineItem::Month || it.kind == TimelineItem::Label; };
        auto gap = [&](int k) {
            int over = 0, run = 0;
            for (const TimelineItem &it : m_items) {
                if (anchor(it))
                    run = 0;
                else if (it.kind != TimelineItem::Current && ++run > k)
                    ++over;
            }
            const double N = n - over;
            return (colH - 2 * N) / N;
        };
        if (n == 0 || !(gap(31) < 4))
            return;
        // with 3 a run still too many FUN_00049d22 regroups by date: not ported, 3 kept
        int k = 31;
        if (gap(3) < 4)
            k = 3;
        else
            while (k > 1 && gap(k - 1) < 4)
                --k;
        for (int i = 0; i < n;) {
            if (!anchor(m_items[i])) {
                ++i;
                continue;
            }
            int first = i + 1, len = 0;
            while (first + len < n && !anchor(m_items[first + len]) && m_items[first + len].kind != TimelineItem::Current)
                ++len;
            if (len > k) {
                // the run's items dealt into k buckets, ratio len / k each, from 0.5
                const double ratio = double(len) / k;
                double acc = 0.5;
                int bucket = 0;
                std::vector<int> owner(static_cast<size_t>(len));
                for (int j = 0; j < len; ++j) {
                    if (ratio < acc) {
                        ++bucket;
                        acc -= ratio;
                    }
                    owner[size_t(j)] = bucket;
                    acc += 1;
                }
                for (int j = 0; j < len;) {
                    int e = j;
                    while (e < len && owner[size_t(e)] == owner[size_t(j)])
                        ++e;
                    if (e - j > 1)
                        for (int m = j; m < e; ++m) {
                            TimelineItem &it = m_items[first + m];
                            it.cluster = first + j;
                            it.member = m - j;
                            it.members = e - j;
                        }
                    j = e;
                }
            }
            i = first + len;
        }
    }

    // the items' centres in the timeline's picture (flipped: item 0 at the top), with the hover's spacing
    std::vector<double> timelineLayout(double h, double mouseY) const
    {
        const double H = m_view.height() - 50, colTop = 30, colBottom = H - 30;
        const int n = int(m_items.size());
        double colH = colBottom - colTop;
        // a cluster takes one slot (N its members counted once)
        int slotCount = 0;
        for (const TimelineItem &it : m_items)
            slotCount += it.member == 0;
        const double slot = std::min(colH / std::max(slotCount, 1), 16.0);
        colH = slot * slotCount;
        std::vector<double> y(static_cast<size_t>(n), 0.0);
        if (slotCount < n) {
            // FUN_0004725a, FUN_00044cba…e20: each item's gap before and after, (slot − 2) / 2 at rest
            // (a cluster's at its ends only), 7 magnified; its tick 2, a member's 2 / members at rest
            double pos = colBottom - colH;
            for (int i = 0; i < n; ++i) {
                const TimelineItem &it = m_items[i];
                const bool head = it.member == 0, tail = it.member == it.members - 1;
                const double edge = (slot - 2) / 2;
                const double before = (head ? edge : 0) * (1 - h) + 7 * h, after = (tail ? edge : 0) * (1 - h) + 7 * h;
                const double core = 2.0 / it.members * (1 - h) + 2 * h;
                y[size_t(i)] = pos + before + core / 2;
                pos += before + core + after;
            }
            if (h > 0) {
                int anchor = 0;
                for (int i = 0; i < n; ++i)
                    if (std::fabs(y[size_t(i)] - mouseY) < std::fabs(y[size_t(anchor)] - mouseY))
                        anchor = i;
                if (m_tlAnchor >= 0 && m_tlAnchor < n)
                    anchor = m_tlAnchor;
                // the anchor's place at rest kept
                const std::vector<double> rest = timelineLayout(0, mouseY);
                const double shift = rest[size_t(anchor)] - y[size_t(anchor)] + m_tlScroll;
                for (double &v : y)
                    v += shift;
            }
            return y;
        }
        for (int i = 0; i < n; ++i)
            y[size_t(i)] = colBottom - colH + (i + 0.5) * slot;
        if (h > 0) {
            // the spacing grows to 16; the item under the mouse when the magnification began stays
            // where it is (FUN_000493bc), the rest reached by the auto-scroll
            int anchor = 0;
            for (int i = 0; i < n; ++i)
                if (std::fabs(y[size_t(i)] - mouseY) < std::fabs(y[size_t(anchor)] - mouseY))
                    anchor = i;
            if (m_tlAnchor >= 0 && m_tlAnchor < n)
                anchor = m_tlAnchor;
            const double sp = slot + (16 - slot) * h, ay = y[size_t(anchor)];
            for (int i = 0; i < n; ++i)
                y[size_t(i)] = ay + (i - anchor) * sp + m_tlScroll;
        }
        return y;
    }

    // FUN_00049620: near either end of the column (30 points) the magnified column auto-scrolls by
    // ±(1 + 39f) points every 0.04 s, f the depth into the zone, as far as its ends
    void autoScrollTimeline(double now)
    {
        const double dt = std::clamp(now - m_tlScrollTime, 0.0, 0.1);
        m_tlScrollTime = now;
        const double h = m_hover.value(now);
        if (h <= 0 || m_phase != Browsing || m_items.empty()) {
            m_tlScroll = 0;
            m_tlAnchor = -1;
            return;
        }
        if (m_tlAnchor < 0) {
            const std::vector<double> y = timelineLayout(0, m_mouse.y());
            int a = 0;
            for (int i = 0; i < int(y.size()); ++i)
                if (std::fabs(y[size_t(i)] - m_mouse.y()) < std::fabs(y[size_t(a)] - m_mouse.y()))
                    a = i;
            m_tlAnchor = a;
        }
        const double H = m_view.height() - 50, colTop = 30, colBottom = H - 30, my = m_mouse.y();
        double v = 0;
        if (my >= 0 && my < colTop + 30)
            v = 1 + 39 * std::clamp((colTop + 30 - my) / 30, 0.0, 1.0);    // towards the older items
        else if (my > colBottom - 30 && my <= H)
            v = -(1 + 39 * std::clamp((my - (colBottom - 30)) / 30, 0.0, 1.0));
        if (v == 0)
            return;
        const double before = m_tlScroll;
        m_tlScroll += v * dt / 0.04;
        // as far as the first item reaching the column's top, the last its bottom
        const double s = m_tlScroll;
        m_tlScroll = 0;
        const std::vector<double> y = timelineLayout(h, my);
        m_tlScroll = std::clamp(s, std::min(0.0, colBottom - y.back()), std::max(0.0, colTop - y.front()));
        if (m_tlScroll != before)
            m_timelineDirty = true;
    }

    int timelineItemAt(double screenY) const
    {
        const double h = m_hover.value(time());
        const std::vector<double> y = timelineLayout(h, screenY);
        int best = -1;
        for (int i = 0; i < int(y.size()); ++i)
            if (m_items[i].kind == TimelineItem::Snapshot || m_items[i].kind == TimelineItem::Current)
                if (best < 0 || std::fabs(y[size_t(i)] - screenY) < std::fabs(y[size_t(best)] - screenY))
                    best = i;
        return best >= 0 && std::fabs(y[size_t(best)] - screenY) < 20 ? best : -1;
    }

    void rebuildTimeline(double now)
    {
        m_timelineDirty = false;
        const int H = m_view.height() - 50;
        QImage img(400, H, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const double h = m_hover.value(now);
        const double mouseY = m_mouse.y();
        const std::vector<double> ys = timelineLayout(h, mouseY);
        static const Spline lengthCurve{{0, 1}, {0.19, 0.77}, {0.295, 0.53}, {1, 0}}, thicknessCurve{{0, 1}, {0.673, 0.13}, {1, 0}};
        const int hovered = h > 0 ? timelineItemAt(mouseY) : -1;
        const QLocale en(QLocale::English, QLocale::UnitedStates);
        for (int i = 0; i < int(m_items.size()); ++i) {
            const TimelineItem &it = m_items[i];
            const double y = ys[size_t(i)];
            if (y < -20 || y > H + 20)
                continue;
            const double d = std::min(std::fabs(y - mouseY) / 110, 1.0);
            const double mL = std::clamp(h * lengthCurve(d), 0.0, 1.0), mT = std::clamp(h * thicknessCurve(d), 0.0, 1.0);
            const bool big = it.kind == TimelineItem::Month || it.kind == TimelineItem::Label;
            const double L0 = big ? 36 : 16, T0 = big ? 3 : 2;
            const double length = L0 * (1 - mL) + L0 * 4.34 * mL;
            double thick = T0 * (1 - mT) + T0 * 9.789 * mT;
            if (it.members > 1)   // FUN_00044cba: the member's share of the cluster's tick at rest
                thick = (1 - h) * (2.0 / it.members * (1 - h) + 2 * h) + h * thick;
            double alpha = 0.6;
            if (it.kind == TimelineItem::Snapshot || it.kind == TimelineItem::Current) {
                const bool valid = m_layers[it.layer].valid;
                alpha = it.layer == m_current ? 1.0 : i == hovered ? 200 / 255.0 : !valid ? 50 / 255.0 : 153 / 255.0;
            } else if (it.kind == TimelineItem::Placeholder) {
                alpha = 50 / 255.0;
            }
            alpha *= std::clamp(std::min(y, H - y) / 30, 0.0, 1.0);   // the fade at either end
            if (it.members > 1 && it.member == 0 && h < 1) {
                // FUN_00048be6: the cluster's own tick, 16 long over its members' ticks, its alpha the
                // highest of theirs (FUN_00048726), fading as the magnification grows
                double top = y, bottom = y, a = 0;
                for (int m = 0; m < it.members; ++m) {
                    const TimelineItem &member = m_items[i + m];
                    top = std::min(top, ys[size_t(i + m)]);
                    bottom = std::max(bottom, ys[size_t(i + m)]);
                    const double ma = member.layer == m_current ? 1.0 : i + m == hovered ? 200 / 255.0
                                    : !m_layers[member.layer].valid ? 50 / 255.0 : 153 / 255.0;
                    a = std::max(a, ma);
                }
                const double t = 2.0 / it.members;
                p.setOpacity((1 - h) * a * std::clamp(std::min(y, H - y) / 30, 0.0, 1.0));
                p.fillRect(QRectF(400 - 16, top - t / 2, 16, bottom - top + t), Qt::white);
            }
            const QRectF tick(400 - length, y - thick / 2, length, thick);
            p.setOpacity(alpha);
            if (thick > 3.5) {   // magnified: TickCap.png and TickFill.png
                const double capW = std::min(8.0, 8 * thick / 25);
                p.drawImage(QRectF(tick.x(), tick.y(), capW, thick), m_tick[0]);
                p.drawImage(QRectF(tick.x() + capW, tick.y(), tick.width() - capW, thick), m_tick[1]);
            } else {
                p.fillRect(tick, Qt::white);
            }
            // the labels: LucidaGrande-Bold 12, right-aligned 8 points left of the tick, with a shadow
            QString text = it.text;
            QColor colour(153, 153, 153);
            if (it.kind == TimelineItem::Snapshot) {
                if (i != hovered)
                    continue;
                const QDateTime dt = m_layers[it.layer].date;
                const QString time = en.toString(dt.time(), "h:mm AP");
                const QDate today = QDate::currentDate();
                text = dt.date() == today ? bu("U18", "Today at ^0").replace("^0", time)
                       : dt.date() == today.addDays(-1) ? bu("U19", "Yesterday at ^0").replace("^0", time)
                                                        : en.toString(dt.date(), "dddd, MMMM d, yyyy") + " " + time;
                colour = Qt::white;
            } else if (it.kind == TimelineItem::Current) {
                if (i == hovered)
                    text = bu("BU20", "Today (Now)");
                if (m_current == 0 || i == hovered)
                    colour = Qt::white;
            } else if (it.kind == TimelineItem::Placeholder) {
                continue;
            }
            p.setOpacity(std::clamp(std::min(y, H - y) / 30, 0.0, 1.0));
            p.setFont(Assets::uiFont(12, true));
            const QRectF r(0, y - 8 + 2 - 2, tick.x() - 8, 16);
            p.setPen(QColor(0, 0, 0, 160));
            Aqua::drawText(p, r.translated(0, 2), Qt::AlignRight | Qt::AlignVCenter, text);
            p.setPen(colour);
            Aqua::drawText(p, r, Qt::AlignRight | Qt::AlignVCenter, text);
        }
        p.end();
        if (m_timelineTex)
            m_f->glDeleteTextures(1, &m_timelineTex);
        m_timelineTex = uploadTexture(m_f, img);
    }

    // ---- restore

    void restore(bool all)
    {
        const Layer &l = m_layers[m_current];
        QStringList names;
        for (const Entry &e : l.entries)
            if (all || m_selection.contains(e.name))
                names << e.name;
        if (names.isEmpty())
            return;
        m_pendingFrom = l.folder;
        m_pendingNames = names;
        m_pendingTo = m_target;
        // restoreImages: every visible selected item's cell (icon and name) and its frame in the window
        const QRect visible(0, 0, m_B.width(), m_B.height() - 23);   // above the status bar
        for (int i = 0; i < l.entries.size(); ++i) {
            if (!m_selection.contains(l.entries[i].name))
                continue;
            QRect cell;
            const QImage img = restoreImage(l.entries[i], m_B, i, iconsView(), &cell);
            if (!visible.contains(cell))
                continue;
            RestoreLayer r;
            r.texture = uploadTexture(m_f, img);
            // y up: the window's bottom-left corner is m_front's
            r.frame = QRectF(m_front.x() + cell.x(), m_front.y() + m_B.height() - cell.bottom() - 1, cell.width(), cell.height());
            m_restoreLayers.push_back(r);
        }
        // the flight back to Now, then the exit; the copy after the dismissal
        {
            const int n = std::min(m_current, 10);
            const double d = n / (std::log2(double(std::max(n, 1))) + 1);
            m_restoreStart = time();
            m_restoreDuration = n * (0.5 / d) * slow() + 2;
        }
        gotoSnapshot(0, {});
        m_afterFlight = [this] { startExit(); };
    }

    struct RestoreLayer {
        GLuint texture = 0;
        QRectF frame;
    };
    QVector<RestoreLayer> m_restoreLayers;
    double m_restoreStart = 0, m_restoreDuration = 1;
    QString m_target;
    QString m_volumeHint;                     // the volume folder entered from the Computer
    QHash<QString, QString> m_computerPaths;  // the Computer's volumes: name -> mount point
    QImage m_desktop;
    bool m_icons = false;
    bool iconsView() const { return m_icons && m_search.isEmpty(); }   // a search's results in list view
    QSet<QString> m_selection;
    // the search field (host 10): its text, the folder searched, typing in it, the search after a pause
    QString m_search, m_searchScope;
    bool m_searchFocus = false;
    QTimer m_searchTimer;
    bool m_quickLook = false, m_finderUsed = false;   // the Finder's Quick Look panel opened from here, a Finder command sent
    QStringList m_quickLookPaths;
    QSize m_view, m_B;
    QRectF m_front, m_live;
    QString m_machine;
    QVector<Snapshot> m_snapshots;
    QVector<Layer> m_layers;
    int m_current = 0, m_requested = -1;
    Phase m_phase = Entering;
    double m_revealStart = 0, m_revealDuration = 1.5;
    Anim m_top, m_galaxy, m_zOffset, m_hover;
    double m_tlScroll = 0, m_tlScrollTime = 0;   // the magnified timeline's auto-scroll
    int m_tlAnchor = -1;                         // the item kept in place while magnified
    std::function<void()> m_afterFlight;
    QString m_pendingFrom, m_pendingTo;
    QStringList m_pendingNames;

    QOpenGLFunctions_2_1 *m_f = nullptr;
    std::unique_ptr<StarField> m_stars;
    QElapsedTimer m_clock;
    QTimer m_timer;
    GLuint m_desktopTex = 0, m_toolbarTex = 0, m_timelineTex = 0;
    std::map<QString, QImage> m_art;
    std::map<QString, GLuint> m_arrowTex;
    std::map<QString, QSize> m_arrowSize;
    std::map<QString, Button> m_arrows;
    QImage m_tick[2];
    Button m_cancel, m_restore, m_restoreAll, m_date;
    QString m_pressed;
    bool m_toolbarDirty = true, m_timelineDirty = true;
    QPointF m_mouse{-1000, -1000};
    std::vector<TimelineItem> m_items;
};

qint64 TimeMachine::QStorageInfoAvailable(const QString &path)
{
    struct statvfs s;
    return statvfs(QFile::encodeName(path).constData(), &s) == 0 ? qint64(s.f_bavail) * qint64(s.f_frsize) : -1;
}

// "name (original).ext" (TUniqueNamer with CP6), unique
QString originalName(const QString &folder, const QString &name)
{
    const QString suffix = finder("CP6", " (original)");
    const int dot = name.lastIndexOf('.');
    const QString base = dot > 0 ? name.left(dot) : name, ext = dot > 0 ? name.mid(dot) : QString();
    QString n = base + suffix + ext;
    for (int i = 2; QFileInfo::exists(folder + "/" + n); ++i)
        n = base + suffix + " " + QString::number(i) + ext;
    return n;
}

// FUN_1001ddace: the pending restore after the dismissal: the enclosing folder (recreated), then a
// Finder copy with the restore flag: a conflict is ProgressRenameConflictView's Keep Original (skip),
// Keep Both (the one in place renamed " (original)"), Replace
void runRestore(const QString &from, const QStringList &names, const QString &to)
{
    const QIcon icon(QPixmap::fromImage(Assets::image("/Applications/Time Machine.app/Contents/Resources/backup.icns", QSize(64, 64))));
    if (!QFileInfo(to).isDir()) {
        QMessageBox box;
        box.setIconPixmap(icon.pixmap(64, 64));
        box.setText(names.size() == 1 ? finder("BU37", "The enclosing folder for “^0” no longer exists at its original location.").replace("^0", names.first())
                                      : finder("BU38", "The enclosing folders for some items no longer exist in their original locations."));
        QPushButton *recreate = box.addButton(finder("BU40", "Recreate Enclosing Folders"), QMessageBox::AcceptRole);
        box.addButton(finder("BU9", "Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != recreate || !QDir().mkpath(to))
            return;
    }
    enum Choice { Ask, KeepOriginal, KeepBoth, Replace } all = Ask;
    for (const QString &relative : names) {
        // a search's result: its path under the scope, its enclosing folders recreated
        const QString src = from + "/" + relative, dst = to + "/" + relative;
        const QString name = relative.section('/', -1), dir = QFileInfo(dst).path();
        QDir().mkpath(dir);
        Choice c = KeepBoth;
        if (QFileInfo::exists(dst) || QFileInfo(dst).isSymLink()) {
            c = all;
            if (c == Ask) {
                // ProgressRenameConflictView (399 x 100): Keep Original, Keep Both, Replace; Apply to All
                const int dot = name.lastIndexOf('.');
                const QString text =
                    dot > 0 ? finder("PE32.1", "An item named “^0” with extension “^1” already exists in this location. Do you want to replace it "
                                               "with the one you’re restoring?")
                                  .replace("^0", name.left(dot))
                                  .replace("^1", name.mid(dot + 1))
                            : finder("PE1.1", "The location to which you’re restoring “^0” already contains an item with the same name. Do you "
                                              "want to replace it with the one you’re restoring?")
                                  .replace("^0", name);
                QDialog box;
                box.setWindowTitle("Copy");
                auto *grid = new QGridLayout(&box);
                auto *pic = new QLabel;
                pic->setPixmap(icon.pixmap(64, 64));
                grid->addWidget(pic, 0, 0, 2, 1, Qt::AlignTop);
                auto *label = new QLabel(text);
                label->setWordWrap(true);
                label->setMinimumWidth(330);
                grid->addWidget(label, 0, 1, 1, 4);
                QCheckBox apply("Apply to All");
                if (names.size() > 1)
                    grid->addWidget(&apply, 1, 1);
                Choice picked = KeepOriginal;
                int col = 2;
                for (auto [title, choice] : {std::pair<const char *, Choice>{"Keep Original", KeepOriginal}, {"Keep Both", KeepBoth}, {"Replace", Replace}}) {
                    auto *b = new QPushButton(title);
                    QObject::connect(b, &QPushButton::clicked, &box, [&, choice] {
                        picked = choice;
                        box.accept();
                    });
                    if (choice == Replace)
                        b->setDefault(true);
                    grid->addWidget(b, 1, col++);
                }
                box.exec();
                c = picked;
                if (apply.isChecked())
                    all = c;
            }
            if (c == KeepOriginal)
                continue;
            if (c == KeepBoth)
                QFile::rename(dst, dir + "/" + originalName(dir, name));
            else
                QProcess::execute("rm", {"-rf", "--", dst});
        }
        QProcess::execute("cp", {"-a", "--", src, dst});
    }
}

} // namespace

// Browse Other Time Machine Disks (TBackup::ChooseAndEnterBackupStore, host 13): the Finder's
// BackupVolumeBrowserWindow.nib, "Choose Time Machine Disk to Browse", one row per computer backup
// folder on the mounted disks, "“^0” on “^1”" (BU41) with the volume's icon at 32 × 32; Use Selected
// Disk (enabled with a selection, Return, a double click) or Cancel (Escape). The chosen computer
// folder, or nothing.
QString chooseBackupStore()
{
    QDialog dialog;
    dialog.setWindowTitle("Choose Time Machine Disk to Browse");
    dialog.setMinimumSize(336, 352);
    dialog.resize(372, 374);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 12);
    auto *text = new QLabel("These disks contain Time Machine backups. Select the disk you want to use with Time Machine:");
    text->setWordWrap(true);
    QFont bold = text->font();
    bold.setBold(true);
    text->setFont(bold);
    layout->addWidget(text);
    auto *list = new QListWidget;
    list->setIconSize(QSize(32, 32));
    list->setSpacing(1);
    const QVector<BackupStore> stores = backupStores();
    for (const BackupStore &b : stores) {
        const QString name = finder("BU41", "“^0” on “^1”").replace("^0", b.computerName).replace("^1", b.volume.name);
        auto *item = new QListWidgetItem(QIcon(QPixmap::fromImage(volumeIcon(b.volume, 64))), name);
        item->setData(Qt::UserRole, b.computer);
        list->addItem(item);
    }
    layout->addWidget(list, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancel = new QPushButton("Cancel");
    auto *ok = new QPushButton("Use Selected Disk");
    ok->setDefault(true);
    ok->setEnabled(false);
    buttons->addWidget(cancel);
    buttons->addWidget(ok);
    layout->addLayout(buttons);
    QObject::connect(list, &QListWidget::itemSelectionChanged, ok, [&] { ok->setEnabled(!list->selectedItems().isEmpty()); });
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || list->selectedItems().isEmpty())
        return {};
    return list->selectedItems().first()->data(Qt::UserRole).toString();
}

// The Finder's alert with the Time Machine icon ('mill'/'peab', FUN_1000d8e4c): the message, the
// informative text, button 1 the default (Return), 2 Cancel (Escape), 3 the other one, on the left;
// the number of the button clicked
int timeMachineAlert(const QString &message, const QString &informative, const QString &b1, const QString &b2, const QString &b3 = {})
{
    // NSAlert's layout: the 64-point icon at 24, 15; the message in the bold system font, the
    // informative text in the small one; the buttons on the right, the third one on the left
    QDialog box;
    box.setWindowTitle(QApplication::applicationName());
    auto *grid = new QGridLayout(&box);
    grid->setContentsMargins(24, 15, 20, 20);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    auto *icon = new QLabel;
    icon->setPixmap(QPixmap::fromImage(Assets::image("/Applications/Time Machine.app/Contents/Resources/backup.icns", QSize(64, 64))));
    icon->setAlignment(Qt::AlignTop);
    grid->addWidget(icon, 0, 0, 2, 1);
    auto *text = new QLabel(message);
    QFont bold = text->font();
    bold.setBold(true);
    text->setFont(bold);
    text->setWordWrap(true);
    text->setMinimumWidth(360);
    grid->addWidget(text, 0, 1);
    if (!informative.isEmpty()) {
        auto *info = new QLabel(informative);
        QFont small = info->font();
        small.setPointSizeF(small.pointSizeF() * 11 / 13);
        info->setFont(small);
        info->setWordWrap(true);
        grid->addWidget(info, 1, 1);
    }
    auto *row = new QHBoxLayout;
    row->setSpacing(12);
    int clicked = 2;
    auto button = [&](const QString &title, int n) {
        auto *b = new QPushButton(title);
        b->setMinimumWidth(82);
        QObject::connect(b, &QPushButton::clicked, &box, [&, n] { clicked = n; box.accept(); });
        return b;
    };
    if (!b3.isEmpty())
        row->addWidget(button(b3, 3));
    row->addStretch();
    row->addWidget(button(b2, 2));
    QPushButton *def = button(b1, 1);
    def->setDefault(true);
    row->addWidget(def);
    grid->addLayout(row, 2, 0, 1, 2);
    grid->setRowMinimumHeight(1, 0);
    box.exec();     // Escape rejects: Cancel (2)
    return clicked;
}

bool timemachineAlertSetUp(const QString &message, const QString &informative)
{
    return timeMachineAlert(message, informative, finder("BU3", "Set Up Time Machine"), finder("AL1", "Cancel")) == 1;
}

// TBackup::PreflightShowBackup (0x1001db08c) when the session isn't configured, and
// TBackup::BackupStoreNotAvailable (0x1001de02a) when the mounting task finds no store: which alert by
// whether a backup disk is set (timemachine.conf's BackupPath, the session's) and whether any mounted
// disk holds backups (FUN_10012bf0e, the chooser's rows). True: Use Previous Disk… (the chooser).
bool preflightAlert()
{
    QSettings st(configDir() + "/timemachine.conf", QSettings::IniFormat);
    const bool configured = !st.value("BackupPath").toString().isEmpty() || !st.value("BackupAlias").toString().isEmpty();
    const bool others = !backupStores().isEmpty();
    if (!configured) {
        if (!others) {
            // rBackupNotConfiguredIndex, configure: BU1 / BU2, Set Up Time Machine, Cancel
            if (timemachineAlertSetUp(finder("BU1", "You haven’t selected a location for Time Machine backups."),
                                      finder("BU2", "To select a location for backups, set up Time Machine.")))
                openPreferences();
            return false;
        }
        // rBackupNotConfiguredIndex, disk not found: Use Previous Disk… (1), Cancel (2), Time Machine Preferences… (3)
        const int b = timeMachineAlert(finder("BU1", "You haven’t selected a location for Time Machine backups."),
                                       finder("BU32", "You can set up a new backup disk in Time Machine preferences, or use a previous backup disk."),
                                       finder("BU33", "Use Previous Disk…"), finder("AL1", "Cancel"), finder("BU34", "Time Machine Preferences…"));
        if (b == 3)
            openPreferences();
        return b == 1;
    }
    if (!others) {
        // BU2.1 / BU2.2 (empty), Set Up Time Machine, Cancel
        if (timemachineAlertSetUp(finder("BU2.1", "Your Time Machine backup disk can’t be found."), finder("BU2.2", "")))
            openPreferences();
        return false;
    }
    // BU31 / BU32: Time Machine Preferences… (1), Cancel (2), Use Previous Disk… (3)
    const int b = timeMachineAlert(finder("BU31", "The current Time Machine backup disk can‘t be found."),
                                   finder("BU32", "You can set up a new backup disk in Time Machine preferences, or use a previous backup disk."),
                                   finder("BU34", "Time Machine Preferences…"), finder("AL1", "Cancel"), finder("BU33", "Use Previous Disk…"));
    if (b == 1)
        openPreferences();
    return b == 3;
}

// UAlerts::FrontWindowIsModal (FUN_10002d817): the active window a modal one (_NET_WM_STATE_MODAL);
// PreflightShowBackup then brings it forward (FUN_10009bdba) and beeps (FUN_10012a228)
bool frontWindowIsModal()
{
    if (X11::wayland() || !X11::conn())
        return false;
    const xcb_window_t w = X11::activeWindow();
    if (!w)
        return false;
    for (uint32_t state : X11::cardinals(w, X11::atom("_NET_WM_STATE"), XCB_ATOM_ATOM))
        if (state == X11::atom("_NET_WM_STATE_MODAL")) {
            X11::activate(w);
            QApplication::beep();
            return true;
        }
    return false;
}

int main(int argc, char **argv)
{
    Aqua::preinit();
    QApplication app(argc, argv);
    app.setApplicationName("Time Machine");
    Aqua::init(app);
    {
        // the panels' light grey whatever the desktop's colour scheme
        QPalette pal;
        pal.setColor(QPalette::Window, QColor(232, 232, 232));
        pal.setColor(QPalette::WindowText, Qt::black);
        pal.setColor(QPalette::Base, Qt::white);
        pal.setColor(QPalette::Text, Qt::black);
        pal.setColor(QPalette::Button, QColor(232, 232, 232));
        pal.setColor(QPalette::ButtonText, Qt::black);
        pal.setColor(QPalette::Highlight, QColor(56, 117, 215));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        app.setPalette(pal);
    }

    QStringList args = app.arguments().mid(1);
    QString folder;
    int command = 0;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--folder" && i + 1 < args.size())
            folder = args[++i];
        else if (args[i] == "--browse-other-disks")
            command = 4;
        else
            command = args[i].toInt();
    }
    // the launcher's commands (Time Machine.app's main: the Dock's menu passes the number)
    if (command == 1) {
        openPreferences();
        return 0;
    }
    if (command == 2) {
        QProcess::startDetached(appsDir() + "/backupd.bundle/Contents/Resources/backupd", {"--now"});
        return 0;
    }
    if (command == 3) {
        QSettings st(configDir() + "/backupd.status.conf", QSettings::IniFormat);
        const int pid = st.value("pid").toInt();
        if (pid > 0)
            kill(pid, SIGTERM);
        return 0;
    }
    // UAlerts::FrontWindowIsModal: a modal window in front is brought forward with a beep
    if (frontWindowIsModal())
        return 0;
    // Browse Other Time Machine Disks: the chooser, then EnterBackupWithStore with the Computer as target
    QString machine;
    if (command == 4) {
        machine = chooseBackupStore();
        if (machine.isEmpty())
            return 0;
        folder = ComputerTarget;
    } else {
        machine = machineFolder();
        // PreflightShowBackup's and BackupStoreNotAvailable's alerts; Use Previous Disk… shows the chooser
        if (machine.isEmpty()) {
            if (!preflightAlert())
                return 0;
            machine = chooseBackupStore();
            if (machine.isEmpty())
                return 0;
            folder = ComputerTarget;
        }
    }

    // windowForTimeMachineWithDefaultTarget: the Finder's front window, else the Desktop
    const FinderFront front = finderFront();
    QSize windowSize = front.size;
    QStringList selection = front.selection;
    if (folder.isEmpty())
        folder = !front.folder.isEmpty() && QFileInfo(front.folder).isDir() ? front.folder : desktopDir();
    if (folder != front.folder)
        selection.clear();
    if (folder != ComputerTarget)
        folder = QDir(folder).absolutePath();
    const QRect live = folder == front.folder ? finderWindowRect(front.pid, displayName(folder)) : QRect();
    if (!live.isNull() && !windowSize.isValid())
        windowSize = live.size();

    // the desktop's picture before the window covers it (setRevealImages:'s reveal layer)
    QScreen *screen = QGuiApplication::primaryScreen();
    QImage desktop;
#ifdef SNOWLEOPARD_WAYLAND
    if (X11::wayland()) {
        // the window server's capture (CGSHWCaptureWindowList's counterpart): Qt can't grab on Wayland
        const QRect g = screen->geometry();
        const std::string png = WindowServerClient::captureScreen(g.x(), g.y(), g.width(), g.height());
        desktop = QImage::fromData(reinterpret_cast<const uchar *>(png.data()), int(png.size()), "PNG")
                      .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
#endif
    if (desktop.isNull())
        desktop = screen->grabWindow(0).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (desktop.isNull() || desktop.size() != screen->geometry().size()) {
        desktop = Assets::wallpaperFor(screen->geometry().size()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    TimeMachine tm(machine, folder, windowSize, live, desktop, selection, folder == front.folder && front.icons);
    tm.onRestore = runRestore;
    tm.showFullScreen();
    tm.requestActivate();
    return app.exec();
}
