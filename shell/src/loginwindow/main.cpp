// SPDX-License-Identifier: LGPL-2.1-or-later
// loginwindow: About This Mac (below), Log Out, Restart and Shut Down (logout.cpp), Force Quit
// (forcequit.cpp).
// loginwindow's About This Mac, on AquaKit: its own nib (AboutThisMac.nib) filled and laid out as
// -[AboutThisMacController updateAboutBox] does on 10.6, reversed from loginwindow's PowerPC slice
// (the Intel ones are encrypted): the version line that turns into the build and the serial number
// when clicked, the processor, its L3 cache, the memory, the startup disk; More Info... opens its Linux
// counterpart, Software Update... the shell's Software Update. The version and build are the files' own
// (SystemVersion.plist): 10.6.8 once Software Update has installed the Combo Update.
//   loginwindow --about
#include "controls.h"
#include "logout.h"
#include "foundation.h"
#include "nib.h"

#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

extern char **environ;

using namespace AK;

namespace {

const char *const Bundle = "/System/Library/CoreServices/loginwindow.app";

std::string tr(const std::string &key, const std::string &fallback)
{
    return Assets::tr(Bundle, "AboutThisMac", key, fallback);
}

// "%@" filled in order (the strings' formats)
std::string format(const std::string &f, const std::vector<std::string> &values)
{
    std::string out;
    size_t v = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f.compare(i, 2, "%@") == 0) {
            out += v < values.size() ? values[v++] : std::string();
            ++i;
        } else {
            out += f[i];
        }
    }
    return out;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && std::isspace((unsigned char)s.back()))
        s.pop_back();
    return s;
}

