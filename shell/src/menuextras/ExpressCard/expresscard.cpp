// SPDX-License-Identifier: LGPL-2.1-or-later
// ExpressCard.menu's code (AppleExpressCardExtra, ExpressCardExtraLib) on AquaKit's SystemUIPlugin,
// loaded by SystemUIServer as on the Mac. Reversed from 10.6's ExpressCard.menu 2.0, x86_64, with REA
// (docs/reverse-status.md, section 35):
//
//   - -initWithBundle: (0x12bb): no ExpressCard controller (ExpressCardGetControllerCount) and the
//     extra isn't made; ExpressCard.pdf as a template; the strings "Power off Card", "Unknown Card
//     Type"; notifications of cards arriving and leaving (IOPCIDevice with IOPCIEjectable, FUN_1e38,
//     FUN_1d78): the sockets made again, the extra unloaded when none is left (FUN_0000103d);
//   - -createSocketNames (0x155c): a card's first function only (FunctionNumber 0); its names from
//     VersionOneInfo (its manufacturer, then its product, trimmed), else its class code's
//     description (-getCardDescriptionWithClassCode: 0x1412: "ClassCode%02x%02x%02x", then
//     "ClassCode%02x%02x", then "ClassCode%02x" in Localizable.strings, else "Unknown Card Type");
//     a PCI-to-PCI bridge (0x0604xx) noted;
//   - -menu (0x18a3): each card, bridges only when nothing else is there, a line between them: its
//     manufacturer and its product (disabled), "Power off Card" (-_powerOffMenuHit:,
//     ExpressCardPowerOffCard).
//
// On Linux: the PCI Express hot-plug slots holding a card (/sys/bus/pci/slots/*, adapter 1), their
// function 0's class (/sys/bus/pci/devices/*/class) and names (lspci -mm, the PCI ID database:
// vendor and device standing for VersionOneInfo's); powered off by the slot's power file, through
// pkexec when it needs root. Read every two seconds.
#include "systemuiplugin.h"

#include "foundation.h"
#include "runloop.h"

#include <dirent.h>

#include <cctype>
#include <cstdio>
#include <fstream>

using namespace AK;

namespace {

struct Socket {
    std::string slot, device;   // /sys/bus/pci/slots/N, its function 0 (0000:01:00.0)
    std::string maker, product;
    bool bridge = false;
};

std::string firstLine(const std::string &file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

// lspci -mm's quoted fields: slot, class, vendor, device…
std::vector<std::string> lspci(const std::string &device)
{
    std::vector<std::string> out;
    FILE *p = popen(("lspci -mm -s " + device + " 2>/dev/null").c_str(), "r");
    if (!p)
        return out;
    char buf[1024];
    std::string line = fgets(buf, sizeof buf, p) ? buf : "";
    pclose(p);
    for (size_t i = 0; i < line.size();) {
        if (line[i] == '"') {
            const size_t end = line.find('"', i + 1);
            out.push_back(line.substr(i + 1, end - i - 1));
            i = end == std::string::npos ? line.size() : end + 1;
        } else if (line[i] == ' ' || line[i] == '\n') {
            ++i;
        } else {
            const size_t end = line.find(' ', i);
            out.push_back(line.substr(i, end - i));
            i = end == std::string::npos ? line.size() : end;
        }
    }
    return out;
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front()))
        s.erase(0, 1);
    return s;
}

} // namespace

class AppleExpressCardExtra : public SystemUI::MenuExtra {
public:
    explicit AppleExpressCardExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        setImage(templateImage("ExpressCard.pdf"));
        setAlternateImage(templateImage("ExpressCard.pdf", true));
        createSocketNames();
        // the cards' arrival and removal
        m_poll = App::shared().addTimer(2, true, [this] {
            createSocketNames();
            if (m_sockets.empty())
                unload();   // FUN_0000103d: no function left
        });
    }
    ~AppleExpressCardExtra() override { App::shared().removeTimer(m_poll); }

    std::vector<MenuItem> menu() override
    {
        createSocketNames();
        std::vector<MenuItem> m;
        size_t bridges = 0;
        for (const Socket &s : m_sockets)
            bridges += s.bridge;
        bool first = true;
        for (const Socket &s : m_sockets) {
            if (s.bridge && bridges < m_sockets.size())
                continue;
            if (!first)
                m.push_back(MenuItem::line());
            first = false;
            for (const std::string &name : {s.maker, s.product})
                if (!name.empty()) {
                    MenuItem item{name};
                    item.enabled = false;
                    m.push_back(item);
                }
            MenuItem off{localized("Power off Card", "Power off Card")};
            const std::string slot = s.slot;
            off.action = [slot] { powerOff(slot); };
            m.push_back(off);
        }
        return m;
    }

private:
    // -createSocketNames
    void createSocketNames()
    {
        m_sockets.clear();
        DIR *d = opendir("/sys/bus/pci/slots");
        if (!d)
            return;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.')
                continue;
            const std::string slot = std::string("/sys/bus/pci/slots/") + e->d_name;
            if (firstLine(slot + "/adapter") != "1")
                continue;
            Socket s;
            s.slot = slot;
            s.device = firstLine(slot + "/address") + ".0";   // FunctionNumber 0
            const std::string cls = firstLine("/sys/bus/pci/devices/" + s.device + "/class");
            const unsigned code = cls.empty() ? 0 : unsigned(std::stoul(cls, nullptr, 16));
            s.bridge = (code & 0xffff00) == 0x060400;
            const std::vector<std::string> f = lspci(s.device);
            if (f.size() >= 4) {
                s.maker = trimmed(f[2]);
                s.product = trimmed(f[3]);
            }
            if (s.product.empty())
                s.product = description(code);
            m_sockets.push_back(s);
        }
        closedir(d);
    }

    // -getCardDescriptionWithClassCode:
    std::string description(unsigned code) const
    {
        char key[32];
        std::snprintf(key, sizeof key, "ClassCode%02x%02x%02x", code >> 16 & 0xff, code >> 8 & 0xff, code & 0xff);
        std::string s = localized(key, "");
        if (s.empty()) {
            std::snprintf(key, sizeof key, "ClassCode%02x%02x", code >> 16 & 0xff, code >> 8 & 0xff);
            s = localized(key, "");
        }
        if (s.empty()) {
            std::snprintf(key, sizeof key, "ClassCode%02x", code >> 16 & 0xff);
            s = localized(key, "");
        }
        return s.empty() ? localized("Unknown Card Type", "Unknown Card Type") : s;
    }

    // ExpressCardPowerOffCard: the slot's power off
    static void powerOff(const std::string &slot)
    {
        const std::string file = slot + "/power";
        std::ofstream out(file);
        if (out && (out << "0" << std::flush))
            return;
        Task::launchDetached({"pkexec", "sh", "-c", "echo 0 > \"$1\"", "sh", file});
    }

    std::vector<Socket> m_sockets;
    int m_poll = 0;
};

SYSTEMUI_REGISTER_EXTRA("AppleExpressCardExtra", AppleExpressCardExtra);
