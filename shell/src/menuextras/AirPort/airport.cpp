// SPDX-License-Identifier: LGPL-2.1-or-later
#include "airport.h"

#include "coreui.h"
#include "dbus.h"
#include "foundation.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <regex>
#include <strings.h>

using namespace AK;

namespace {

const char *const NM = "org.freedesktop.NetworkManager";

std::vector<std::string> split(const std::string &s, const std::string &sep)
{
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t at; (at = s.find(sep, from)) != std::string::npos; from = at + sep.size())
        out.push_back(s.substr(from, at - from));
    out.push_back(s.substr(from));
    return out;
}

std::vector<std::string> lines(const std::string &s)
{
    std::vector<std::string> out;
    for (const std::string &l : split(s, "\n"))
        if (!l.empty())
            out.push_back(l);
    return out;
}

std::string trim(std::string s)
{
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front()))
        s.erase(0, 1);
    return s;
}

// nmcli's terse output: fields split on ':', '\:' and '\\' escaped
std::vector<std::string> terseFields(const std::string &line)
{
    std::vector<std::string> out{std::string()};
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size())
            out.back() += line[++i];
        else if (line[i] == ':')
            out.emplace_back();
        else
            out.back() += line[i];
    }
    return out;
}

std::string format(std::string f, const std::string &v)
{
    const size_t at = f.find("%@");
    if (at != std::string::npos)
        f.replace(at, 2, v);
    return f;
}

std::string localizedSecurity(const std::string &nm)
{
    // -getLocalizedSecurityTypeForNetwork:, in 10.6's words
    auto has = [&](const char *s) { return nm.find(s) != std::string::npos; };
    if (nm.empty() || nm == "--")
        return "None";
    if (has("802.1X"))
        return has("WPA2") || has("WPA3") ? "WPA2 Enterprise" : "WPA Enterprise";
    if (has("WPA2") || has("WPA3"))
        return "WPA2 Personal";
    if (has("WPA"))
        return "WPA Personal";
    if (has("WEP"))
        return "WEP";
    return nm;
}

// NSProgressIndicator's small spinning style (16 points): twelve spokes, the leading one darkest
Image spinner(int step)
{
    Image img(16, 16);
    cairo_t *cr = cairo_create(img.surface());
    cairo_translate(cr, 8, 8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 1.6);
    for (int i = 0; i < 12; ++i) {
        const int age = (step - i + 12) % 12;
        cairo_set_source_rgba(cr, 0, 0, 0, std::max(40, 230 - age * 20) / 255.0);
        const double a = i * M_PI / 6;
        cairo_move_to(cr, 3.4 * std::sin(a), -3.4 * std::cos(a));
        cairo_line_to(cr, 6.6 * std::sin(a), -6.6 * std::cos(a));
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    return img;
}

// kAirPortConnectionFailed*: NMActiveConnectionStateReason
std::string failure(uint32_t reason)
{
    switch (reason) {
    case 9:
    case 10:
        return "Invalid password";
    case 6:
        return "Connection timeout";
    default:
        return "Connection failed";
    }
}

std::string percentDecode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

class AirPortView : public SystemUI::MenuExtraView {
public:
    explicit AirPortView(AirPortExtra *e) : MenuExtraView(e), airport(e) {}
    AirPortExtra *airport;
    void draw(cairo_t *cr) override
    {
        const Image img = airport->statusImage(airport->isMenuDown());
        if (!img.isNull())
            img.draw(cr, Point{std::floor((frame().w - img.width()) / 2), std::floor((frame().h - 1 - img.height()) / 2)});
    }
};

int onStateChanged(sd_bus_message *m, void *userdata, sd_bus_error *)
{
    uint32_t state = 0, reason = 0;
    if (sd_bus_message_read(m, "uu", &state, &reason) >= 0)
        static_cast<AirPortExtra *>(userdata)->joinStateChanged(state, reason);
    return 0;
}

int onAdded(sd_bus_message *m, void *userdata, sd_bus_error *)
{
    auto *a = static_cast<AirPortExtra *>(userdata);
    if (sd_bus_message_is_method_error(m, nullptr)) {
        a->panelFailed("Connection failed");
        return 0;
    }
    const char *settings = nullptr, *active = nullptr;
    if (sd_bus_message_read(m, "oo", &settings, &active) < 0) {
        a->panelFailed("Connection failed");
        return 0;
    }
    a->joinAdded(settings ? settings : "", active ? active : "");
    return 0;
}

// a{sv} entries
void str(sd_bus_message *m, const char *key, const std::string &v)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_append(m, "v", "s", v.c_str());
    sd_bus_message_close_container(m);
}
void boolean(sd_bus_message *m, const char *key, bool v)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_append(m, "v", "b", int(v));
    sd_bus_message_close_container(m);
}
void u32(sd_bus_message *m, const char *key, uint32_t v)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_append(m, "v", "u", v);
    sd_bus_message_close_container(m);
}
void bytes(sd_bus_message *m, const char *key, const std::string &v)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "ay");
    sd_bus_message_append_array(m, 'y', v.data(), v.size());
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}
void strv(sd_bus_message *m, const char *key, const std::vector<std::string> &v)
{
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", key);
    sd_bus_message_open_container(m, 'v', "as");
    sd_bus_message_open_container(m, 'a', "s");
    for (const std::string &s : v)
        sd_bus_message_append(m, "s", s.c_str());
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}
// one setting group {s a{sv}}
template <typename F> void group(sd_bus_message *m, const char *name, F fill)
{
    sd_bus_message_open_container(m, 'e', "sa{sv}");
    sd_bus_message_append(m, "s", name);
    sd_bus_message_open_container(m, 'a', "{sv}");
    fill();
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
}

} // namespace

