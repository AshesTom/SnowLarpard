// SPDX-License-Identifier: LGPL-2.1-or-later
// Spaces in WindowServer. On 10.6 the window server keeps the workspaces (CGSSetWorkspace, each
// window's CGSGetWindowWorkspace) and the Dock drives them; both are here, reversed from the Dock's
// x86_64 slice, decrypted (tools/deprotect.py), with REA (docs/reverse-status.md, section 30):
//
//   - the preferences (com.apple.dock, read again when they change, FUN_100048505): workspaces (off
//     by default), workspaces-rows and workspaces-cols (2 and 2; fewer than one, or one by one, is 2
//     by 2, FUN_10004d5cc), workspaces-switch-modifier-binding and
//     workspaces-switchDirectly-modifier-binding (0x40000, Control: the arrows and the numbers),
//     workspaces-wrap-arrows (on), workspaces-swoosh-animation-off (off); AppleSpacesSwitchOnActivate
//     (any application's, on) (FUN_10007822b); Spaces turned off, every window back on space 1
//     (FUN_1000784e9);
//   - the switch (FUN_10007a320): left and right go round the grid's rows when wrapping (past the
//     first column the previous row's last, past the last space the first), up and down stop at the
//     edges; the same space, nothing; the windows of the two spaces slide together, the old ones by
//     (old column - new column) × the screens' width and (old row - new row) × their height, the new
//     ones from minus that (FUN_100079079), eased 0.5 - 0.5 cos(π t / 250 ms) (DAT_10019e9d0, 2.5 s
//     with Shift), the desktop staying; a window being dragged comes along;
//   - the bezel (WVBezelLayer, dispatched with the switch, FUN_10007780d): the grid of spaces in
//     squares of the screen's shape at most 100 points (45 at least), 11 % of a square apart, a dark
//     rounded panel, the cells, the current one white; at (screen height - 100) / 5 over the screen's
//     bottom, centred; shown at once, its indicator moving to the new cell, faded 0.25 s after.
//
// EWMH for the others: _NET_NUMBER_OF_DESKTOPS, _NET_DESKTOP_LAYOUT, _NET_CURRENT_DESKTOP, each
// window's _NET_WM_DESKTOP; _NET_CURRENT_DESKTOP and _NET_WM_DESKTOP messages switch and move (the
// Spaces menu extra's com.apple.switchSpaces).
#include "windowserver.h"

#include "x11.h"

#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStandardPaths>
#include <cmath>

using X11::atom;

namespace {

constexpr double SlideSeconds = 0.25;            // DAT_10019e9d0: 250 (ms)
constexpr double BezelHideDelay = 0.25, BezelFade = 0.25;   // -scheduleHide: beginTime + 0.25, CATransaction's 0.25 s

QString configFile(const char *name)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/snowleopard/" + name + ".conf";
}

// the Dock's modifier bindings (NSEvent's flags) as X's
uint16_t xModifier(int flags)
{
    uint16_t m = 0;
    if (flags & 0x20000)
        m |= XCB_MOD_MASK_SHIFT;
    if (flags & 0x40000)
        m |= XCB_MOD_MASK_CONTROL;
    if (flags & 0x80000)
        m |= XCB_MOD_MASK_1;
    if (flags & 0x100000)
        m |= XCB_MOD_MASK_4;
    return m;
}

constexpr xcb_keysym_t keyLeft = 0xff51, keyUp = 0xff52, keyRight = 0xff53, keyDown = 0xff54;

} // namespace

