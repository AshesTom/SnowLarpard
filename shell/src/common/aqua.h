// SPDX-License-Identifier: LGPL-2.1-or-later
// What the shell's programs share at start: the fonts, Snow Leopard's menus (white, with the blue
// selection gradient) and the X11 platform.
#pragma once

#include <QRectF>
#include <QString>

class QApplication;
class QMenu;
class QPainter;

namespace Aqua {

// Before QApplication: X11 (XWayland under a Wayland desktop), no Qt scaling of our own.
void preinit();
// After it: fonts, the menus' style.
void init(QApplication &app);
// A popup menu of the shell's: its window type, its translucency when a compositor runs.
void prepareMenu(QMenu *menu);
// One line of text in the painter's font and pen colour, as QPainter::drawText places it (the
// alignment flags, Qt::TextShowMnemonic), drawn as Quartz draws it with font smoothing: unhinted
// outlines, dilated, the baseline on a whole pixel, and the LCD smoothing where the painter draws on
// an image (quartztext.h).
void drawText(QPainter &p, const QRectF &rect, int flags, const QString &text);

} // namespace Aqua