// ---- what AME_AirPortNetworksOrganizer computes

int AirPortExtra::barsForSignal(int rssi)
{
    // +barsForSignal: (0xdbaf): the bars over {0, 20, 40, 60, 80} that the signal (RSSI + 100) passes
    static const int table[5] = {0, 20, 40, 60, 80};
    const int s = rssi < 1 ? rssi + 100 : rssi;
    int i = 0;
    while (table[i] < s && i != 4)
        ++i;
    return i;
}

int AirPortExtra::barsForMappedTransmitRate(int rate)
{
    // +barsForMappedTransmitRate: the rate, 54 Mbit/s at most, over {0, 1, 5, 12, 36}
    static const int table[5] = {0, 1, 5, 12, 36};
    const int r = std::min(rate, 54);
    int i = 0;
    while (table[i] < r && i != 4)
        ++i;
    return i;
}

int AirPortExtra::rssiForQuality(int quality)
{
    // NetworkManager's quality is the RSSI from -100 to -40 dBm, as 0 to 100
    if (quality <= 0)
        return 0;
    return quality >= 100 ? -40 : int(std::lround(quality * 0.6 - 100));
}

// ---- the extra

AirPortExtra::AirPortExtra(const std::string &bundle) : MenuExtra(bundle)
{
    setView(new AirPortView(this));
    m_pollTimer = App::shared().addTimer(5, true, [this] { poll(false); });
    poll(false);
}

AirPortExtra::~AirPortExtra() { willUnload(); }

void AirPortExtra::willUnload()
{
    for (int *t : {&m_pollTimer, &m_animation, &m_spinner, &m_scanTimer, &m_joinTimeout})
        if (*t) {
            App::shared().removeTimer(*t);
            *t = 0;
        }
    sd_bus_slot_unref(m_joinSlot);
    m_joinSlot = nullptr;
    m_poll.reset();
    m_panel.reset();
}

