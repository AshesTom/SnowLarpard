// SPDX-License-Identifier: LGPL-2.1-or-later
// WWAN.menu's code (AppleWWANExtra, WWANExtraView) on AquaKit's SystemUIPlugin, loaded by
// SystemUIServer as on the Mac. Reversed from 10.6's WWAN.menu 2.0, x86_64, with REA
// (docs/reverse-status.md, section 36):
//
//   - -initWithBundle: (0x1fdb): the view 22 x 16; the modems (IOKit's WWAN services); none and the
//     extra isn't made; -loadImages (0x1970): the signal's pictures, not connected (Off, 0Disc to
//     5Disc) and connected (Off, 0 to 5);
//   - -updateImage (0x171c): NoSim with no SIM or a locked one, 5Disc while initializing, else the
//     bars (-convertRSSIToNBars: 0x16af: -77 dBm or more 5, -86 4, -92 3, -101 2, -108 1, below 0;
//     no service or no reading Off) among the connected or the disconnected pictures;
//   - -menu (0x2504): with Option "RSSI: %d dBm"; a GSM modem without a SIM "No SIM" alone; a
//     locked SIM "SIM PIN Locked" and "Unlock SIM"; initializing "Initializing…" alone; else the
//     network's name ("Unknown"), " ", its technology (2G, 2.5G, 3G by the service state) and the
//     connection (": Disconnected", ": Connecting…", ": Connected", ": Disconnecting…", ": Not
//     Configured"), disabled; "Turn WWAN Off"; a line and Connect / Disconnect (Connecting and
//     Disconnecting while they happen); a line, "About WWAN…", "Open Network Preferences…"; with
//     several modems a line and one item each, the active one checked.
//
// On Linux: ModemManager's modems (mmcli), the signal from its quality (3GPP's CSQ scale, -113 to -51
// dBm), the SIM's lock from unlock-required, the network from operator-name and the access
// technologies; the connection is NetworkManager's mobile broadband connection (nmcli), "Turn WWAN
// Off" its WWAN radio. Read every five seconds; the extra unloads without a modem.
#include "systemuiplugin.h"

#include "foundation.h"
#include "panels.h"
#include "runloop.h"

#include <cmath>
#include <cstdio>
#include <map>

using namespace AK;

namespace {

std::string run(const std::string &command)
{
    std::string out;
    FILE *p = popen((command + " 2>/dev/null").c_str(), "r");
    if (!p)
        return out;
    char buf[1024];
    while (fgets(buf, sizeof buf, p))
        out += buf;
    pclose(p);
    return out;
}

// mmcli -K's "key : value" lines
std::map<std::string, std::string> keyValues(const std::string &text)
{
    std::map<std::string, std::string> out;
    size_t from = 0;
    while (from < text.size()) {
        size_t end = text.find('\n', from);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = text.substr(from, end - from);
        const size_t colon = line.find(" : ");
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon), v = line.substr(colon + 3);
            while (!k.empty() && k.back() == ' ')
                k.pop_back();
            while (!v.empty() && v.front() == ' ')
                v.erase(0, 1);
            out[k] = v == "--" ? std::string() : v;
        }
        from = end + 1;
    }
    return out;
}

struct Modem {
    std::string path, name;           // /org/freedesktop/ModemManager1/Modem/0, "Vendor Model"
    std::map<std::string, std::string> info;
};

std::vector<Modem> modems()
{
    std::vector<Modem> out;
    const auto list = keyValues(run("mmcli -L -K"));
    for (const auto &[k, v] : list)
        if (k.rfind("modem-list.value", 0) == 0 && !v.empty()) {
            Modem m;
            m.path = v;
            m.info = keyValues(run("mmcli -m " + v + " -K"));
            m.name = m.info["modem.generic.manufacturer"] + " " + m.info["modem.generic.model"];
            out.push_back(m);
        }
    return out;
}

// -convertRSSIToNBars:
int bars(int rssi, int serviceState)
{
    if (serviceState < 1 || rssi == 0)
        return -1;
    if (rssi >= -77)
        return 5;
    if (rssi >= -86)
        return 4;
    if (rssi >= -92)
        return 3;
    if (rssi >= -101)
        return 2;
    return rssi >= -108 ? 1 : 0;
}

} // namespace

