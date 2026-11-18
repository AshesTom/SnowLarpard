// SPDX-License-Identifier: LGPL-2.1-or-later
#include "frame.h"

#include "aqua.h"
#include "artfile.h"
#include "assets.h"
#include "x11.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Frame {

// ---- the title bar

namespace {

// The buttons' places in the title bar: 14 x 16 pieces, 7 px apart, from 8 px in (measured by eye on 10.6)
constexpr int ButtonLeft = 8, ButtonPitch = 21, ButtonTop = 3, ButtonWidth = 14, ButtonHeight = 16;
// A utility window's: titlebarcontrols' "utilwin" "variant2" pieces, 13 x 14, 18 apart from 5 in, 1 down
// (a 10.6 screenshot of About This Mac: the rings from x 6 and y 2, 11 wide)
constexpr int UtilityButtonLeft = 5, UtilityButtonPitch = 18, UtilityButtonTop = 1, UtilityButtonWidth = 13, UtilityButtonHeight = 14;
// The toolbar button (titlebarcontrols' "toolbar", 20 x 13) of a window with a toolbar: 7 px from the
// right, centred on the other buttons (measured on a 10.6 screenshot)
constexpr int ToolbarButtonRight = 7, ToolbarButtonTop = 5, ToolbarButtonWidth = 20, ToolbarButtonHeight = 13;
// The proxy icon: 16 x 16 before the title, 4 px from it, the two centred together
constexpr int ProxyIconSize = 16, ProxyIconGap = 4, ProxyIconTop = 3;

QImage buttonImage(const TitleBar &t, int button)
{
    static const char *names[] = {"close", "minimize", "zoom", "toolbar"};
    // -[NSWindow setDocumentEdited:]: the close button with its dot (closedirty)
    const char *name = button == Close && t.documentEdited ? "closedirty" : names[button];
    QStringList path = {"titlebarcontrols", name, button == ToolbarButton ? "colorsrgb" : "colorsryg"};
    path << (t.utility ? QStringList{"utilwin", "variant2"} : QStringList{"regularwin"});
    if (!t.active)
        path << "inactive";
    else if (t.pressed == button)
        path << "pressed";
    else if (t.overButtons && button != ToolbarButton)
        path << "rollover";   // the three show their glyphs together, as on the Mac
    return ArtFile::element(path).single();
}

} // namespace

QRect buttonRect(const TitleBar &t, int b)
{
    if (b == ToolbarButton)
        return QRect(t.width - ToolbarButtonRight - ToolbarButtonWidth, ToolbarButtonTop, ToolbarButtonWidth, ToolbarButtonHeight);
    if (t.utility)
        return QRect(UtilityButtonLeft + b * UtilityButtonPitch, UtilityButtonTop, UtilityButtonWidth, UtilityButtonHeight);
    return QRect(ButtonLeft + b * ButtonPitch, ButtonTop, ButtonWidth, ButtonHeight);
}

int buttonAt(const TitleBar &t, QPoint p)
{
    for (int b = Close; b <= (t.buttons ? int(Zoom) : int(Close)); ++b)
        if (buttonRect(t, b).contains(p))
            return b;
    if (t.hasToolbar && buttonRect(t, ToolbarButton).contains(p))
        return ToolbarButton;
    return None;
}

QColor unifiedToolbarColor(int row, int totalHeight, bool active) { return X11::unifiedToolbarColor(row, totalHeight, active); }