void WindowServer::readSpacesPrefs()
{
    QSettings dock(configFile("dock"), QSettings::IniFormat);
    QSettings global(configFile("appearance"), QSettings::IniFormat);
    const bool enabled = dock.value("workspaces", false).toBool();
    int rows = dock.value("workspaces-rows", 2).toInt(), cols = dock.value("workspaces-cols", 2).toInt();
    if (rows < 1 || cols < 1 || (rows == 1 && cols == 1))
        rows = cols = 2;
    if (!enabled)
        rows = cols = 1;
    m_wrapArrows = dock.value("workspaces-wrap-arrows", true).toBool();
    m_swooshOff = dock.value("workspaces-swoosh-animation-off", false).toBool();
    // a binding of 0 ("–" in Exposé & Spaces): no keys
    const int switchBinding = dock.value("workspaces-switch-modifier-binding", 0x40000).toInt();
    const int directBinding = dock.value("workspaces-switchDirectly-modifier-binding", 0x40000).toInt();
    const uint16_t switchModifier = switchBinding ? xModifier(switchBinding) : 0xFFFF;
    const uint16_t directModifier = directBinding ? xModifier(directBinding) : 0xFFFF;
    if (switchModifier != m_switchModifier || directModifier != m_directModifier) {
        grabSpacesKeys(false);   // with the old modifiers
        m_switchModifier = switchModifier;
        m_directModifier = directModifier;
        grabSpacesKeys(m_spacesEnabled);
    }
    m_switchOnActivate = global.value("AppleSpacesSwitchOnActivate", true).toBool();
    const bool changed = enabled != m_spacesEnabled || rows != m_rows || cols != m_cols;
    m_spacesEnabled = enabled;
    m_rows = rows;
    m_cols = cols;
    if (!changed)
        return;
    // fewer spaces: the windows of those gone on the last one (Spaces off: all on space 1)
    if (m_space >= spaceCount())
        switchSpace(spaceCount() - 1);
    for (Client *c : std::as_const(m_clients))
        if (c->space >= spaceCount())
            setClientSpace(c, spaceCount() - 1);
    m_bezelPanel = QImage();
    grabSpacesKeys(m_spacesEnabled);
    publishSpaces();
}

void WindowServer::publishSpaces()
{
    const uint32_t count = uint32_t(spaceCount()), current = uint32_t(m_space);
    // _NET_DESKTOP_LAYOUT: horizontal, columns, rows, from the top left
    const uint32_t layout[4] = {0, uint32_t(m_cols), uint32_t(m_rows), 0};
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_NUMBER_OF_DESKTOPS"), XCB_ATOM_CARDINAL, 32, 1, &count);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_DESKTOP_LAYOUT"), XCB_ATOM_CARDINAL, 32, 4, layout);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_root, atom("_NET_CURRENT_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &current);
    xcb_flush(m_conn);
}

// CGSMoveWorkspaceWindowList: the window to a space, shown or not
void WindowServer::setClientSpace(Client *c, int space)
{
    c->space = qBound(0, space, spaceCount() - 1);
    const uint32_t desktop = c->allSpaces ? 0xFFFFFFFF : uint32_t(c->space);
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, c->window, atom("_NET_WM_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &desktop);
    if (c->minimized || (m_slide.running && (m_slide.in.contains(c) || m_slide.out.contains(c) || m_slide.carried == c)))
        return;
    if (onCurrentSpace(c))
        xcb_map_window(m_conn, c->frame);
    else
        xcb_unmap_window(m_conn, c->frame);
    if (!onCurrentSpace(c) && m_active == c)
        focus(nullptr);
}

void WindowServer::switchSpaceBy(int direction)
{
    // FUN_10007a320's directions, on the grid's columns and rows
    const int col = m_space % m_cols, row = m_space / m_cols;
    int c = col, r = row;
    switch (direction) {
    case 1:   // left
        if (--c < 0) {
            if (!m_wrapArrows) {
                c = 0;
            } else {
                c = m_cols - 1;
                if (--r < 0)
                    r = m_rows - 1;
            }
        }
        break;
    case 2:   // right
        if (++c == m_cols) {
            if (!m_wrapArrows) {
                c = col;
            } else {
                c = 0;
                if (++r == m_rows)
                    r = 0;
            }
        }
        break;
    case 3:   // up
        r = qMax(0, r - 1);
        break;
    case 4:   // down
        r = qMin(m_rows - 1, r + 1);
        break;
    }
    // a window being dragged goes with it (WVSpaces -gatherApplicationWindowsForWindow:...)
    switchSpace(r * m_cols + c, m_drag == Moving ? m_dragClient : nullptr);
}

void WindowServer::switchSpace(int target, Client *carried)
{
    target = qBound(0, target, spaceCount() - 1);
    if (m_slide.running)
        finishSlide();
    if (target == m_space)
        return;   // nothing, not even the bezel
    const int from = m_space;
    if (carried) {
        carried->space = target;
        const uint32_t desktop = uint32_t(target);
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, carried->window, atom("_NET_WM_DESKTOP"), XCB_ATOM_CARDINAL, 32, 1, &desktop);
    }
    m_slide = Slide();
    m_slide.from = from;
    m_slide.to = target;
    m_slide.carried = carried;
    m_slide.delta = QPoint((from % m_cols - target % m_cols) * m_screen.width(), (from / m_cols - target / m_cols) * m_screen.height());
    for (Client *c : std::as_const(m_clients)) {
        if (c->allSpaces || c->minimized || c == carried)
            continue;
        if (c->space == from)
            m_slide.out << c;
        else if (c->space == target)
            m_slide.in << c;
    }
    m_space = target;
    publishSpaces();
    showBezel(from, target);
    if (!m_compositor.running() || m_swooshOff) {
        for (Client *c : std::as_const(m_slide.out))
            xcb_unmap_window(m_conn, c->frame);
        for (Client *c : std::as_const(m_slide.in))
            xcb_map_window(m_conn, c->frame);
        m_slide.running = true;
        finishSlide();
        return;
    }
    // the new space's windows from beyond the screen, then all of them sliding
    for (Client *c : std::as_const(m_slide.in)) {
        m_compositor.setOffset(c->frame, -m_slide.delta);
        xcb_map_window(m_conn, c->frame);
    }
    xcb_flush(m_conn);
    m_slide.running = true;
    m_slide.clock.start();
    m_slideTimer.start();
}