void AirPortExtra::poll(bool rescan)
{
    if (m_poll && m_poll->isRunning())
        return;
    if (rescan)
        m_scanning = true;
    m_poll = std::make_unique<Task>();
    m_poll->onExit = [this, rescan](int code, const std::string &out) {
        if (code == 0 || !out.empty())
            parse(out);
        if (rescan)
            m_scanning = false;
        // a scan's results with the menu down: its networks in place (-updateMenu)
        if (isMenuDown() && Menu::isOpen()) {
            if (!m_scanning && m_spinner) {
                App::shared().removeTimer(m_spinner);
                m_spinner = 0;
                m_showScanning = false;
            }
            Menu::update(items());
        }
    };
    const std::string script =
        "nmcli -t -f TYPE,STATE,DEVICE device; echo @@; nmcli radio wifi; echo @@;"
        " nmcli -t -f CONNECTIVITY general; echo @@; nmcli -t -f NAME,TYPE connection show; echo @@;"
        " nmcli -t -e yes -f IN-USE,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY,BSSID device wifi list --rescan " +
        std::string(rescan ? "yes" : "no") +
        "; echo @@;"
        // the link as the driver has it: the RSSI in dBm, the rate, the MCS index
        " d=$(nmcli -t -f DEVICE,TYPE device | awk -F: '$2==\"wifi\"{print $1; exit}');"
        " [ -n \"$d\" ] && command -v iw >/dev/null && iw dev \"$d\" link 2>/dev/null";
    if (!m_poll->launch({"sh", "-c", script}))
        m_hardware = false;
}

void AirPortExtra::parse(const std::string &out)
{
    const std::vector<std::string> sections = split(out, "@@\n");
    auto section = [&](size_t i) { return i < sections.size() ? lines(sections[i]) : std::vector<std::string>(); };
    bool hardware = false, connecting = false;
    std::string device;
    for (const std::string &l : section(0)) {
        const auto f = terseFields(l);
        if (f.size() >= 3 && f[0] == "wifi") {
            hardware = true;
            device = f[2];
            connecting = f[1].rfind("connecting", 0) == 0;
            break;
        }
    }
    const auto radio = section(1);
    const bool power = !radio.empty() && trim(radio[0]) == "enabled";
    const auto conn = section(2);
    const std::string connectivity = conn.empty() ? "" : trim(conn[0]);
    std::vector<std::string> profiles;
    for (const std::string &l : section(3)) {
        const auto f = terseFields(l);
        if (f.size() >= 2 && f[1] == "802-11-wireless")
            profiles.push_back(f[0]);
    }
    std::vector<Network> networks;
    for (const std::string &l : section(4)) {
        const auto f = terseFields(l);
        if (f.size() < 9 || f[1].empty())
            continue;   // hidden networks: no name to show
        Network n;
        n.active = f[0] == "*";
        n.ssid = f[1];
        n.adhoc = f[2] == "Ad-Hoc";
        n.channel = std::atoi(f[3].c_str());
        n.frequency = std::atoi(f[4].c_str());
        n.rate = std::atoi(f[5].c_str());
        n.signal = std::atoi(f[6].c_str());
        n.security = f[7] == "--" ? "" : f[7];
        n.bssid = f[8];
        networks.push_back(n);
    }
    // iw's link: "signal: -45 dBm", "tx bitrate: 780.0 MBit/s VHT-MCS 9 80MHz VHT-NSS 2"
    m_link = Link();
    static const std::regex mcs(R"(\b(HE-|VHT-|EHT-)?MCS (\d+))");
    for (const std::string &raw : section(5)) {
        const std::string t = trim(raw);
        if (t.rfind("signal:", 0) == 0) {
            m_link.rssi = std::atoi(t.c_str() + 7);
        } else if (t.rfind("tx bitrate:", 0) == 0) {
            m_link.rate = int(std::atof(t.c_str() + 11));
            std::smatch m;
            if (std::regex_search(t, m, mcs)) {
                m_link.mcs = std::stoi(m[2]);
                const std::string kind = m[1];
                m_link.phy = kind == "EHT-" ? "802.11be" : kind == "HE-" ? "802.11ax" : kind == "VHT-" ? "802.11ac" : "802.11n";
            }
        }
    }
    // the joined network's rate, averaged over the last three samples (-averageTxRateWithNewSample:)
    auto active = std::find_if(networks.begin(), networks.end(), [](const Network &n) { return n.active; });
    if (active != networks.end()) {
        if (m_link.rssi)
            active->rssi = m_link.rssi;
        if (m_link.rate)
            active->rate = m_link.rate;
        const int sample = active->rate;
        const int count = 1 + (m_txSamples[1] > 0) + (m_txSamples[0] > 0);
        const int older = m_txSamples[1], old = m_txSamples[0];
        m_txSamples[2] = older, m_txSamples[1] = old, m_txSamples[0] = sample;
        const int avg = count == 2 ? std::max(sample, old) : count < 2 ? sample : (sample + old + older) / count;
        const int rssi = active->rssi ? active->rssi : rssiForQuality(active->signal);
        m_bars = rssi ? std::max(barsForSignal(rssi), barsForMappedTransmitRate(avg)) : 0;
    }
    m_hardware = hardware;
    m_device = device;
    m_powerOn = power;
    m_connectivity = connectivity;
    m_profiles = profiles;
    m_networks = networks;
    // -startScanningAnimation (0x1074e): AirPortScanning1, 2, 3, 4, 3, 2 at half a second each,
    // stopped after 30 seconds (-stopExtraImageAnimation)
    if (connecting && !m_connecting && !m_animation) {
        m_frame = 0;
        m_animationStart = now();
        m_animation = App::shared().addTimer(0.5, true, [this] {
            m_frame = (m_frame + 1) % 6;
            if (now() - m_animationStart >= 30) {
                App::shared().removeTimer(m_animation);
                m_animation = 0;
            }
            setNeedsDisplay();
        });
    } else if (!connecting && m_animation) {
        App::shared().removeTimer(m_animation);
        m_animation = 0;
    }
    m_connecting = connecting;
    setNeedsDisplay();
}