std::string collapse(std::string s)
{
    std::string out;
    for (char c : s)
        if (!(std::isspace((unsigned char)c) && (out.empty() || out.back() == ' ')))
            out += std::isspace((unsigned char)c) ? ' ' : c;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

void erase(std::string &s, const std::string &what)
{
    for (size_t at; (at = s.find(what)) != std::string::npos;)
        s.erase(at, what.size());
}

// AppleSystemInfo's sizes (loginwindow's 0x46770): under a gigabyte "%d MB", else "%.*f GB" with no
// decimal unless the fraction is a hundredth or more, then one, or two when one isn't enough
std::string sizeString(unsigned long long bytes)
{
    const char *const Info = "/System/Library/CoreServices";
    auto f = [&](const char *key, const char *fallback) { return Assets::tr(Info, "AppleSystemInfo", key, fallback); };
    const unsigned long long mb = bytes >> 20;
    char buf[64];
    if (mb < 1024) {
        std::snprintf(buf, sizeof buf, "%d", int(mb));
        std::string s = f("DecimalMB", "%d MB");
        return s.replace(s.find("%d"), 2, buf);
    }
    double v = double(mb) / 1024;
    std::string unit = f("DecimalGB", "%.*f GB");
    for (const char *next : {"DecimalTB", "DecimalPB", "DecimalEB"}) {
        if (v < 1024)
            break;
        v /= 1024;
        unit = f(next, "%.*f TB");
    }
    int decimals = 0;
    if (v - std::floor(v) >= 0.01)
        decimals = (v * 10 - std::floor(v * 10)) >= 0.01 ? 2 : 1;
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    const size_t at = unit.find("%.*f");
    return at == std::string::npos ? std::string(buf) : unit.replace(at, 4, buf);
}

// ---- the machine, as Linux tells it

struct Processor {
    std::string description;   // "2.66 GHz Intel Core i7"
    int packages = 1;
    unsigned long long l3 = 0;
};

Processor processor()
{
    Processor p;
    std::ifstream in("/proc/cpuinfo");
    std::string line, model;
    std::set<std::string> ids;
    double mhz = 0;
    while (std::getline(in, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = collapse(line.substr(0, colon)), value = collapse(line.substr(colon + 1));
        if (key == "model name" && model.empty())
            model = value;
        else if (key == "physical id")
            ids.insert(value);
        else if (key == "cpu MHz" && mhz == 0)
            mhz = std::atof(value.c_str());
    }
    p.packages = std::max(1, int(ids.size()));
    // the clock: the one the name gives ("@ 3.70GHz"), else the highest the cpufreq driver knows
    double ghz = 0;
    const size_t at = model.find(" @ ");
    if (at != std::string::npos) {
        ghz = std::atof(model.c_str() + at + 3);
        model.erase(at);
    }
    if (ghz == 0) {
        const std::string khz = readFile("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        ghz = !khz.empty() ? std::atof(khz.c_str()) / 1e6 : mhz / 1000;
    }
    // the brand as the Mac writes it: "Intel Core i7", no trademarks, no "CPU", no core count
    for (const char *mark : {"(R)", "(r)", "(TM)", "(tm)", " CPU", " Processor"})
        erase(model, mark);
    // Linux's names go on about the graphics ("AMD Ryzen 5 7535HS with Radeon Graphics")
    const size_t with = model.find(" with ");
    if (with != std::string::npos)
        model.erase(with);
    const size_t core = model.find("-Core");
    if (core != std::string::npos) {
        size_t start = model.rfind(' ', core);
        model.erase(start == std::string::npos ? 0 : start, core + 5 - (start == std::string::npos ? 0 : start));
    }
    model = collapse(model);
    std::string clock;
    if (ghz > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.2f", ghz);
        clock = buf;
        while (clock.back() == '0')
            clock.pop_back();
        if (clock.back() == '.')
            clock.pop_back();
        clock += " GHz ";
    }
    p.description = (p.packages > 1 ? std::to_string(p.packages) + " x " : std::string()) + clock + model;
    // hw.l3cachesize: cpu0's level 3 cache
    for (int i = 0; i < 8; ++i) {
        const std::string dir = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(i) + "/";
        if (readFile(dir + "level") != "3")
            continue;
        const std::string size = readFile(dir + "size");
        unsigned long long n = std::strtoull(size.c_str(), nullptr, 10);
        if (!size.empty() && size.back() == 'K')
            n <<= 10;
        else if (!size.empty() && size.back() == 'M')
            n <<= 20;
        p.l3 = n;
    }
    return p;
}

// The memory modules (SMBIOS type 17) as udev keeps them for everyone (/run/udev/data/+dmi:id):
// what is installed, and the first module's speed and type ("4800 MHz DDR5"), as the Mac reads its
// DIMMs' from the device tree; without them, Linux's MemTotal up to the next gigabyte
std::string memory()
{
    std::ifstream udev("/run/udev/data/+dmi:id");
    std::string line, speed, configured, type;
    unsigned long long installed = 0;
    while (std::getline(udev, line)) {
        if (line.rfind("E:MEMORY_DEVICE_", 0) != 0)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        auto ends = [&](const char *s) { return key.size() > std::strlen(s) && key.compare(key.size() - std::strlen(s), std::string::npos, s) == 0; };
        if (ends("_SIZE") && !ends("_VOLATILE_SIZE") && !ends("_NON_VOLATILE_SIZE") && !ends("_CACHE_SIZE") && !ends("_LOGICAL_SIZE"))
            installed += std::strtoull(value.c_str(), nullptr, 10);
        else if (ends("_CONFIGURED_SPEED_MTS") && configured.empty())
            configured = value;
        else if (ends("_SPEED_MTS") && speed.empty())
            speed = value;
        else if (ends("_TYPE") && type.empty() && value != "Unknown")
            type = value;
    }
    if (!configured.empty() && configured != "0")
        speed = configured;
    if (installed == 0) {
        std::ifstream in("/proc/meminfo");
        std::string key;
        unsigned long long kb = 0;
        while (in >> key) {
            if (key == "MemTotal:") {
                in >> kb;
                break;
            }
            in.ignore(1 << 10, '\n');
        }
        const unsigned long long gb = 1ull << 30;
        installed = (kb * 1024 + gb - 1) / gb * gb;
    }
    const std::string kind = !speed.empty() && speed != "0" && !type.empty() ? " " + speed + " MHz " + type : std::string();
    return format(tr("ABOUT_BOX_MEMORY_FIELD_FORMAT", "%@%@"), {sizeString(installed), kind});
}

// The startup disk: the volume "/" is on, by its label (the Mac's volume name); the other volumes
// mounted (the Mac's /Volumes holding more than the startup disk) make it shown
struct Disks {
    std::string startup;
    int volumes = 0;
};

Disks disks()
{
    Disks d;
    std::ifstream in("/proc/self/mountinfo");
    std::string line, rootSource;
    std::set<std::string> sources;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string id, parent, dev, root, point, field;
        ss >> id >> parent >> dev >> root >> point;
        while (ss >> field && field != "-") {
        }
        std::string fstype, source;
        ss >> fstype >> source;
        if (source.rfind("/dev/", 0) != 0)
            continue;
        if (point == "/")
            rootSource = source;
        if (point == "/" || point.rfind("/run/media/", 0) == 0 || point.rfind("/media/", 0) == 0 || point.rfind("/mnt/", 0) == 0)
            sources.insert(source);
    }
    d.volumes = int(sources.size());
    char real[4096];
    std::string rootDev = rootSource;
    if (realpath(rootSource.c_str(), real))
        rootDev = real;
    for (const std::string &label : files("/dev/disk/by-label")) {
        char target[4096];
        if (realpath(("/dev/disk/by-label/" + label).c_str(), target) && rootDev == target) {
            // udev escapes the label's spaces as \x20
            std::string name = label;
            for (size_t at; (at = name.find("\\x20")) != std::string::npos;)
                name.replace(at, 4, " ");
            d.startup = name;
        }
    }
    if (d.startup.empty())
        d.startup = fileName(rootDev);
    return d;
}

std::string serialNumber()
{
    // the machine's (readable by root only on most systems: then the version line has two states)
    for (const char *f : {"/sys/class/dmi/id/product_serial", "/sys/class/dmi/id/board_serial"}) {
        const std::string s = readFile(f);
        if (!s.empty() && s != "0" && s.find("O.E.M.") == std::string::npos && s.find("Default") == std::string::npos &&
            s.find("To be filled") == std::string::npos)
            return s;
    }
    return {};
}

std::string inPath(const std::vector<std::string> &programs)
{
    const char *path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin");
    std::string dir;
    std::vector<std::string> dirs;
    while (std::getline(ss, dir, ':'))
        dirs.push_back(dir);
    for (const std::string &p : programs)
        for (const std::string &d : dirs)
            if (access((d + "/" + p).c_str(), X_OK) == 0)
                return d + "/" + p;
    return {};
}

bool spawn(const std::vector<std::string> &argv)
{
    std::vector<char *> args;
    for (const std::string &a : argv)
        args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid;
    const bool ok = posix_spawn(&pid, args[0], nullptr, &attr, args.data(), environ) == 0;
    posix_spawnattr_destroy(&attr);
    return ok;
}

// The shell's own System Preferences, beside this program (apps/<Name>.app/Contents/MacOS/<Name>)
std::string ownApp(const std::string &name)
{
    std::string dir = dirName(executablePath());
    for (int i = 0; i < 5; ++i) {
        const std::string exe = dir + "/" + name + ".app/Contents/MacOS/" + name;
        if (access(exe.c_str(), X_OK) == 0)
            return exe;
        dir = dirName(dir);
    }
    return {};
}

// ---- the controller

class AboutThisMac {
public:
    bool load()
    {
        const std::string res = Assets::path(std::string(Bundle) + "/Contents/Resources");
        if (!m_nib.load(res + "/English.lproj/AboutThisMac.nib/keyedobjects.nib", res))
            return false;
        m_content = m_nib.instantiate("aboutMacPanel");
        if (!m_content)
            return false;
        m_size = m_nib.size();
        auto field = [&](const char *name) { return m_nib.outlet<Label>(name); };
        topHalf = m_nib.outlet("topHalfGroupView");
        buildField = m_nib.outlet<Button>("_buildField");
        memoryField = field("memoryField");
        memoryLabel = field("memoryLabel");
        processorField = field("processorField");
        processorLabel = field("processorLabel");
        l3CacheField = field("l3CacheField");
        copyrightField = field("copyrightField");
        startupDiskField = field("startupDiskField");
        startupDiskLabel = field("startupDiskLabel");
        moreInfo = m_nib.outlet<Button>("moreInfoButton");
        softwareUpdate = m_nib.outlet<Button>("softwareUpdateButton");
        profilerText = field("launchingProfilerTextField");
        profilerProgress = m_nib.outlet<ProgressIndicator>("launchingProfilerProgress");
        updateText = field("launchingSoftwareUpdateTextField");
        updateProgress = m_nib.outlet<ProgressIndicator>("launchingSoftwareUpdateProgress");
        if (!topHalf || !buildField || !memoryField || !memoryLabel || !processorField || !processorLabel || !l3CacheField ||
            !startupDiskField || !startupDiskLabel)
            return false;
        init();
        updateAboutBox();
        return true;
    }

    View *content() const { return m_content; }
    Size size() const { return m_size; }
    std::function<void()> close;

private:
    // -[AboutThisMacController init]: the launch feedback hidden, the labels from AboutThisMac.strings,
    // the buttons' applications looked for (disabled without one)
    void init()
    {
        for (View *v : std::initializer_list<View *>{profilerText, updateText, profilerProgress, updateProgress})
            if (v)
                v->setHidden(true);
        processorLabel->setText(tr("ABOUT_BOX_PROCESSOR_LABEL", "Processor"));
        memoryLabel->setText(tr("ABOUT_BOX_MEMORY_LABEL", "Memory"));
        // System Profiler: what stands for it on Linux
        m_profiler = inPath({"kinfocenter", "hardinfo2", "hardinfo", "gnome-system-monitor", "lshw-gtk"});
        if (moreInfo) {
            moreInfo->setEnabled(!m_profiler.empty());
            moreInfo->action = [this] { launch(true); };
        }
        if (profilerText)
            profilerText->setText(format(profilerText->text(), {"System Profiler"}));
        // Software Update: the shell's own (Apple's catalog), else System Preferences' pane
        m_updater = ownApp("Software Update");
        m_preferences = ownApp("System Preferences");
        if (softwareUpdate) {
            softwareUpdate->setEnabled(!m_updater.empty() || !m_preferences.empty());
            softwareUpdate->action = [this] { launch(false); };
        }
        if (updateText)
            updateText->setText(format(updateText->text(), {"Software Update"}));
        buildField->action = [this] { toggleBuildField(); };
    }

    // moreInfoClicked: / softwareUpdateClicked:: the button gives way to "Opening …" and the wheel
    // while the application starts; then the panel closes
    void launch(bool profiler)
    {
        const bool ok = profiler            ? spawn({m_profiler})
                        : !m_updater.empty() ? spawn({m_updater})
                                             : spawn({m_preferences, "--pane", "softwareupdate"});
        if (!ok)
            return;
        Button *button = profiler ? moreInfo : softwareUpdate;
        Label *text = profiler ? profilerText : updateText;
        ProgressIndicator *wheel = profiler ? profilerProgress : updateProgress;
        if (button)
            button->setHidden(true);
        if (text)
            text->setHidden(false);
        if (wheel) {
            wheel->setHidden(false);
            wheel->start();
        }
        App::shared().addTimer(2.5, false, [this] {
            if (close)
                close();
        });
    }

    // reloadBuildInfoStrings: the version line's states, in disabledControlTextColor (12 point):
    // "Version 10.6", "Build 10A432", "Serial Number …" when there is one
    void reloadBuildInfoStrings()
    {
        const auto version = Assets::plistStrings("/System/Library/CoreServices/SystemVersion.plist");
        auto get = [&](const char *k, const char *fallback) {
            auto it = version.find(k);
            return it == version.end() ? std::string(fallback) : it->second;
        };
        m_states.clear();
        m_states.push_back(format(tr("ABOUT_BOX_VERSION_FORMAT_STRING", "Version %@"), {get("ProductUserVisibleVersion", "10.6")}));
        m_states.push_back(format(tr("ABOUT_BOX_BUILD_FORMAT_STRING", "Build %@"), {get("ProductBuildVersion", "10A432")}));
        const std::string serial = serialNumber();
        if (!serial.empty())
            m_states.push_back(format(tr("ABOUT_BOX_SERIAL_NUMBER_FORMAT_STRING", "Serial Number %@"), {serial}));
        m_state %= int(m_states.size());
        buildField->title = m_states[size_t(m_state)];
        buildField->titleColor = Color::gray(0, 0.5);   // disabledControlTextColor
        buildField->setNeedsDisplay();
        if (copyrightField && m_copyright.empty()) {
            m_copyright = copyrightField->text();
            copyrightField->setText(m_copyright + " " + get("ProductCopyright", "1983-2009 Apple Inc."));
        }
    }

    void toggleBuildField()
    {
        // _buildFieldState = (state + 1) % states, when there is more than one
        if (m_states.size() < 2)
            return;
        m_state = (m_state + 1) % int(m_states.size());
        buildField->title = m_states[size_t(m_state)];
        buildField->setNeedsDisplay();
    }

    // setL3CacheHidden: hidden, the top half goes 14 points down and the panel 14 shorter (its top
    // where it was: what hangs from the top stays, what sits at the bottom comes up)
    void setL3CacheHidden(bool hidden)
    {
        if (l3CacheField->isHidden() == hidden)
            return;
        l3CacheField->setHidden(hidden);
        const double d = hidden ? 14 : -14;
        Rect f = topHalf->frame();
        topHalf->setFrame({f.x, f.y + d, f.w, f.h});
        resize(-d);
    }

    // The panel's height changed by dh at its bottom: the views at the bottom (the buttons, the
    // copyright: NSViewMaxYMargin, or no vertical margin) move with it
    void resize(double dh)
    {
        const std::set<View *> fromTop = {topHalf, memoryField, memoryLabel, l3CacheField, startupDiskField, startupDiskLabel};
        for (View *v : m_content->subviews())
            if (!fromTop.count(v)) {
                const Rect f = v->frame();
                v->setFrame({f.x, f.y + dh, f.w, f.h});
            }
        m_size.h += dh;
        m_content->setFrame({0, 0, m_size.w, m_size.h});
    }

    static double fitWidth(Label *l) { return std::ceil(textWidth(l->text(), l->style.font)) + 4; }   // sizeToFit

    // -[AboutThisMacController updateAboutBox]
    void updateAboutBox()
    {
        const Processor cpu = processor();
        memoryField->setText(memory());
        processorField->setText(format(tr(cpu.packages == 1 ? "ABOUT_BOX_SINGLE_PROCESSOR_FIELD_FORMAT" : "ABOUT_BOX_MULTIPLE_PROCESSORS_FIELD_FORMAT", "%@"),
                                       {cpu.description}));
        if (cpu.l3 == 0) {
            setL3CacheHidden(true);
        } else {
            setL3CacheHidden(false);
            l3CacheField->setText(format(tr(cpu.packages == 1 ? "ABOUT_BOX_SINGLE_PROCESSOR_L3CACHE_FORMAT" : "ABOUT_BOX_MULTIPLE_PROCESSORS_L3CACHE_FORMAT",
                                            "%@ L3 cache"),
                                         {sizeString(cpu.l3)}));
        }
        // /Volumes with the startup disk alone: no Startup Disk line, the panel 28 shorter
        const Disks d = disks();
        if (d.volumes < 2) {
            startupDiskLabel->setHidden(true);
            startupDiskField->setHidden(true);
            resize(-28);
        } else {
            startupDiskField->setText(d.startup);
        }
        // the labels sized to fit, right-aligned against the values 5 points on, the two columns
        // centred in the panel with 40 points around them at least: the values shortened to that, or
        // the panel wider when even the shortest won't do
        const bool l3 = !l3CacheField->isHidden(), disk = !startupDiskField->isHidden();
        double labels = std::max(fitWidth(memoryLabel), fitWidth(processorLabel));
        if (disk)
            labels = std::max(labels, fitWidth(startupDiskLabel));
        double extra = 0;   // the widest of the lines that can't be shortened
        if (l3)
            extra = fitWidth(l3CacheField);
        if (disk)
            extra = std::max(extra, fitWidth(startupDiskField));
        double values = std::max({fitWidth(memoryField), fitWidth(processorField), extra});
        const double left = labels + 5;
        double width = topHalf->frame().w;
        double slack = width - (left + values);
        if (slack < 40) {
            if (extra + left + 40 <= width) {
                values -= 40 - slack;
            } else {
                width = extra + left + 40;
                values = extra;
                m_size.w = width;
                m_content->setFrame({0, 0, m_size.w, m_size.h});
                for (View *v : m_content->subviews()) {
                    // the views across the panel (NSViewWidthSizable) widen with it
                    const Rect f = v->frame();
                    if (f.w >= 270)
                        v->setFrame({f.x, f.y, f.w + (width - 307), f.h});
                }
            }
            slack = width - (left + values);
        }
        const double x = std::floor(slack * 0.5);
        auto place = [](Label *l, double x, double w) {
            const Rect f = l->frame();
            l->setFrame({x, f.y, w, f.h});
        };
        for (Label *l : {memoryLabel, processorLabel, startupDiskLabel})
            place(l, x, labels);
        for (Label *l : {memoryField, processorField, l3CacheField, startupDiskField})
            place(l, x + left, values);
        reloadBuildInfoStrings();
    }

    Nib m_nib;
    View *m_content = nullptr;
    Size m_size;
    View *topHalf = nullptr;
    Button *buildField = nullptr, *moreInfo = nullptr, *softwareUpdate = nullptr;
    Label *memoryField = nullptr, *memoryLabel = nullptr, *processorField = nullptr, *processorLabel = nullptr,
          *l3CacheField = nullptr, *copyrightField = nullptr, *startupDiskField = nullptr, *startupDiskLabel = nullptr,
          *profilerText = nullptr, *updateText = nullptr;
    ProgressIndicator *profilerProgress = nullptr, *updateProgress = nullptr;
    std::vector<std::string> m_states;
    int m_state = 0;
    std::string m_copyright, m_profiler, m_preferences, m_updater;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && (std::strcmp(argv[1], "--logout") == 0 || std::strcmp(argv[1], "--logout-now") == 0))
        return LoginWindow::logOut(std::strcmp(argv[1], "--logout") == 0);
    if (argc >= 2 && std::strcmp(argv[1], "--shutdown") == 0)
        return LoginWindow::restart(1);
    if (argc >= 2 && std::strcmp(argv[1], "--restart") == 0)
        return LoginWindow::restart(2);
    if (argc >= 2 && std::strcmp(argv[1], "--power") == 0)
        return LoginWindow::restart(0);
    if (argc >= 2 && std::strcmp(argv[1], "--force-quit") == 0)
        return LoginWindow::forceQuit();
    if (argc < 2 || std::strcmp(argv[1], "--about") != 0) {
        std::fprintf(stderr, "usage: loginwindow --about | --logout | --logout-now | --restart | --shutdown | --power | --force-quit\n");
        return 2;
    }
    App &app = App::shared();
    if (!app.open()) {
        std::fprintf(stderr, "loginwindow: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    AboutThisMac about;
    if (!about.load()) {
        std::fprintf(stderr, "loginwindow: AboutThisMac.nib not found (extract the DVD's files: tools/extract.py)\n");
        return 1;
    }
    Window window(about.size(), "About This Mac");
    window.contentView()->addSubview(about.content());
    window.setFloatingPanel();
    window.onClose = [&app] { app.quit(); };
    about.close = [&window] { window.close(); };
    window.show();
    return app.run();
}
