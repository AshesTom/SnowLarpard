// SPDX-License-Identifier: LGPL-2.1-or-later
// An application's main menu (NSApp's mainMenu) for the shell's menu bar. The menus are its
// MainMenu.nib's `_NSMainMenu`: each item's title, key equivalent (NSKeyEquiv, NSKeyEquivModMask) and
// action (the nib's NSNibControlConnector to the first responder). The application validates the items
// when a menu opens (-validateMenuItem:: enabled, checked, a new title) and performs their actions.
//
// The menu bar (SystemUIServer, another process) reaches it over an abstract Unix socket,
// "snowleopard-menus-<pid>", one request per connection, a line of UTF-8:
//   "menus"     the answer: every item shown, one per line, then the end of the connection:
//               <depth> TAB <kind: M submenu, I item, S separator> TAB <enabled 0/1> TAB <checked 0/1>
//               TAB <key equivalent as drawn: "⇧⌘S"> TAB <action> TAB <path: "2.5.1"> TAB <title>
//   "do <path>" the item's action is performed (no answer)
// Alternates (an item shown in place of the one before with Option) and hidden items are not sent.
//
// The key equivalents work in the application's own windows too (-[NSMenu performKeyEquivalent:]):
// a window's key that no view took is given to performKeyEquivalent.
#pragma once

#include "appkit.h"

#include <functional>
#include <string>
#include <vector>

namespace AK {

struct MainMenuItem {
    std::string title;
    std::string key;          // NSKeyEquiv: "o", "S" (Shift implied), "" (Page Up)...
    unsigned modifiers = 0;   // AK::Modifier
    std::string action;       // its selector, "openDocument:"
    int tag = 0;
    bool separator = false, alternate = false, hidden = false;
    std::vector<MainMenuItem> items;   // a submenu's
    bool hasSubmenu = false;
};

// -validateMenuItem:'s answer for an item: enabled, and its state and title when they change
struct MenuValidation {
    bool enabled = false;
    bool checked = false;
    std::string title;   // empty: the nib's
};

class MainMenu {
public:
    // MainMenu.nib's keyedobjects.nib (or the .nib folder)
    bool load(const std::string &nib);
    const std::vector<MainMenuItem> &menus() const { return m_menus; }
    // ...to change before publishing them (items the application's code adds, menus it removes)
    std::vector<MainMenuItem> &menus() { return m_menus; }

    // Enabled or not (an action the application doesn't know: disabled); may set its check and title
    std::function<bool(const MainMenuItem &, MenuValidation &)> validate;
    std::function<void(const MainMenuItem &)> perform;

    // Listens for the menu bar (once the application is open)
    void publish();
    // A key typed in a window: the item with it as its key equivalent, when enabled, performed
    bool performKeyEquivalent(const Event &e);
    // The key equivalent as the menus draw it ("⌥⌘H")
    static std::string keyLabel(const MainMenuItem &item);

    // -[NSApplication showHelp:]: the bundle's help book (Info.plist's CFBundleHelpBookName), else its
    // anchor in Mac Help (CFAppleHelpAnchor), in Help Viewer. The menus' "showHelp:" item does it by
    // itself; false when the bundle has no help
    bool showHelp() const;
    bool hasHelp() const;

private:
    void answer(int fd);
    std::string m_bundle;   // the .app the nib is in (its Info.plist names its help)
    void describe(const std::vector<MainMenuItem> &items, int depth, const std::string &path, std::string &out);
    const MainMenuItem *at(const std::string &path) const;
    const MainMenuItem *forKey(const std::vector<MainMenuItem> &items, const Event &e) const;
    std::vector<MainMenuItem> m_menus;
    int m_listener = -1;
};

} // namespace AK