Image AirPortExtra::statusImage(bool white) const
{
    // -updateStatusImage: the warning, else the interface's state: off, joining, joined (its bars)
    const bool joined = std::any_of(m_networks.begin(), m_networks.end(), [](const Network &n) { return n.active; });
    std::string name;
    bool dimmed = false;
    if (!m_powerOn) {
        name = "AirPortOff";
        dimmed = !white;   // -disabledImageFromMyBundle:, the alternate image plain
    } else if (m_connecting && m_animation) {
        static const int frames[6] = {1, 2, 3, 4, 3, 2};
        name = "AirPortScanning" + std::to_string(frames[m_frame]);
    } else if (joined && (m_connectivity == "limited" || m_connectivity == "portal" || m_connectivity == "none")) {
        name = "AirPortWarning";
    } else if (joined) {
        name = "AirPort" + std::to_string(m_bars);
    } else {
        name = "AirPort0";
    }
    Image img = templateImage(name + ".pdf", white);
    if (dimmed && !img.isNull()) {
        Image half(img.width(), img.height());
        cairo_t *cr = cairo_create(half.surface());
        img.draw(cr, Point{0, 0}, 0.5);
        cairo_destroy(cr);
        return half;
    }
    return img;
}

// The lock and the bars at the right of a network's title (AME_AirPortNetworkItemView, 0x985d): 10
// points, AppKit's NSImageNameLockLockedTemplate (CoreUI's) in its 9 by 12, 6 more, the bars' 17 by 13
// (AirportInMenu0-4); both at 63% (fraction 0.63), white when selected
MenuItem AirPortExtra::networkItem(const Network &n, bool current, int bars)
{
    MenuItem item(n.ssid, false, true, current);
    Image trailing(10 + 9 + 6 + 17, 13);
    cairo_t *cr = cairo_create(trailing.surface());
    if (!n.security.empty()) {
        static const Image lock = CoreUI::named("LockLocked");
        if (!lock.isNull()) {
            const double w = std::min(9.0, double(lock.width())), h = std::min(12.0, double(lock.height()));
            lock.draw(cr, Rect{10 + (9 - w) / 2, (13 - h) / 2, w, h}, 0.63);
        }
    }
    const Image signal = templateImage("AirportInMenu" + std::to_string(std::clamp(bars, 0, 4)) + ".pdf");
    if (!signal.isNull())
        signal.draw(cr, Point{25, 0}, 0.63);
    cairo_destroy(cr);
    item.trailingImage = trailing;
    item.trailingTemplate = true;
    if (!current) {
        const Network copy = n;
        item.action = [this, copy] { join(copy); };
    }
    return item;
}

