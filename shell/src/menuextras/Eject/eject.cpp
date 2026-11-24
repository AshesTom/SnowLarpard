// SPDX-License-Identifier: LGPL-2.1-or-later
// Eject.menu's code (EjectExtra) on AquaKit's SystemUIPlugin, loaded by SystemUIServer as on the Mac.
// Reversed from 10.6's Eject.menu 2.0.2 (213), x86_64, with REA (docs/reverse-status.md, section 25):
//
//   - -initWithBundle:: Eject.pdf as a template image, an empty menu whose delegate it is;
//   - -menuNeedsUpdate:: "No Drives" alone without an optical drive; else an item a drive, the
//     internal ones first, titled by -validateMenuItem: "%1$@ %2$@" (the action, the drive's name),
//     "%1$@ %2$@ %3$@" with its place (Upper, Lower, Left, Right) when there are two internal drives;
//   - -validateMenuItem:: media in the drive "Eject" (tag 3), the tray open and closable by software
//     "Close" (tag 2), else "Open" (tag 1);
//   - -formattedDeviceName:: by what the drive writes: DVD "SuperDrive", CD only "Combo Drive" when it
//     reads DVDs, else "CD-R/RW Drive"; DVD-RAM "DVD-RAM"; otherwise its own name, else "Unknown Drive";
//   - -deviceMenuPicked:: tag 2 -closeTray, 3 -ejectMedia, else -openTray, on a thread of its own.
//
// On Linux: the kernel's CD-ROM drives (/sys/class/block/sr*, /proc/sys/dev/cdrom/info), the tray's
// state by CDROM_DRIVE_STATUS, eject(1) for the actions (udisks unmounting the media first).
#include "systemuiplugin.h"

#include "foundation.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <fstream>
#include <map>
#include <sstream>

using namespace AK;

namespace {

std::string readLine(const std::string &path)
{
    std::ifstream in(path);
    std::string s;
    std::getline(in, s);
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front()))
        s.erase(0, 1);
    return s;
}

struct Drive {
    std::string name;   // sr0
    std::string model;  // the drive's own name (DRDevice displayName)
    bool internal = true;
    bool writesDVD = false, readsDVD = false, writesCD = false, writesDVDRAM = false;
    bool canClose = true;   // a tray the software can close (not a slot)
    int status = CDS_NO_INFO;
};

