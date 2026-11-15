// SPDX-License-Identifier: LGPL-2.1-or-later
// The applications: the Linux ones (their .desktop entries), and Snow Leopard's, each standing for
// the Linux program that does its job (Safari for the web browser, Mail for the mail client...),
// with the Mac's name and icon. The Finder's own programs will join them as they are ported.
#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

struct App {
    QString id;          // the .desktop file's name without .desktop, or "finder"
    QString name;        // as shown: the Mac's name when it stands for a Mac application
    QString macBundle;   // "/Applications/Safari.app" when it stands for one
    QStringList exec;    // the command (field codes removed)
    QString wmClass;     // StartupWMClass, or the guess from the command
    QString iconName;    // the .desktop entry's Icon
    bool valid() const { return !id.isEmpty(); }
};

namespace Apps {

// Every installed application, by id.
const QVector<App> &all();
App byId(const QString &id);
// The Mac application `bundle` stands for, if one of its Linux counterparts is installed.
App forMacBundle(const QString &bundle);
// The application a window belongs to: by WM_CLASS, instance name or process.
App forWindow(const QString &wmClass, const QString &instance, uint32_t pid);
// The Finder: our own once it is built, the desktop's file manager meanwhile.
App finder();

QImage icon(const App &app, int size);
bool launch(const App &app, const QStringList &files = {});
// A document opened as the Finder opens it: PDFs and pictures in Preview
bool openFile(const QString &path);

} // namespace Apps
