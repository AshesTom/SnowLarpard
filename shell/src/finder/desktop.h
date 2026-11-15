// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's desktop: the desktop picture (Desktop & Screen Saver's settings: the placement, the
// colour around it, the picture changing by itself) and its contextual menu. Its icons (the disks, the
// files of ~/Desktop) and the Finder's windows are the next part of the port.
#pragma once

#include "desktopsettings.h"

#include <QImage>
#include <QTimer>
#include <QWidget>

class DesktopWindow : public QWidget {
    Q_OBJECT
public:
    explicit DesktopWindow(QScreen *screen);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    void reload();
    void nextPicture();
    void showMenu(const QPoint &global);
    QImage m_picture;
    QTimer m_change;
    QString m_loaded;   // the settings it was drawn with, to redraw only when they change
};