std::vector<MenuItem> AirPortExtra::menu()
{
    // -menu: the scan in the background, "Looking for Networks…" if it lasts 1.5 seconds
    m_showScanning = false;
    poll(true);
    if (m_scanTimer)
        App::shared().removeTimer(m_scanTimer);
    m_scanTimer = App::shared().addTimer(1.5, false, [this] {
        m_scanTimer = 0;
        // -setScanningStatus: (0x4285): the title, and the status view's indicator going
        if (m_scanning && m_powerOn && isMenuDown() && Menu::isOpen()) {
            m_showScanning = true;
            m_spinner = App::shared().addTimer(0.083, true, [this] {
                m_spin = (m_spin + 1) % 12;
                if (isMenuDown() && Menu::isOpen())
                    Menu::update(items());
            });
            Menu::update(items());
        }
    });
    return items();
}

void AirPortExtra::menuDidClose()
{
    for (int *t : {&m_spinner, &m_scanTimer})
        if (*t) {
            App::shared().removeTimer(*t);
            *t = 0;
        }
    m_showScanning = false;
}

std::vector<MenuItem> AirPortExtra::items()
{
    // AirPortExtraImplementation -menu (0x5284): the status, the power, then the networks (-updateMenu)
    std::vector<MenuItem> m;
    const std::string scanning = tr("kAirPortStatusScanningKey", "AirPort: Looking for Networks…");
    MenuItem status(m_powerOn ? (m_showScanning ? scanning : tr("kAirPortStatusOnKey", "AirPort: On")) : tr("kAirPortStatusOffKey", "AirPort: Off"),
                    false, false);
    if (m_showScanning)
        status.trailingImage = spinner(m_spin);
    m.push_back(status);
    const bool joined = std::any_of(m_networks.begin(), m_networks.end(), [](const Network &n) { return n.active; });
    if (m_powerOn && joined && (m_connectivity == "limited" || m_connectivity == "portal" || m_connectivity == "none")) {
        // the connection's warning, AlertTriangleSmall at its right (AirPortMenuBadgeView)
        MenuItem w(m_connectivity == "portal" ? tr("kAirPortCaptivePortal", "Requires Authentication…")
                                              : tr("kAirPortInternetUnavaliable", "No Internet Connection…"));
        w.trailingImage = templateImage("AlertTriangleSmall.pdf");
        w.action = [] { openPreferencePane("network"); };
        m.push_back(w);
    }
    MenuItem power(m_powerOn ? tr("kAirPortTurnOffKey", "Turn AirPort Off") : tr("kAirPortTurnOnKey", "Turn AirPort On"));
    power.action = [this] {
        Task::launchDetached({"nmcli", "radio", "wifi", m_powerOn ? "off" : "on"});
        m_powerOn = !m_powerOn;
        setNeedsDisplay();
        App::shared().addTimer(1.5, false, [this] { poll(false); });
    };
    m.push_back(power);
    m.push_back(MenuItem::line());
    if (m_powerOn) {
        networks(m);
        m.push_back(MenuItem::line());
        MenuItem other(tr("kAirPortOtherKey", "Join Other Network…"));
        other.action = [this] {
            // -handleJoinOtherNetwork: AirPortClientDialogJoinNetwork
            m_panelHidden = true;
            runPanel({"join"});
        };
        m.push_back(other);
        MenuItem create(tr("kAirPortCreateNetworkKey", "Create Network…"));
        create.action = [this] {
            // -handleCreateIBSSNetwork: AirPortClientDialogCreateIBSS
            m_panelHidden = false;
            runPanel({"create"});
        };
        m.push_back(create);
    }
    MenuItem prefs(tr("kAirPortOpenNetworkPrefsKey", "Open Network Preferences…"));
    prefs.action = [] { openPreferencePane("network"); };
    m.push_back(prefs);
    return m;
}

