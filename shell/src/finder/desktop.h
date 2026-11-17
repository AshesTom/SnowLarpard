// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's desktop: the desktop picture (Desktop & Screen Saver's settings: the placement, the
// colour around it, the picture changing by itself), its icons (desktopicons.h: the disks, the files of
// ~/Desktop) and their contextual menus. The Finder's windows are FinderWindows' (browser/).
#pragma once

#include "desktopsettings.h"

#include <QImage>
#include <QTimer>
#include <QWidget>

class DesktopIcons;

class DesktopWindow : public QWidget {
    Q_OBJECT
public:
    explicit DesktopWindow(QScreen *screen);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
private:
    void reload();
    void nextPicture();
    void showMenu(const QPoint &global);
    QImage m_picture;
    QTimer m_change;
    QString m_loaded;
    DesktopIcons *m_icons = nullptr;   // the disks and the Desktop folder's items (the main screen's)   // the settings it was drawn with, to redraw only when they change
};
