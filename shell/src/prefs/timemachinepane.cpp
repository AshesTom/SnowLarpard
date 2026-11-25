// SPDX-License-Identifier: LGPL-2.1-or-later
// Time Machine's pane, from TimeMachine.prefPane 10.6 (BackupPref-92), x86_64, read with REA
// (docs/reverse-status.md, section 40): TMPreferencePane, TMDestinationSheetController,
// TMOptionsSheetController, AppleTMSettings, TMSliderView / TMSliderCell, TMOnOffButton.
//
// The settings are the shell's backupd's (section 37): timemachine.conf's AutoBackup, BackupPath (for
// BackupAlias: a folder, a mounted volume's mount point), SkipPaths and ExcludeByPath, RequiresACPower,
// AlwaysShowDeletedBackupsWarning; its results in TimeMachine.Results.conf (RESULT,
// BACKUP_COMPLETED_DATE) and the session's status in backupd.status.conf, as the menu extra reads them.
//
// The destination sheet lists what CSBackupVolMgrCopyLocalVolumes / CopyReadOnlyVolumes give and
// -_isSupportedDestinationAtLocalMountPoint:readOnly: keeps: on Linux the mounted disks (a device's
// own root, /proc/self/mountinfo) but "/" (the code's own rule), the volume holding the home (the one
// backed up here, as "/" is on the Mac) and the system's (/boot, /efi, /usr, /var), disk images (loop
// devices); journaled native file systems (HFS+ / ACFS / ZFS there: ext2-4, btrfs, xfs, f2fs, zfs,
// bcachefs, jfs, nilfs2, reiserfs, hfsplus here) writable; FAT and NTFS, which the Mac lists as
// "reformat required" and erases, left out (erasing a disk isn't ported). Besides: any mounted disk
// holding Backups.backupdb, and the folder BackupPath names when it isn't a mount point.
#include "foundation.h"
#include "nibpane.h"
#include "panels.h"
#include "runloop.h"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace AK;

namespace {

const char *const Bundle = "/System/Library/PreferencePanes/TimeMachine.prefPane";
const char *const TimeMachineExtra = "/System/Library/CoreServices/Menu Extras/TimeMachine.menu";
const char *const CoreTypes = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/";
const char *const Storage = "/System/Library/Extensions/IOStorageFamily.kext/Contents/Resources/";

std::string tr(const std::string &key, const std::string &fallback = {})
{
    return Assets::tr(Bundle, "Localizable", key, fallback.empty() ? key : fallback);
}

std::string replaced(std::string s, const std::string &what, const std::string &with)
{
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size()))
        s.replace(at, what.size(), with);
    return s;
}

// "%1$@" and "%@" in turn
std::string format(std::string f, const std::vector<std::string> &args)
{
    size_t next = 0;
    for (size_t i = 0; i < args.size(); ++i)
        f = replaced(f, "%" + std::to_string(i + 1) + "$@", args[i]);
    for (size_t at = f.find("%@"); at != std::string::npos && next < args.size(); at = f.find("%@", at))
        f.replace(at, 2, args[next]), at += args[next++].size();
    return f;
}

// humanReadableStringForBytes: (TMAdditions 0x20d8, NSLocalizedFileSizeDescription): base 1000, the
// unit's fraction digits, trailing zeros dropped (the menu extra's, section 31)
std::string bytesString(long long bytes)
{
    if (bytes <= 0)
        return "Zero KB";
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    static const int digits[] = {0, 0, 1, 2, 2, 2, 2};
    double v = double(bytes);
    int u = 0;
    auto rounded = [](double x, int d) {
        const double f = std::pow(10.0, d);
        return std::round(x * f) / f;
    };
    while (u < 6 && rounded(v, digits[u]) >= 1000) {
        v /= 1000;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits[u], rounded(v, digits[u]));
    std::string n = buf;
    if (n.find('.') != std::string::npos) {
        while (n.back() == '0')
            n.pop_back();
        if (n.back() == '.')
            n.pop_back();
    }
    return n + " " + units[u];
}

// TMDateFormatter -stringFromDate:attributes:maxWidth: (the menu extra's): "Today, 6:36 PM",
// "Yesterday, …", else the long date, which fits the pane's fields
std::string dateString(std::time_t when)
{
    auto backupd = [](const std::string &key, const std::string &fallback) {
        return Assets::tr("/System/Library/CoreServices/backupd.bundle", "MDDateFormatter", key, fallback);
    };
    std::tm t{};
    localtime_r(&when, &t);
    const std::time_t now = std::time(nullptr);
    std::tm today{};
    localtime_r(&now, &today);
    today.tm_hour = today.tm_min = today.tm_sec = 0;
    const std::time_t midnight = std::mktime(&today);
    char buf[96];
    std::string relative;
    if (when >= midnight && when < midnight + 86400)
        relative = backupd("TODAY", "Today");
    else if (when >= midnight + 86400 && when < midnight + 2 * 86400)
        relative = backupd("TOMORROW", "Tomorrow");
    else if (when >= midnight - 86400 && when < midnight)
        relative = backupd("YESTERDAY", "Yesterday");
    if (!relative.empty()) {
        std::snprintf(buf, sizeof buf, "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
        return format(backupd("DATE_TIME_FORMAT", "%1$@, %2$@"), {relative, buf});
    }
    static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};
    std::snprintf(buf, sizeof buf, "%s %d, %d", months[t.tm_mon], t.tm_mday, t.tm_year + 1900);
    return buf;
}

