// SPDX-License-Identifier: LGPL-2.1-or-later
#include "bluetooth.h"

#include "foundation.h"
#include "text.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>
#include <poll.h>
#include <regex>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace AK;

namespace {

std::vector<std::string> split(const std::string &s, const std::string &sep)
{
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t at; (at = s.find(sep, from)) != std::string::npos; from = at + sep.size())
        out.push_back(s.substr(from, at - from));
    out.push_back(s.substr(from));
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

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// "  Key: value" in bluetoothctl's output
std::string field(const std::string &text, const std::string &key)
{
    for (const std::string &l : split(text, "\n")) {
        const std::string t = trim(l);
        if (t.rfind(key + ":", 0) == 0)
            return trim(t.substr(key.size() + 1));
    }
    return {};
}

std::string format(std::string f, const std::string &v)
{
    const size_t at = f.find("%@");
    if (at != std::string::npos)
        f.replace(at, 2, v);
    return f;
}

void bluetoothctl(const std::vector<std::string> &args)
{
    std::vector<std::string> a{"bluetoothctl"};
    a.insert(a.end(), args.begin(), args.end());
    Task::launchDetached(a);
}

// IOBluetoothDevice -readRSSIForDevice: HCI_Read_RSSI (OGF 0x05, OCF 0x0005) on the connection's
// handle, through a raw HCI socket (the kernel lets anyone send that one); the handle from
// HCIGETCONNINFO. The BR/EDR value is relative to the golden receive range. INT_MIN when unread.
int readRSSI(const std::string &address, int dev = 0)
{
    constexpr int AfBluetooth = 31, BtprotoHci = 1, SolHci = 0, HciFilter = 2;
    constexpr unsigned long HciGetConnInfo = 0x800448d5;   // _IOR('H', 213, int)
    const std::vector<std::string> bytes = split(address, ":");
    if (bytes.size() != 6)
        return INT_MIN;
    const int s = socket(AfBluetooth, SOCK_RAW | SOCK_CLOEXEC, BtprotoHci);
    if (s < 0)
        return INT_MIN;
    struct {
        sa_family_t family;
        unsigned short dev, channel;
    } sa{AfBluetooth, static_cast<unsigned short>(dev), 0};
    int rssi = INT_MIN;
    if (bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0) {
        unsigned char req[8 + 16] = {};
        for (int i = 0; i < 6; ++i)
            req[i] = static_cast<unsigned char>(std::strtoul(bytes[size_t(5 - i)].c_str(), nullptr, 16));
        int handle = -1;
        for (unsigned char type : {0x01, 0x80}) {   // ACL, LE
            req[6] = type;
            if (ioctl(s, HciGetConnInfo, req) == 0) {
                handle = req[8] | (req[9] << 8);
                break;
            }
        }
        struct {
            uint32_t typeMask;
            uint32_t eventMask[2];
            uint16_t opcode;
        } filter{1u << 4, {1u << 0x0e, 0}, 0};   // events: Command Complete
        const uint16_t opcode = (0x05 << 10) | 0x0005;
        const unsigned char cmd[] = {0x01, uint8_t(opcode & 0xff), uint8_t(opcode >> 8), 2, uint8_t(handle & 0xff), uint8_t(handle >> 8)};
        if (handle >= 0 && setsockopt(s, SolHci, HciFilter, &filter, sizeof filter) == 0 && write(s, cmd, sizeof cmd) == ssize_t(sizeof cmd)) {
            const double start = now();
            while (now() - start < 0.5) {
                pollfd p{s, POLLIN, 0};
                if (poll(&p, 1, int((0.5 - (now() - start)) * 1000)) <= 0)
                    break;
                unsigned char ev[260];
                const ssize_t n = read(s, ev, sizeof ev);
                if (n >= 10 && ev[0] == 0x04 && ev[1] == 0x0e && (ev[4] | (ev[5] << 8)) == opcode) {
                    if (ev[6] == 0)
                        rssi = static_cast<int8_t>(ev[9]);
                    break;
                }
            }
        }
    }
    close(s);
    return rssi;
}

class BluetoothView : public SystemUI::MenuExtraView {
public:
    explicit BluetoothView(AppleBluetoothExtra *e) : MenuExtraView(e), bt(e) {}
    AppleBluetoothExtra *bt;
    void draw(cairo_t *cr) override
    {
        const Image img = bt->statusImage(bt->isMenuDown());
        if (!img.isNull())
            img.draw(cr, Point{std::floor((frame().w - img.width()) / 2), std::floor((frame().h - 1 - img.height()) / 2)});
    }
};

} // namespace