QImage paintTitleBar(const TitleBar &t)
{
    const int th = t.height(), w = t.width;
    if (w <= 0)
        return {};
    QImage img(w, th, QImage::Format_RGB32);
    QPainter p(&img);
    const bool active = t.active;
    // Measured on 10.6 (Software Update's window): a white first row, then 239 down to 217, then a 153
    // line over the content. The inactive bar: lighter, estimated.
    if (t.unifiedToolbar > 0) {
        // a unified window: the title bar's rows of the gradient its toolbar goes on with, no line under
        for (int y = 0; y < th; ++y)
            p.fillRect(0, y, w, 1, unifiedToolbarColor(y, th + t.unifiedToolbar, active));
    } else if (t.utility) {
        // as a 10.6 screenshot shows it (About This Mac, not key)
        p.fillRect(0, 0, w, 1, QColor(242, 242, 242));
        for (int y = 1; y <= 14; ++y) {
            const int g = int(std::lround(237 + (217 - 237) * (y - 1) / 13.0));
            p.fillRect(0, y, w, 1, QColor(g, g, g));
        }
        p.fillRect(0, 15, w, 1, QColor(166, 166, 166));
        p.fillRect(0, 16, w, 1, QColor(217, 217, 217));
    } else {
        p.fillRect(0, 0, w, 1, QColor(255, 255, 255));
        QLinearGradient g(0, 1, 0, th - 1);
        g.setColorAt(0, active ? QColor(239, 239, 239) : QColor(246, 246, 246));
        g.setColorAt(1, active ? QColor(217, 217, 217) : QColor(236, 236, 236));
        p.fillRect(0, 1, w, th - 2, g);
        p.fillRect(0, th - 1, w, 1, active ? QColor(153, 153, 153) : QColor(186, 186, 186));
    }

    for (int b = Close; b <= (t.buttons ? int(Zoom) : int(Close)); ++b) {
        QImage button = buttonImage(t, b);
        if (!button.isNull())
            p.drawImage(buttonRect(t, b).topLeft(), button);
        else {
            static const QColor colors[] = {QColor(0xE0, 0x44, 0x3E), QColor(0xDE, 0xA1, 0x23), QColor(0x5E, 0xB5, 0x34)};
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(active ? colors[b] : QColor(190, 190, 190));
            p.setPen(Qt::NoPen);
            p.drawEllipse(QRectF(buttonRect(t, b)).adjusted(0.5, 1.5, -0.5, -1.5));
        }
    }

    // a window with a toolbar: the button that shows and hides it, at the right
    if (t.hasToolbar) {
        QImage button = buttonImage(t, ToolbarButton);
        if (!button.isNull())
            p.drawImage(buttonRect(t, ToolbarButton).topLeft(), button);
    }

    // the title, centred on the window, in Lucida Grande 13, engraved; the proxy icon before it
    // (a utility window's in the small system font, 11)
    QFont font = Assets::uiFont(t.utility ? 11 : 13);
    p.setFont(font);
    const int left = buttonRect(t, Zoom).right() + 1 + 6;
    const bool proxy = !t.proxyIcon.isNull();
    const int iconRoom = proxy ? ProxyIconSize + ProxyIconGap : 0;
    const int room = w - 2 * left - iconRoom;
    const QFontMetrics fm(font);
    QString text = fm.elidedText(t.title, Qt::ElideMiddle, qMax(0, room));
    const int groupWidth = iconRoom + fm.horizontalAdvance(text);
    const int x = (w - groupWidth) / 2;
    if (proxy) {
        p.setOpacity(active ? 1 : 0.5);
        p.drawImage(QRect(x, ProxyIconTop, ProxyIconSize, ProxyIconSize), t.proxyIcon);
        p.setOpacity(1);
    }
    QRect r(x + iconRoom, 0, w - x - iconRoom, t.utility ? 15 : th - 1);
    p.setPen(QColor(255, 255, 255, 120));
    Aqua::drawText(p, r.translated(0, 1), Qt::AlignLeft | Qt::AlignVCenter, text);
    p.setPen(active ? QColor(0x26, 0x26, 0x26) : QColor(0x7F, 0x7F, 0x7F));
    Aqua::drawText(p, r, Qt::AlignLeft | Qt::AlignVCenter, text);
    p.end();
    return img;
}

// ---- shadows

