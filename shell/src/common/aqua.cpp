// SPDX-License-Identifier: LGPL-2.1-or-later
#include "aqua.h"

#include "art.h"
#include "artfile.h"
#include "aquastyle.h"
#include "assets.h"
#include "x11.h"

#include <QApplication>
#include <QMenu>

namespace Aqua {

void preinit()
{
    qputenv("QT_QPA_PLATFORM", "xcb");
    qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1");
    // the shell's programs belong to the Snow Leopard session, not to the desktop they are started
    // from: without this, KDE's session manager saves them and starts them again at the next login
    qunsetenv("SESSION_MANAGER");
}

// 10.6's menus: 14 px Lucida Grande, 20 px rows, the selection a blue gradient with white text.
static const char *menuStyle = R"(
QMenu {
    background: rgba(255, 255, 255, 243);
    border: 1px solid rgba(0, 0, 0, 40);
    border-top: none;
    padding: 4px 0px 5px 0px;
}
QMenu::item {
    color: black;
    padding: 2px 22px 2px 20px;
    min-height: 16px;
    background: transparent;
}
QMenu::item:selected {
    color: white;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #6a8ef8, stop:1 #2155ef);
}
QMenu::item:disabled { color: rgba(0, 0, 0, 90); }
QMenu::item:disabled:selected { color: rgba(0, 0, 0, 90); background: transparent; }
QMenu::separator { height: 1px; background: rgba(0, 0, 0, 25); margin: 5px 1px 6px 1px; }
QMenu::indicator { width: 14px; left: 4px; }
QMenu::right-arrow { right: 8px; }
)";

void init(QApplication &app)
{
    Assets::loadFonts();
    // CoreUI's controls (aquastyle.h); the menu style sheet only where CoreUI's art is missing
    app.setStyle(new AquaStyle);
    app.setFont(Assets::uiFont(13));
    if (ArtFile::element({"menu", "allround"}).isNull()) {
        QString style = QString::fromLatin1(menuStyle);
        QString selected = Art::sharedFile(Art::MenuItemSelected);
        if (!selected.isEmpty())
            style += QStringLiteral("QMenu::item:selected { background: transparent; border-image: url(%1) 0 0 0 0 stretch stretch; }")
                         .arg(selected);
        app.setStyleSheet(style);
    }
    app.setQuitOnLastWindowClosed(false);
}

void prepareMenu(QMenu *menu)
{
    menu->setFont(Assets::uiFont(14));
    for (QAction *a : menu->actions())
        if (a->menu()) {
            a->menu()->setProperty("aquaMenu", menu->property("aquaMenu"));
            prepareMenu(a->menu());
        }
}

} // namespace Aqua