std::string unescaped(const std::string &s)   // mountinfo's \040
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit((unsigned char)s[i + 1])) {
            out += char(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- the disks (CSBackupVolMgr, TMVolumeInfo)

struct Mount {
    std::string point, root, fstype, device, options;
};

std::vector<Mount> mounts()
{
    std::vector<Mount> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string id, parent, dev, field;
        Mount m;
        ss >> id >> parent >> dev >> m.root >> m.point >> m.options;
        while (ss >> field && field != "-") {
        }
        ss >> m.fstype >> m.device;
        m.point = unescaped(m.point);
        m.root = unescaped(m.root);
        out.push_back(m);
    }
    return out;
}

std::string realPath(const std::string &p)
{
    char buf[4096];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

// the mount point holding a path (the longest)
std::string mountPointOf(const std::string &path)
{
    std::string best = "/";
    const std::string p = realPath(path);
    for (const Mount &m : mounts())
        if ((p == m.point || p.rfind(m.point == "/" ? "/" : m.point + "/", 0) == 0) && m.point.size() >= best.size())
            best = m.point;
    return best;
}

const Mount *mountAt(const std::vector<Mount> &all, const std::string &point)
{
    const Mount *found = nullptr;
    for (const Mount &m : all)
        if (m.point == point)
            found = &m;   // the last one mounted there
    return found;
}

// /dev/disk/by-label's names, by device
std::map<std::string, std::string> labels()
{
    std::map<std::string, std::string> out;
    for (const std::string &label : files("/dev/disk/by-label")) {
        std::string name = label;
        for (size_t at; (at = name.find("\\x20")) != std::string::npos;)
            name.replace(at, 4, " ");
        out[realPath("/dev/disk/by-label/" + label)] = name;
    }
    return out;
}

// the whole disk a partition is on (DMManager -wholeDiskForDisk:)
std::string wholeDisk(const std::string &device)
{
    const std::string dev = fileName(realPath(device));
    const std::string sys = realPath("/sys/class/block/" + dev);
    if (isFile(sys + "/partition"))
        return fileName(dirName(sys));
    return dev;
}

bool removableDisk(const std::string &device)
{
    std::ifstream in("/sys/class/block/" + wholeDisk(device) + "/removable");
    int r = 0;
    in >> r;
    return r == 1;
}

bool nativeFileSystem(const std::string &t)
{
    // TMVolumeInfo -isVolumeHFSPlus: / -isVolumeACFS: / -isVolumeZFS:: journaled, hard links
    static const std::set<std::string> native = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "zfs", "bcachefs", "jfs", "nilfs2", "reiserfs", "hfsplus"};
    return native.count(t) != 0;
}

struct Volume {
    std::string name, path, device;
    long long free = 0, total = 0;
    bool removable = false, mountPoint = true;
};

void measure(Volume &v)
{
    struct statvfs s;
    if (statvfs(v.path.c_str(), &s) == 0) {
        v.free = (long long)s.f_bavail * (long long)s.f_frsize;
        v.total = (long long)s.f_blocks * (long long)s.f_frsize;
    }
}

bool writable(const Mount &m)
{
    struct statvfs s;
    const bool ro = (m.options == "ro" || m.options.rfind("ro,", 0) == 0) || (statvfs(m.point.c_str(), &s) == 0 && (s.f_flag & ST_RDONLY));
    return !ro && access(m.point.c_str(), W_OK) == 0;
}

std::string volumeName(const std::string &device, const std::string &point)
{
    static std::map<std::string, std::string> names;
    if (names.empty())
        names = labels();
    auto l = names.find(realPath(device));
    if (l != names.end())
        return l->second;
    return point == "/" ? std::string("Macintosh HD") : fileName(point);
}

// -updateTableViewContents: the local volumes, then the read-only ones, those
// -_isSupportedDestinationAtLocalMountPoint:readOnly: keeps
std::vector<Volume> destinations(const std::string &configured)
{
    std::vector<Volume> out;
    const std::vector<Mount> all = mounts();
    const std::string homeVolume = mountPointOf(home());
    std::set<std::string> seen;
    auto system = [](const std::string &p) {
        for (const char *s : {"/boot", "/efi", "/usr", "/var", "/proc", "/sys", "/dev", "/snap"})
            if (p == s || p.rfind(std::string(s) + "/", 0) == 0)
                return true;
        return p.rfind("/run/", 0) == 0 && p.rfind("/run/media/", 0) != 0;
    };
    for (const Mount &m : all) {
        if (m.device.rfind("/dev/", 0) != 0 || m.device.rfind("/dev/loop", 0) == 0)
            continue;   // not a disk, or a disk image (TMVolumeInfo -isDiskImageMountPoint:)
        if (mountAt(all, m.point) != &m)
            continue;   // covered by a later mount
        const std::string device = realPath(m.device);
        if (seen.count(device) || m.point == "/")
            continue;
        const bool holdsBackups = isDir(m.point + "/Backups.backupdb");
        // a disk's own root (not a bind mount or a subvolume), not the system's or the home's
        if (!holdsBackups && (m.root != "/" || system(m.point) || m.point == homeVolume))
            continue;
        if (!nativeFileSystem(m.fstype) || !writable(m))
            continue;
        seen.insert(device);
        Volume v;
        v.path = m.point;
        v.device = device;
        v.name = volumeName(device, m.point);
        v.removable = removableDisk(device) || m.point.rfind("/run/media/", 0) == 0 || m.point.rfind("/media/", 0) == 0;
        measure(v);
        out.push_back(v);
    }
    // the folder backupd backs up to when it isn't a disk of its own
    if (!configured.empty() && isDir(configured)) {
        const std::string c = realPath(configured);
        if (std::none_of(out.begin(), out.end(), [&](const Volume &v) { return realPath(v.path) == c; })) {
            Volume v;
            v.path = configured;
            v.mountPoint = mountPointOf(configured) == c;
            v.name = fileName(c);
            if (const Mount *m = mountAt(all, mountPointOf(configured)))
                v.device = realPath(m->device);
            measure(v);
            out.push_back(v);
        }
    }
    return out;
}

// NSWorkspace -iconForFile: (AppleTMSettings -iconForPath:): a disk's, a folder's, a file's
Image iconForPath(const std::string &path, double size)
{
    const Size s{size, size};
    if (path.empty())
        return Image::load(Assets::path(std::string(Storage) + "External.icns"), s);
    if (path == "/")
        return Image::load(Assets::path(std::string(Storage) + "Internal.icns"), s);
    if (mountPointOf(path) == realPath(path)) {
        const bool removable = path.rfind("/run/media/", 0) == 0 || path.rfind("/media/", 0) == 0;
        return Image::load(Assets::path(std::string(Storage) + (removable ? "Removable.icns" : "External.icns")), s);
    }
    if (realPath(path) == realPath(home()))
        return Image::load(Assets::path(std::string(CoreTypes) + "HomeFolderIcon.icns"), s);
    return Image::load(Assets::path(std::string(CoreTypes) + (isDir(path) ? "GenericFolderIcon.icns" : "GenericDocumentIcon.icns")), s);
}

// ---- backupd's state (TMSystemBackup: the session's status, the results)

struct BackupState {
    int state = 0;   // -daemonState: 0 idle, 1 stopping, 2 starting, 3 copying, 4 finishing, 5 6 thinning,
                     // 7 indexing, 8 calculating, 9 mounting, 10 deleting
    long long total = 0, done = 0;
    double percent = 0;
    int pid = 0;
};

BackupState backupState()
{
    BackupState s;
    Settings st("backupd.status");
    if (!st.boolean("Running"))
        return s;
    s.pid = st.integer("pid");
    if (s.pid > 0 && kill(s.pid, 0) != 0 && errno == ESRCH)
        return s;
    if (st.boolean("Stopping")) {
        s.state = 1;
        return s;
    }
    static const std::pair<const char *, int> phases[] = {
        {"DeletingOldBackups", 10}, {"ThinningPostBackup", 6}, {"Finishing", 4},           {"WaitingForSpotlight", 7},
        {"ThinningPreBackup", 5},   {"CalculatingChanges", 8}, {"MountingBackupVol", 9}, {"Starting", 2},
        {"Copying", 3}};
    const std::string phase = st.string("BackupPhase");
    for (const auto &[name, value] : phases)
        if (phase == name)
            s.state = value;
    if (phase.empty())
        return s;
    s.percent = std::clamp(std::atof(st.string("Percent", "0").c_str()), 0.0, 1.0);
    s.total = std::atoll(st.string("totalBytes", "0").c_str());
    s.done = std::atoll(st.string("bytes", "0").c_str());
    return s;
}

// <disk>/Backups.backupdb/<computer>: its snapshots' dates (yyyy-MM-dd-HHmmss)
std::vector<std::time_t> snapshotDates(const std::string &disk)
{
    std::vector<std::time_t> out;
    if (disk.empty())
        return out;
    char host[256] = {};
    gethostname(host, sizeof host - 1);
    std::string computer = host;
    computer = computer.substr(0, computer.find('.'));
    DIR *d = opendir((disk + "/Backups.backupdb/" + computer).c_str());
    if (!d)
        return out;
    while (dirent *e = readdir(d)) {
        std::tm t{};
        const std::string n = e->d_name;
        if (n.size() < 17 || n.find(".inProgress") != std::string::npos)
            continue;
        if (std::sscanf(n.c_str(), "%4d-%2d-%2d-%2d%2d%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) == 6) {
            t.tm_year -= 1900;
            t.tm_mon -= 1;
            t.tm_isdst = -1;
            out.push_back(std::mktime(&t));
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string appsDir()
{
    // .../apps/System Preferences.app/Contents/MacOS/System Preferences
    return dirName(dirName(dirName(dirName(executablePath()))));
}

// TMPreferencePane -hasBattery
bool hasBattery()
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return false;
    bool found = false;
    while (dirent *e = readdir(d)) {
        std::ifstream type(std::string("/sys/class/power_supply/") + e->d_name + "/type");
        std::string t;
        type >> t;
        found = found || t == "Battery";
    }
    closedir(d);
    return found;
}

bool onBattery()
{
    // TMPreferencePane -isOnBattery (IOPSGetProvidingPowerSourceType: "Battery Power")
    bool battery = false, ac = false;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d)
        return false;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        const std::string p = std::string("/sys/class/power_supply/") + e->d_name;
        std::ifstream type(p + "/type"), online(p + "/online");
        std::string t;
        int on = 0;
        type >> t;
        if (t == "Mains" && (online >> on) && on)
            ac = true;
        if (t == "Battery")
            battery = true;
    }
    closedir(d);
    return battery && !ac;
}

// ---- TMSliderView: the big OFF / ON switch (SliderWell.png, SliderHandle(Down).png under
// OverlayMask.png); TMSliderCell -knobRectFlipped: x = trackRect.x + floor((trackRect.w - 44 + 4) ×
// value) - 2; -stopTracking:at:inView:mouseIsUp: a click goes to the other side, a drag to the other
// side past 0.2 of the way, else back; -setDoubleValue:animate: slides it

struct SliderSwitch : View {
    Image well, handle, handleDown, mask;
    double value = 0, target = 0;
    bool pressed = false, dragged = false;
    double grab = 0, start = 0;
    int timer = 0;
    std::function<void(int)> action;   // onOffSliderPressed:
    ~SliderSwitch() override
    {
        if (timer)
            App::shared().removeTimer(timer);
    }
    int state() const { return target >= 0.5 ? 1 : 0; }
    double travel() const { return frame().w - 4 - handle.width() + 4; }   // trackRect 2 points in
    void setState(int s, bool animate)
    {
        const double t = s ? 1 : 0;
        if (t == target && !pressed && value == t)
            return;
        target = t;
        if (!animate) {
            value = t;
            setNeedsDisplay();
            return;
        }
        if (!timer)
            timer = App::shared().addTimer(1.0 / 60, true, [this] {
                const double step = 1.0 / 12;   // 0.2 s
                value = value < target ? std::min(target, value + step) : std::max(target, value - step);
                setNeedsDisplay();
                if (value == target) {
                    App::shared().removeTimer(timer);
                    timer = 0;
                }
            });
    }
    void draw(cairo_t *cr) override
    {
        const double a = isEnabled() ? 1 : 0.5;
        if (!well.isNull())
            well.draw(cr, Point{0, 1}, isEnabled() ? 1 : 0.3);
        const Image &k = pressed ? handleDown : handle;
        if (!k.isNull())
            k.draw(cr, Point{2 + std::floor(travel() * value) - 2, 0}, a);
        if (!mask.isNull())
            mask.draw(cr, Point{0, 0});
    }
    void mouseDown(Event &e) override
    {
        if (!isEnabled())
            return;
        pressed = true;
        dragged = false;
        grab = e.location.x;
        start = value;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        if (!pressed)
            return;
        if (std::abs(e.location.x - grab) > 2)
            dragged = true;
        if (dragged && travel() > 0) {
            value = std::clamp(start + (e.location.x - grab) / travel(), 0.0, 1.0);
            setNeedsDisplay();
        }
    }
    void mouseUp(Event &) override
    {
        if (!pressed)
            return;
        pressed = false;
        const double old = target;
        double to = old != 0 ? 0 : 1;
        if (dragged && std::abs(value - old) < 0.2)
            to = old;
        setState(to >= 0.5, true);
        setNeedsDisplay();
        if (action)
            action(to >= 0.5 ? 1 : 0);
    }
};

// TMOnOffButton -drawRect: "OFF" / "ON" centred, a white copy (0.75) one point under, the title black
// at 0.7 (0.35 disabled); ON lit (-setShouldLightUp:) in a teal gradient while on
struct OnOffLabel : View {
    std::string title;
    Font font = Font::boldSystem(13);
    bool lightUp = false, on = false;
    std::function<void()> action;
    void draw(cairo_t *cr) override
    {
        TextStyle t;
        t.font = font;
        t.align = Align::Center;
        t.elide = false;
        const double h = lineHeight(font);
        const Rect r{0, std::floor((frame().h - h) / 2), frame().w, h};
        t.color = Color::gray(255, 0.75);
        drawText(cr, title, Rect{r.x, r.y + 1, r.w, r.h}, t);
        if (on && lightUp) {
            // NSGradient from (0.29, 0.59, 0.59) at the bottom to (0, 0.21, 0.22) at the top, angle 90
            cairo_push_group(cr);
            t.color = Color::gray(0);
            drawText(cr, title, r, t);
            cairo_pattern_t *text = cairo_pop_group(cr);
            cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
            cairo_pattern_add_color_stop_rgba(g, 0, 0, 0.2078, 0.2157, isEnabled() ? 1 : 0.5);
            cairo_pattern_add_color_stop_rgba(g, 1, 0.2914, 0.5882, 0.5920, isEnabled() ? 1 : 0.5);
            cairo_set_source(cr, g);
            cairo_mask(cr, text);
            cairo_pattern_destroy(g);
            cairo_pattern_destroy(text);
        } else {
            t.color = Color::gray(0, isEnabled() ? 0.7 : 0.35);
            drawText(cr, title, r, t);
        }
    }
    void mouseUp(Event &e) override
    {
        if (isEnabled() && bounds().contains(e.location) && action)
            action();
    }
};

// TMEmbossedPushButton -_createImage: (its image and alternate image): LeftCap, UpFill stretched,
// RightCap (LeftCapDown, DownFill, RightCapDown pressed), 25 high; the title centred on its line, a
// white copy (0.8) one point under; drawn over the nib's button, whose action it sends
struct EmbossedButton : View {
    Button *button = nullptr;
    Image left, fill, right, leftDown, fillDown, rightDown;
    bool pressed = false, inside = false;
    void draw(cairo_t *cr) override
    {
        const Image &l = pressed && inside ? leftDown : left, &f = pressed && inside ? fillDown : fill, &r = pressed && inside ? rightDown : right;
        if (!l.isNull() && !f.isNull() && !r.isNull()) {
            l.draw(cr, Point{0, 0});
            f.draw(cr, Rect{double(l.width()), 0, frame().w - l.width() - r.width(), double(f.height())});
            r.draw(cr, Point{frame().w - r.width(), 0});
        }
        TextStyle t;
        t.font = button ? button->font : Font::system(13);
        t.align = Align::Center;
        const double h = lineHeight(t.font), y = std::floor((frame().h - h) / 2);
        t.color = Color::gray(255, 0.8);
        drawText(cr, button ? button->title : std::string(), Rect{0, y + 1, frame().w, h}, t);
        t.color = Color::gray(0, isEnabled() ? 1 : 0.5);
        drawText(cr, button ? button->title : std::string(), Rect{0, y, frame().w, h}, t);
    }
    void mouseDown(Event &) override
    {
        if (!isEnabled())
            return;
        pressed = inside = true;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        if (inside != bounds().contains(e.location)) {
            inside = !inside;
            setNeedsDisplay();
        }
    }
    void mouseUp(Event &e) override
    {
        const bool fire = pressed && bounds().contains(e.location);
        pressed = inside = false;
        setNeedsDisplay();
        if (fire && button && button->action)
            button->action();
    }
};

// ---- the pane

struct TimeMachinePane {
    NibPane &p;
    Settings conf{"timemachine"};
    TabView *tabs = nullptr;
    SliderSwitch *slider = nullptr;
    OnOffLabel *onLabel = nullptr, *offLabel = nullptr;
    Label *name = nullptr, *available = nullptr, *oldest = nullptr, *latest = nullptr, *next = nullptr, *nextLabel = nullptr,
          *progressText = nullptr, *server = nullptr;
    ProgressIndicator *progress = nullptr;
    View *stop = nullptr, *errorInfo = nullptr;
    ImageView *destIcon = nullptr;
    CheckBox *menuExtra = nullptr;
    Nib *destinationNib = nullptr, *optionsNib = nullptr;
    int restartSeconds = 0;   // -initiateRestartCountdown: the backup coming
    int countdownTimer = 0;
    long long lastAvailable = 0;
    std::time_t availableAt = 0;

    explicit TimeMachinePane(NibPane &pane) : p(pane) {}
    ~TimeMachinePane()
    {
        if (countdownTimer)
            App::shared().removeTimer(countdownTimer);
    }

    std::string backupPath() const { return Settings("timemachine").string("BackupPath"); }
    bool hasDestination() const { return !backupPath().empty() || !Settings("timemachine").string("BackupAlias").empty(); }
    // AppleTMSettings -backupPath: nil while the alias doesn't resolve (the disk not mounted)
    bool reachable() const { return hasDestination() && isDir(backupPath()); }
    bool autoBackup() const { return Settings("timemachine").boolean("AutoBackup", false); }
    void setAutoBackup(bool on) { Settings("timemachine").set("AutoBackup", on); }

    Window *window() const { return p.window(); }

    void setup()
    {
        tabs = p.nib.outlet<TabView>("_tabView");
        name = p.nib.outlet<Label>("_destinationNameTextField");
        available = p.nib.outlet<Label>("_availableSpaceTextField");
        oldest = p.nib.outlet<Label>("_oldestBackupTextField");
        latest = p.nib.outlet<Label>("_latestBackupTextField");
        next = p.nib.outlet<Label>("_nextBackupTextField");
        nextLabel = p.nib.outlet<Label>("_nextBackupLabelTextField");
        progressText = p.nib.outlet<Label>("_progressTextField");
        server = p.nib.outlet<Label>("_destinationServerTextField");
        progress = p.nib.outlet<ProgressIndicator>("_progressIndicator");
        stop = p.nib.outlet("_stopButton");
        errorInfo = p.nib.outlet("_errorInfoButton");
        destIcon = p.nib.outlet<ImageView>("_destinationIconImageView");
        menuExtra = p.nib.outlet<CheckBox>("_menuExtraCheckbox");
        if (server)
            server->setHidden(true);   // a network disk's server only
        if (auto *choose = p.nib.outlet<ImageView>("_chooseDestinationImageView"))
            choose->image = Image::load(Assets::path(std::string(CoreTypes) + "GenericTimeMachineDiskIcon.icns"), {64, 64});
        if (progress)
            progress->setHidden(true);

        // the switch in its custom view, the OFF and ON titles over the nib's buttons
        if (View *v = p.nib.outlet("_onOffSliderView")) {
            slider = v->add<SliderSwitch>();
            slider->setFrame(v->bounds());
            slider->well = p.nib.image("SliderWell");
            slider->handle = p.nib.image("SliderHandle");
            slider->handleDown = p.nib.image("SliderHandleDown");
            slider->mask = p.nib.image("OverlayMask");
            slider->action = [this](int s) { setAutoBackupState(s); };   // -onOffSliderPressed:
        }
        auto overlay = [&](const std::string &outlet, bool lightUp, int state) -> OnOffLabel * {
            View *b = p.nib.outlet(outlet);
            if (!b || !b->superview())
                return nullptr;
            auto *l = b->superview()->add<OnOffLabel>();
            l->setFrame(b->frame());
            if (auto *button = dynamic_cast<Button *>(b)) {
                l->title = button->title;
                l->font = Font::boldSystem(std::max(13.0, button->font.size));
            }
            l->lightUp = lightUp;
            l->action = [this, state] { setAutoBackupState(state); };   // -onPressed:, -offPressed:
            b->setHidden(true);
            return l;
        };
        onLabel = overlay("_onButton", true, 1);
        offLabel = overlay("_offButton", false, 0);

        // Select Backup Disk…, Select Disk…, Options…: TMEmbossedPushButton
        for (const char *action : {"showDestinationSheet:", "showOptionsSheet:"})
            for (View *v : p.nib.withAction(action))
                if (auto *b = dynamic_cast<Button *>(v); b && b->superview()) {
                    auto *e = b->superview()->add<EmbossedButton>();
                    e->button = b;
                    e->left = p.nib.image("LeftCap");
                    e->fill = p.nib.image("UpFill");
                    e->right = p.nib.image("RightCap");
                    e->leftDown = p.nib.image("LeftCapDown");
                    e->fillDown = p.nib.image("DownFill");
                    e->rightDown = p.nib.image("RightCapDown");
                    const Rect f = b->frame();
                    e->setFrame({f.x, f.y + std::floor((f.h - 25) / 2), f.w, 25});
                    b->setHidden(true);
                }
        // TMEmbossedTextField: NSBackgroundStyleRaised, the labels engraved
        if (tabs)
            for (int i = 0; i < tabs->count(); ++i) {
                std::function<void(View *)> engrave = [&](View *v) {
                    if (auto *l = dynamic_cast<Label *>(v))
                        l->style.engraved = true;
                    for (View *c : v->subviews())
                        engrave(c);
                };
                engrave(tabs->page(i));
            }
        for (View *b : p.nib.withAction("showDestinationSheet:"))
            then(b, [this] { reopenDestinationSheet(); });
        for (View *b : p.nib.withAction("showOptionsSheet:"))
            then(b, [this] { App::shared().later([this] { showOptionsSheet(); }); });
        if (stop)
            then(stop, [this] { stopPressed(); });
        if (menuExtra)
            then(menuExtra, [this] {   // -menuExtraCheckboxPressed:
                showMenuExtra(TimeMachineExtra, menuExtra->checked());
                updateMenuExtraGUI();
            });
        updateGUI(true);
        // TMSystemBackup's notifications, the preferences' (com.apple.backup.prefschanged): followed
        p.every(1.0, [this] { updateGUI(false); });
    }

    // -_setAutoBackupState:
    void setAutoBackupState(int s)
    {
        if (s == 1) {
            if (!hasDestination()) {
                if (slider)
                    slider->setState(1, false);
                reopenDestinationSheet();
                return;
            }
            setAutoBackup(true);
            updateAutoBackupGUI(true);
        } else {
            setAutoBackup(false);
            updateAutoBackupGUI(true);
            cancelRestartCountdown();
            stopBackingUp();
        }
        updateNextBackupGUI();
    }

    void updateGUI(bool initial)
    {
        // -updateGUI: not while a sheet is over the window
        if (window() && Sheet::isOpen(window()))
            return;
        updateAutoBackupGUI(!initial);
        updateBackupDestinationGUI();
        updateAvailableSpaceGUI();
        updateOldestBackupGUI();
        updateLatestBackupGUI();
        updateNextBackupGUI();
        updateMenuExtraGUI();
    }

    void updateAutoBackupGUI(bool animate)
    {
        const bool on = autoBackup();
        if (slider && !slider->pressed)
            slider->setState(on, animate);
        for (OnOffLabel *l : {onLabel, offLabel})
            if (l) {
                const bool lit = (l == onLabel) == on;
                if (l->on != lit) {
                    l->on = lit;
                    l->setNeedsDisplay();
                }
            }
    }

    static void setText(Label *l, const std::string &s, Color c = Color::gray(0))
    {
        if (!l)
            return;
        if (l->text() != s)
            l->setText(s);
        if (l->style.color.r != c.r || l->style.color.g != c.g || l->style.color.b != c.b) {
            l->style.color = c;
            l->setNeedsDisplay();
        }
    }

    // -updateBackupDestinationGUI: the ChooseDisk tab without a disk, Settings with one
    void updateBackupDestinationGUI()
    {
        if (!tabs)
            return;
        if (!hasDestination()) {
            if (tabs->selected() != 0)
                tabs->select(0);
            return;
        }
        if (tabs->selected() != 1)
            tabs->select(1);
        // -destinationDisplayName: the volume's display name, the path's last component
        const std::string path = backupPath();
        std::string display;
        if (!path.empty()) {
            const std::vector<Mount> all = mounts();
            const Mount *m = mountAt(all, realPath(path));
            display = m && reachable() ? volumeName(m->device, m->point) : fileName(path);
        }
        setText(name, display, reachable() ? Color::gray(0) : Color::gray(128));
        if (destIcon) {
            // -destinationIcon: the path's icon while readable, else the generic disk's
            const std::string key = reachable() ? path : std::string();
            if (destIcon->image.isNull() || destIconPath != key) {
                destIconPath = key;
                destIcon->image = reachable() ? iconForPath(path, 64) : Image::load(Assets::path(std::string(Storage) + "External.icns"), {64, 64});
                destIcon->setNeedsDisplay();
            }
        }
    }
    std::string destIconPath = "-";

    // -updateAvailableSpaceGUI: "%@ of %@" (free, total), every 60 seconds
    void updateAvailableSpaceGUI()
    {
        if (!reachable()) {
            setText(available, "");
            return;
        }
        const std::time_t now = std::time(nullptr);
        if (available && !available->text().empty() && now - availableAt < 60 && lastPath == backupPath())
            return;
        availableAt = now;
        lastPath = backupPath();
        Volume v;
        v.path = backupPath();
        measure(v);
        setText(available, format(tr("AVAILABLE_AND_TOTAL_DISK_SPACE_FORMAT", "%@ of %@"), {bytesString(v.free), bytesString(v.total)}));
    }
    std::string lastPath;

    // -updateOldestBackupGUI: the oldest snapshot, else "--" (the disk there) or nothing
    void updateOldestBackupGUI()
    {
        const std::vector<std::time_t> d = reachable() ? snapshotDates(backupPath()) : std::vector<std::time_t>{};
        setText(oldest, !d.empty() ? dateString(d.front()) : reachable() ? "--" : "");
    }

    // TMSystemBackup -shortErrorMessage: the last result's (as the menu extra's -_refreshBackupIdle)
    std::string shortErrorMessage() const
    {
        Settings r("TimeMachine.Results");
        const std::string code = r.string("RESULT");
        if (code.empty())
            return {};
        const int c = std::atoi(code.c_str());
        if (c == 0 || c == 13 || c == 22)
            return {};
        if (c == 18 || c == 19 || c == 21)
            return tr("DELAYED", "Delayed");
        return tr("FAILED", "Failed");
    }

    std::time_t lastSuccessfulBackup() const
    {
        Settings r("TimeMachine.Results");
        const long long t = std::atoll(r.string("BACKUP_COMPLETED_DATE", "0").c_str());
        if (t > 0)
            return std::time_t(t);
        const std::vector<std::time_t> d = reachable() ? snapshotDates(backupPath()) : std::vector<std::time_t>{};
        return d.empty() ? 0 : d.back();
    }

    // -updateLatestBackupGUI
    void updateLatestBackupGUI()
    {
        const bool inProgress = backupState().state != 0;
        const std::string error = inProgress ? std::string() : shortErrorMessage();
        if (!error.empty()) {
            setText(latest, error, Color::rgb(169, 0, 0));   // (0.663, 0, 0)
            if (errorInfo)
                errorInfo->setHidden(true);   // -longErrorMessage: none here
            return;
        }
        const std::time_t last = lastSuccessfulBackup();
        setText(latest, last ? dateString(last) : reachable() ? "--" : "");
        if (errorInfo)
            errorInfo->setHidden(true);
    }

    // AppleTMSettings -nextBackupDate: CSBackupSettingsCopyNextBackupDate, the last backup and
    // com.apple.backupd-auto's StartInterval (3600); past, the shell's backupd --daemon (it looks
    // every minute) backs up within the minute
    std::time_t nextBackupDate() const
    {
        const std::time_t last = lastSuccessfulBackup();
        const std::time_t now = std::time(nullptr);
        if (last && last + 3600 > now)
            return last + 3600;
        return now - now % 60 + 60;
    }

    // -updateNextBackupGUI
    void updateNextBackupGUI()
    {
        const BackupState s = backupState();
        if (s.state == 0) {
            if (restartSeconds <= 0) {
                if (stop)
                    stop->setHidden(true);
                setText(nextLabel, tr("NEXT_BACKUP", "Next Backup:"));
                setText(progressText, "");
                Settings conf("timemachine");
                if (!autoBackup() || !hasDestination())
                    setText(next, tr("NEXT_BACKUP_TURNED_OFF"));
                else if (conf.boolean("RequiresACPower", false) && onBattery())
                    setText(next, tr("NEXT_BACKUP_WHEN_PLUGGED_IN"));
                else if (!reachable())
                    setText(next, tr("NEXT_BACKUP_WHEN_CONNECTED"));
                else
                    setText(next, dateString(nextBackupDate()));
            } else {
                // the restart countdown: "120 seconds…", "one second…"
                setText(next, restartSeconds == 1 ? tr("WILL_START_IN_ONE_SEC") : format(tr("WILL_START_IN_X_SECS"), {std::to_string(restartSeconds)}));
                if (stop)
                    stop->setHidden(false);
                setText(nextLabel, tr("NEXT_BACKUP", "Next Backup:"));
                setText(progressText, "");
            }
            if (progress) {
                progress->setHidden(true);
                progress->stop();
            }
            if (next)
                next->setHidden(false);
            return;
        }
        std::string text;
        bool indeterminate = true;
        switch (s.state) {
        case 1: text = tr("STOPPING"); break;
        case 2: text = tr("PREPARING"); break;
        case 4: text = tr("FINISHING"); break;
        case 5:
        case 6: text = tr("THINNING"); break;
        case 7: text = tr("INDEXING"); break;
        case 8:
            text = tr("CALCULATING_SIZE");
            indeterminate = !(s.percent > 0 && s.percent <= 1);
            break;
        case 9: text = tr("MOUNTING"); break;
        case 10: text = tr("DELETING"); break;
        default:
            // copying: "%1$@ of %2$@" while the total is known
            if (s.total >= 1 && s.total >= s.done) {
                indeterminate = false;
                text = format(tr("BYTES_COMPLETED_AND_TOTAL_FORMAT", "%1$@ of %2$@"), {bytesString(s.done), bytesString(s.total)});
            }
        }
        if (stop)
            stop->setHidden(s.state == 1);
        setText(progressText, text);
        setText(nextLabel, tr("BACKING_UP", "Backing Up:"));
        if (next)
            next->setHidden(true);
        if (progress) {
            progress->indeterminate = indeterminate;
            progress->minValue = 0;
            progress->maxValue = 1;
            progress->value = s.percent;
            progress->setHidden(false);
            progress->start();
            progress->setNeedsDisplay();
        }
    }

    // -updateMenuExtraGUI: CoreMenuExtraGetMenuExtra(com.apple.menuextra.TimeMachine)
    void updateMenuExtraGUI()
    {
        if (menuExtra && menuExtra->checked() != menuExtraShown(TimeMachineExtra))
            menuExtra->setChecked(menuExtraShown(TimeMachineExtra));
    }

    // TMSystemBackup -backupNow: / -stopBackingUp (the menu extra's: backupd --now, SIGTERM)
    void backupNow()
    {
        const std::string backupd = appsDir() + "/backupd.bundle/Contents/Resources/backupd";
        if (isFile(backupd))
            Task::launchDetached({backupd, "--now"});
    }
    void stopBackingUp()
    {
        const BackupState s = backupState();
        if (s.pid > 0 && kill(s.pid, SIGTERM) == 0)
            Settings("backupd.status").set("Stopping", true);
    }

    // -initiateRestartCountdown, -restartCountdownTimerFired:: the first backup after a disk is
    // chosen (or the exclusions changed) starts after a countdown, the stop button ending it
    void initiateRestartCountdown()
    {
        cancelRestartCountdown();
        restartSeconds = 120;   // _restartInterval 120.0, the timer every second
        stopBackingUp();
        countdownTimer = App::shared().addTimer(1.0, true, [this] {
            if (window() && Sheet::isOpen(window()))
                return;   // -pauseRestartCountdown while a sheet is up
            if (--restartSeconds <= 0) {
                cancelRestartCountdown();
                backupNow();
            }
            updateNextBackupGUI();
        });
        updateNextBackupGUI();
    }
    void cancelRestartCountdown()
    {
        if (countdownTimer)
            App::shared().removeTimer(countdownTimer);
        countdownTimer = 0;
        restartSeconds = 0;
    }

    // -stopPressed:: the countdown cancelled, else the backup stopped
    void stopPressed()
    {
        if (restartSeconds > 0)
            cancelRestartCountdown();
        else
            stopBackingUp();
        updateNextBackupGUI();
    }

    Nib *part(Nib *&cache, const char *name)
    {
        if (!cache)
            cache = p.loadPart(name);
        return cache;
    }

    // a sheet of a part's window: its closing buttons end it, `ended` after it's gone
    void begin(View *content, Size size, std::function<void()> ended)
    {
        Sheet::begin(window(), content, size, p.toWindow({0, 0}).y, std::move(ended));
    }

    // ---- TMDestinationSheetController

    struct DestinationSheet {
        std::vector<Volume> volumes;
        bool none = false;   // the "None" row (a disk already set): Stop Backing Up
        std::string chosen;  // the path confirmed with the button
        bool confirmed = false;
        bool pickFolder = false;   // the "Other Folder…" row: a folder on any disk (not 10.6's)
        Image deleteIcon, folderIcon;
    };

    void showDestinationSheet()
    {
        Nib *nib = part(destinationNib, "TMDestinationSheet");
        if (!nib || !window() || Sheet::isOpen(window()))
            return;
        Size size;
        View *content = nib->instantiateWindow(nib->outletObject("_destinationSheet"), &size);
        if (!content)
            return;
        auto sheet = std::make_shared<DestinationSheet>();
        sheet->deleteIcon = p.nib.image("Delete");
        sheet->folderIcon = Image::load(Assets::path(std::string(CoreTypes) + "GenericFolderIcon.icns"), {32, 32});
        // -updateTableViewContents (the local volumes then the read-only ones), the None row
        sheet->volumes = destinations(backupPath());
        sheet->none = hasDestination();
        auto *table = nib->outlet<TableView>("_destinationsTableView");
        auto *ok = nib->outlet<Button>("_okButton");
        auto *cancel = nib->outlet<Button>("_cancelButton");
        // -_isTimeCapsuleUtilityInstalled: AirPort Utility isn't there
        if (View *tc = nib->outlet("_setUpTimeCapsuleButton"))
            tc->setHidden(true);
        if (table) {
            table->rows.clear();
            for (const Volume &v : sheet->volumes)
                table->rows.push_back({"", v.name, v.free > 0 ? bytesString(v.free) : ""});
            // not 10.6's: a folder of the user's choosing, for a computer with its startup disk only
            table->rows.push_back({"", tr("DISK_PICKER_OTHER_FOLDER", "Other Folder\u2026"), ""});
            if (sheet->none)
                table->rows.push_back({"", tr("DISK_PICKER_NONE", "None"), ""});
            // -updateTableViewSelection: the disk set now
            table->selected = -1;
            const std::string current = backupPath().empty() ? std::string() : realPath(backupPath());
            for (size_t i = 0; i < sheet->volumes.size(); ++i)
                if (!current.empty() && realPath(sheet->volumes[i].path) == current)
                    table->selected = int(i);
            table->drawCell = [sheet, table](cairo_t *cr, int row, int column, const Rect &cell, bool highlighted) {
                const bool folderRow = size_t(row) == sheet->volumes.size();
                const bool noneRow = size_t(row) > sheet->volumes.size();
                if (column == 0) {
                    // the volume's icon (-iconForPath:, else -genericDiskIcon), the None row's Delete.tiff
                    Image icon = noneRow ? sheet->deleteIcon : folderRow ? sheet->folderIcon : iconForPath(sheet->volumes[size_t(row)].path, 32);
                    if (!icon.isNull()) {
                        const Size s = scaledToFit(icon.size(), {32, 32});
                        icon.draw(cr, Rect{cell.x + std::floor((cell.w - s.w) / 2), cell.y + std::floor((cell.h - s.h) / 2), s.w, s.h});
                    }
                    return true;
                }
                TextStyle t;
                t.font = Font::system(13);
                t.color = highlighted ? Color::gray(255) : Color::gray(0);
                if (column == 2)
                    t.align = Align::Right;
                const std::string &text = size_t(row) < table->rows.size() && size_t(column) < table->rows[size_t(row)].size()
                                              ? table->rows[size_t(row)][size_t(column)] : std::string();
                drawText(cr, text, Rect{cell.x + 3, cell.y, cell.w - 8, cell.h}, t);
                return true;
            };
            table->reload();
        }
        // -updateButtons: "Use for Backup" while a disk is chosen, "Stop Backing Up" on None
        auto updateButtons = [this, sheet, table, ok] {
            if (!ok || !table)
                return;
            const int row = table->selected;
            const bool disk = row >= 0 && size_t(row) <= sheet->volumes.size();
            ok->setEnabled(hasDestination() || row >= 0);
            ok->title = disk || !hasDestination() ? tr("DISK_PICKER_BUTTON_USE", "Use for Backup") : tr("DISK_PICKER_BUTTON_STOP", "Stop Backing Up");
            if (!hasDestination())
                ok->setEnabled(row >= 0);
            ok->setNeedsDisplay();
        };
        updateButtons();
        if (table)
            then(table, updateButtons);   // -tableViewSelectionDidChange:
        Window *w = window();
        // -okButtonPressed:: the chosen row's path, nil for None
        auto press = [w, sheet, table, ok] {
            if (!ok || !ok->isEnabled() || !table)
                return;
            const int row = table->selected;
            sheet->chosen = row >= 0 && size_t(row) < sheet->volumes.size() ? sheet->volumes[size_t(row)].path : std::string();
            sheet->pickFolder = row >= 0 && size_t(row) == sheet->volumes.size();
            sheet->confirmed = true;
            Sheet::end(w);
        };
        if (ok) {
            ok->action = press;
            ok->isDefault = true;
        }
        if (table)
            table->onDoubleClick = [press](int) { press(); };
        if (cancel)
            cancel->action = [w] { Sheet::end(w); };   // -cancelButtonPressed:
        begin(content, size, [this, sheet] {
            if (sheet->confirmed && sheet->pickFolder) {
                App::shared().later([this, sheet] { chooseBackupFolder(*sheet); });
                return;
            }
            if (sheet->confirmed)
                confirmBackupPath(sheet->chosen, *sheet);
            updateGUI(false);
        });
    }

    // the destination sheet again once the sheet in its place (an alert) has finished sliding away:
    // Sheet::end gives the first responder back and removes its host on the run loop's next turn
    // after it stops being open, so the new sheet comes one turn after that
    void reopenDestinationSheet()
    {
        if (window() && Sheet::isOpen(window())) {
            App::shared().addTimer(0.05, false, [this] { reopenDestinationSheet(); });
            return;
        }
        App::shared().later([this] { showDestinationSheet(); });
    }

    // "Other Folder…": the folder chosen, then confirmed as a disk would be (the same-device warning
    // when it is on the disk backed up)
    void chooseBackupFolder(const DestinationSheet &sheet)
    {
        OpenPanel::chooseDirectory(tr("DISK_PICKER_BUTTON_USE", "Use for Backup"), home(), [this, sheet](std::string dir) {
            if (dir.empty()) {
                reopenDestinationSheet();
                return;
            }
            dir = realPath(dir);
            // the folder backed up (the home), or one holding it, can't take its own backups; nor
            // a folder that can't be written to
            const std::string source = realPath(home());
            const bool holdsSource = dir == source || dir == "/" || source.rfind(dir + "/", 0) == 0;
            if (holdsSource || access(dir.c_str(), W_OK) != 0) {
                Alert::beginSheet(window(), p.info.icon, "\u201c" + fileName(dir) + "\u201d can\u2019t be used for backups.",
                                  holdsSource ? "It holds the folder being backed up. Choose a folder inside it, or on another disk."
                                              : "You don\u2019t have permission to write to it.",
                                  {tr("CHOOSE_ANOTHER_DISK_BUTTON", "Select Another Volume")},
                                  [this](int) { reopenDestinationSheet(); });
                return;
            }
            DestinationSheet copy = sheet;
            Volume v;
            v.path = dir;
            v.name = fileName(dir);
            v.mountPoint = mountPointOf(dir) == dir;
            const std::vector<Mount> all = mounts();
            if (const Mount *m = mountAt(all, mountPointOf(dir)))
                v.device = realPath(m->device);
            copy.volumes.push_back(v);
            App::shared().later([this, dir, copy] { confirmBackupPath(dir, copy); });
        });
    }

    // -confirmBackupPath:: None stops backing up; a disk: the same-device warning
    // (-_sourceDisksConflictingWithDestinationDisk:), then -setBackupPath:error:, automatic backups on,
    // the restart countdown
    void confirmBackupPath(const std::string &path, const DestinationSheet &sheet, bool conflictAccepted = false)
    {
        if (path.empty()) {
            Settings("timemachine").set("BackupPath", "");
            setAutoBackup(false);
            cancelRestartCountdown();
            updateGUI(false);
            return;
        }
        if (!conflictAccepted) {
            const Volume *v = nullptr;
            for (const Volume &x : sheet.volumes)
                if (x.path == path)
                    v = &x;
            const std::vector<Mount> all = mounts();
            const Mount *source = mountAt(all, mountPointOf(home()));
            if (v && !v->device.empty() && source && source->device.rfind("/dev/", 0) == 0 && wholeDisk(v->device) == wholeDisk(source->device)) {
                // -_displayDeviceConflictAlertForSourceDisks:destinationDisk:
                // the volume backed up, as backupd names it: its label, else "Macintosh HD"
                const std::map<std::string, std::string> names = labels();
                auto label = names.find(realPath(source->device));
                const std::string sourceName = label != names.end() ? label->second : std::string("Macintosh HD");
                std::string text = format(tr("WARNING_SAME_DISK_ONE_CONFLICT"), {v->name, sourceName});
                while (!text.empty() && text.back() == '\n')
                    text.pop_back();
                const DestinationSheet copy = sheet;
                Alert::beginSheet(window(), p.info.icon, tr("WARNING_SAME_DISK_TITLE"), text,
                                  {tr("USE_SELECTED_DISK_BUTTON"), tr("CHOOSE_ANOTHER_DISK_BUTTON")}, [this, path, copy](int b) {
                                      if (b == 0)
                                          App::shared().later([this, path, copy] { confirmBackupPath(path, copy, true); });
                                      else
                                          reopenDestinationSheet();
                                  });
                return;
            }
        }
        // -setBackupPath:error:: the alias, the results of the previous disk removed
        // (/var/db/.TimeMachine.Results.plist)
        Settings conf("timemachine");
        conf.set("BackupPath", path);
        unlink((configDir() + "/TimeMachine.Results.conf").c_str());
        setAutoBackup(true);
        initiateRestartCountdown();
        updateGUI(false);
    }

    // ---- TMOptionsSheetController

    struct Sizes {
        std::mutex lock;
        std::map<std::string, long long> bytes;   // -1 while calculating
        long long total = -1;                     // the home, the exclusions taken out
        std::atomic<bool> cancelled{false};
    };

    static long long folderSize(const std::string &path, const std::atomic<bool> &cancelled, dev_t dev = 0)
    {
        struct stat st;
        if (cancelled || lstat(path.c_str(), &st) != 0)
            return 0;
        if (!dev)
            dev = st.st_dev;
        if (!S_ISDIR(st.st_mode))
            return S_ISREG(st.st_mode) ? (long long)st.st_blocks * 512 : 0;
        if (st.st_dev != dev)
            return 0;
        long long sum = (long long)st.st_blocks * 512;
        if (DIR *d = opendir(path.c_str())) {
            while (dirent *e = readdir(d)) {
                if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                    continue;
                sum += folderSize(path + "/" + e->d_name, cancelled, dev);
                if (cancelled)
                    break;
            }
            closedir(d);
        }
        return sum;
    }

    void showOptionsSheet()
    {
        Nib *nib = part(optionsNib, "TMOptionsSheet");
        if (!nib || !window() || Sheet::isOpen(window()))
            return;
        Size size;
        View *content = nib->instantiateWindow(nib->outletObject("_optionsSheet"), &size);
        if (!content)
            return;
        Settings conf("timemachine");
        // -showOptionsSheet:: automatic backups off while it's up (put back by Done or Cancel)
        const bool previousAutoBackup = autoBackup();
        setAutoBackup(false);
        cancelRestartCountdown();
        struct State {
            std::vector<std::string> skipped, toAdd, toRemove, items;
            std::shared_ptr<Sizes> sizes = std::make_shared<Sizes>();
            int timer = 0;
            bool done = false;
        };
        auto st = std::make_shared<State>();
        for (const char *key : {"SkipPaths", "ExcludeByPath"})
            for (std::string s : conf.list(key)) {
                if (s.rfind("~/", 0) == 0)
                    s = home() + s.substr(1);
                if (std::find(st->skipped.begin(), st->skipped.end(), s) == st->skipped.end())
                    st->skipped.push_back(s);
            }
        auto *table = nib->outlet<TableView>("_skippedItemsTableView");
        auto *battery = nib->outlet<CheckBox>("_backupOnBatteryPowerCheckbox");
        auto *warn = nib->outlet<CheckBox>("_warnCullingCheckbox");
        auto *total = nib->outlet<Label>("_totalIncludedTextField");
        View *addRemove = nib->outlet("_addRemoveButtons");
        if (battery)
            battery->setChecked(!conf.boolean("RequiresACPower", false));   // -requiresACPower: absent is NO
        if (warn)
            warn->setChecked(conf.boolean("AlwaysShowDeletedBackupsWarning", true));   // absent is YES
        if (!hasBattery() && battery && !battery->isHidden()) {
            battery->setHidden(true);
            if (addRemove)
                addRemove->setFrame({addRemove->frame().x, addRemove->frame().y + 20, addRemove->frame().w, addRemove->frame().h});
            if (table)
                table->setFrame({table->frame().x, table->frame().y, table->frame().w, table->frame().h + 20});
            if (total)
                total->setFrame({total->frame().x, total->frame().y + 20, total->frame().w, total->frame().h});
        }
        Button *add = nullptr, *remove = nullptr;
        if (addRemove) {
            // the + and − under the list
            for (View *v : addRemove->subviews())
                v->setHidden(true);
            add = addRemove->add<Button>("+", Button::SmallSquare);
            add->setFrame({0, 0, 23, std::min(22.0, addRemove->frame().h)});
            remove = addRemove->add<Button>("−", Button::SmallSquare);
            remove->setFrame({22, 0, 23, std::min(22.0, addRemove->frame().h)});
        }
        // sizes: TMFolderSizerOperation, TMTotalIncludedSizerOperation, on their own thread
        auto measureAll = [st] {
            std::vector<std::string> want;
            {
                std::lock_guard<std::mutex> g(st->sizes->lock);
                for (const std::string &i : st->items)
                    if (!st->sizes->bytes.count(i)) {
                        st->sizes->bytes[i] = -1;
                        want.push_back(i);
                    }
                st->sizes->total = -1;
            }
            std::shared_ptr<Sizes> sizes = st->sizes;
            std::vector<std::string> items = st->items;
            std::thread([sizes, want, items] {
                for (const std::string &w : want) {
                    const long long n = folderSize(w, sizes->cancelled);
                    std::lock_guard<std::mutex> g(sizes->lock);
                    sizes->bytes[w] = n;
                }
                // the estimated full backup: the home without what's excluded inside it
                long long all = folderSize(home(), sizes->cancelled);
                std::lock_guard<std::mutex> g(sizes->lock);
                for (const std::string &i : items)
                    if (i.rfind(home() + "/", 0) == 0 && sizes->bytes[i] > 0)
                        all -= sizes->bytes[i];
                sizes->total = std::max(0LL, all);
            }).detach();
        };
        // -updateTableViewContents: the skipped items, those added and removed here, the backup disk
        auto reload = [this, st, table, remove, measureAll] {
            st->items.clear();
            for (const std::string &s : st->skipped)
                if (std::find(st->toRemove.begin(), st->toRemove.end(), s) == st->toRemove.end())
                    st->items.push_back(s);
            for (const std::string &s : st->toAdd)
                if (std::find(st->items.begin(), st->items.end(), s) == st->items.end())
                    st->items.push_back(s);
            const std::string disk = backupPath();
            if (!disk.empty() && std::find(st->items.begin(), st->items.end(), disk) == st->items.end())
                st->items.push_back(disk);
            // only what exists (-fileExistsAtPath:)
            st->items.erase(std::remove_if(st->items.begin(), st->items.end(), [](const std::string &s) {
                                struct stat sb;
                                return lstat(s.c_str(), &sb) != 0;
                            }),
                            st->items.end());
            std::sort(st->items.begin(), st->items.end());
            if (!table)
                return;
            table->rows.clear();
            for (const std::string &i : st->items) {
                // a volume by its name, else the path with ~
                std::string n = i;
                if (mountPointOf(i) == realPath(i)) {
                    const std::vector<Mount> all = mounts();
                    if (const Mount *m = mountAt(all, realPath(i)))
                        n = volumeName(m->device, m->point);
                } else if (i == home() || i.rfind(home() + "/", 0) == 0) {
                    n = "~" + i.substr(home().size());
                }
                table->rows.push_back({"", n, ""});
            }
            if (table->selected >= int(table->rows.size()))
                table->selected = -1;
            table->reload();
            if (remove)
                remove->setEnabled(table->selected >= 0);   // -updateAddRemoveButtons
            measureAll();
        };
        if (table) {
            table->drawCell = [st, table](cairo_t *cr, int row, int column, const Rect &cell, bool highlighted) {
                if (size_t(row) >= st->items.size())
                    return false;
                const std::string &item = st->items[size_t(row)];
                if (column == 0) {
                    const Image icon = iconForPath(item, 16);
                    if (!icon.isNull())
                        icon.draw(cr, Rect{cell.x + std::floor((cell.w - 16) / 2), cell.y + std::floor((cell.h - 16) / 2), 16, 16});
                    return true;
                }
                TextStyle t;
                t.font = Font::system(11);
                t.color = highlighted ? Color::gray(255) : Color::gray(0);
                std::string text;
                if (column == 1) {
                    text = size_t(row) < table->rows.size() ? table->rows[size_t(row)][1] : item;
                } else {
                    // "Calculating Size" while measuring, else the size
                    long long b = -1;
                    {
                        std::lock_guard<std::mutex> g(st->sizes->lock);
                        auto f = st->sizes->bytes.find(item);
                        if (f != st->sizes->bytes.end())
                            b = f->second;
                    }
                    text = b < 0 ? tr("EXCLUDE_LIST_CALCULATING_SIZE", "Calculating Size") : bytesString(b);
                    t.align = Align::Right;
                }
                drawText(cr, text, Rect{cell.x + 3, cell.y, cell.w - 6, cell.h}, t);
                return true;
            };
            then(table, [table, remove] {
                if (remove)
                    remove->setEnabled(table->selected >= 0);
            });
        }
        reload();
        // -updateTotalIncluded: "Estimated size of full backup: %@", "Calculating size of full backup…"
        st->timer = App::shared().addTimer(0.5, true, [st, table, total] {
            long long t;
            {
                std::lock_guard<std::mutex> g(st->sizes->lock);
                t = st->sizes->total;
            }
            if (total) {
                const std::string s = t < 0 ? tr("CALCULATING") : format(tr("INCLUDED"), {bytesString(t)});
                if (total->text() != s)
                    total->setText(s);
            }
            if (table)
                table->setNeedsDisplay();
        });
        Window *w = window();
        if (add)
            add->action = [this, st, reload, w] {
                // -addSkippedItemPressed:: files and folders, several, "Exclude"
                OpenPanel::chooseDirectory(tr("EXCLUDE", "Exclude"), home(), [st, reload](std::string dir) {
                    if (dir.empty() || st->done)
                        return;
                    st->toRemove.erase(std::remove(st->toRemove.begin(), st->toRemove.end(), dir), st->toRemove.end());
                    if (std::find(st->toAdd.begin(), st->toAdd.end(), dir) == st->toAdd.end())
                        st->toAdd.push_back(dir);
                    reload();
                });
                (void)w;
            };
        if (remove)
            remove->action = [this, st, table, reload] {
                // -removeSkippedItemPressed:; the backup disk stays
                if (!table || table->selected < 0 || size_t(table->selected) >= st->items.size())
                    return;
                const std::string item = st->items[size_t(table->selected)];
                if (item == backupPath())
                    return;
                st->toAdd.erase(std::remove(st->toAdd.begin(), st->toAdd.end(), item), st->toAdd.end());
                st->toRemove.push_back(item);
                table->selected = -1;
                reload();
            };
        auto done = std::make_shared<bool>(false);
        for (View *b : nib->withAction("donePressed:"))
            if (auto *button = dynamic_cast<Button *>(b)) {
                button->isDefault = true;
                button->action = [w, done, battery, warn, st] {
                    // -donePressed:: the checkboxes, the items skipped and unskipped
                    *done = true;
                    Settings conf("timemachine");
                    if (warn && warn->checked() != conf.boolean("AlwaysShowDeletedBackupsWarning", true))
                        conf.set("AlwaysShowDeletedBackupsWarning", warn->checked());
                    if (battery && !battery->isHidden() && battery->checked() == conf.boolean("RequiresACPower", false))
                        conf.set("RequiresACPower", !battery->checked());
                    const std::vector<std::string> oldSkip = conf.list("SkipPaths"), oldByPath = conf.list("ExcludeByPath");
                    std::vector<std::string> skip = oldSkip, byPath = oldByPath;
                    auto drop = [](std::vector<std::string> &list, const std::string &item) {
                        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string &s) {
                                       return s == item || (s.rfind("~/", 0) == 0 && home() + s.substr(1) == item);
                                   }),
                                   list.end());
                    };
                    for (const std::string &r : st->toRemove) {
                        drop(skip, r);
                        drop(byPath, r);
                    }
                    for (const std::string &a : st->toAdd)
                        if (std::find(skip.begin(), skip.end(), a) == skip.end())
                            skip.push_back(a);
                    if (skip != oldSkip)
                        conf.set("SkipPaths", skip);   // -skipItem: / -unskipItem:
                    if (byPath != oldByPath)
                        conf.set("ExcludeByPath", byPath);
                    Sheet::end(w);
                };
            }
        for (View *b : nib->withAction("cancelPressed:"))
            if (auto *button = dynamic_cast<Button *>(b))
                button->action = [w] { Sheet::end(w); };
        begin(content, size, [this, st, done, previousAutoBackup] {
            st->done = true;
            st->sizes->cancelled = true;
            App::shared().removeTimer(st->timer);
            // the automatic backups as they were; the countdown again when the exclusions changed
            setAutoBackup(previousAutoBackup);
            if (*done && previousAutoBackup && (!st->toAdd.empty() || !st->toRemove.empty()))
                initiateRestartCountdown();
            updateGUI(false);
        });
    }
};

} // namespace

void timeMachine(NibPane &p)
{
    auto *pane = new TimeMachinePane(p);
    pane->setup();
    // kept with the pane: a view of no size that deletes it
    struct Keeper : View {
        TimeMachinePane *pane;
        explicit Keeper(TimeMachinePane *p) : pane(p) {}
        ~Keeper() override { delete pane; }
    };
    p.add<Keeper>(pane)->setFrame({0, 0, 0, 0});
}
