// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dockmenu.h"

#include "art.h"
#include "assets.h"
#include "x11.h"

#include <QPaintEvent>
#include <QPainter>
#include <QScreen>

// Where the frame's opaque body starts in its pieces (measured on the art: the edges' alpha reaches the
// body's 217 at these depths). The callout replaces a stretch of the bottom row: its tip is in that
// row, 9 pixels above the window's bottom.
static constexpr int BodyLeft = 26, BodyRight = 26, BodyTop = 21, BodyBottom = 23, TipFromBottom = 9;

DockMenu::DockMenu(const QImage &wallpaper, QWidget *parent, bool callout)
    : QMenu(parent), m_wallpaper(wallpaper), m_callout(callout)
{
    setAttribute(Qt::WA_DeleteOnClose);
    if (X11::compositing())
        setAttribute(Qt::WA_TranslucentBackground);
    setFont(Assets::uiFont(14));
    // The items inside the body, a few pixels in, white on the dark body. The margins are the contents'
    // margins: a QMenu style sheet's padding has one value for the top and the bottom.
    setContentsMargins(BodyLeft, BodyTop + 4, BodyRight, BodyBottom + 4);
    setStyleSheet(QStringLiteral("QMenu { background: transparent; border: none; padding: 0px; }"
                                 "QMenu::item { color: white; padding: 2px 22px 2px 18px; }"
                                 "QMenu::item:disabled { color: rgba(255, 255, 255, 110); }"
                                 "QMenu::separator { height: 1px; background: rgba(255, 255, 255, 45); margin: 5px 0px; }"));
}

DockMenu *DockMenu::addDockMenu(const QString &title)
{
    auto *sub = new DockMenu(m_wallpaper, this, false);
    sub->setAttribute(Qt::WA_DeleteOnClose, false);
    sub->setTitle(title);
    addMenu(sub);
    return sub;
}

void DockMenu::popupAbove(int anchorX, int iconTop)
{
    QSize s = sizeHint();
    QRect screen = QGuiApplication::screenAt(QPoint(anchorX, iconTop))->geometry();
    int x = qBound(screen.left(), anchorX - s.width() / 2, screen.right() - s.width());
    m_anchorX = anchorX - x;
    popup(QPoint(x, iconTop - (m_callout ? s.height() - TipFromBottom : s.height())));
}

void DockMenu::paintEvent(QPaintEvent *e)
{
    {
        QPainter p(this);
        if (!m_wallpaper.isNull()) {
            QPoint at = mapToGlobal(QPoint(0, 0)) - screen()->geometry().topLeft();
            p.drawImage(QPoint(0, 0), m_wallpaper, QRect(at, size()));
        }
        const QImage tl = Art::shared(Art::DockMenuTopLeft), t = Art::shared(Art::DockMenuTop),
                     tr = Art::shared(Art::DockMenuTopRight), l = Art::shared(Art::DockMenuLeft),
                     c = Art::shared(Art::DockMenuCentre), r = Art::shared(Art::DockMenuRight),
                     bl = Art::shared(Art::DockMenuBottomLeft), b = Art::shared(Art::DockMenuBottom),
                     br = Art::shared(Art::DockMenuBottomRight), arrow = Art::shared(Art::DockMenuCalloutDown);
        if (tl.isNull() || bl.isNull()) {
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QColor(255, 255, 255, 60));
            p.setBrush(QColor(21, 21, 21, 217));
            p.drawRoundedRect(QRectF(rect()).adjusted(BodyLeft, BodyTop, -BodyRight, -BodyBottom), 8, 8);
        } else {
            // the frame; the callout in its bottom row
            const int W = width(), H = height();
            const int cw = tl.width(), th = tl.height(), bh = bl.height();
            p.drawImage(QRect(0, 0, cw, th), tl);
            p.drawImage(QRect(cw, 0, W - 2 * cw, th), t);
            p.drawImage(QRect(W - cw, 0, cw, th), tr);
            p.drawImage(QRect(0, th, cw, H - th - bh), l);
            p.drawImage(QRect(cw, th, W - 2 * cw, H - th - bh), c);
            p.drawImage(QRect(W - cw, th, cw, H - th - bh), r);
            p.drawImage(QRect(0, H - bh, cw, bh), bl);
            p.drawImage(QRect(W - cw, H - bh, cw, bh), br);
            if (m_callout && !arrow.isNull()) {
                int ax = qBound(cw, (m_anchorX < 0 ? W / 2 : m_anchorX) - arrow.width() / 2, W - cw - arrow.width());
                p.drawImage(QRect(cw, H - bh, ax - cw, bh), b);
                p.drawImage(QPoint(ax, H - bh), arrow);
                p.drawImage(QRect(ax + arrow.width(), H - bh, W - cw - ax - arrow.width(), bh), b);
            } else {
                p.drawImage(QRect(cw, H - bh, W - 2 * cw, bh), b);
            }
        }
    }
    QMenu::paintEvent(e);   // the items, on the transparent background
}
