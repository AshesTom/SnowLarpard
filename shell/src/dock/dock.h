// SPDX-License-Identifier: LGPL-2.1-or-later
// The Dock: Snow Leopard's glass floor at the bottom of the screen, the applications kept in it and
// the running ones (with their lights), a separator, the folders and the Trash. Magnification,
// hiding, the bounce while an application starts, the names above the icons, the menus.
// Its settings are ~/.config/snowleopard/dock.conf (the Mac's com.apple.dock keys: tilesize,
// largesize, magnification, autohide, orientation, persistent-apps).
#pragma once

#include "apps.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QPointer>
#include <QImage>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <xcb/xcb.h>

namespace X11 { class Watcher; }
class DockMenu;

class Dock : public QWidget {
    Q_OBJECT
public:
    explicit Dock(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    struct Tile {
        enum Kind { Application, Separator, Folder, Trash } kind = Application;
        App app;
        QString entry;      // its persistent-apps entry ("mac:/Applications/Safari.app", "app:firefox"), when kept
        QString folder;     // a stack's folder (stack.h: a path, or "applications:")
        QString label;
        QVector<xcb_window_t> windows;
        qint64 bounceStart = -1;   // ms on m_clock while it launches
        // laid out:
        qreal x = 0, size = 0;     // centre and icon size
    };

    void loadSettings();
    void saveApps();
    void refreshWindows();
    void layoutTiles();
    void placeWindow();
    void paintDock(QPainter &p);
    QRegion m_keep;   // without a compositor: where the window takes the pointer besides what it draws
    QImage iconFor(const Tile &t, int size);
    int tileAt(const QPointF &p) const;
    void activate(Tile &t);
    void showMenu(int index, const QPoint &global);
    QString menuText(const char *key, const char *fallback) const;
    bool trashFull() const;
    void showStack(int index);
    void stackMenu(DockMenu *menu, const Tile &t);
    void foldersChanged(const QString &path);

    QVector<Tile> m_tiles;
    QStringList m_kept;            // persistent-apps entries
    int m_tileSize = 48, m_largeSize = 72;
    bool m_magnify = false, m_autohide = false;
    qreal m_mouseX = -1;           // for magnification, -1: away
    int m_hover = -1, m_pressed = -1, m_dragging = -1;
    QPointF m_pressPos;
    qreal m_hideOffset = 0;        // 0 shown .. 1 hidden
    QRectF m_floor;                // the floor's front edge rectangle, for hit tests
    QImage m_wallpaper;            // under the Dock when nothing composites
    QHash<QString, QImage> m_iconCache;
    QElapsedTimer m_clock;
    QTimer m_anim, m_hideTimer;
    QFileSystemWatcher m_watch;
    QFileSystemWatcher m_stackWatch;           // the stacks' folders: their piles, Downloads' bounce
    QHash<QString, int> m_stackCounts;         // items a stack had
    int m_stackStamp = 0;                      // changes when a stack's icon must be made again
    QPointer<QWidget> m_stack;                 // the fan or the grid shown, and its tile
    int m_stackTile = -1;
    X11::Watcher *m_watcher;
};