void AirPortExtra::networks(std::vector<MenuItem> &out)
{
    const bool option = modifierFlags() & Option;
    auto disabled = [&](const std::string &title) { out.emplace_back(title, false, false); };
    // the network joined, checked, then its details with the Option key, else "No network selected"
    auto current = std::find_if(m_networks.begin(), m_networks.end(), [](const Network &n) { return n.active && !n.adhoc; });
    if (current != m_networks.end()) {
        out.push_back(networkItem(*current, true, m_bars));
        if (option) {
            const Network &n = *current;
            auto line = [&](const std::string &text) {
                MenuItem i(text, false, false);
                i.indentation = 1;
                out.push_back(i);
            };
            std::string phy = m_link.phy;
            if (phy.empty())
                phy = n.frequency > 4900 ? (n.rate > 54 ? "802.11n" : "802.11a") : (n.rate > 54 ? "802.11n" : "802.11g");
            std::string bssid = n.bssid;
            std::transform(bssid.begin(), bssid.end(), bssid.begin(), ::tolower);
            line(format(tr("kAirPortPHYModeInfoKey", "PHY Mode: %@"), phy));
            line(format(tr("kAirPortBSSIDInfoKey", "BSSID: %@"), bssid));
            line(format(tr("kAirPortChannelInfoKey", "Channel: %@"),
                        std::to_string(n.channel) + (n.frequency > 4900 ? " (5 GHz)" : " (2.4 GHz)")));
            line(format(tr("kAirPortSecurityInfoKey", "Security: %@"), localizedSecurity(n.security)));
            line(format(tr("kAirPortSignalInfoKey", "RSSI: %@"), std::to_string(n.rssi ? n.rssi : rssiForQuality(n.signal))));
            line(format(tr("kAirPortRateInfoKey", "Transmit Rate: %@"), std::to_string(n.rate)));
            if (m_link.mcs >= 0)
                line(format(tr("kAirPortMCSIndexInfoKey", "MCS Index: %@"), std::to_string(m_link.mcs)));
        }
    } else {
        disabled(tr("kAirPortNoNetworkKey", "No network selected"));
    }
    // the networks found, by name, each name once (its strongest), the computer-to-computer ones
    // under Devices
    std::vector<Network> found;
    for (const Network &n : m_networks) {
        if (current != m_networks.end() && n.ssid == current->ssid && !n.adhoc)
            continue;
        auto same = std::find_if(found.begin(), found.end(), [&](const Network &o) { return o.ssid == n.ssid && o.adhoc == n.adhoc; });
        if (same == found.end())
            found.push_back(n);
        else if (n.signal > same->signal)
            *same = n;
    }
    std::sort(found.begin(), found.end(), [](const Network &a, const Network &b) { return strcasecmp(a.ssid.c_str(), b.ssid.c_str()) < 0; });
    for (const Network &n : found)
        if (!n.adhoc)
            out.push_back(networkItem(n, false, barsForSignal(rssiForQuality(n.signal))));
    bool devices = false;
    for (const Network &n : found) {
        if (!n.adhoc)
            continue;
        if (!devices) {
            out.push_back(MenuItem::line());
            disabled(tr("kGroupModeIBSSKey", "Devices"));
            devices = true;
        }
        out.push_back(networkItem(n, n.active, barsForSignal(rssiForQuality(n.signal))));
    }
}

// ---- joining: AirPort's panels (Apple80211's ClientDialogs), then NetworkManager's
// AddAndActivateConnection2 over D-Bus, the password never on a command line

void AirPortExtra::runPanel(const std::vector<std::string> &args)
{
    if (m_panel && m_panel->isRunning())
        return;   // one panel at a time, as ClientController shows them
    m_panel = std::make_unique<Task>();
    m_panel->keepInput = true;
    m_panel->errors = Task::Errors::Inherit;
    m_panelOutput.clear();
    m_panel->onLine = [this](const std::string &line) {
        if (line != "end") {
            m_panelOutput += line + "\n";
            return;
        }
        std::map<std::string, std::string> fields;
        for (const std::string &l : lines(m_panelOutput)) {
            const size_t eq = l.find('=');
            if (eq != std::string::npos && eq > 0)
                fields[l.substr(0, eq)] = percentDecode(l.substr(eq + 1));
        }
        m_panelOutput.clear();
        panelAccepted(fields);
    };
    // Apple80211.framework's ClientDialogs.bundle, among the private frameworks beside SystemUIServer.app
    const std::string apps = dirName(dirName(dirName(dirName(executablePath()))));
    std::vector<std::string> argv{apps + "/PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle/Contents/MacOS/ClientDialogs"};
    argv.insert(argv.end(), args.begin(), args.end());
    m_panel->launch(argv);
}