namespace {

// AppKit's __NSSetWindowShadow (CGSSetWindowShadowAndRimParametersWithStretch): the standard
// deviation, the density, how far under the window. The key window's, another window's, and the
// other style's (panels, menus).
struct ShadowParams {
    double sigma, density, dy;
};
const ShadowParams shadowParams[] = {{0, 0, 0}, {20, 0.49, 15}, {10, 0.44, 10}, {8, 0.5, 6}};

// One pass of the window server's tent blur (CGSOffsetTentBlur8): each value becomes the mean of its
// neighbours less than `radius` away, weighted radius - distance (the weights' sum: radius squared).
// A tent is a box blurred by a box, so two running sums do it.
void tentBlur(float *values, int count, int step, int radius, std::vector<float> &scratch)
{
    if (radius < 2)
        return;
    scratch.assign(size_t(count), 0.f);
    float sum = 0;
    for (int i = 0; i < count; ++i) {   // the `radius` values ending here
        sum += values[size_t(i) * size_t(step)];
        if (i >= radius)
            sum -= values[size_t(i - radius) * size_t(step)];
        scratch[size_t(i)] = sum;
    }
    sum = 0;
    const float scale = 1.f / (float(radius) * float(radius));
    for (int i = count - 1; i >= 0; --i) {   // the `radius` sums starting here
        sum += scratch[size_t(i)];
        if (i + radius < count)
            sum -= scratch[size_t(i + radius)];
        values[size_t(i) * size_t(step)] = sum * scale;
    }
}

// The window's shadow as an alpha mask, made as 10.6's window server makes it (CGXWindowShadowGet,
// CoreGraphics 0x2b74e1): the window's outline, blurred by a tent 1.7 standard deviations wide
// across then down, times the density, moved under the window; a rim one pixel around the outline
// (CGSMergeShadowRim: an eighth and a thirty-second of it, or an eighth less a sixty-fourth around
// a window that isn't key); nothing under the window itself (CGSSubtractMaskWithThreshold, 0x40);
// then the server's gamma table (the 1.23 power, to a level).
} // namespace

std::vector<uint8_t> windowOutline(int ww, int wh, bool roundedBottom)
{
    std::vector<uint8_t> outline(size_t(qMax(0, ww)) * size_t(qMax(0, wh)), 255);
    // CoreUI's CreateWindowPath: the top corners rounded, radius 4, as WindowServer shapes its frames
    constexpr double r = 4;
    for (int y = 0; y < int(r) && y < wh; ++y) {
        const double dy = r - (y + 0.5);
        const int inset = qMin(ww, int(std::ceil(r - std::sqrt(r * r - dy * dy) - 0.5)));
        for (int row : {y, roundedBottom ? wh - 1 - y : -1}) {
            if (row < 0)
                continue;
            uint8_t *line = outline.data() + size_t(row) * size_t(ww);
            memset(line, 0, size_t(inset));
            memset(line + ww - inset, 0, size_t(inset));
        }
    }
    return outline;
}

std::vector<uint8_t> alphaOutline(const QImage &image)
{
    const QImage a = image.convertToFormat(QImage::Format_ARGB32);
    std::vector<uint8_t> outline(size_t(a.width()) * size_t(a.height()));
    for (int y = 0; y < a.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            outline[size_t(y) * size_t(a.width()) + size_t(x)] = uint8_t(qAlpha(line[x]));
    }
    return outline;
}

