// SPDX-License-Identifier: LGPL-2.1-or-later
// Activity Monitor.app (10.6, 212) on AquaKit, as 10.6.8's (its x86_64 code, docs/reverse-status.md,
// section 22): ActivityMonitor.nib's window, menus and tabs, the processes and the system read from
// Linux's /proc.
//   The window: Defaults.plist's toolbar (Quit Process, Inspect, Sample Process, a flexible space, Show,
//   Filter), the processes' outline (the nib's columns; 10.6's default ones: Process ID, Process Name
//   with its icon, User, % CPU, Threads, Real Mem, Kind), the tabs under it.
//   -[SMProcessController initialize]: UpdatePeriod 2 seconds ("Normally"), the Show popup's My
//   Processes. -quitProcessWithProcess:window:: a sheet, "Do you really want to quit “%@”?" with Quit
//   (SIGTERM), Force Quit (SIGKILL) and Cancel; another user's process can't be quit from here.
//   CPU (SMCPUInfo, CPUHistoryView): % User, % System, % Idle, Threads, Processes, the history in bars
//   four points apart, user over system over nice, the background's lines every three points.
//   System Memory (SMCPUInfo's GRPieDataSet, start angle 90): Wired red, Active yellow, Inactive blue,
//   Free green (-awakeFromNib's defaults), Used, VM size, Page ins/outs, Swap used.
//   Disk Activity, Network (SMDiskInfo, SMNetworkInfo, GRLineDataSet): 40 samples on black, in green
//   and out red, IO / Data (or Packets / Data), the peak above. Disk Usage: the volumes, used blue,
//   free green.
#include "appkit.h"
#include "controls.h"
#include "foundation.h"
#include "launchservices.h"
#include "mainmenu.h"
#include "nib.h"
#include "panels.h"
#include "text.h"
#include "toolbar.h"
#include "windows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mntent.h>
#include <pwd.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

using namespace AK;

namespace {

const char *const Bundle = "/Applications/Utilities/Activity Monitor.app";

std::string res(const std::string &name) { return Assets::path(std::string(Bundle) + "/Contents/Resources/" + name); }
std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }

std::string format(std::string f, const std::vector<std::string> &args)
{
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string pos = "%" + std::to_string(i + 1) + "$@";
        for (size_t at; (at = f.find(pos)) != std::string::npos;)
            f.replace(at, pos.size(), args[i]);
    }
    size_t next = 0;
    for (size_t at = 0; (at = f.find("%@", at)) != std::string::npos && next < args.size();) {
        f.replace(at, 2, args[next]);
        at += args[next++].size();
    }
    return f;
}

std::string grouped(long long n)
{
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3)
        s.insert(size_t(i), ",");
    return n < 0 ? "-" + s : s;
}

// SMBase's formatters: "#,##0.0" for megabytes and less, "#,##0.00" for gigabytes
std::string bytes(double b)
{
    char buf[64];
    if (b >= 1024.0 * 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024.0 * 1024)
        std::snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)
        std::snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
    else
        std::snprintf(buf, sizeof buf, "%.0f bytes", b);
    return buf;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string userName(uid_t uid)
{
    static std::map<uid_t, std::string> cache;
    auto it = cache.find(uid);
    if (it != cache.end())
        return it->second;
    const passwd *pw = getpwuid(uid);
    return cache[uid] = pw ? pw->pw_name : std::to_string(uid);
}

// ---- the processes (SMProcess)

struct Proc {
    int pid = 0, ppid = 0;
    uid_t uid = 0;
    std::string name, user, kind;
    LaunchServices::App app;   // an application's, for its name and icon
    double cpu = 0;
    long threads = 0;
    long long rss = 0, vsize = 0;
    unsigned long long ticks = 0;
    bool windowed = false;
};

// SMProcess -architectureDescription: the executable's ELF class and machine
std::string kindOf(int pid)
{
    unsigned char h[20] = {};
    std::ifstream in("/proc/" + std::to_string(pid) + "/exe", std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(h), sizeof h) || std::memcmp(h, "\x7f" "ELF", 4) != 0)
        return {};
    const unsigned machine = unsigned(h[18]) | unsigned(h[19]) << 8;
    const bool is64 = h[4] == 2;
    if (machine == 62 || machine == 3)   // x86-64, i386
        return tr(is64 ? "Intel (64 bit)" : "Intel");
    if (machine == 20 || machine == 21)
        return tr(is64 ? "PowerPC (64 bit)" : "PowerPC");
    if (machine == 183)
        return "ARM (64 bit)";
    return tr("Unknown");
}

class Sampler {
public:
    std::vector<Proc> procs;
    // the whole system (/proc/stat), in ticks since the last sample
    double user = 0, system = 0, nice = 0, idle = 100;
    long threads = 0;
    std::vector<std::array<double, 3>> perCPU;   // user, system, nice per processor