bool AppleBluetoothExtra::Device::has(const char *uuid16) const
{
    // the 16-bit UUIDs in the Bluetooth base UUID, as bluetoothctl lists them
    const std::string full = std::string("0000") + uuid16 + "-0000-1000-8000-00805f9b34fb";
    return std::any_of(uuids.begin(), uuids.end(), [&](const std::string &u) { return strcasecmp(u.c_str(), full.c_str()) == 0; });
}

AppleBluetoothExtra::AppleBluetoothExtra(const std::string &bundle) : MenuExtra(bundle)
{
    setView(new BluetoothView(this));
    m_timer = App::shared().addTimer(10, true, [this] { poll(); });
    poll();
}

AppleBluetoothExtra::~AppleBluetoothExtra() { willUnload(); }

void AppleBluetoothExtra::willUnload()
{
    if (m_timer)
        App::shared().removeTimer(m_timer);
    m_timer = 0;
    m_poll.reset();
}

void AppleBluetoothExtra::poll()
{
    if (m_poll && m_poll->isRunning())
        return;
    m_poll = std::make_unique<Task>();
    m_poll->onExit = [this](int, const std::string &out) {
        parse(out);
        if (isMenuDown() && Menu::isOpen())
            Menu::update(items());
    };
    // bluetoothctl waits for bluetoothd forever when it isn't there: a few seconds at most
    m_poll->launch({"sh", "-c",
                    "command -v bluetoothctl >/dev/null || exit 1; b='timeout 4 bluetoothctl';"
                    " $b show; echo @@; $b version; echo @@;"
                    // the PAN connections NetworkManager has (a bt device per phone)
                    " nmcli -t -f DEVICE,TYPE,STATE device 2>/dev/null; echo @@;"
                    " for d in $( { $b devices Paired; $b devices Connected; } 2>/dev/null | awk '/^Device/{print $2}' | sort -u);"
                    " do $b info \"$d\"; echo @@; done"});
}

void AppleBluetoothExtra::parse(const std::string &out)
{
    const std::vector<std::string> sections = split(out, "@@\n");
    const std::string show = sections.empty() ? "" : sections[0];
    static const std::regex controller(R"((^|\n)Controller ([0-9A-F:]{17}))"), device(R"((^|\n)Device ([0-9A-F:]{17}))"),
        rssiRe(R"(RSSI:.*\((-?\d+)\))"), batteryRe(R"(Battery Percentage:.*\((\d+)\))"), uuidRe(R"(UUID:.*\(([0-9a-fA-F-]{36})\))");
    std::smatch m;
    m_controller = std::regex_search(show, m, controller);
    m_address = m_controller ? m[2].str() : "";
    m_name = field(show, "Alias");
    if (m_name.empty())
        m_name = field(show, "Name");
    m_powered = field(show, "Powered") == "yes";
    m_discoverable = field(show, "Discoverable") == "yes";
    m_version = sections.size() > 1 ? trim(sections[1]) : "";
    if (m_version.rfind(' ') != std::string::npos)
        m_version = m_version.substr(m_version.rfind(' ') + 1);
    m_pan.clear();
    if (sections.size() > 2)
        for (std::string line : split(sections[2], "\n")) {
            // "80\:99\:E7...:bt:connected"
            for (size_t p; (p = line.find("\\:")) != std::string::npos;)
                line.replace(p, 2, "-");
            const std::vector<std::string> f = split(line, ":");
            if (f.size() >= 3 && f[1] == "bt") {
                std::string addr = f[0];
                std::replace(addr.begin(), addr.end(), '-', ':');
                m_pan[upper(addr)] = f[2];
            }
        }
    m_devices.clear();
    for (size_t i = 3; i < sections.size(); ++i) {
        const std::string &info = sections[i];
        if (!std::regex_search(info, m, device))
            continue;
        Device d;
        d.address = m[2];
        d.name = field(info, "Alias");
        if (d.name.empty())
            d.name = field(info, "Name");
        d.icon = field(info, "Icon");
        d.connected = field(info, "Connected") == "yes";
        if (std::regex_search(info, m, rssiRe))
            d.rssi = std::stoi(m[1]);
        if (std::regex_search(info, m, batteryRe))
            d.battery = std::stoi(m[1]);
        for (auto it = std::sregex_iterator(info.begin(), info.end(), uuidRe); it != std::sregex_iterator(); ++it)
            d.uuids.push_back((*it)[1]);
        m_devices.push_back(d);
    }
    setNeedsDisplay();
}