void AirPortExtra::panelAccepted(const std::map<std::string, std::string> &f)
{
    auto get = [&](const char *k, const std::string &fallback = {}) {
        auto it = f.find(k);
        return it == f.end() ? fallback : it->second;
    };
    sd_bus *bus = DBus::system();
    if (!bus) {
        panelFailed("Connection failed");
        return;
    }
    // the device's object
    sd_bus_message *reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    const char *devicePath = nullptr;
    std::string device;
    if (sd_bus_call_method(bus, NM, "/org/freedesktop/NetworkManager", NM, "GetDeviceByIpIface", &error, &reply, "s", m_device.c_str()) >= 0 &&
        sd_bus_message_read(reply, "o", &devicePath) >= 0 && devicePath)
        device = devicePath;
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    if (device.empty()) {
        panelFailed("Connection failed");
        return;
    }
    const std::string ssid = get("ssid");
    const int type = std::atoi(get("security", "0").c_str());
    const bool create = f.count("channel") > 0;
    const int channel = std::atoi(get("channel", "0").c_str());
    const bool hidden = !create && get("hidden", m_panelHidden ? "1" : "0") == "1";
    const bool remember = get("remember", "1") == "1";
    const std::string password = get("password"), username = get("username");

    sd_bus_message *m = nullptr;
    if (sd_bus_message_new_method_call(bus, &m, NM, "/org/freedesktop/NetworkManager", NM, "AddAndActivateConnection2") < 0) {
        panelFailed("Connection failed");
        return;
    }
    sd_bus_message_open_container(m, 'a', "{sa{sv}}");
    group(m, "connection", [&] {
        str(m, "id", ssid);
        str(m, "type", "802-11-wireless");
        if (create)
            boolean(m, "autoconnect", false);
    });
    group(m, "802-11-wireless", [&] {
        bytes(m, "ssid", ssid);
        str(m, "mode", create ? "ap" : "infrastructure");
        if (create && channel > 0) {
            // Create Network…: a computer-to-computer network; this computer shares its connection
            str(m, "band", channel > 14 ? "a" : "bg");
            u32(m, "channel", uint32_t(channel));
        }
        if (hidden)
            boolean(m, "hidden", true);   // a network named in Join Other: maybe one that doesn't broadcast
    });
    if (create) {
        group(m, "ipv4", [&] { str(m, "method", "shared"); });
        group(m, "ipv6", [&] { str(m, "method", "ignore"); });
    }
    // the security type's settings (the panels' tags: -getLocalizedNameOfSecurityType:)
    switch (type) {
    case 1:   // WEP Password: a passphrase
        group(m, "802-11-wireless-security", [&] {
            str(m, "key-mgmt", "none");
            str(m, "wep-key0", password);
            u32(m, "wep-key-type", 2);
        });
        break;
    case 2:
    case 3:   // WEP 40/128-bit hex or ASCII
        group(m, "802-11-wireless-security", [&] {
            str(m, "key-mgmt", "none");
            str(m, "wep-key0", password);
            u32(m, "wep-key-type", 1);
        });
        break;
    case 4:   // LEAP
        group(m, "802-11-wireless-security", [&] {
            str(m, "key-mgmt", "ieee8021x");
            str(m, "auth-alg", "leap");
            str(m, "leap-username", username);
            str(m, "leap-password", password);
        });
        break;
    case 5:
    case 7:
        group(m, "802-11-wireless-security", [&] {
            str(m, "key-mgmt", "wpa-psk");
            str(m, "psk", password);
        });
        break;
    case 6:
    case 8:
    case 9:   // Enterprise and 802.1X WEP: PEAP / MSCHAPv2, as 10.6's "Automatic" profile mostly was
        group(m, "802-11-wireless-security", [&] { str(m, "key-mgmt", type == 9 ? "ieee8021x" : "wpa-eap"); });
        group(m, "802-1x", [&] {
            strv(m, "eap", {"peap", "ttls"});
            str(m, "identity", username);
            str(m, "password", password);
            str(m, "phase2-auth", "mschapv2");
        });
        break;
    default:
        break;
    }
    sd_bus_message_close_container(m);
    sd_bus_message_append(m, "oo", device.c_str(), "/");
    // Remember this network: kept on disk; else for this time only
    sd_bus_message_open_container(m, 'a', "{sv}");
    str(m, "persist", remember ? "disk" : "volatile");
    sd_bus_message_close_container(m);
    sd_bus_call_async(bus, nullptr, m, onAdded, this, 30000000);
    sd_bus_message_unref(m);
    DBus::flush(bus);
}

