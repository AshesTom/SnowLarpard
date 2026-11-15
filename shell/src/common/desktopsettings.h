// SPDX-License-Identifier: LGPL-2.1-or-later
// The desktop picture and how it is shown: what Desktop & Screen Saver's Desktop tab sets, the Finder's
// desktop draws, and the Dock and the menu bar draw under themselves. ~/.config/snowleopard/desktop.conf.
// From the 10.6 pane (DesktopPictures.prefPane, docs/reverse-status.md section 7).
#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Desktop {

// The placements, as the pane's popup lists them (DSKPlacementType 1 to 5)
enum Placement { FillScreen = 1, FitToScreen = 2, StretchToFillScreen = 3, Center = 4, Tile = 5 };

struct Settings {
    QString picture;            // empty: 10.6's default
    int placement = FillScreen;
    QColor color = QColor(0x3D, 0x5B, 0x8C);   // around a fitted, centred or tiled picture
    int change = 0;             // 0: never, else an index into changeIntervals()
    bool random = false;
    QString changeFolder;       // where a changing picture comes from
    QStringList folders;        // the folders added with + (Folders, after Pictures)
};

Settings load();
void save(const Settings &s);
QString configFile();

// The picture's path: the chosen one if it is there, else 10.6's default
QString picturePath(const Settings &s);
// The desktop as shown on a screen of `size`
QImage render(const Settings &s, const QSize &size);

struct Source {
    QString name;
    QString path;
    bool apple = true;
};
// Apple's: DSKAppleRootSource::init's folders, in its order, that are on the DVD
QVector<Source> appleSources();
// The pictures of a folder (not its subfolders), by name
QStringList pictures(const QString &folder);

struct Interval {
    QString label;
    int seconds;   // -1: when logging in, -2: when waking from sleep
};
// "Change picture:", as its popup lists them
const QVector<Interval> &changeIntervals();

} // namespace Desktop
