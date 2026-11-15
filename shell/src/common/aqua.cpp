// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aqua.h"

#include "art.h"
#include "artfile.h"
#include "aquastyle.h"
#include "assets.h"
#include "x11.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace Aqua {

void preinit()
{
    qputenv("QT_QPA_PLATFORM", "xcb");
    qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");
    // the shell's programs belong to the Snow Leopard session, not to the desktop they are started
    // from: without this, KDE's session manager saves them and starts them again at the next login
    qunsetenv("SESSION_MANAGER");
}

// 10.6's menus: 14 px Lucida Grande, 20 px rows, the selection a blue gradient with white text.
static const char *menuStyle = R"(
QMenu {
    background: rgba(255, 255, 255, 243);
    border: 1px solid rgba(0, 0, 0, 40);
    border-top: none;
    padding: 4px 0px 5px 0px;
}
QMenu::item {
    color: black;
    padding: 2px 22px 2px 20px;
    min-height: 16px;
    background: transparent;
}
QMenu::item:selected {
    color: white;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #6a8ef8, stop:1 #2155ef);
}
QMenu::item:disabled { color: rgba(0, 0, 0, 90); }
QMenu::item:disabled:selected { color: rgba(0, 0, 0, 90); background: transparent; }
QMenu::separator { height: 1px; background: rgba(0, 0, 0, 25); margin: 5px 1px 6px 1px; }
QMenu::indicator { width: 14px; left: 4px; }
QMenu::right-arrow { right: 8px; }
)";

void init(QApplication &app)
{
    Assets::loadFonts();
    // CoreUI's controls (aquastyle.h); the menu style sheet only where CoreUI's art is missing
    app.setStyle(new AquaStyle);
    app.setFont(Assets::uiFont(13));
    if (ArtFile::element({"menu", "allround"}).isNull()) {
        QString style = QString::fromLatin1(menuStyle);
        QString selected = Art::sharedFile(Art::MenuItemSelected);
        if (!selected.isEmpty())
            style += QStringLiteral("QMenu::item:selected { background: transparent; border-image: url(%1) 0 0 0 0 stretch stretch; }")
                         .arg(selected);
        app.setStyleSheet(style);
    }
    app.setQuitOnLastWindowClosed(false);
}

void prepareMenu(QMenu *menu)
{
    menu->setFont(Assets::uiFont(14));
    for (QAction *a : menu->actions())
        if (a->menu()) {
            a->menu()->setProperty("aquaMenu", menu->property("aquaMenu"));
            prepareMenu(a->menu());
        }
}

// Quartz's dilation of a glyph's outline (path_dilator_iterate): every segment is moved, across by
// dx outwards (to the right when it goes up, to the left when it goes down) and, when it runs along
// the top of a stroke, up by twice dy; those along the bottom stay. The outer contours turn the way
// Quartz's own orientation pass leaves them.
static QPainterPath dilatedOutline(const QPainterPath &glyphs, qreal dx, qreal dy)
{
    constexpr qreal fine = 16;   // the curves flattened sixteen times finer than a pixel
    QList<QPolygonF> contours = glyphs.toSubpathPolygons(QTransform::fromScale(fine, fine));
    qreal area = 0;
    for (QPolygonF &c : contours) {
        if (c.size() > 1 && c.first() == c.last())
            c.removeLast();
        for (int i = 0; i < c.size(); ++i) {
            const QPointF a = c[i], b = c[(i + 1) % c.size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
    }
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    for (QPolygonF c : contours) {
        if (area > 0)
            std::reverse(c.begin(), c.end());
        QPolygonF moved;
        for (int i = 0; i < c.size(); ++i) {
            const QPointF a = c[i] / fine, b = c[(i + 1) % c.size()] / fine;
            const qreal across = b.x() - a.x(), down = b.y() - a.y();
            if (across == 0 && down == 0)
                continue;
            const QPointF by(down < 0 ? dx : down > 0 ? -dx : 0, across > 0 ? 0 : across < 0 ? -2 * dy : -dy);
            moved << a + by << b + by;
        }
        if (moved.size() > 2) {
            out.addPolygon(moved);
            out.closeSubpath();
        }
    }
    return out;
}

void drawText(QPainter &p, const QRectF &rect, int flags, const QString &text)
{
    QString line = text;
    if (flags & Qt::TextShowMnemonic)
        line.remove(QLatin1Char('&'));
    if (line.isEmpty())
        return;
    QFont font = p.font();
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(font);
    const qreal width = fm.horizontalAdvance(line), height = fm.ascent() + fm.descent();
    qreal x = rect.left();
    if (flags & Qt::AlignHCenter)
        x = rect.left() + (rect.width() - width) / 2;
    else if (flags & Qt::AlignRight)
        x = rect.right() - width;
    qreal top = rect.top();
    if (flags & Qt::AlignVCenter)
        top = rect.top() + (rect.height() - height) / 2;
    else if (flags & Qt::AlignBottom)
        top = rect.bottom() - height;
    const qreal baseline = std::round(top + fm.ascent());

    QPainterPath glyphs;
    glyphs.addText(QPointF(x, baseline), font, line);
    const qreal size = font.pixelSize() > 0 ? font.pixelSize() : fm.height();
    const QPainterPath dilated = dilatedOutline(glyphs, qMin(0.3, 0.0121 * size), qMin(0.3, 0.015125 * size));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(dilated, p.pen().color());
    p.restore();
}

} // namespace Aqua
