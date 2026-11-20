// SPDX-License-Identifier: LGPL-2.1-or-later
// The applications: the Linux ones (their .desktop entries), and Snow Leopard's, each standing for
// the Linux program that does its job (Mail for the mail client, Preview for the PDF viewer...),
// with the Mac's name and icon. A Linux program that only does the job of a Mac application not
// ported (Firefox, Chromium... for Safari) keeps its own name and icon. The Finder's own programs
// will join them as they are ported.
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

struct App {
    QString id;          // the .desktop file's name without .desktop, or "finder"
    QString name;        // as shown: the Mac's name when it stands for a Mac application
    QString macBundle;   // "/Applications/Mail.app" when it stands for one
    QStringList exec;    // the command (field codes removed)
    QString wmClass;     // StartupWMClass, or the guess from the command
    QString iconName;    // the .desktop entry's Icon
    QStringList categories;   // the .desktop entry's Categories
    // in the applications' lists, as an application bundle in /Applications: not NoDisplay, Hidden
    // or Terminal (a command, /usr/bin's on the Mac), and meant for this desktop (OnlyShowIn, NotShowIn)
    bool shown = true;
    // LSUIElement (or LSBackgroundOnly) in a shell component's Info.plist: never a tile in the Dock,
    // as LaunchServices' application type isn't Foreground (the Dock's FUN_1000117f1)
    bool uiElement = false;
    bool valid() const { return !id.isEmpty(); }
};

namespace Apps {

// Every installed application, by id.
const QVector<App> &all();
App byId(const QString &id);
// The Mac application `bundle` stands for, if one of its Linux counterparts is installed.
App forMacBundle(const QString &bundle);
// The web browser (xdg-settings' default-web-browser when it is one, else the first one installed): Safari's place
// in a new account's Dock, as itself
App webBrowser();
// A Linux program doing the job of a Mac application the Applications folder has (KCalc for
// Calculator, Okular for Preview), whether it stands for it or the shell's own port does: not listed twice
bool doesMacJob(const App &app);
// In the Applications folder's Utilities: the system's tools and settings (Categories System, Settings)
bool isUtility(const App &app);
// The application a window belongs to: by WM_CLASS, instance name or process.
App forWindow(const QString &wmClass, const QString &instance, uint32_t pid);
// The Finder: our own once it is built, the desktop's file manager meanwhile.
App finder();

QImage icon(const App &app, int size);
bool launch(const App &app, const QStringList &files = {});
// A document opened as the Finder opens it: PDFs and pictures in Preview
bool openFile(const QString &path);

// The shell's AquaKit applications running on this display (their menus' sockets,
// "snowleopard-menus-<pid>"), with or without windows: by pid
QVector<QPair<uint32_t, App>> aquaKitApps();
// The application that came in front without a window (the root's _SNOWLEOPARD_FRONT_APP: its pid,
// and a serial that changes each time it comes forward); 0 when none (or it is gone)
uint32_t frontWithoutWindows(uint32_t *serial = nullptr);
App forPid(uint32_t pid);

} // namespace Apps
