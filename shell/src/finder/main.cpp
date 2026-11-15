// SPDX-License-Identifier: LGPL-2.1-or-later
// Finder.app: for now the desktop alone (see docs/architecture.md for what comes next).
#include "aqua.h"
#include "desktop.h"

#include <QApplication>
#include <QScreen>

int main(int argc, char **argv)
{
    Aqua::preinit();
    QApplication app(argc, argv);
    app.setApplicationName("Finder");
    Aqua::init(app);
    QList<DesktopWindow *> desktops;
    for (QScreen *s : app.screens()) {
        auto *d = new DesktopWindow(s);
        d->show();
        desktops << d;
    }
    QObject::connect(&app, &QGuiApplication::screenAdded, [](QScreen *s) { (new DesktopWindow(s))->show(); });
    return app.exec();
}