class AppleWWANExtra : public SystemUI::MenuExtra {
public:
    explicit AppleWWANExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        // -loadImages: Off, then 0 to 5 bars
        m_disconnected.push_back(templateImage("WWANSignal5BarsOff.pdf"));
        m_connected.push_back(templateImage("WWANSignal5BarsOff.pdf"));
        for (int i = 0; i <= 5; ++i) {
            m_disconnected.push_back(templateImage("WWANSignal5Bars" + std::to_string(i) + "Disc.pdf"));
            m_connected.push_back(templateImage("WWANSignal5Bars" + std::to_string(i) + ".pdf"));
        }
        m_noSim = templateImage("WWANSignal5BarsNoSim.pdf");
        refresh();
        m_poll = App::shared().addTimer(5, true, [this] {
            refresh();
            if (m_modems.empty())
                unload();   // no WWAN service left
        });
    }
    ~AppleWWANExtra() override { App::shared().removeTimer(m_poll); }

    std::vector<MenuItem> menu() override
    {
        refresh();
        std::vector<MenuItem> m;
        auto add = [&](const std::string &title, bool enabled, std::function<void()> action = {}) {
            MenuItem item{title};
            item.enabled = enabled;
            item.action = std::move(action);
            m.push_back(item);
        };
        if (modifierFlags() & Option)
            add("RSSI: " + std::to_string(m_rssi) + " dBm", false);
        if (m_lock == "NO SIM" && m_gsm) {
            add("No SIM", false);
            return m;
        }
        if (m_lock == "LOCKED" && m_gsm) {
            add("SIM PIN Locked", false);
            // -handleSIMLocking: SIMPanel.nib's PIN sheet; here the Network preferences
            add("Unlock SIM", true, [] { openPreferencePane("network"); });
            return m;
        }
        if (m_initializing) {
            add("Initializing…", false);
            return m;
        }
        static const std::map<int, const char *> suffix = {
            {Disconnected, ": Disconnected"}, {Connecting, ": Connecting…"}, {Connected, ": Connected"},
            {Disconnecting, ": Disconnecting…"}, {NotConfigured, ": Not Configured"}};
        add((m_network.empty() ? std::string("Unknown") : m_network) + " " + m_technology + suffix.at(m_ppp), false);
        add("Turn WWAN Off", true, [] { Task::launchDetached({"nmcli", "radio", "wwan", "off"}); });   // -turnCardOff:
        m.push_back(MenuItem::line());
        const std::string uuid = m_connection;
        switch (m_ppp) {
        case Disconnected: add("Connect", true, [uuid] { Task::launchDetached({"nmcli", "connection", "up", "uuid", uuid}); }); break;
        case Connecting: add("Connecting", true); break;
        case Connected: add("Disconnect", true, [uuid] { Task::launchDetached({"nmcli", "connection", "down", "uuid", uuid}); }); break;
        case Disconnecting: add("Disconnecting", true); break;
        default: add("PPPNotConfigured", false); break;
        }
        m.push_back(MenuItem::line());
        add("About WWAN…", true, [this] { aboutBox(); });
        add("Open Network Preferences…", true, [] { openPreferencePane("network"); });
        if (m_modems.size() > 1) {
            m.push_back(MenuItem::line());
            for (size_t i = 0; i < m_modems.size(); ++i) {
                MenuItem item{m_modems[i].name};
                item.checked = i == m_active;
                item.action = [this, i] {   // -setActiveDevice:
                    m_active = i;
                    refresh();
                };
                m.push_back(item);
            }
        }
        return m;
    }