// The window's shadow as an alpha mask, made as 10.6's window server makes it (CGXWindowShadowGet,
// CoreGraphics 0x2b74e1): the window's outline, blurred by a tent 1.7 standard deviations wide
// across then down, times the density, moved under the window; a rim one pixel around the outline
// (CGSMergeShadowRim: an eighth and a thirty-second of it, or an eighth less a sixty-fourth around
// a window that isn't key); nothing under the window itself (CGSSubtractMaskWithThreshold, 0x40);
// then the server's gamma table (the 1.23 power, to a level).
QImage shadowMask(const std::vector<uint8_t> &windowOutline, int ww, int wh, int kind, QPoint *origin)
{
    if (kind <= NoShadow || kind > PanelShadow || ww <= 0 || wh <= 0 || windowOutline.size() < size_t(ww) * size_t(wh))
        return {};
    const ShadowParams &p = shadowParams[kind];
    const int radius = int(std::lround(1.7 * p.sigma)), dy = int(p.dy);
    const int margin = radius + 1;
    const int sw = ww + 2 * margin, sh = wh + 2 * margin + dy;

    // the outline with the shadow's room around it
    std::vector<uint8_t> outline(size_t(sw) * size_t(sh), 0);
    for (int y = 0; y < wh; ++y)
        memcpy(outline.data() + size_t(y + margin) * size_t(sw) + size_t(margin), windowOutline.data() + size_t(y) * size_t(ww), size_t(ww));

    // blurred across then down, and moved under the window
    std::vector<float> blurred(size_t(sw) * size_t(sh), 0.f);
    for (int y = 0; y < wh; ++y)
        for (int x = 0; x < ww; ++x)
            blurred[size_t(y + margin + dy) * size_t(sw) + size_t(x + margin)] = outline[size_t(y + margin) * size_t(sw) + size_t(x + margin)];
    std::vector<float> scratch;
    for (int y = margin + dy; y < margin + dy + wh; ++y)
        tentBlur(blurred.data() + size_t(y) * size_t(sw), sw, 1, radius, scratch);
    for (int x = 0; x < sw; ++x)
        tentBlur(blurred.data() + size_t(x), sh, sw, radius, scratch);

    uint8_t gamma[256];
    for (int i = 0; i < 256; ++i)
        gamma[i] = uint8_t(std::lround(255 * std::pow(i / 255.0, 1.23)));
    const bool key = kind != WindowShadow;
    QImage mask(sw, sh, QImage::Format_Alpha8);
    mask.fill(0);
    for (int y = 0; y < sh; ++y) {
        uchar *line = mask.scanLine(y);
        for (int x = 0; x < sw; ++x) {
            const size_t at = size_t(y) * size_t(sw) + size_t(x);
            if (outline[at] >= 0x40)
                continue;   // under the window: none
            int around = 0;   // the outline grown by a pixel: the rim
            for (int ny = qMax(0, y - 1); ny <= qMin(sh - 1, y + 1); ++ny)
                for (int nx = qMax(0, x - 1); nx <= qMin(sw - 1, x + 1); ++nx)
                    around = qMax(around, int(outline[size_t(ny) * size_t(sw) + size_t(nx)]));
            const int rim = (around >> 3) + (key ? around >> 5 : -(around >> 6));
            const int value = int(std::lround(p.density * blurred[at])) + rim;
            line[x] = gamma[qBound(0, value, 255)];
        }
    }
    if (origin)
        *origin = QPoint(-margin, -margin);
    return mask;
}

// ---- Minimizing: 10.6's Dock warps the window into its tile (docs/reverse-status.md, the Dock)

