// SPDX-License-Identifier: LGPL-2.1-or-later
#include "mainmenu.h"

#include "archive.h"
#include "foundation.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace AK {

namespace {

// NSEventModifierFlags in a nib's NSKeyEquivModMask
constexpr long long NSShift = 1 << 17, NSControl = 1 << 18, NSAlternate = 1 << 19, NSCommand = 1 << 20;

bool isLetter(const std::string &k) { return k.size() == 1 && std::isalpha(static_cast<unsigned char>(k[0])); }

// The characters AppKit's NSFunctionKey range uses (NSLeftArrowFunctionKey...), as UTF-8
std::string utf8(unsigned cp)
{
    std::string s;
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
    return s;
}

// The key a keysym is in a nib's NSKeyEquiv terms
std::string keyOf(uint32_t keysym)
{
    switch (keysym) {
    case XKB_KEY_Up: return utf8(0xF700);
    case XKB_KEY_Down: return utf8(0xF701);
    case XKB_KEY_Left: return utf8(0xF702);
    case XKB_KEY_Right: return utf8(0xF703);
    case XKB_KEY_Page_Up: return utf8(0xF72C);
    case XKB_KEY_Page_Down: return utf8(0xF72D);
    case XKB_KEY_Home: return utf8(0xF729);
    case XKB_KEY_End: return utf8(0xF72B);
    case XKB_KEY_BackSpace: return "\x08";
    case XKB_KEY_Delete: return utf8(0xF728);
    case XKB_KEY_Return: return "\r";
    case XKB_KEY_Escape: return "\x1b";
    case XKB_KEY_KP_Add: return "+";
    case XKB_KEY_KP_Subtract: return "-";
    case XKB_KEY_KP_Multiply: return "*";
    default: break;
    }
    if (keysym >= 0x20 && keysym < 0x7F)
        return std::string(1, char(keysym));
    return {};
}

std::string socketName(int pid) { return "snowleopard-menus-" + std::to_string(pid); }

sockaddr_un address(const std::string &name, socklen_t &length)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t n = std::min(name.size(), sizeof addr.sun_path - 1);
    std::memcpy(addr.sun_path + 1, name.data(), n);   // sun_path[0] = 0: abstract (QLocalSocket's AbstractNamespaceOption)
    length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
    return addr;
}

std::string clean(std::string s)
{
    for (char &c : s)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return s;
}

} // namespace

bool MainMenu::load(const std::string &nib)
{
    Archive a;
    std::string path = nib;
    if (isDir(path))
        path += "/keyedobjects.nib";
    if (!a.load(path))
        return false;
    const int data = a.top("IB.objectdata");
    // the items' actions: their connectors to the first responder
    std::vector<std::pair<int, std::string>> actions;
    for (int c : a.array(data, "NSConnections"))
        if (a.isA(c, "NSNibControlConnector"))
            actions.emplace_back(a.object(c, "NSSource"), a.string(c, "NSLabel"));
    int main = -1;
    for (int o : a.array(data, "NSObjectsKeys"))
        if (a.isA(o, "NSMenu") && a.string(o, "NSName") == "_NSMainMenu")
            main = o;
    if (main < 0)
        return false;
    std::function<std::vector<MainMenuItem>(int)> items = [&](int menu) {
        std::vector<MainMenuItem> out;
        for (int i : a.array(menu, "NSMenuItems")) {
            MainMenuItem it;
            it.title = a.string(i, "NSTitle");
            it.key = a.string(i, "NSKeyEquiv");
            const long long mask = a.integer(i, "NSKeyEquivModMask");
            if (mask & NSCommand)
                it.modifiers |= Command;
            if (mask & NSShift)
                it.modifiers |= Shift;
            if (mask & NSAlternate)
                it.modifiers |= Option;
            if (mask & NSControl)
                it.modifiers |= Control;
            // an upper case letter is typed with Shift
            if (isLetter(it.key) && std::isupper(static_cast<unsigned char>(it.key[0])))
                it.modifiers |= Shift;
            it.tag = int(a.integer(i, "NSTag"));
            it.separator = a.boolean(i, "NSIsSeparator");
            it.alternate = a.boolean(i, "NSIsAlternate");
            it.hidden = a.boolean(i, "NSIsHidden");
            for (const auto &[source, action] : actions)
                if (source == i)
                    it.action = action;
            const int sub = a.object(i, "NSSubmenu");
            if (sub >= 0) {
                it.hasSubmenu = true;
                it.items = items(sub);
            }
            out.push_back(std::move(it));
        }
        return out;
    };
    m_menus = items(main);
    return !m_menus.empty();
}

std::string MainMenu::keyLabel(const MainMenuItem &item)
{
    if (item.key.empty())
        return {};
    std::string out;
    // the order the menus draw them in: Control, Option, Shift, Command
    if (item.modifiers & Control)
        out += "⌃";
    if (item.modifiers & Option)
        out += "⌥";
    if (item.modifiers & Shift)
        out += "⇧";
    if (item.modifiers & Command)
        out += "⌘";
    static const std::vector<std::pair<std::string, std::string>> special = {
        {utf8(0xF700), "↑"}, {utf8(0xF701), "↓"}, {utf8(0xF702), "←"}, {utf8(0xF703), "→"}, {utf8(0xF72C), "⇞"},
        {utf8(0xF72D), "⇟"}, {utf8(0xF729), "↖"}, {utf8(0xF72B), "↘"}, {utf8(0xF728), "⌦"}, {"\x08", "⌫"},
        {"\x7f", "⌫"},       {"\r", "↩"},         {"\x1b", "⎋"},       {" ", "Space"}};
    for (const auto &[k, label] : special)
        if (item.key == k)
            return out + label;
    std::string key = item.key;
    for (char &c : key)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    return out + key;
}

