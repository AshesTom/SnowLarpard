// SPDX-License-Identifier: LGPL-2.1-or-later
// WindowServer: the window manager of the Snow Leopard session.
#include "aqua.h"
#include "assets.h"
#include "windowserver.h"

#include <QGuiApplication>
#include <cstdio>

int main(int argc, char **argv)
{
    Aqua::preinit();
    QGuiApplication app(argc, argv);
    app.setApplicationName("WindowServer");
    app.setQuitOnLastWindowClosed(false);
    Assets::loadFonts();
    WindowServer server;
    if (!server.start()) {
        fprintf(stderr, "WindowServer: another window manager has the display\n");
        return 1;
    }
    return app.exec();
}