namespace {

// The rows' curve (the Dock's 0x1000ef17a): the edges come in as a half cosine, from none at the
// window's top to the whole way over `count` pixels, the distance to the tile
struct GenieCurve {
    double y0, count, left, right;
    GenieRow row(double y) const
    {
        const double k = 0.5 - 0.5 * std::cos((y - y0) * M_PI / count);
        return {y, left * k, right * k, 0};
    }
};

// The rows between a and b (the Dock's 0x1000ef20a): halved while the middle one strays 0.6 px
// from the straight line, or the gap is a tile high; 8 px at least, 32 levels at most
void genieSubdivide(const GenieCurve &c, double a, double b, double tile, int depth, QVector<GenieRow> &out)
{
    if (depth + 1 >= 0x41 || std::abs(b - a) < 8)
        return;
    const double mid = (a + b) * 0.5;
    const GenieRow ra = c.row(a), rm = c.row(mid), rb = c.row(b);
    if (std::abs(b - a) >= tile || std::abs((ra.left + rb.left) * 0.5 - rm.left) >= 0.6 ||
        std::abs((ra.right + rb.right) * 0.5 - rm.right) >= 0.6) {
        genieSubdivide(c, a, mid, tile, depth + 2, out);
        out.append(rm);
        genieSubdivide(c, mid, b, tile, depth + 2, out);
    }
}

// The genie at p (0: the window, 1: in its tile), the tile under the window's top (the Dock's step
// at 0x1000ef474, direction 2). Two phases on one clock: the bottom edges come in to the tile's
// width, the window slides down the curve; each takes 50 a step of the logarithm of half its
// distance, and the slide starts once the edges have come in as far as the window's height
// leaves them. The window isn't stretched: row y of the screen shows its row y - top, its
// bottom edges straight in the tile, cut at the tile's bottom.
QVector<GenieRow> genieRowsDown(const QRectF &win, const QRectF &tile, double p)
{
    const double x = win.x(), y = win.y(), w = win.width(), h = win.height();
    double left = tile.x() - x, right = tile.right() - (x + w);
    const double gap = tile.y() - (y + h);
    double dist = tile.y() - y, up = 0;
    if (gap < 0) {   // over the tile: first up so that its bottom meets the tile's top
        up = gap;
        dist = h;
    }
    const double d = std::max(std::abs(2 * up), (std::abs(left) + std::abs(right)) * 0.125);
    double inTime = int(std::log(d * 0.5 + 1)) * 50.0;
    const double downTime = int(std::log(std::abs(dist) * 0.5 + 1)) * 50.0;
    const double rest = std::abs(dist - h);
    const double meet = dist != 0 ? std::acos(std::clamp(1 - 2 * rest / std::abs(dist), -1.0, 1.0)) / M_PI * downTime : 0;
    double lead = 0;
    if (meet < inTime)
        lead = inTime - meet;
    else
        inTime = meet;
    const double t = (lead + downTime) * p;
    const double s1 = inTime > 0 ? 0.5 - 0.5 * std::cos(std::clamp(t, 0.0, inTime) / inTime * M_PI) : p;
    const double s2 = downTime > 0 ? 0.5 - 0.5 * std::cos(std::clamp(t - lead, 0.0, downTime) / downTime * M_PI) : p;
    left *= s1;
    right *= s1;
    const double moved = dist * s2, y0 = y + s1 * up, top = y0 + moved, bottom = top + h;
    const double end = s2 > 0 && bottom > tile.y() ? tile.y() : bottom;
    const GenieCurve curve{y0, std::max(1.0, double(int(dist + 0.5))), left, right};
    QVector<GenieRow> rows{curve.row(top)};
    genieSubdivide(curve, top, end, tile.height(), 0, rows);
    rows.append(curve.row(end));
    if (std::abs(moved) > rest)
        rows.append({std::min(bottom, tile.bottom()), left, right, 0});
    for (GenieRow &r : rows)
        r.local = r.y - top;
    return rows;
}

} // namespace

QVector<GenieRow> genieRows(const QRectF &win, const QRectF &tile, double p)
{
    // the tile above the window's top: the same, upside down
    if (tile.center().y() >= win.y())
        return genieRowsDown(win, tile, p);
    auto mirror = [](const QRectF &r) { return QRectF(r.x(), -r.bottom(), r.width(), r.height()); };
    QVector<GenieRow> rows = genieRowsDown(mirror(win), mirror(tile), p);
    std::reverse(rows.begin(), rows.end());
    for (GenieRow &r : rows) {
        r.y = -r.y;
        r.local = win.height() - r.local;
    }
    return rows;
}

} // namespace Frame