void AirPortExtra::joinAdded(const std::string &settings, const std::string &active)
{
    m_joinSettings = settings;
    m_joinActive = active;
    sd_bus_slot_unref(m_joinSlot);
    m_joinSlot = nullptr;
    sd_bus_match_signal(DBus::system(), &m_joinSlot, NM, active.c_str(), "org.freedesktop.NetworkManager.Connection.Active",
                        "StateChanged", onStateChanged, this);
    m_joinTimeout = App::shared().addTimer(45, false, [this] {
        m_joinTimeout = 0;
        finishJoin("Connection timeout");
    });
}

void AirPortExtra::joinStateChanged(uint32_t state, uint32_t reason)
{
    // NM_ACTIVE_CONNECTION_STATE: 2 activated, 4 deactivated
    if (state == 2)
        App::shared().later([this] { finishJoin({}); });
    else if (state == 4)
        App::shared().later([this, reason] { finishJoin(failure(reason)); });
}

void AirPortExtra::finishJoin(const std::string &error)
{
    if (m_joinTimeout) {
        App::shared().removeTimer(m_joinTimeout);
        m_joinTimeout = 0;
    }
    sd_bus_slot_unref(m_joinSlot);
    m_joinSlot = nullptr;
    if (!error.empty() && !m_joinSettings.empty())
        // a network that couldn't be joined isn't remembered
        sd_bus_call_method_async(DBus::system(), nullptr, NM, m_joinSettings.c_str(), "org.freedesktop.NetworkManager.Settings.Connection",
                                 "Delete", nullptr, nullptr, "");
    m_joinActive.clear();
    m_joinSettings.clear();
    if (error.empty()) {
        if (m_panel)
            m_panel->write("done\n");
    } else {
        panelFailed(error);
    }
    poll(false);
}

void AirPortExtra::panelFailed(const std::string &error)
{
    if (m_panel)
        m_panel->write("error " + error + "\n");
}

void AirPortExtra::join(const Network &n)
{
    // -handleJoinSelectedNetwork: a network NetworkManager knows, or an open one: as it is
    if (std::find(m_profiles.begin(), m_profiles.end(), n.ssid) != m_profiles.end()) {
        Task::launchDetached({"nmcli", "connection", "up", "id", n.ssid});
        return;
    }
    if (n.security.empty()) {
        Task::launchDetached({"nmcli", "device", "wifi", "connect", n.ssid});
        return;
    }
    // else AirPortClientDialogPassword: the type as -showAuthViewForACSecurityType: has it
    auto has = [&](const char *s) { return n.security.find(s) != std::string::npos; };
    int type = 7;
    if (has("802.1X"))
        type = has("WPA2") || has("WPA3") ? 8 : 6;
    else if (has("WEP"))
        type = 1;
    else if (!has("WPA2") && !has("WPA3") && has("WPA"))
        type = 5;
    m_panelHidden = false;
    std::vector<std::string> args{"password", n.ssid, std::to_string(type)};
    if (modifierFlags() & Option)
        args.push_back("--option");
    runPanel(args);
}

SYSTEMUI_REGISTER_EXTRA("AirPortExtra", AirPortExtra);
