// SPDX-License-Identifier: LGPL-2.1-or-later
// Dock.app: the Dock, a program of its own as on the Mac.
#include "aqua.h"
#include "dock.h"

#include <QApplication>

int main(int argc, char **argv)
{
    Aqua::preinit();
    QApplication app(argc, argv);
    app.setApplicationName("Dock");
    Aqua::init(app);
    Dock dock;
    dock.show();
    return app.exec();
}