void WindowServer::stepSlide()
{
    if (!m_slide.running) {
        m_slideTimer.stop();
        return;
    }
    const double t = qMin(m_slide.clock.elapsed() / 1000.0, SlideSeconds);
    const double p = 0.5 - 0.5 * std::cos(M_PI * t / SlideSeconds);
    for (Client *c : std::as_const(m_slide.out))
        if (m_clients.contains(c))
            m_compositor.setOffset(c->frame, QPoint(int(std::lround(m_slide.delta.x() * p)), int(std::lround(m_slide.delta.y() * p))));
    for (Client *c : std::as_const(m_slide.in))
        if (m_clients.contains(c))
            m_compositor.setOffset(c->frame,
                                   QPoint(int(std::lround(-m_slide.delta.x() * (1 - p))), int(std::lround(-m_slide.delta.y() * (1 - p)))));
    stepBezel();
    if (t >= SlideSeconds)
        finishSlide();
}

void WindowServer::finishSlide()
{
    if (!m_slide.running)
        return;
    m_slide.running = false;
    m_slideTimer.stop();
    for (Client *c : std::as_const(m_slide.out))
        if (m_clients.contains(c) && !onCurrentSpace(c))
            xcb_unmap_window(m_conn, c->frame);
    m_compositor.clearOffsets();
    // the front application the new space's: its top window (CPSSetFrontProcessWithOptions)
    Client *front = nullptr;
    if (m_slide.carried && m_clients.contains(m_slide.carried))
        front = m_slide.carried;
    for (int i = m_clients.size() - 1; i >= 0 && !front; --i)
        if (!m_clients[i]->minimized && onCurrentSpace(m_clients[i]))
            front = m_clients[i];
    focus(front);
    m_slide.out.clear();
    m_slide.in.clear();
    m_slide.carried = nullptr;
    // the bezel stays, then fades (-scheduleHide)
    m_bezel.hideAt = m_bezel.clock.elapsed() / 1000.0 + BezelHideDelay;
    xcb_flush(m_conn);
}

bool WindowServer::activateSpaceFor(Client *c)
{
    if (!c || onCurrentSpace(c))
        return false;
    // AppleSpacesSwitchOnActivate (WorkspaceSwitchOnAppActivation): to the window's space; otherwise
    // the window comes to this one
    if (m_switchOnActivate) {
        switchSpace(c->space);
        return true;
    }
    setClientSpace(c, m_space);
    return false;
}

