// SPDX-License-Identifier: LGPL-2.1-or-later
#include "startup.h"

#include "foundation.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <regex>
#include <sys/stat.h>

using namespace AK;

namespace Startup {

namespace {

bool exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// a /sys/class folder's entries
std::vector<std::string> entries(const std::string &dir)
{
    std::vector<std::string> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.')
                out.push_back(dir + "/" + e->d_name);
        closedir(d);
    }
    return out;
}

std::string firstLine(const std::string &file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

// SUISStartupObject (autloadMethods), Linux's way to tell
bool canLoad(const std::string &method)
{
    if (method == "_clockCanLoad")
        return true;   // always
    if (method == "_batteryCanLoad") {   // IOPMCopyBatteryInfo: a battery
        for (const std::string &s : entries("/sys/class/power_supply"))
            if (firstLine(s + "/type") == "Battery")
                return true;
        return false;
    }
    if (method == "_volumeCanLoad") {   // AppleSoundSettings: an output device
        std::ifstream cards("/proc/asound/cards");
        std::string line;
        return std::getline(cards, line) && line.find("no soundcards") == std::string::npos;
    }
    if (method == "_airportCanLoad") {   // an AirPort card: a wireless interface
        for (const std::string &n : entries("/sys/class/net"))
            if (exists(n + "/wireless") || exists(n + "/phy80211"))
                return true;
        return false;
    }
    if (method == "_bluetoothCanLoad")   // an IOBluetoothHCIController
        return !entries("/sys/class/bluetooth").empty();
    if (method == "_ejectCanLoad") {
        // two optical drives not internal (the DVD drives on USB or FireWire), or a keyboard with
        // F12 as its eject key (HIDKeyboardSupportsF12Eject: Apple's, vendor 05AC)
        int external = 0;
        for (const std::string &n : entries("/sys/class/block")) {
            if (fileName(n).rfind("sr", 0) != 0)
                continue;
            char real[4096];
            const ssize_t len = readlink(n.c_str(), real, sizeof real - 1);
            const std::string path = len > 0 ? std::string(real, size_t(len)) : std::string();
            external += path.find("/usb") != std::string::npos || path.find("/firewire") != std::string::npos;
        }
        if (external >= 2)
            return true;
        for (const std::string &h : entries("/sys/bus/hid/devices"))
            if (fileName(h).find(":05AC:") != std::string::npos)
                return true;
        return false;
    }
    if (method == "_userCanLoad")   // fast user switching: MultipleSessionEnabled (the global domain's)
        return Settings("appearance").boolean("MultipleSessionEnabled", false);
    if (method == "_textInputCanLoad") {
        // TSMShouldTextInputMenuBeDisplayed: more than one input source (XKB's layouts here)
        FILE *p = popen("setxkbmap -query 2>/dev/null", "r");
        std::string q;
        char buf[256];
        while (p && fgets(buf, sizeof buf, p))
            q += buf;
        if (p)
            pclose(p);
        const size_t at = q.find("layout:");
        if (at == std::string::npos) {
            const char *env = std::getenv("XKB_DEFAULT_LAYOUT");
            return env && std::strchr(env, ',');
        }
        const std::string layouts = q.substr(at, q.find('\n', at) - at);
        return layouts.find(',') != std::string::npos;
    }
    // _timeMachineCanLoad (0x1000121f8): any user but the guest account (User -isGuestUser); Linux has
    // no guest account but one named so
    if (method == "_timeMachineCanLoad") {
        const char *user = std::getenv("USER");
        return !user || std::string(user) != "guest";
    }
    // _pppCanLoad, _pppoeCanLoad: ShowPPPMenuExtra, ShowPPPoEMenuExtra (any application's, any user's,
    // the current host's; Network's "Show modem status in menu bar"), off unless set
    if (method == "_pppCanLoad")
        return Settings("network").boolean("ShowPPPMenuExtra", false);
    if (method == "_pppoeCanLoad")
        return Settings("network").boolean("ShowPPPoEMenuExtra", false);
    // _remoteDesktopCanLoad: com.apple.RemoteManagement's LoadRemoteManagementMenuExtra, off unless set
    if (method == "_remoteDesktopCanLoad")
        return Settings("remotemanagement").boolean("LoadRemoteManagementMenuExtra", false);
    // _verizonCanLoad: a modem (IOModemSerialStreamSync) with WWAN = true; _expressCardCanLoad: an
    // ejectable PCI device (IOPCIEjectable). Linux: a WWAN interface (/sys/class/net/*/device's
    // wwan), a hot-pluggable PCI Express slot holding a card
    if (method == "_verizonCanLoad") {
        for (const std::string &n : entries("/sys/class/net"))
            if (fileName(n).rfind("wwan", 0) == 0 || exists(n + "/wwan"))
                return true;
        return !entries("/sys/class/wwan").empty();
    }
    if (method == "_expressCardCanLoad") {
        for (const std::string &s : entries("/sys/bus/pci/slots"))
            if (exists(s + "/power") && firstLine(s + "/adapter") == "1")
                return true;
        return false;
    }
    return false;
}

// -mapPosition:
int mapPosition(int id)
{
    return id <= -10000 && id >= -10024 ? id + 9999 : 0;
}

struct Autoload {
    int id;
    std::string method, path;
};

// SystemUIServer.app's Autoload.plist (extract.py's JSON), else 10.6.8's own
std::vector<Autoload> autoload()
{
    std::vector<Autoload> out;
    std::ifstream in(Assets::path("/System/Library/CoreServices/SystemUIServer.app/Contents/Resources/Autoload.plist.json"));
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    static const std::regex entry(R"re(\{[^{}]*\})re"), id(R"re("id":\s*"?(-?\d+))re"),
        method(R"re("method":\s*"([^"]+)")re"), path(R"re("path":\s*"([^"]+)")re");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), entry); it != std::sregex_iterator(); ++it) {
        const std::string e = it->str();
        std::smatch i, m, p;
        if (std::regex_search(e, i, id) && std::regex_search(e, m, method) && std::regex_search(e, p, path))
            out.push_back({std::stoi(i[1]), m[1], p[1]});
    }
    if (out.empty()) {
        const std::string extras = "/System/Library/CoreServices/Menu Extras/";
        out = {{-10014, "_userCanLoad", extras + "User.menu"},       {-10000, "_clockCanLoad", extras + "Clock.menu"},
               {-10016, "_textInputCanLoad", extras + "TextInput.menu"}, {-10001, "_batteryCanLoad", extras + "Battery.menu"},
               {-10002, "_volumeCanLoad", extras + "Volume.menu"},   {-10008, "_ejectCanLoad", extras + "Eject.menu"},
               {-10004, "_airportCanLoad", extras + "AirPort.menu"}, {-10006, "_pppCanLoad", extras + "PPP.menu"},
               {-10007, "_pppoeCanLoad", extras + "PPPoE.menu"},     {-10010, "_bluetoothCanLoad", extras + "Bluetooth.menu"},
               {-10018, "_remoteDesktopCanLoad", extras + "RemoteDesktop.menu"},
               {-10024, "_timeMachineCanLoad", extras + "TimeMachine.menu"}};
    }
    return out;
}