    void sample()
    {
        const double t = now();
        const double elapsed = m_last > 0 ? t - m_last : 0;
        m_last = t;
        const long hz = sysconf(_SC_CLK_TCK);
        std::set<int> windowedPids;
        for (xcb_window_t w : Windows::clientWindows())
            if (const uint32_t pid = Windows::pid(w))
                windowedPids.insert(int(pid));
        std::map<int, unsigned long long> ticks;
        std::vector<Proc> out;
        threads = 0;
        if (DIR *d = opendir("/proc")) {
            while (dirent *e = readdir(d)) {
                const int pid = std::atoi(e->d_name);
                if (pid <= 0)
                    continue;
                Proc p;
                p.pid = pid;
                const std::string dir = "/proc/" + std::string(e->d_name);
                const std::string stat = readFile(dir + "/stat");
                const size_t open = stat.find('('), close = stat.rfind(')');
                if (open == std::string::npos || close == std::string::npos)
                    continue;
                p.name = stat.substr(open + 1, close - open - 1);
                std::istringstream fields(stat.substr(close + 2));
                std::vector<std::string> f{std::istream_iterator<std::string>(fields), {}};
                if (f.size() < 22)
                    continue;
                p.ppid = std::atoi(f[1].c_str());
                p.ticks = std::stoull(f[11]) + std::stoull(f[12]);
                p.threads = std::atol(f[17].c_str());
                p.vsize = std::atoll(f[20].c_str());
                p.rss = std::atoll(f[21].c_str()) * sysconf(_SC_PAGESIZE);
                struct stat st;
                if (::stat(dir.c_str(), &st) == 0)
                    p.uid = st.st_uid;
                p.user = userName(p.uid);
                // the full name of the executable rather than comm's 15 characters
                const std::string cmdline = readFile(dir + "/cmdline");
                if (!cmdline.empty()) {
                    std::string first = cmdline.substr(0, cmdline.find('\0'));
                    first = first.substr(first.rfind('/') + 1);
                    if (!first.empty() && first.rfind(p.name, 0) == 0)
                        p.name = first;
                }
                p.windowed = windowedPids.count(pid) != 0;
                if (p.windowed) {
                    p.app = LaunchServices::forPid(uint32_t(pid));
                    if (p.app.valid() && !p.app.name.empty())
                        p.name = p.app.name;
                }
                auto prev = m_ticks.find(pid);
                if (prev != m_ticks.end() && elapsed > 0)
                    p.cpu = 100.0 * double(p.ticks - std::min(p.ticks, prev->second)) / (elapsed * double(hz));
                ticks[pid] = p.ticks;
                auto k = m_kinds.find(pid);
                p.kind = k != m_kinds.end() ? k->second : (m_kinds[pid] = kindOf(pid));
                threads += p.threads;
                out.push_back(std::move(p));
            }
            closedir(d);
        }
        m_ticks.swap(ticks);
        procs.swap(out);
        sampleCPU();
    }

private:
    void sampleCPU()
    {
        std::istringstream in(readFile("/proc/stat"));
        std::string line;
        std::vector<std::array<unsigned long long, 4>> now;   // user, nice, system(+irq), idle(+iowait)
        while (std::getline(in, line)) {
            if (line.rfind("cpu", 0) != 0)
                break;
            std::istringstream l(line);
            std::string name;
            unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0;
            l >> name >> u >> n >> s >> i >> w >> irq >> sirq;
            now.push_back({u, n, s + irq + sirq, i + w});
        }
        perCPU.clear();
        for (size_t c = 0; c < now.size(); ++c) {
            if (c >= m_cpu.size())
                break;
            double d[4], total = 0;
            for (int k = 0; k < 4; ++k)
                total += d[k] = double(now[c][size_t(k)] - std::min(now[c][size_t(k)], m_cpu[c][size_t(k)]));
            if (total <= 0)
                total = 1;
            if (c == 0) {
                user = 100 * d[0] / total;
                nice = 100 * d[1] / total;
                system = 100 * d[2] / total;
                idle = 100 * d[3] / total;
            } else {
                perCPU.push_back({100 * d[0] / total, 100 * d[2] / total, 100 * d[1] / total});
            }
        }
        m_cpu = now;
    }

    double m_last = 0;
    std::map<int, unsigned long long> m_ticks;
    std::map<int, std::string> m_kinds;
    std::vector<std::array<unsigned long long, 4>> m_cpu;
};

// ---- the charts (GraphKit's GRChartView, CPUHistoryView)

// CPUHistoryView -drawRect:: a bar every 4 points from the right, its height user + system + nice
// in thirds of the view, the background's lines every 3 points over them
class CPUHistoryView : public View {
public:
    std::deque<std::array<double, 3>> history;   // user, system, nice; the newest last
    Color userColor = Color::rgb(0, 255, 0), systemColor = Color::rgb(255, 0, 0), niceColor = Color::rgb(0, 0, 255);
    Color backColor = Color::gray(0);
    bool grid = true;
    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        backColor.set(cr);
        cairo_rectangle(cr, 0, 0, b.w, b.h);
        cairo_fill(cr);
        double x = b.w - 4;
        for (auto it = history.rbegin(); it != history.rend() && x > -4; ++it, x -= 4) {
            const double u = std::round((*it)[0] / 100 * b.h), s = std::round((*it)[1] / 100 * b.h), n = std::round((*it)[2] / 100 * b.h);
            double y = b.h;
            auto bar = [&](double h, const Color &c) {
                c.set(cr);
                cairo_rectangle(cr, x, y - h, 3, h);
                cairo_fill(cr);
                y -= h;
            };
            bar(n, niceColor);
            bar(s, systemColor);
            bar(u, userColor);
        }
        if (grid) {
            backColor.set(cr);
            for (double y = 0; y <= b.h; y += 3) {
                cairo_rectangle(cr, 0, y, b.w, 1);
                cairo_fill(cr);
            }
        }
    }
};