bool WindowServer::spacesKey(xcb_keysym_t sym, uint16_t state)
{
    if (!m_spacesEnabled)
        return false;
    const uint16_t mods = state & (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4);
    if (mods == m_switchModifier) {
        if (sym == keyLeft)
            return switchSpaceBy(1), true;
        if (sym == keyRight)
            return switchSpaceBy(2), true;
        if (sym == keyUp)
            return switchSpaceBy(3), true;
        if (sym == keyDown)
            return switchSpaceBy(4), true;
    }
    if (mods == m_directModifier && sym >= 0x31 && sym <= 0x39) {
        const int space = int(sym - 0x31);
        if (space < spaceCount())
            switchSpace(space, m_drag == Moving ? m_dragClient : nullptr);
        return true;
    }
    return false;
}

void WindowServer::grabSpacesKeys(bool grab)
{
    if (!m_keySymbols || grab == m_spacesKeysGrabbed)
        return;
    m_spacesKeysGrabbed = grab;
    QVector<xcb_keysym_t> syms = {keyLeft, keyRight, keyUp, keyDown};
    for (xcb_keysym_t d = 0x31; d <= 0x39; ++d)
        syms << d;
    for (xcb_keysym_t sym : syms) {
        xcb_keycode_t *codes = xcb_key_symbols_get_keycode(m_keySymbols, sym);
        if (!codes)
            continue;
        const uint16_t mod = sym >= 0x31 && sym <= 0x39 ? m_directModifier : m_switchModifier;
        if (mod == 0xFFFF) {   // none
            free(codes);
            continue;
        }
        for (xcb_keycode_t *k = codes; *k; ++k)
            for (uint16_t extra : {uint16_t(0), uint16_t(XCB_MOD_MASK_LOCK), uint16_t(XCB_MOD_MASK_2), uint16_t(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)}) {
                if (grab)
                    xcb_grab_key(m_conn, 1, m_root, uint16_t(mod | extra), *k, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
                else
                    xcb_ungrab_key(m_conn, *k, m_root, uint16_t(mod | extra));
            }
        free(codes);
    }
    xcb_flush(m_conn);
}

// ---- the bezel (WVBezelLayer)

namespace {

struct BezelMetrics {
    double squareW = 0, squareH = 0;
    int gap = 0, cellW = 0, cellH = 0, radius = 0;
    QSize size;
};

// -setRows:columns:displayFrameSize:initialSpace:, -_generateSpaceIndicator
BezelMetrics bezelMetrics(QSize screen, int rows, int cols)
{
    BezelMetrics m;
    const double scale = float(std::min(100.0 / screen.width(), 100.0 / screen.height()));
    m.squareW = std::max(45.0, scale * screen.width());
    m.squareH = std::max(45.0, scale * screen.height());
    m.gap = int(m.squareW * 0.11);
    m.size = QSize(int(cols * m.squareW), int(rows * m.squareH));
    m.cellW = (m.size.width() - (cols + 1) * m.gap) / cols;
    m.cellH = (m.size.height() - (rows + 1) * m.gap) / rows;
    m.radius = int(std::min(m.squareW, m.squareH) * 0.25);
    return m;
}

QPainterPath rounded(const QRectF &r, double radius)
{
    QPainterPath p;
    p.addRoundedRect(r, radius, radius);
    return p;
}

} // namespace

QRect WindowServer::bezelCell(int space) const
{
    // -_generatePositionForWsID:: the cells from the top left, a gap around each
    const BezelMetrics m = bezelMetrics(m_screen.size(), m_rows, m_cols);
    const int col = space % m_cols, row = space / m_cols;
    return QRect(col * (m.cellW + m.gap) + m.gap, row * (m.cellH + m.gap) + m.gap, m.cellW, m.cellH);
}

QImage WindowServer::bezelPanel() const
{
    // the grid layer (FUN_1000c4599) over the background one (FUN_1000c4de8)
    const BezelMetrics m = bezelMetrics(m_screen.size(), m_rows, m_cols);
    QImage img(m.size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    // the panel: black at 0.85, rounded by a quarter of a square
    p.fillPath(rounded(QRectF(QPointF(0, 0), QSizeF(m.size)), m.radius), QColor::fromRgbF(0, 0, 0, 0.85f));
    // its shine: the top third, a grey ramp (the axial shading from 0.2 to 0.91; its alphas by eye)
    QLinearGradient shine(0, 0, 0, m.size.height() * 0.33);
    shine.setColorAt(0, QColor::fromRgbF(1, 1, 1, 0.22f));
    shine.setColorAt(1, QColor::fromRgbF(1, 1, 1, 0.06f));
    p.fillPath(rounded(QRectF(1, 1, m.size.width() - 2, m.size.height() * 0.33), m.radius), shine);
    // the cells: black at 0.45, a white line at 0.2, a black one at 0.5 along their tops
    for (int s = 0; s < spaceCount(); ++s) {
        const QRectF cell = bezelCell(s);
        const QPainterPath path = rounded(cell, std::max(0, m.radius - m.gap));
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillPath(path, QColor::fromRgbF(0, 0, 0, 0.45f));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(QColor::fromRgbF(1, 1, 1, 0.2f), 1));
        p.drawPath(path);
        p.setPen(QPen(QColor::fromRgbF(0, 0, 0, 0.5f), 1));
        p.drawLine(QPointF(cell.left() + m.radius - m.gap, cell.top() + 0.5), QPointF(cell.right() - (m.radius - m.gap), cell.top() + 0.5));
    }
    return img;
}

void WindowServer::showBezel(int from, int to)
{
    if (!m_compositor.running())
        return;
    if (m_bezelPanel.isNull())
        m_bezelPanel = bezelPanel();
    m_bezel.from = m_bezel.shown && m_bezel.hideAt < 0 ? m_bezel.to : from;
    m_bezel.to = to;
    m_bezel.clock.start();
    m_bezel.hideAt = -1;
    m_bezel.shown = true;
    stepBezel();
    if (!m_bezelTimer.isActive())
        m_bezelTimer.start();
}

void WindowServer::stepBezel()
{
    if (!m_bezel.shown) {
        m_bezelTimer.stop();
        return;
    }
    const double t = m_bezel.clock.elapsed() / 1000.0;
    double opacity = 1;
    if (m_bezel.hideAt >= 0 && t > m_bezel.hideAt)
        opacity = 1 - qMin(1.0, (t - m_bezel.hideAt) / BezelFade);
    if (opacity <= 0) {
        m_bezel.shown = false;
        m_bezelTimer.stop();
        m_compositor.setOverlay(QImage(), QPoint(), 0);
        return;
    }
    // the indicator moving from the old cell to the new (the layer's implicit 0.25 s animation)
    const double k = qMin(1.0, t / SlideSeconds), e = 0.5 - 0.5 * std::cos(M_PI * k);
    const QRect a = bezelCell(m_bezel.from), b = bezelCell(m_bezel.to);
    const QRectF cell(a.x() + (b.x() - a.x()) * e, a.y() + (b.y() - a.y()) * e, a.width(), a.height());
    QImage img = m_bezelPanel;
    {
        // the indicator (FUN_1000c41f7): the cell 3 in, white at 0.9, its top 40% shining
        const BezelMetrics m = bezelMetrics(m_screen.size(), m_rows, m_cols);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF in = cell.adjusted(3, 3, -3, -3);
        const QPainterPath path = rounded(in, std::max(0, int(std::min(m.squareW, m.squareH) * 0.25) - m.gap));
        p.fillPath(path, QColor::fromRgbF(1, 1, 1, 0.9f));
        QLinearGradient shine(0, in.top(), 0, in.top() + in.height() * 0.4);
        shine.setColorAt(0, QColor::fromRgbF(1, 1, 1, 0.85f));
        shine.setColorAt(1, QColor::fromRgbF(0.85, 0.85, 0.85, 0.0f));
        p.save();
        p.setClipPath(path);
        p.fillRect(QRectF(in.left(), in.top(), in.width(), in.height() * 0.4), shine);
        p.restore();
    }
    // -show: centred across, its bottom (screen height - 100) / 5 over the screen's bottom
    const QPoint at(m_screen.x() + (m_screen.width() - img.width()) / 2,
                    m_screen.y() + m_screen.height() - img.height() - (m_screen.height() - 100) / 5);
    m_compositor.setOverlay(img, at, opacity);
}