// /proc/sys/dev/cdrom/info: a row a capability, a column a drive ("drive name: sr0 sr1")
std::map<std::string, std::map<std::string, std::string>> cdromInfo()
{
    std::map<std::string, std::map<std::string, std::string>> out;
    std::ifstream in("/proc/sys/dev/cdrom/info");
    std::vector<std::string> names;
    std::string line;
    while (std::getline(in, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = line.substr(0, colon);
        std::istringstream values(line.substr(colon + 1));
        std::vector<std::string> v;
        for (std::string w; values >> w;)
            v.push_back(w);
        if (key == "drive name") {
            names = v;
            continue;
        }
        for (size_t i = 0; i < v.size() && i < names.size(); ++i)
            out[names[i]][key] = v[i];
    }
    return out;
}

std::vector<Drive> drives()
{
    std::vector<Drive> out;
    const auto info = cdromInfo();
    std::vector<std::string> names;
    if (DIR *dir = opendir("/sys/class/block")) {
        while (dirent *e = readdir(dir))
            names.push_back(e->d_name);
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    for (const std::string &n : names) {
        if (n.rfind("sr", 0) != 0)
            continue;
        Drive d;
        d.name = n;
        const std::string dev = "/sys/class/block/" + n + "/device/";
        const std::string vendor = readLine(dev + "vendor"), model = readLine(dev + "model");
        d.model = vendor.empty() ? model : model.empty() ? vendor : vendor + " " + model;
        // DRDevicePhysicalInterconnectLocationInternal: not on a USB, FireWire or Thunderbolt bus
        char real[4096];
        const ssize_t len = readlink(("/sys/class/block/" + n).c_str(), real, sizeof real - 1);
        const std::string path = len > 0 ? std::string(real, size_t(len)) : std::string();
        d.internal = path.find("/usb") == std::string::npos && path.find("/firewire") == std::string::npos;
        auto it = info.find(n);
        if (it != info.end()) {
            auto cap = [&](const char *k) {
                auto f = it->second.find(k);
                return f != it->second.end() && f->second == "1";
            };
            d.writesDVD = cap("Can write DVD-R");
            d.readsDVD = cap("Can read DVD");
            d.writesCD = cap("Can write CD-R") || cap("Can write CD-RW");
            d.writesDVDRAM = cap("Can write DVD-RAM");
            d.canClose = cap("Can close tray");
        }
        const int fd = open(("/dev/" + n).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            d.status = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
            close(fd);
        }
        out.push_back(d);
    }
    // the internal ones first (internalDeviceList, then externalDeviceList)
    std::stable_partition(out.begin(), out.end(), [](const Drive &d) { return d.internal; });
    return out;
}

} // namespace

class EjectExtra : public SystemUI::MenuExtra {
public:
    explicit EjectExtra(const std::string &bundle) : MenuExtra(bundle)
    {
        setImage(templateImage("Eject.pdf"));
        setAlternateImage(templateImage("Eject.pdf", true));
    }

    std::vector<MenuItem> menu() override
    {
        // -menuNeedsUpdate:
        std::vector<MenuItem> m;
        const std::vector<Drive> list = drives();
        if (list.empty()) {
            MenuItem none{tr("No Drives", "No Drives")};
            none.enabled = false;   // no action: AppKit disables it
            m.push_back(none);
            return m;
        }
        const long internal = std::count_if(list.begin(), list.end(), [](const Drive &d) { return d.internal; });
        int i = 0;
        for (const Drive &d : list) {
            // -validateMenuItem:: the action by the drive's state
            int tag = 1;
            std::string action = tr("Open", "Open");
            if (d.status == CDS_DISC_OK) {
                tag = 3;
                action = tr("Eject", "Eject");
            } else if (d.status == CDS_TRAY_OPEN && d.canClose) {
                tag = 2;
                action = tr("Close", "Close");
            }
            std::string title;
            if (internal == 2 && d.internal) {
                // two internal drives: their place (the first one Upper; DRDevice's location
                // description isn't known here)
                const std::string place = i == 0 ? tr("Upper", "Upper") : tr("Lower", "Lower");
                title = fill(tr("%@ %@ %@", "%1$@ %2$@ %3$@"), {action, place, deviceName(d)});
            } else {
                title = fill(tr("%@ %@", "%1$@ %2$@"), {action, deviceName(d)});
            }
            MenuItem item{title};
            const std::string dev = "/dev/" + d.name;
            item.action = [tag, dev] { picked(tag, dev); };
            m.push_back(item);
            ++i;
        }
        return m;
    }

private:
    std::string tr(const char *key, const char *fallback) const { return localized(key, fallback); }

    // "%1$@" ... "%3$@" (and "%@" in order)
    static std::string fill(std::string f, const std::vector<std::string> &values)
    {
        for (size_t i = 0; i < values.size(); ++i) {
            const std::string positional = "%" + std::to_string(i + 1) + "$@";
            const size_t at = f.find(positional);
            if (at != std::string::npos)
                f.replace(at, positional.size(), values[i]);
            else if (const size_t plain = f.find("%@"); plain != std::string::npos)
                f.replace(plain, 2, values[i]);
        }
        return f;
    }

    // -formattedDeviceName:
    std::string deviceName(const Drive &d) const
    {
        if (d.internal) {
            if (d.writesDVD)
                return tr("SuperDrive", "SuperDrive");
            if (d.writesCD && d.readsDVD)
                return tr("Combo Drive", "Combo Drive");
            if (d.writesCD)
                return tr("CD-R/RW Drive", "CD-R/RW Drive");
            if (d.writesDVDRAM)
                return tr("DVD-RAM", "DVD-RAM");
        }
        return d.model.empty() ? tr("Unknown Drive", "Unknown Drive") : d.model;
    }

    // -deviceMenuPicked:: 2 closeTray, 3 ejectMedia (its volumes unmounted first), else openTray
    static void picked(int tag, const std::string &dev)
    {
        if (tag == 2)
            Task::launchDetached({"eject", "-t", dev});
        else if (tag == 3)
            Task::launchDetached({"sh", "-c", "udisksctl unmount -b \"$0\" >/dev/null 2>&1; eject \"$0\"", dev});
        else
            Task::launchDetached({"eject", dev});
    }
};

SYSTEMUI_REGISTER_EXTRA("EjectExtra", EjectExtra);