std::string expandTilde(const std::string &p)
{
    return p.rfind("~/", 0) == 0 ? home() + p.substr(1) : p;
}

} // namespace

std::vector<std::string> menuExtrasForStartup()
{
    Settings prefs("systemuiserver");
    std::vector<std::string> list;
    for (const std::string &p : prefs.list("menuExtras"))
        list.push_back(expandTilde(p));
    std::vector<std::string> dont;
    for (const std::string &p : prefs.list("dontAutoLoad"))
        dont.push_back(expandTilde(p));
    auto has = [](const std::vector<std::string> &v, const std::string &p) { return std::find(v.begin(), v.end(), p) != v.end(); };
    for (const Autoload &a : autoload()) {
        if (has(list, a.path) || has(dont, a.path) || !canLoad(a.method))
            continue;
        const int at = std::clamp(int(list.size()) + 1 + mapPosition(a.id), 0, int(list.size()));
        list.insert(list.begin() + at, a.path);
    }
    return list;
}

bool canLoadClass(const std::string &c)
{
    // the 36 principal classes SystemUIServer 10.6.8 loads (-_canLoadClass:'s set, from 0x10006b140)
    static const char *const allowed[] = {
        "AirPortExtra", "BatteryExtra", "CPUExtra", "AppleClockExtra", "DisplaysExtra", "IrDAExtra",
        "PPPConnectExtra", "PPPoEConnectExtra", "AppleVolumeExtra", "EjectExtra", "AppleUser", "ProcessExtra",
        "AppleVPNConnectExtra", "AppleFezExtra", "AppleExpressCardExtra", "WorkspaceMenu",
        "AppleBluetoothExtra", "AppleBRExtra", "AppleRDExtra", "AppleOSAScriptMenu", "AppleClassicExtra",
        "AppleTextInputExtra", "AppleInkExtra", "AppleVPNExtra", "AppleVerizonExtra", "AppleFaxExtra",
        "AppleMHDExtra", "AppleWWANExtra", "AppleSpacesExtra", "AppleKeychainExtra", "UAMenu",
        "AppleTimeMachineExtra", "CHUDProcessorExtra", "AppleSystemInfo", "AppleConnectExtra", "UNCMenuExtra"};
    return std::find(std::begin(allowed), std::end(allowed), c) != std::end(allowed);
}

void writeMenuExtras(const std::vector<std::string> &bundles)
{
    Settings prefs("systemuiserver");
    if (prefs.list("menuExtras") != bundles)
        prefs.set("menuExtras", bundles);
}

} // namespace Startup