// A GRPieDataSet on a GRChartView without background: the slices from 90 degrees, counterclockwise
class PieView : public View {
public:
    std::vector<double> values;
    std::vector<Color> colors;
    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        const double r = std::min(b.w, b.h) / 2 - 2, cx = b.w / 2, cy = b.h / 2;
        double total = 0;
        for (double v : values)
            total += std::max(0.0, v);
        if (total <= 0 || r <= 0)
            return;
        double a = -M_PI / 2;   // 90 degrees, Cocoa's y up: the top
        for (size_t i = 0; i < values.size(); ++i) {
            const double sweep = 2 * M_PI * std::max(0.0, values[i]) / total;
            cairo_move_to(cr, cx, cy);
            cairo_arc_negative(cr, cx, cy, r, a, a - sweep);
            cairo_close_path(cr);
            colors[i % colors.size()].set(cr);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            a -= sweep;
        }
        // the dome's light: a white glaze over the top
        cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy - r * 0.4, 0, cx, cy, r);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.35);
        cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
};

// GRLineDataSet ×2 on black, 40 samples (GRAxesXAxisWindowSize), no axes, from 0
class LineGraphView : public View {
public:
    std::deque<std::pair<double, double>> samples;   // in, out
    Color inColor = Color::rgb(0, 255, 0), outColor = Color::rgb(255, 0, 0);
    void add(double in, double out)
    {
        samples.push_back({in, out});
        while (samples.size() > 40)
            samples.pop_front();
        setNeedsDisplay();
    }
    double peak() const
    {
        double m = 0;
        for (const auto &s : samples)
            m = std::max({m, s.first, s.second});
        return m;
    }
    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_rectangle(cr, 0, 0, b.w, b.h);
        cairo_fill(cr);
        const double top = std::max(1.0, peak());
        auto line = [&](bool in, const Color &c) {
            if (samples.size() < 2)
                return;
            const double step = b.w / 39;
            double x = b.w - step * double(samples.size() - 1);
            bool first = true;
            for (const auto &s : samples) {
                const double v = in ? s.first : s.second, y = b.h - 1 - v / top * (b.h - 2);
                first ? cairo_move_to(cr, x, y) : cairo_line_to(cr, x, y);
                first = false;
                x += step;
            }
            c.set(cr);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        };
        line(true, inColor);
        line(false, outColor);
    }
};

// ---- the window

class Monitor {
public:
    Monitor()
    {
        m_period = std::max(0.5, std::atof(Settings("activitymonitor").string("UpdatePeriod", "2").c_str()));
        m_menu.load(res("English.lproj/ActivityMonitor.nib"));
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
        m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
        m_menu.publish();
        build();
        refresh();
        m_timer = App::shared().addTimer(m_period, true, [this] { refresh(); });
    }

    // launched again (the Dock): its window forward
    void comeForward() { m_window->orderFront(); }

private:
    // ActivityMonitor.nib's _window: its outline (a TableView here) and its tabs, under the toolbar
    void build()
    {
        m_nib.load(res("English.lproj/ActivityMonitor.nib/keyedobjects.nib"), res(""));
        Size size{640, 469};
        View *content = nullptr;
        for (const Nib::WindowEntry &w : m_nib.windows())
            if (w.title == "Activity Monitor")
                content = m_nib.instantiateWindow(w.object, &size);
        m_window = std::make_unique<Window>(Size{size.w, size.h + Toolbar::Height}, "Activity Monitor");
        View *root = m_window->contentView();
        m_toolbar = root->add<Toolbar>();
        buildToolbar();
        if (content) {
            // the nib's views straight into the root, under the toolbar
            std::vector<View *> kids = content->subviews();
            for (View *v : kids) {
                if (!m_table)
                    m_table = dynamic_cast<TableView *>(v);
                if (!m_tabs)
                    m_tabs = dynamic_cast<TabView *>(v);
            }
            m_content = content;
            root->addSubview(content);
        }
        if (m_table) {
            // 10.6's default columns, the nib's titles and widths
            std::vector<TableView::Column> all = m_table->columns, shown;
            for (const char *title : {"PID", "Process Name", "User", "% CPU", "Threads", "Real Mem", "Kind"})
                for (const TableView::Column &c : all)
                    if (c.title == title)
                        shown.push_back(c);
            m_table->columns = shown;
            m_table->drawCell = [this](cairo_t *cr, int row, int column, const Rect &cell, bool highlighted) {
                return drawCell(cr, row, column, cell, highlighted);
            };
            m_table->onSelect = [this](int row) {
                m_selectedPid = row >= 0 && size_t(row) < m_shown.size() ? m_shown[size_t(row)].pid : 0;
                updateToolbar();
            };
            m_table->onDoubleClick = [this](int) { inspect(); };
        }
        if (m_tabs)
            buildTabs();
        m_window->setUnifiedToolbarHeight(int(Toolbar::Gradient));
        m_window->setIcon(Image::load(res("ActivityMonitor.icns"), {128, 128}));
        m_window->setResizable({500, 400});
        m_window->onResize = [this](Size) { layout(); };
        m_window->onUnhandledKey = [this](Event &e) { m_menu.performKeyEquivalent(e); };
        m_window->onClose = [] { App::shared().quit(); };
        layout();
        m_window->show();
    }