Image AppleBluetoothExtra::statusImage(bool white) const
{
    // -checkAndSetPicture: no controller BrokenConnection, off Off, a device connected Connected, else
    // Idle; -lowBatteryNotification: lowbatt
    const bool low = std::any_of(m_devices.begin(), m_devices.end(), [](const Device &d) { return d.connected && d.battery >= 0 && d.battery <= 10; });
    std::string name;
    if (!m_controller)
        name = "BlueTooth_BrokenConnection";
    else if (!m_powered)
        name = "BlueTooth_Off";
    else if (low)
        name = "lowbatt";
    else if (std::any_of(m_devices.begin(), m_devices.end(), [](const Device &d) { return d.connected; }))
        name = "BlueTooth_Connected";
    else
        name = "BlueTooth_Idle";
    return templateImage(name + ".pdf", white);
}

bool AppleBluetoothExtra::deviceItem(const Device &d, MenuItem &out)
{
    // DeviceMenuCreator -createMenuItemForDevice: its name, its submenu: what it does
    // (-menuItemsForDevice:), then its preferences (-preferenceItemsForDevice:); none without
    const bool option = modifierFlags() & Option;
    MenuItem item(d.name);
    std::vector<MenuItem> &sub = item.submenu;
    const std::string addr = d.address;
    auto after = [this] { App::shared().addTimer(2.5, false, [this] { poll(); }); };
    auto action = [](const std::string &title, std::function<void()> f) {
        MenuItem i(title);
        i.action = std::move(f);
        return i;
    };
    if (d.has("1105"))
        sub.push_back(action(tr("kSendFileToDevice", "Send File…"), [addr] {
            if (!startFirst({"bluedevil-sendfile"}, {"-k", "bluetooth://" + addr}))
                startFirst({"bluetooth-sendto", "blueman-sendto"}, {"--device=" + addr});
        }));
    if (d.has("1106"))
        // -handleBrowseDevice: here the desktop's OBEX FTP (KIO's obexftp:)
        sub.push_back(action(tr("kBrowseDevice", "Browse Device…"), [addr] {
            std::string dashed = addr;
            std::replace(dashed.begin(), dashed.end(), ':', '-');
            startFirst({"kioclient", "kioclient5"}, {"exec", "obexftp:/" + dashed + "/"});
        }));
    if (d.stereo() || d.headset()) {
        if (d.connected)
            sub.push_back(action(d.stereo() ? tr("kTurnOffHeadphones", "Don’t Use as Audio Device") : tr("kTurnOffHeadset", "Don’t Use as Audio Device"),
                                 [addr, after] {
                                     bluetoothctl({"disconnect", addr});
                                     after();
                                 }));
        else
            sub.push_back(action(d.stereo() ? tr("kTurnOnHeadphones", "Use as Audio Device (Stereo)") : tr("kTurnOnHeadset", "Use as Audio Device (Mono)"),
                                 [addr, after] {
                                     bluetoothctl({"connect", addr});
                                     after();
                                 }));
    }
    // -createConnectPANMenuItemForDevice: (0x92b5) a phone's network (NAP, 0x1116)
    bool panActive = false;
    if (d.has("1116")) {
        auto it = m_pan.find(upper(addr));
        const std::string state = it == m_pan.end() ? "" : it->second;
        panActive = state == "connected";
        const bool changing = state.rfind("connecting", 0) == 0 || state.rfind("deactivating", 0) == 0;
        MenuItem pan = action(panActive ? tr("kKeyDisconnectFromNetwork", "Disconnect from Network") : tr("kJoinNetwork", "Connect to Network"),
                              [addr, panActive, after] {
                                  Task::launchDetached({"nmcli", "device", panActive ? "disconnect" : "connect", addr});
                                  after();
                              });
        pan.enabled = !changing;
        sub.push_back(pan);
    }
    if (d.hid() && !d.connected)
        sub.push_back(action(tr("kConnectDevice", "Connect"), [addr, after] {
            bluetoothctl({"connect", addr});
            after();
        }));
    if (d.connected && !panActive && !d.stereo() && !d.headset())
        sub.push_back(action(tr("kDisconnectDevice", "Disconnect"), [addr, after] {
            bluetoothctl({"disconnect", addr});
            after();
        }));
    if (option)
        sub.push_back(action(tr("kRemoveDevice", "Remove This Device"), [addr, after] {
            bluetoothctl({"remove", addr});
            after();
        }));
    std::vector<MenuItem> prefs;
    auto pref = [&](const std::string &title, const std::string &pane) { prefs.push_back(action(title, [pane] { openPreferencePane(pane); })); };
    auto info = [&](const std::string &title) { prefs.emplace_back(title, false, false); };
    // with the Option key: its RSSI (the HCI command; else BlueZ's, from a scan) and its address
    if (option && d.connected) {
        int rssi = readRSSI(d.address);
        if (rssi == INT_MIN && d.rssi)
            rssi = d.rssi;
        if (rssi != INT_MIN)
            info("RSSI: " + std::to_string(rssi) + " dBm");
    }
    if (option)
        info("Address: " + d.address);
    if (d.connected && d.battery > 0)
        info("Battery Level: " + std::to_string(d.battery) + "%");
    if (d.icon == "input-mouse")
        pref(tr("kMousePreferences", "Open Mouse Preferences…"), "mouse");
    else if (d.icon == "input-tablet")
        pref("Open Trackpad Preferences…", "trackpad");
    if (d.icon == "input-keyboard")
        pref(tr("kKeyboardPreferences", "Open Keyboard Preferences…"), "keyboard");
    if (d.icon == "printer")
        pref(tr("kOpenPrintPrefsKey", "Open Printer Preferences…"), "printfax");
    if (d.stereo() || d.headset())
        pref(tr("kOpenSoundPrefsKey", "Open Sound Preferences…"), "sound");
    if (!prefs.empty()) {
        if (!sub.empty())
            sub.push_back(MenuItem::line());
        sub.insert(sub.end(), prefs.begin(), prefs.end());
    }
    if (sub.empty())
        return false;
    out = item;
    return true;
}

