// SPDX-License-Identifier: LGPL-2.1-or-later
// Stacks: the Dock's folders on 10.6. A folder's tile is the pile of its first items (or the folder's
// icon: "Display as Folder"); a click shows its items as a fan rising from the tile, a grid on the
// Dock's dark panel (the Dock's stackbackground art) or a list in a Dock menu. Their settings, as 10.6's
// com.apple.dock persistent-others keep them: arrangement (1 name, 2 date added, 3 date modified,
// 4 date created, 5 kind), displayas (0 stack, 1 folder), showas (0 automatic, 1 fan, 2 grid, 3 list);
// the defaults are the Dock's default.plist (Applications, Documents, Downloads), yours in dock.conf.
#pragma once

#include "apps.h"

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;

namespace Stacks {

enum Sort { Name = 1, DateAdded = 2, DateModified = 3, DateCreated = 4, Kind = 5 };
enum View { Automatic = 0, Fan = 1, Grid = 2, List = 3 };

// A stack's folder: a directory, or "applications:" (the Mac applications that have a Linux program
// standing for them; "applications:/Utilities" their Utilities folder)
struct Settings {
    Sort sort = DateAdded;
    bool asFolder = false;
    View view = Automatic;
};
Settings settings(const QString &folder);
void save(const QString &folder, const Settings &s);
QString name(const QString &folder);   // "Downloads", "Applications"

struct Item {
    QString name;
    QString path;      // a file, or a folder (a stack's own folder for "applications:")
    App app;           // an application of the Applications stack
    bool folder = false;
    QDateTime added, modified, created;
    QString kind;
};
QVector<Item> items(const QString &folder, Sort sort);

QImage folderIcon(const QString &folder, int size);
// An item's icon as the Mac shows it: the application's, a picture's thumbnail, Preview's PDF and
// picture documents, TextEdit's text ones, CoreTypes' folder and document
QImage itemIcon(const Item &item, int size);
// "Display as Stack": the first items piled up, the first on top, those under it turned a little
QImage pile(const QString &folder, const Settings &s, int size);

// What a new 10.6 account finds in its stacks: "About Stacks.pdf" in Documents, "About Downloads.pdf"
// in Downloads (the DVD's User Template), put there once, never over a file of yours
void installTemplate();

// How many icons the fan holds over a tile (its top at tileTop): what fits under the menu bar
int fanCapacity(QPoint tileCentre, int tileTop);

// Opens an item: its application, its document with its program, a folder in the Finder
void open(const Item &item);

// The fan or the grid, over the Dock above its tile; closes itself when an item is opened or the
// pointer clicks elsewhere
class Popup : public QWidget {
    Q_OBJECT
public:
    // tileCentre: the tile's centre on the screen, tileTop its icon's top
    Popup(const QString &folder, View view, const QImage &wallpaper, QPoint tileCentre, int tileTop);
    View view() const { return m_view; }
    QString folder() const { return m_folder; }

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    struct Cell {
        Item item;
        QRectF icon, label;   // in the window
        QImage image;
        qreal angle = 0;      // the fan's turn, degrees
    };
    void layoutFan();
    void layoutGrid();
    void show(const QString &folder);   // the grid's folder (it goes into subfolders)
    int cellAt(QPointF p) const;
    void paintFan(QPainter &p);
    void paintGrid(QPainter &p);

    QString m_root, m_folder;
    View m_view;
    QImage m_wallpaper;
    QPoint m_tileCentre;
    int m_tileTop;
    QVector<Item> m_items;
    QVector<Cell> m_cells;
    int m_more = 0;           // the fan: items left out ("N More in Finder")
    int m_hover = -1;
    qreal m_progress = 0;     // the opening
    qreal m_scroll = 0, m_contentHeight = 0;
    QRectF m_panel, m_body, m_finderButton, m_backButton;
    int m_tipX = 0;
    QTimer *m_anim;
};

} // namespace Stacks
