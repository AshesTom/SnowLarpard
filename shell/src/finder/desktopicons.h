// SPDX-License-Identifier: LGPL-2.1-or-later
// The desktop's icons (TDesktopViewController, TDesktopIconLayoutController, TDesktopIcon): the disks
// Preferences > General asks for (ShowHardDrivesOnDesktop, ShowExternalHardDrivesOnDesktop,
// ShowRemovableMediaOnDesktop, ShowMountedServersOnDesktop), then the Desktop folder's items.
// The grid is the Finder's (TLayoutGrid in layout style 2, set by -setupLayoutGrids): the
// window positioning bounds (the screen less the menu bar and the Dock), its origin at their right
// edge (-calculateGridOriginForGridSize:), filled column by column from the right, each from the top
// (-nextOpenLocation:); an anchor 4 left of its cell's centre (-anchorAtGridLocation:); the cell's size
// TDesktopViewController -calculateGridSize's (iconview.h has the formula). The settings are the
// desktop's (DesktopViewSettings: finder.conf's Desktop keys, View Options' icon page); the files'
// places the Desktop folder's .DS_Store (Iloc), the disks' FXDesktopVolumePositions'.
// The labels are drawn without font smoothing (-drawLabelInContext:forDrag:), white with a shadow.
// The item commands go to the Finder's windows' process (FinderWindows: "items:<command>" with the
// paths, "drop:" for icons dropped on its windows); it tells the desktop where items it dropped on
// the desktop go ("place:"), over this desktop's socket.
#pragma once

#include "files.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

class QLineEdit;
class QLocalServer;
class QPainter;
class QWidget;

class DesktopIcons : public QObject {
    Q_OBJECT
public:
    explicit DesktopIcons(QWidget *desktop);
    void reload();                 // the items and the settings again
    void paint(QPainter &p, const QRect &dirty);
    bool mousePress(QPoint at, Qt::MouseButton button, Qt::KeyboardModifiers mods, int clicks);
    void mouseMove(QPoint at);
    void mouseRelease(QPoint at, Qt::KeyboardModifiers mods);
    bool keyPress(int key, Qt::KeyboardModifiers mods, const QString &text);
    void showMenu(QPoint at, QPoint global);   // the item's or the desktop's
    void setActive(bool active);

    // the desktop's state for the menu bar (what its commands can do), and its commands
    bool perform(const QString &cmd);
    bool eventFilter(QObject *o, QEvent *e) override;   // the rename field's Escape and focus

private:
    struct Icon {
        Finder::Item item;
        bool volume = false, ejectable = false;
        QPoint anchor;
        bool placed = false;   // its own place (Iloc, FXDesktopVolumePositions), else the grid's
    };
    struct Metrics {
        double pad, box, labelWidth, lineHeight;
        QSize grid;
    };
    Metrics metrics() const;
    QRect available() const;
    QPoint spot(int column, int row) const;
    void layout();
    QRect iconRect(const Icon &i) const;
    QRect labelRect(const Icon &i) const;
    QStringList labelLines(const QString &name) const;
    int iconAt(QPoint p) const;
    QImage imageFor(const Icon &i);
    void savePlaces(const QVector<int> &moved);
    void select(int i, bool extend);
    QVector<Finder::Item> selectedItems() const;
    QStringList selectedPaths() const;
    void open(const QVector<Finder::Item> &items);
    void send(const QString &line);
    void startRename();
    void endRename(bool commit);
    void cleanUp();
    void arrangeBy(const QString &by);
    void newFolder(QPoint at);
    void setLabel(int index);
    void writeState();
    void incoming();

    QWidget *m_desktop;
    QVector<Icon> m_icons;
    QSet<int> m_selection;
    int m_anchor = -1;
    // the settings (DesktopViewSettings)
    int m_iconSize = 64, m_gridSpacing = 54, m_textSize = 12;
    bool m_labelOnBottom = true, m_iconPreview = true;
    QString m_arrangeBy = "none";
    // the mouse
    enum Drag { None, Moving, Marquee } m_drag = None;
    QPoint m_pressAt, m_dragAt, m_menuAt;
    QSet<int> m_marqueeBase;
    bool m_active = false;
    QLineEdit *m_rename = nullptr;
    int m_renaming = -1;
    QLocalServer *m_server = nullptr;
    QVector<QPair<QString, QPoint>> m_pendingPlaces;   // dropped items, placed once they show up
    QHash<QString, QImage> m_images;
};