    void layout()
    {
        const Size s = m_window->size();
        m_toolbar->setFrame({0, 0, s.w, Toolbar::Height});
        if (m_content)
            m_content->setFrame({0, Toolbar::Height, s.w, s.h - Toolbar::Height});
        const double tabsH = 166;   // the tab view's {656, 176} less the 10 under the window
        if (m_tabs)
            m_tabs->setFrame({-8, s.h - Toolbar::Height - tabsH, s.w + 16, 176});
        if (m_table)
            m_table->setFrame({-1, -1, s.w + 2, s.h - Toolbar::Height - tabsH - 12});
    }

    // Defaults.plist's ProcessInfoToolbar: its default items
    void buildToolbar()
    {
        auto image = [](const std::string &name) {
            auto *b = new Button({}, Button::Plain);
            b->image = Image::load(res(name), {32, 32});
            return b;
        };
        m_quit = image("stop.tiff");
        m_quit->action = [this] { quitProcess(); };
        m_toolbar->addItem("LSToolbarQuitApplication", tr("Quit Process"), m_quit, 40);
        m_inspect = image("Inspect.tiff");
        m_inspect->action = [this] { inspect(); };
        m_toolbar->addItem("LSToolbarInspectItem", tr("Inspect"), m_inspect, 40);
        m_sample = image("Sampler.tiff");
        m_sample->setEnabled(false);   // sample(1) has no Linux counterpart here
        m_toolbar->addItem("LSToolbarSample", tr("Sample Process"), m_sample, 40);
        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);
        m_show = new PopUpButton;
        for (const char *t : {"All Processes", "All Processes, Hierarchically", "My Processes", "System Processes", "Other User Processes",
                              "Active Processes", "Inactive Processes", "Windowed Processes", "Selected Processes"})
            m_show->items.push_back(MenuItem(t));
        m_show->setSelected(Settings("activitymonitor").integer("ShowCategory", 2));
        m_show->onSelect = [this](int i) {
            Settings("activitymonitor").set("ShowCategory", i);
            updateTable();
        };
        m_toolbar->addItem("LSToolbarShowFilterItem", tr("Show"), m_show, 170);
        m_filter = new SearchField;
        m_filter->onChange = [this](const std::string &) { updateTable(); };
        m_toolbar->addItem("LSToolbarFilterItem", tr("Filter"), m_filter, 120, 160);
    }

    void updateToolbar()
    {
        const bool any = m_selectedPid != 0;
        m_quit->setEnabled(any);
        m_inspect->setEnabled(any);
        m_quit->setNeedsDisplay();
        m_inspect->setNeedsDisplay();
    }

    // A view of a tab page by its place in the nib (bottom-left coordinates in a page 130 high)
    View *at(int page, double x, double y, double w, double h) const
    {
        View *p = m_tabs ? m_tabs->page(page) : nullptr;
        if (!p)
            return nullptr;
        const Rect want{x, 130 - y - h, w, h};
        std::function<View *(View *)> find = [&](View *v) -> View * {
            for (View *c : v->subviews()) {
                const Rect f = c->frame();
                // a text field's frame is its cell's, 2 in on each side (NSTextFieldCell's inset)
                if (std::abs(f.x - want.x) <= 2.5 && std::abs(f.y - want.y) < 1 && std::abs(f.w - want.w) <= 4.5 && std::abs(f.h - want.h) < 1)
                    return c;
                if (View *in = find(c))
                    return in;
            }
            return nullptr;
        };
        return find(p);
    }

    static void setText(View *v, const std::string &t)
    {
        if (auto *l = dynamic_cast<Label *>(v))
            l->setText(t);
        else if (auto *f = dynamic_cast<TextField *>(v))
            f->setText(t);
    }

    template <typename T> T *chartIn(View *host)
    {
        if (!host)
            return nullptr;
        auto *c = host->add<T>();
        c->setFrame(host->bounds());
        return c;
    }

    void wells(int page, std::initializer_list<std::pair<std::pair<double, double>, Color>> list)
    {
        for (const auto &[xy, color] : list)
            if (auto *w = dynamic_cast<ColorWell *>(at(page, xy.first, xy.second, 20, 20))) {
                w->color = color;
                w->setNeedsDisplay();
            }
    }

    void buildTabs()
    {
        const Color green = Color::rgb(0, 255, 0), red = Color::rgb(255, 0, 0), blue = Color::rgb(0, 0, 255), yellow = Color::rgb(255, 255, 0);
        // CPU
        m_cpuUser = at(0, 119, 93, 60, 17);
        m_cpuSys = at(0, 119, 71, 60, 17);
        m_cpuIdle = at(0, 119, 52, 60, 14);
        m_threads = at(0, 325, 96, 81, 14);
        m_processes = at(0, 325, 74, 81, 14);
        wells(0, {{{187, 95}, green}, {{187, 73}, red}, {{187, 51}, Color::gray(0)}});
        m_history1 = chartIn<CPUHistoryView>(at(0, 426, 77, 91, 33));
        m_history2 = chartIn<CPUHistoryView>(at(0, 426, 41, 91, 33));
        // System Memory
        m_free = at(1, 118, 104, 65, 14);
        m_wired = at(1, 118, 82, 65, 14);
        m_active = at(1, 118, 60, 65, 14);
        m_inactive = at(1, 118, 38, 65, 14);
        m_used = at(1, 118, 16, 65, 14);
        m_vmSize = at(1, 307, 104, 81, 14);
        m_pageIns = at(1, 307, 82, 81, 14);
        m_pageOuts = at(1, 307, 60, 81, 14);
        m_swap = at(1, 307, 38, 81, 14);
        m_total = at(1, 426, 6, 91, 33);
        wells(1, {{{186, 103}, green}, {{186, 81}, red}, {{186, 59}, yellow}, {{186, 37}, blue}});
        m_memPie = chartIn<PieView>(at(1, 426, 41, 91, 69));
        if (m_memPie)
            m_memPie->colors = {red, yellow, blue, green};
        // Disk Activity
        m_readsIn = at(2, 119, 96, 81, 14);
        m_writesOut = at(2, 119, 74, 81, 14);
        m_readsSec = at(2, 119, 52, 70, 14);
        m_writesSec = at(2, 119, 30, 70, 14);
        m_dataRead = at(2, 305, 96, 81, 14);
        m_dataWritten = at(2, 305, 74, 81, 14);
        m_dataReadSec = at(2, 305, 52, 63, 14);
        m_dataWrittenSec = at(2, 305, 30, 63, 14);
        m_diskHigh = at(2, 409, 111, 124, 17);
        wells(2, {{{373, 51}, green}, {{373, 29}, red}});
        m_diskGraph = chartIn<LineGraphView>(at(2, 426, 41, 91, 69));
        // Disk Usage
        m_volumes = dynamic_cast<PopUpButton *>(at(3, 40, 89, 190, 22));
        m_diskUsedText = at(3, 131, 52, 81, 14);
        m_diskFreeText = at(3, 131, 30, 81, 14);
        m_diskUsedBytes = at(3, 220, 52, 148, 14);
        m_diskFreeBytes = at(3, 220, 30, 148, 14);
        m_diskTotal = at(3, 426, 11, 91, 28);
        wells(3, {{{373, 51}, blue}, {{373, 29}, green}});
        m_diskPie = chartIn<PieView>(at(3, 426, 41, 91, 69));
        if (m_diskPie)
            m_diskPie->colors = {blue, green};
        fillVolumes();
        // Network
        m_packetsIn = at(4, 119, 93, 81, 17);
        m_packetsOut = at(4, 119, 71, 81, 17);
        m_packetsInSec = at(4, 119, 49, 81, 17);
        m_packetsOutSec = at(4, 119, 27, 81, 17);
        m_netIn = at(4, 305, 96, 81, 14);
        m_netOut = at(4, 305, 74, 81, 14);
        m_netInSec = at(4, 305, 52, 63, 14);
        m_netOutSec = at(4, 305, 30, 63, 14);
        m_netHigh = at(4, 409, 111, 124, 17);
        wells(4, {{{373, 51}, green}, {{373, 29}, red}});
        m_netGraph = chartIn<LineGraphView>(at(4, 426, 41, 91, 69));
    }

    // the mounted volumes (Disk Usage's popup): real file systems only
    void fillVolumes()
    {
        m_mounts.clear();
        if (FILE *f = setmntent("/proc/self/mounts", "r")) {
            while (mntent *m = getmntent(f)) {
                const std::string type = m->mnt_type, dev = m->mnt_fsname;
                if (dev.rfind("/dev/", 0) != 0 || type == "squashfs")
                    continue;
                if (std::find(m_mounts.begin(), m_mounts.end(), std::string(m->mnt_dir)) == m_mounts.end())
                    m_mounts.push_back(m->mnt_dir);
            }
            endmntent(f);
        }
        if (!m_volumes)
            return;
        m_volumes->items.clear();
        for (const std::string &m : m_mounts)
            m_volumes->items.push_back(MenuItem(m == "/" ? "/" : m.substr(m.rfind('/') + 1)));
        m_volumes->setSelected(0);
        m_volumes->onSelect = [this](int) { updateDiskUsage(); };
        m_volumes->setNeedsDisplay();
    }

    void refresh()
    {
        m_sampler.sample();
        updateTable();
        updateCPU();
        updateMemory();
        updateDisks();
        updateDiskUsage();
        updateNetwork();
    }

    std::vector<Proc> filtered() const
    {
        const int show = m_show ? m_show->selected() : 2;
        const uid_t me = getuid();
        std::string filter = m_filter ? m_filter->text : "";
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
        std::vector<Proc> out;
        for (const Proc &p : m_sampler.procs) {
            bool keep = true;
            switch (show) {
            case 2: keep = p.uid == me; break;                  // My Processes
            case 3: keep = p.uid == 0; break;                   // System Processes
            case 4: keep = p.uid != me && p.uid != 0; break;    // Other User Processes
            case 5: keep = p.cpu > 0; break;                    // Active Processes
            case 6: keep = p.cpu <= 0; break;                   // Inactive Processes
            case 7: keep = p.windowed; break;                   // Windowed Processes
            case 8: keep = p.pid == m_selectedPid; break;       // Selected Processes
            default: break;
            }
            if (keep && !filter.empty()) {
                std::string n = p.name;
                std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                keep = n.find(filter) != std::string::npos || std::to_string(p.pid).find(filter) != std::string::npos;
            }
            if (keep)
                out.push_back(p);
        }
        // by % CPU, then by name
        std::sort(out.begin(), out.end(), [](const Proc &a, const Proc &b) {
            return a.cpu != b.cpu ? a.cpu > b.cpu : a.name < b.name;
        });
        return out;
    }

    void updateTable()
    {
        if (!m_table)
            return;
        m_shown = filtered();
        m_table->rows.clear();
        int selected = -1;
        for (size_t i = 0; i < m_shown.size(); ++i) {
            const Proc &p = m_shown[i];
            char cpu[32];
            std::snprintf(cpu, sizeof cpu, "%.1f", p.cpu);
            m_table->rows.push_back({std::to_string(p.pid), p.name, p.user, cpu, std::to_string(p.threads), bytes(double(p.rss)), p.kind});
            if (p.pid == m_selectedPid)
                selected = int(i);
        }
        m_table->selected = selected;
        if (selected < 0)
            m_selectedPid = 0;
        m_table->reload();
        updateToolbar();
    }

    // ImageTextCell: the application's icon before its name; the numbers right-aligned
    bool drawCell(cairo_t *cr, int row, int column, const Rect &cell, bool highlighted)
    {
        if (row < 0 || size_t(row) >= m_shown.size())
            return false;
        const Proc &p = m_shown[size_t(row)];
        TextStyle style;
        style.font = Font::system(11);
        style.color = highlighted ? Color::gray(255) : Color::gray(0);
        if (column == 1) {
            double x = cell.x + 2;
            if (p.app.valid()) {
                const Image icon = LaunchServices::icon(p.app, 16);
                if (!icon.isNull())
                    icon.draw(cr, Rect{x, cell.y + (cell.h - 16) / 2, 16, 16});
            }
            x += 20;
            drawText(cr, p.name, Rect{x, cell.y + 1, cell.w - (x - cell.x) - 2, cell.h - 2}, style);
            return true;
        }
        // the others in the small system font too, the numbers right-aligned
        if (column == 0 || column == 3 || column == 4 || column == 5)
            style.align = Align::Right;
        const std::string &t = m_table->rows[size_t(row)][size_t(column)];
        drawText(cr, t, Rect{cell.x + 2, cell.y + 1, cell.w - 4, cell.h - 2}, style);
        return true;
    }

    void updateCPU()
    {
        char buf[32];
        auto pct = [&](double v) {
            std::snprintf(buf, sizeof buf, "%.2f", v);
            return std::string(buf);
        };
        setText(m_cpuUser, pct(m_sampler.user + m_sampler.nice));
        setText(m_cpuSys, pct(m_sampler.system));
        setText(m_cpuIdle, pct(m_sampler.idle));
        setText(m_threads, grouped(m_sampler.threads));
        setText(m_processes, grouped(long(m_sampler.procs.size())));
        auto push = [](CPUHistoryView *v, std::array<double, 3> s) {
            if (!v)
                return;
            v->history.push_back(s);
            while (v->history.size() > 200)
                v->history.pop_front();
            v->setNeedsDisplay();
        };
        const auto &cpus = m_sampler.perCPU;
        // a view a processor (two here at most, as the nib's); one: the whole system's
        if (cpus.empty())   // the first sample: nothing to compare with yet
            return;
        if (cpus.size() >= 2) {
            push(m_history1, cpus[0]);
            push(m_history2, cpus[1]);
        } else {
            push(m_history1, {m_sampler.user, m_sampler.system, m_sampler.nice});
        }
        if (m_history2)
            m_history2->setHidden(cpus.size() < 2);
    }

    void updateMemory()
    {
        std::map<std::string, double> mem;
        std::istringstream in(readFile("/proc/meminfo"));
        std::string key;
        double value;
        std::string unit;
        while (in >> key >> value) {
            std::getline(in, unit);
            key.pop_back();
            mem[key] = value * 1024;
        }
        const double total = mem["MemTotal"], free = mem["MemFree"], active = mem["Active"], inactive = mem["Inactive"];
        const double wired = std::max(0.0, total - free - active - inactive);
        setText(m_free, bytes(free));
        setText(m_wired, bytes(wired));
        setText(m_active, bytes(active));
        setText(m_inactive, bytes(inactive));
        setText(m_used, bytes(total - free));
        double vm = 0;
        for (const Proc &p : m_sampler.procs)
            vm += double(p.vsize);
        setText(m_vmSize, bytes(vm));
        std::map<std::string, double> vmstat;
        std::istringstream vs(readFile("/proc/vmstat"));
        while (vs >> key >> value)
            vmstat[key] = value;
        setText(m_pageIns, bytes(vmstat["pgpgin"] * 1024));
        setText(m_pageOuts, bytes(vmstat["pgpgout"] * 1024));
        setText(m_swap, bytes(mem["SwapTotal"] - mem["SwapFree"]));
        setText(m_total, bytes(total));
        if (m_memPie) {
            m_memPie->values = {wired, active, inactive, free};
            m_memPie->setNeedsDisplay();
        }
    }

    // /proc/diskstats: the whole disks' (not their partitions) reads, writes and sectors
    void updateDisks()
    {
        std::istringstream in(readFile("/proc/diskstats"));
        std::string line;
        double reads = 0, writes = 0, rsec = 0, wsec = 0;
        while (std::getline(in, line)) {
            std::istringstream l(line);
            int major, minor;
            std::string name;
            double f[11] = {};
            l >> major >> minor >> name;
            for (double &v : f)
                l >> v;
            if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 || name.rfind("zram", 0) == 0 || name.rfind("dm-", 0) == 0)
                continue;
            if (!isDir("/sys/block/" + name))   // a partition
                continue;
            reads += f[0];
            rsec += f[2];
            writes += f[4];
            wsec += f[6];
        }
        const double dt = m_period;
        const double rs = m_lastDisk[0] > 0 ? (reads - m_lastDisk[0]) / dt : 0, ws = m_lastDisk[1] > 0 ? (writes - m_lastDisk[1]) / dt : 0;
        const double rb = m_lastDisk[2] > 0 ? (rsec - m_lastDisk[2]) * 512 / dt : 0, wb = m_lastDisk[3] > 0 ? (wsec - m_lastDisk[3]) * 512 / dt : 0;
        m_lastDisk = {reads, writes, rsec, wsec};
        setText(m_readsIn, grouped((long long)reads));
        setText(m_writesOut, grouped((long long)writes));
        setText(m_readsSec, grouped((long long)rs));
        setText(m_writesSec, grouped((long long)ws));
        setText(m_dataRead, bytes(rsec * 512));
        setText(m_dataWritten, bytes(wsec * 512));
        setText(m_dataReadSec, bytes(rb));
        setText(m_dataWrittenSec, bytes(wb));
        if (m_diskGraph) {   // the Data graph (DiskGraphType 1)
            m_diskGraph->add(rb, wb);
            setText(m_diskHigh, format(tr("Peak: %@/sec"), {bytes(m_diskGraph->peak())}));
        }
    }

    void updateDiskUsage()
    {
        if (m_mounts.empty())
            return;
        const int i = m_volumes ? std::clamp(m_volumes->selected(), 0, int(m_mounts.size()) - 1) : 0;
        struct statvfs st;
        if (statvfs(m_mounts[size_t(i)].c_str(), &st) != 0)
            return;
        const double total = double(st.f_blocks) * st.f_frsize, free = double(st.f_bavail) * st.f_frsize, used = total - free;
        setText(m_diskUsedText, bytes(used));
        setText(m_diskFreeText, bytes(free));
        setText(m_diskUsedBytes, grouped((long long)used) + " bytes");
        setText(m_diskFreeBytes, grouped((long long)free) + " bytes");
        setText(m_diskTotal, bytes(total));
        if (m_diskPie) {
            m_diskPie->values = {used, free};
            m_diskPie->setNeedsDisplay();
        }
    }

    void updateNetwork()
    {
        std::istringstream in(readFile("/proc/net/dev"));
        std::string line;
        double bin = 0, pin = 0, bout = 0, pout = 0;
        std::getline(in, line);
        std::getline(in, line);
        while (std::getline(in, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string name = line.substr(0, colon);
            name.erase(0, name.find_first_not_of(' '));
            if (name == "lo")
                continue;
            std::istringstream l(line.substr(colon + 1));
            double f[16] = {};
            for (double &v : f)
                l >> v;
            bin += f[0];
            pin += f[1];
            bout += f[8];
            pout += f[9];
        }
        const double dt = m_period;
        auto rate = [&](double now, double before) { return before > 0 ? (now - before) / dt : 0; };
        const double binS = rate(bin, m_lastNet[0]), pinS = rate(pin, m_lastNet[1]), boutS = rate(bout, m_lastNet[2]), poutS = rate(pout, m_lastNet[3]);
        m_lastNet = {bin, pin, bout, pout};
        setText(m_packetsIn, grouped((long long)pin));
        setText(m_packetsOut, grouped((long long)pout));
        setText(m_packetsInSec, grouped((long long)pinS));
        setText(m_packetsOutSec, grouped((long long)poutS));
        setText(m_netIn, bytes(bin));
        setText(m_netOut, bytes(bout));
        setText(m_netInSec, bytes(binS));
        setText(m_netOutSec, bytes(boutS));
        if (m_netGraph) {
            m_netGraph->add(binS, boutS);
            setText(m_netHigh, format(tr("Peak: %@/sec"), {bytes(m_netGraph->peak())}));
        }
    }

    const Proc *selected() const
    {
        for (const Proc &p : m_shown)
            if (p.pid == m_selectedPid)
                return &p;
        return nullptr;
    }

    // -quitProcessWithProcess:window:
    void quitProcess()
    {
        const Proc *p = selected();
        if (!p)
            return;
        const int pid = p->pid;
        const std::string name = p->name;
        const Image icon = Image::load(res("ActivityMonitor.icns"), {64, 64});
        if (p->uid != getuid()) {
            Alert::beginSheet(m_window.get(), icon, format(tr("You cannot quit the process “%@” because it is owned by %@."), {name, p->user}),
                              "", {"OK"}, nullptr);
            return;
        }
        Alert::beginSheet(m_window.get(), icon, tr("Are you sure you want to quit this process?"),
                          format(tr("Do you really want to quit “%@”?"), {name}), {tr("Quit"), tr("Cancel"), tr("Force Quit")},
                          [this, pid](int b) {
                              if (b == 0)
                                  ::kill(pid, SIGTERM);   // -quitPID:
                              else if (b == 2)
                                  ::kill(pid, SIGKILL);   // -forceQuitPID:
                              App::shared().later([this] { refresh(); });
                          });
    }

    // SMProcessInspector, in short: the process' parent, user, CPU and memory
    void inspect()
    {
        const Proc *p = selected();
        if (!p)
            return;
        std::string parent = std::to_string(p->ppid);
        for (const Proc &q : m_sampler.procs)
            if (q.pid == p->ppid)
                parent = q.name + " (" + parent + ")";
        char cpu[32];
        std::snprintf(cpu, sizeof cpu, "%.1f", p->cpu);
        const std::string info = "Parent Process: " + parent + "\nUser: " + p->user + " (" + std::to_string(p->uid) + ")\n% CPU: " + cpu +
                                 "\nThreads: " + std::to_string(p->threads) + "\nReal Memory: " + bytes(double(p->rss)) +
                                 "\nVirtual Memory: " + bytes(double(p->vsize)) + "\nKind: " + p->kind;
        Alert::run(Image::load(res("ActivityMonitor.icns"), {64, 64}), p->name + " (" + std::to_string(p->pid) + ")", info, {"OK"}, nullptr);
    }

    bool validate(const MainMenuItem &it, MenuValidation &v)
    {
        const std::string &a = it.action;
        if (a == "quitProcess:" || a == "inspect:")
            return m_selectedPid != 0;
        if (a == "changeUpdateFrequency:") {
            static const double periods[] = {0.5, 1, 2, 5};
            v.checked = it.tag >= 0 && it.tag < 4 && std::abs(periods[it.tag] - m_period) < 0.01;
            return true;
        }
        return a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
               a == "performClose:" || a == "performMiniaturize:" || a == "showProcessWindow:" || a == "filter:" ||
               a == "orderFrontStandardAboutPanel:";
    }

    void perform(const MainMenuItem &it)
    {
        const std::string &a = it.action;
        if (a == "terminate:" || a == "performClose:")
            App::shared().quit();
        else if (a == "performMiniaturize:")
            m_window->miniaturize();
        else if (a == "quitProcess:")
            quitProcess();
        else if (a == "inspect:")
            inspect();
        else if (a == "showProcessWindow:")
            m_window->show();
        else if (a == "filter:" && m_filter)
            m_window->makeFirstResponder(m_filter);
        else if (a == "changeUpdateFrequency:") {
            static const double periods[] = {0.5, 1, 2, 5};
            if (it.tag >= 0 && it.tag < 4) {
                m_period = periods[it.tag];
                Settings("activitymonitor").set("UpdatePeriod", std::to_string(m_period));
                App::shared().removeTimer(m_timer);
                m_timer = App::shared().addTimer(m_period, true, [this] { refresh(); });
            }
        }
    }

    MainMenu m_menu;
    Nib m_nib;
    std::unique_ptr<Window> m_window;
    Toolbar *m_toolbar = nullptr;
    View *m_content = nullptr;
    TableView *m_table = nullptr;
    TabView *m_tabs = nullptr;
    Button *m_quit = nullptr, *m_inspect = nullptr, *m_sample = nullptr;
    PopUpButton *m_show = nullptr, *m_volumes = nullptr;
    SearchField *m_filter = nullptr;
    Sampler m_sampler;
    std::vector<Proc> m_shown;
    int m_selectedPid = 0;
    double m_period = 2;
    int m_timer = 0;
    View *m_cpuUser = nullptr, *m_cpuSys = nullptr, *m_cpuIdle = nullptr, *m_threads = nullptr, *m_processes = nullptr;
    CPUHistoryView *m_history1 = nullptr, *m_history2 = nullptr;
    View *m_free = nullptr, *m_wired = nullptr, *m_active = nullptr, *m_inactive = nullptr, *m_used = nullptr, *m_vmSize = nullptr,
         *m_pageIns = nullptr, *m_pageOuts = nullptr, *m_swap = nullptr, *m_total = nullptr;
    PieView *m_memPie = nullptr, *m_diskPie = nullptr;
    View *m_readsIn = nullptr, *m_writesOut = nullptr, *m_readsSec = nullptr, *m_writesSec = nullptr, *m_dataRead = nullptr,
         *m_dataWritten = nullptr, *m_dataReadSec = nullptr, *m_dataWrittenSec = nullptr, *m_diskHigh = nullptr;
    LineGraphView *m_diskGraph = nullptr, *m_netGraph = nullptr;
    View *m_diskUsedText = nullptr, *m_diskFreeText = nullptr, *m_diskUsedBytes = nullptr, *m_diskFreeBytes = nullptr, *m_diskTotal = nullptr;
    std::vector<std::string> m_mounts;
    View *m_packetsIn = nullptr, *m_packetsOut = nullptr, *m_packetsInSec = nullptr, *m_packetsOutSec = nullptr, *m_netIn = nullptr,
         *m_netOut = nullptr, *m_netInSec = nullptr, *m_netOutSec = nullptr, *m_netHigh = nullptr;
    std::array<double, 4> m_lastDisk{}, m_lastNet{};
};

} // namespace

int main()
{
    App &app = App::shared();
    setProgramName("Activity Monitor");
    if (!app.open() || !Windows::open()) {
        std::fprintf(stderr, "Activity Monitor: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    if (!Assets::exists(std::string(Bundle) + "/Contents/Resources/English.lproj/ActivityMonitor.nib/keyedobjects.nib")) {
        std::fprintf(stderr, "Activity Monitor: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    if (Instance::handToRunning("Activity Monitor", {}))
        return 0;
    Monitor monitor;
    Instance::listen("Activity Monitor", [&monitor](std::vector<std::string>) { monitor.comeForward(); });
    return app.run();
}
