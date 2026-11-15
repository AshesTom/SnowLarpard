// SPDX-License-Identifier: LGPL-2.1-or-later
// SystemUIServer.app: the menu bar and its extras. The session ends with it (Log Out).
#include "aqua.h"
#include "menubar.h"

#include <QApplication>

int main(int argc, char **argv)
{
    Aqua::preinit();
    QApplication app(argc, argv);
    app.setApplicationName("SystemUIServer");
    Aqua::init(app);
    MenuBar bar;
    bar.show();
    return app.exec();
}