private:
    enum { NotConfigured = -1, Disconnected = 0, Connecting = 1, Connected = 2, Disconnecting = 3 };

    void refresh()
    {
        m_modems = modems();
        if (m_active >= m_modems.size())
            m_active = 0;
        m_lock.clear();
        m_initializing = false;
        m_rssi = 0;
        int serviceState = 0;
        if (!m_modems.empty()) {
            auto &info = m_modems[m_active].info;
            const std::string state = info["modem.generic.state"];
            const std::string failed = info["modem.generic.state-failed-reason"];
            const std::string unlock = info["modem.generic.unlock-required"];
            m_gsm = info["modem.generic.current-capabilities.value[1]"].find("gsm") != std::string::npos ||
                    !info["modem.3gpp.imei"].empty();
            if (failed == "sim-missing")
                m_lock = "NO SIM";
            else if (state == "locked" || (!unlock.empty() && unlock != "none"))
                m_lock = "LOCKED";
            else
                m_lock = "UNLOCKED";
            m_initializing = state == "initializing" || state == "unknown";
            m_network = info["modem.3gpp.operator-name"];
            // the service state's technology
            const std::string tech = info["modem.generic.access-technologies.value[1]"];
            if (tech == "gsm" || tech == "gsm-compact" || tech == "gprs") {
                m_technology = "2G";
                serviceState = 2;
            } else if (tech == "edge") {
                m_technology = "2.5G";
                serviceState = 3;
            } else if (!tech.empty() && tech != "unknown") {
                m_technology = tech.find("lte") == std::string::npos && tech.find("5gnr") == std::string::npos ? "3G" : "";
                serviceState = 4;
            } else {
                m_technology.clear();
            }
            // ModemManager's quality is the CSQ's 0…31 as a percentage: -113 + 2 dBm a step
            const std::string q = info["modem.generic.signal-quality.value"];
            if (!q.empty() && state != "disabled")
                m_rssi = int(std::lround(-113 + std::stod(q) * 62 / 100));
        }
        // the mobile broadband connection (the PPP connection's place)
        m_ppp = NotConfigured;
        m_connection.clear();
        std::map<std::string, std::string> active;
        for (const std::string &line : lines(run("nmcli -t -f UUID,STATE connection show --active")))
            if (const size_t c = line.find(':'); c != std::string::npos)
                active[line.substr(0, c)] = line.substr(c + 1);
        for (const std::string &line : lines(run("nmcli -t -f UUID,TYPE connection show"))) {
            const size_t c = line.find(':');
            if (c == std::string::npos)
                continue;
            const std::string type = line.substr(c + 1);
            if (type != "gsm" && type != "cdma")
                continue;
            m_connection = line.substr(0, c);
            const auto a = active.find(m_connection);
            const std::string st = a == active.end() ? std::string() : a->second;
            m_ppp = st == "activated" ? Connected : st == "activating" ? Connecting : st == "deactivating" ? Disconnecting : Disconnected;
            if (m_ppp != Disconnected)
                break;
        }
        // -updateImage
        if (m_lock == "NO SIM" || m_lock == "LOCKED") {
            setImage(m_noSim);
        } else if (m_initializing) {
            setImage(m_disconnected[6]);   // 5Disc
        } else {
            const int b = bars(m_rssi, serviceState);
            setImage((m_ppp == Connected ? m_connected : m_disconnected)[size_t(b + 1)]);
        }
        setAlternateImage(image().isNull() ? Image() : image().tinted(Color::gray(255)));
    }

    static std::vector<std::string> lines(const std::string &text)
    {
        std::vector<std::string> out;
        size_t from = 0;
        while (from < text.size()) {
            size_t end = text.find('\n', from);
            if (end == std::string::npos)
                end = text.size();
            if (end > from)
                out.push_back(text.substr(from, end - from));
            from = end + 1;
        }
        return out;
    }

    // -aboutBoxSelected: AboutBox.nib's driver and device versions (AboutVerizonController)
    void aboutBox()
    {
        if (m_modems.empty())
            return;
        auto &info = m_modems[m_active].info;
        std::string text;
        for (const auto &[label, key] : std::vector<std::pair<const char *, const char *>>{
                 {"Manufacturer", "modem.generic.manufacturer"}, {"Model", "modem.generic.model"},
                 {"Firmware", "modem.generic.revision"}, {"Hardware", "modem.generic.hardware-revision"},
                 {"IMEI / ESN", "modem.generic.equipment-identifier"}, {"Phone number", "modem.generic.own-numbers.value[1]"}})
            if (!info[key].empty())
                text += std::string(label) + ": " + info[key] + "\n";
        Alert::run(templateImage("WWAN5-64.png"), "About WWAN", text, {"OK"}, [](int) {});
    }

    std::vector<Image> m_disconnected, m_connected;
    Image m_noSim;
    std::vector<Modem> m_modems;
    size_t m_active = 0;
    std::string m_lock, m_network, m_technology, m_connection;
    bool m_initializing = false, m_gsm = true;
    int m_rssi = 0, m_ppp = NotConfigured;
    int m_poll = 0;
};

SYSTEMUI_REGISTER_EXTRA("AppleWWANExtra", AppleWWANExtra);