std::vector<MenuItem> AppleBluetoothExtra::menu()
{
    poll();
    return items();
}

std::vector<MenuItem> AppleBluetoothExtra::items()
{
    // AppleBluetoothExtra -menu (0x2509) and -createOnOffMenuItems: (0x2e42)
    const bool option = modifierFlags() & Option;
    std::vector<MenuItem> m;
    auto disabled = [&](const std::string &t) { m.emplace_back(t, false, false); };
    auto action = [&](const std::string &title, std::function<void()> f, bool enabled = true) {
        MenuItem i(title, false, enabled);
        i.action = std::move(f);
        m.push_back(i);
    };
    if (!m_controller) {
        disabled(tr("Not Available", "Bluetooth: Not Available"));
        m.push_back(MenuItem::line());
        action(tr("kOpenBTPrefsKey", "Open Bluetooth Preferences…"), [] { openPreferencePane("bluetooth"); });
        return m;
    }
    if (option) {
        // with the Option key: the stack's version, the controller's name and address
        disabled(format(tr("BTWithVersion", "Version: %@"), m_version));
        if (!m_name.empty())
            disabled("Name: " + m_name);
        if (!m_address.empty())
            disabled("Address: " + upper(m_address));
        // Bluetooth Explorer, PacketLogger and the Diagnostics Utility come with the developer tools:
        // here the report alone, between separators
        m.push_back(MenuItem::line());
        action("Create Diagnostics Report on Desktop…", [this] { report(); });
        m.push_back(MenuItem::line());
    }
    disabled(tr("BT", "Bluetooth: ") + (m_powered ? "On" : "Off"));
    action(m_powered ? tr("BTTurnOff", "Turn Bluetooth Off") : tr("BTTurnOn", "Turn Bluetooth On"), [this] {
        bluetoothctl({"power", m_powered ? "off" : "on"});
        m_powered = !m_powered;
        setNeedsDisplay();
        App::shared().addTimer(1.5, false, [this] { poll(); });
    });
    if (m_powered) {
        // -createDiscoverableMenuItems:
        MenuItem disc(tr("BTDiscoverable", "Discoverable"), false, true, m_discoverable);
        disc.action = [this] {
            bluetoothctl({"discoverable", m_discoverable ? "off" : "on"});
            m_discoverable = !m_discoverable;
        };
        m.push_back(disc);
        // -createBatteryWarningMenus: a device's low battery, its preferences
        bool first = true;
        for (const Device &d : m_devices)
            if (d.connected && d.battery >= 0 && d.battery <= 10) {
                if (first)
                    m.push_back(MenuItem::line()), first = false;
                const std::string pane = d.icon == "input-mouse" ? "mouse" : d.icon == "input-tablet" ? "trackpad" : "keyboard";
                action(format(tr("LowBattWarning", "%@ has a low battery."), d.name), [pane] { openPreferencePane(pane); });
            }
        m.push_back(MenuItem::line());
        // Bluetooth File Exchange's two: the desktop's Bluetooth file sender and browser, if installed
        action(tr("kBTSendFileKey", "Send File…"), [] { startFirst({"bluedevil-sendfile", "bluetooth-sendto", "blueman-sendto"}); },
               onPath("bluedevil-sendfile") || onPath("bluetooth-sendto") || onPath("blueman-sendto"));
        action(tr("kBTGetFileKey", "Browse Device…"), [] { startFirst({"kioclient", "kioclient5"}, {"exec", "bluetooth:/"}); },
               onPath("kioclient") || onPath("kioclient5"));
        m.push_back(MenuItem::line());
        // -createDeviceMenuItems: the connected first, then by name
        std::vector<Device> devices = m_devices;
        std::stable_sort(devices.begin(), devices.end(), [](const Device &a, const Device &b) {
            if (a.connected != b.connected)
                return a.connected;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        std::vector<MenuItem> deviceItems;
        for (const Device &d : devices) {
            MenuItem i;
            if (deviceItem(d, i))
                deviceItems.push_back(i);
        }
        if (!deviceItems.empty()) {
            disabled(tr("kDevices", "Devices"));
            m.insert(m.end(), deviceItems.begin(), deviceItems.end());
            m.push_back(MenuItem::line());
        }
        action(tr("kOpenBTSAKey", "Set Up Bluetooth Device…"), [] {
            if (!startFirst({"bluedevil-wizard", "blueman-assistant", "blueman-manager"}))
                openPreferencePane("bluetooth");
        });
    } else {
        m.push_back(MenuItem::line());
    }
    action(tr("kOpenBTPrefsKey", "Open Bluetooth Preferences…"), [] { openPreferencePane("bluetooth"); });
    return m;
}

// -handleOpenBluetoothReporter: IOBluetooth's BluetoothReporter, run as an administrator, leaves its
// report on the desktop; here BlueZ's states and logs, asked for with pkexec, in a file on the Desktop
void AppleBluetoothExtra::report()
{
    char stamp[64];
    const time_t t = time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", std::localtime(&t));
    const std::string file = home() + "/Desktop/Bluetooth Report " + stamp + ".txt";
    const std::string script =
        "echo '== bluetoothctl show'; bluetoothctl show; echo; echo '== btmgmt info'; btmgmt info 2>&1;"
        " for d in $(bluetoothctl devices | awk '/^Device/{print $2}'); do echo; echo \"== bluetoothctl info $d\"; bluetoothctl info \"$d\"; done;"
        " echo; echo '== rfkill'; rfkill list 2>&1; echo; echo '== bluetoothd'; journalctl -b -u bluetooth --no-pager -n 300 2>&1;"
        " echo; echo '== kernel'; dmesg 2>/dev/null | grep -i -E 'bluetooth|btusb|hci' | tail -n 200";
    // the shell writes the file as the user; pkexec runs the script
    Task::launchDetached({"sh", "-c", "pkexec sh -c \"$0\" > \"$1\"", script, file});
}

SYSTEMUI_REGISTER_EXTRA("AppleBluetoothExtra", AppleBluetoothExtra);
