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
// (the chooser isn't ported: the default disk). --folder <path> chooses the folder; otherwise the
// Finder's front window's (its state file), else the Desktop.
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
#include <QTimer>

#include <xcb/xcb.h>

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

// the Finder window's frame on the screen (y down), through X11
QRect finderWindowRect(uint pid, const QString &title)
{
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
};

struct Button {
    QRectF frame;              // on the screen, y up
    QString title;
    bool enabled = true;
};

class TimeMachine : public QOpenGLWindow {
public:
    TimeMachine(const QString &target, QSize windowSize, QRect liveRect, const QImage &desktop, const QStringList &selection, bool icons)
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
        m_machine = machineFolder();
        m_snapshots = snapshots(m_machine);
        retarget(m_target);
        m_clock.start();
        connect(&m_timer, &QTimer::timeout, this, [this] { update(); });
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
            const int row = itemAt(m_B, local, int(l.entries.size()), m_icons);
            if (row < 0) {
                m_selection.clear();
            } else {
                const QString name = l.entries[row].name;
                if (ev->type() == QEvent::MouseButtonDblClick && l.entries[row].dir) {
                    retarget(m_target + "/" + name);   // BUChangeTimeMachineTarget
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
        switch (ev->key()) {
        case Qt::Key_Escape:   // the browser's cancel: → BUTimeMachineAction(1)
            startExit();
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
        now.valid = QFileInfo(target).isDir();
        m_layers.push_back(now);
        for (const Snapshot &s : m_snapshots) {
            Layer l;
            l.folder = folderInSnapshot(s.path, target);
            l.valid = !l.folder.isEmpty();
            l.date = s.date;
            m_layers.push_back(l);
        }
        for (Layer &l : m_layers) {
            if (l.valid)
                l.entries = listFolder(l.folder);
            l.signature = signature(l.entries);
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
    }

    GLuint layerTexture(int k)
    {
        Layer &l = m_layers[k];
        if (l.picture.isNull()) {
            if (l.texture)
                m_f->glDeleteTextures(1, &l.texture);
            if (l.valid) {
                WindowPicture w;
                w.title = displayName(m_target);
                w.entries = l.entries;
                w.selected = m_selection;
                w.home = QDir::homePath();
                w.available = QStorageInfoAvailable(l.folder);
                w.icons = m_icons;
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
        const bool snapshot = m_current > 0 && m_layers[m_current].valid;
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
        m_timelineDirty = true;
    }

    // the items' centres in the timeline's picture (flipped: item 0 at the top), with the hover's spacing
    std::vector<double> timelineLayout(double h, double mouseY) const
    {
        const double H = m_view.height() - 50, colTop = 30, colBottom = H - 30;
        const int n = int(m_items.size());
        double colH = colBottom - colTop;
        const double slot = std::min(colH / n, 16.0);
        colH = slot * n;
        std::vector<double> y(static_cast<size_t>(n), 0.0);
        for (int i = 0; i < n; ++i)
            y[size_t(i)] = colBottom - colH + (i + 0.5) * slot;
        if (h > 0) {
            // the spacing grows to 16; the item under the mouse stays where it is
            int anchor = 0;
            for (int i = 0; i < n; ++i)
                if (std::fabs(y[size_t(i)] - mouseY) < std::fabs(y[size_t(anchor)] - mouseY))
                    anchor = i;
            const double sp = slot + (16 - slot) * h, ay = y[size_t(anchor)];
            for (int i = 0; i < n; ++i)
                y[size_t(i)] = ay + (i - anchor) * sp;
        }
        return y;
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
            const double length = L0 * (1 - mL) + L0 * 4.34 * mL, thick = T0 * (1 - mT) + T0 * 9.789 * mT;
            double alpha = 0.6;
            if (it.kind == TimelineItem::Snapshot || it.kind == TimelineItem::Current) {
                const bool valid = m_layers[it.layer].valid;
                alpha = it.layer == m_current ? 1.0 : i == hovered ? 200 / 255.0 : !valid ? 50 / 255.0 : 153 / 255.0;
            } else if (it.kind == TimelineItem::Placeholder) {
                alpha = 50 / 255.0;
            }
            alpha *= std::clamp(std::min(y, H - y) / 30, 0.0, 1.0);   // the fade at either end
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
        // the flight back to Now, then the exit; the copy after the dismissal
        gotoSnapshot(0, {});
        m_afterFlight = [this] { startExit(); };
    }

    QString m_target;
    QImage m_desktop;
    bool m_icons = false;
    QSet<QString> m_selection;
    QSize m_view, m_B;
    QRectF m_front, m_live;
    QString m_machine;
    QVector<Snapshot> m_snapshots;
    QVector<Layer> m_layers;
    int m_current = 0, m_requested = -1;
    Phase m_phase = Entering;
    double m_revealStart = 0, m_revealDuration = 1.5;
    Anim m_top, m_galaxy, m_zOffset, m_hover;
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
    for (const QString &name : names) {
        const QString src = from + "/" + name, dst = to + "/" + name;
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
                QFile::rename(dst, to + "/" + originalName(to, name));
            else
                QProcess::execute("rm", {"-rf", "--", dst});
        }
        QProcess::execute("cp", {"-a", "--", src, dst});
    }
}

} // namespace

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
    if (command == 4)
        fprintf(stderr, "Time Machine: Browse Other Time Machine Disks isn't ported; the default disk\n");

    // PreflightShowBackup: no backup disk
    if (machineFolder().isEmpty()) {
        QMessageBox box;
        box.setIconPixmap(QPixmap::fromImage(Assets::image("/Applications/Time Machine.app/Contents/Resources/backup.icns", QSize(64, 64))));
        box.setText(finder("BU1", "You haven’t selected a location for Time Machine backups."));
        box.setInformativeText(finder("BU2", "To select a location for backups, set up Time Machine."));
        QPushButton *setUp = box.addButton(finder("BU3", "Set Up Time Machine"), QMessageBox::AcceptRole);
        box.addButton(finder("AL1", "Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == setUp)
            openPreferences();
        return 0;
    }

    // windowForTimeMachineWithDefaultTarget: the Finder's front window, else the Desktop
    const FinderFront front = finderFront();
    QSize windowSize = front.size;
    QStringList selection = front.selection;
    if (folder.isEmpty())
        folder = !front.folder.isEmpty() && QFileInfo(front.folder).isDir() ? front.folder : desktopDir();
    if (folder != front.folder)
        selection.clear();
    folder = QDir(folder).absolutePath();
    const QRect live = folder == front.folder ? finderWindowRect(front.pid, displayName(folder)) : QRect();
    if (!live.isNull() && !windowSize.isValid())
        windowSize = live.size();

    // the desktop's picture before the window covers it (setRevealImages:'s reveal layer)
    QScreen *screen = QGuiApplication::primaryScreen();
    QImage desktop = screen->grabWindow(0).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (desktop.isNull() || desktop.size() != screen->geometry().size()) {
        desktop = Assets::wallpaperFor(screen->geometry().size()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    TimeMachine tm(folder, windowSize, live, desktop, selection, folder == front.folder && front.icons);
    tm.onRestore = runRestore;
    tm.showFullScreen();
    tm.requestActivate();
    return app.exec();
}