void MainMenu::describe(const std::vector<MainMenuItem> &items, int depth, const std::string &path, std::string &out)
{
    for (size_t i = 0; i < items.size(); ++i) {
        const MainMenuItem &it = items[i];
        if (it.alternate || it.hidden)
            continue;
        const std::string p = path.empty() ? std::to_string(i) : path + "." + std::to_string(i);
        if (it.separator) {
            out += std::to_string(depth) + "\tS\t0\t0\t\t\t" + p + "\t\n";
            continue;
        }
        MenuValidation v;
        bool enabled;
        if (it.hasSubmenu) {
            // a submenu's item is enabled (its items are validated), unless it is empty (Services)
            enabled = std::any_of(it.items.begin(), it.items.end(), [](const MainMenuItem &s) { return !s.separator && !s.hidden; });
        } else {
            enabled = !it.action.empty() && validate && validate(it, v);
        }
        out += std::to_string(depth) + "\t" + (it.hasSubmenu ? "M" : "I") + "\t" + (enabled ? "1" : "0") + "\t" + (v.checked ? "1" : "0") +
               "\t" + keyLabel(it) + "\t" + it.action + "\t" + p + "\t" + clean(v.title.empty() ? it.title : v.title) + "\n";
        if (it.hasSubmenu)
            describe(it.items, depth + 1, p, out);
    }
}

const MainMenuItem *MainMenu::at(const std::string &path) const
{
    const std::vector<MainMenuItem> *items = &m_menus;
    const MainMenuItem *found = nullptr;
    size_t from = 0;
    while (from <= path.size()) {
        size_t dot = path.find('.', from);
        if (dot == std::string::npos)
            dot = path.size();
        const std::string part = path.substr(from, dot - from);
        if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos)
            return nullptr;
        const size_t i = std::stoul(part);
        if (!items || i >= items->size())
            return nullptr;
        found = &(*items)[i];
        items = &found->items;
        from = dot + 1;
    }
    return found;
}

void MainMenu::publish()
{
    if (m_listener >= 0)
        return;
    socklen_t length = 0;
    const sockaddr_un addr = address(socketName(int(getpid())), length);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return;
    if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), length) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return;
    }
    m_listener = fd;
    App::shared().watch(fd, [this] {
        for (int c; (c = accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC)) >= 0;)
            answer(c);
    });
}

void MainMenu::answer(int fd)
{
    // the request is one short line, sent as soon as the menu bar connects
    std::string line;
    char buf[256];
    for (int tries = 0; tries < 50 && line.find('\n') == std::string::npos; ++tries) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0)
            line.append(buf, size_t(n));
        else if (n == 0)
            break;
        else
            usleep(2000);
    }
    const size_t nl = line.find('\n');
    if (nl != std::string::npos)
        line.resize(nl);
    if (line == "menus") {
        std::string out;
        describe(m_menus, 0, {}, out);
        const int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        for (size_t done = 0; done < out.size();) {
            const ssize_t n = write(fd, out.data() + done, out.size() - done);
            if (n <= 0)
                break;
            done += size_t(n);
        }
    } else if (line.rfind("do ", 0) == 0) {
        const std::string path = line.substr(3);
        // after the menu bar's menu has closed, as AppKit sends the action once the menu is gone
        App::shared().later([this, path] {
            const MainMenuItem *it = at(path);
            MenuValidation v;
            if (it && !it->action.empty() && perform && (!validate || validate(*it, v)))
                perform(*it);
        });
    }
    close(fd);
}

const MainMenuItem *MainMenu::forKey(const std::vector<MainMenuItem> &items, const Event &e) const
{
    const std::string key = keyOf(e.keysym);
    if (key.empty())
        return nullptr;
    // Command is the Super key, or Control (Linux's own shortcuts), as the views take it
    const bool command = e.modifiers & (Command | Control);
    const bool control = (e.modifiers & Control) && (e.modifiers & Command);
    for (const MainMenuItem &it : items) {
        if (it.hasSubmenu) {
            if (const MainMenuItem *found = forKey(it.items, e))
                return found;
            continue;
        }
        if (it.key.empty() || it.separator || it.action.empty())
            continue;
        if (bool(it.modifiers & Command) != command || bool(it.modifiers & Option) != bool(e.modifiers & Option))
            continue;
        if ((it.modifiers & Control) && !control)
            continue;
        if (isLetter(it.key)) {
            if (std::tolower(static_cast<unsigned char>(it.key[0])) != std::tolower(static_cast<unsigned char>(key[0])) || key.size() != 1)
                continue;
            if (bool(it.modifiers & Shift) != bool(e.modifiers & Shift))
                continue;
        } else {
            // a symbol is typed with Shift on some keyboards ("+"): only an item that asks for it requires it
            if (it.key != key)
                continue;
            if ((it.modifiers & Shift) && !(e.modifiers & Shift))
                continue;
        }
        return &it;
    }
    return nullptr;
}

bool MainMenu::performKeyEquivalent(const Event &e)
{
    const MainMenuItem *it = forKey(m_menus, e);
    if (!it)
        return false;
    MenuValidation v;
    if (validate && !validate(*it, v))
        return true;   // the item is there but disabled: the key does nothing (and isn't typed)
    if (perform)
        perform(*it);
    return true;
}

} // namespace AK
