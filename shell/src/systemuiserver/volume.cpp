// SPDX-License-Identifier: LGPL-2.1-or-later
#include "volume.h"

#include "foundation.h"
#include "runloop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <regex>

using namespace AK;

namespace {

class VolumeView : public SystemUI::MenuExtraView {
public:
    explicit VolumeView(AppleVolumeExtra *e) : MenuExtraView(e), volume(e) {}
    AppleVolumeExtra *volume;
    void draw(cairo_t *cr) override
    {
        // Volume1 (no waves) to Volume4 (three)
        const int l = volume->level();
        const int n = volume->muted() || l == 0 ? 1 : l < 34 ? 2 : l < 67 ? 3 : 4;
        const bool down = volume->isMenuDown();
        const Image img = m_extra->templateImage("Volume" + std::to_string(n) + ".pdf", down);
        if (img.isNull())
            return;
        img.draw(cr, Point{std::floor((frame().w - img.width()) / 2), std::floor((frame().h - 1 - img.height()) / 2)},
                 volume->available() ? 1.0 : 0.5);
    }
};

// the menu's view (Volume.menu's MenuItem.nib, _volumeItemView): 30 x 104, the slider (_levelSlider)
// at {4, 2}, 21 x 100, 0 to 1
class SliderView : public View {
public:
    explicit SliderView(AppleVolumeExtra *v) : volume(v)
    {
        slider = add<Slider>();
        slider->setFrame(Rect{4, 2, 21, 100});
        slider->setValue(volume->muted() ? 0 : volume->level() / 100.0);
        slider->onChange = [this](double v) { volume->set(int(std::lround(v * 100))); };
    }
    AppleVolumeExtra *volume;
    Slider *slider;
};

// pactl's devices: name and description
std::vector<std::pair<std::string, std::string>> devices(const char *kind)
{
    std::vector<std::pair<std::string, std::string>> out;
    FILE *f = popen((std::string("pactl list ") + kind + " 2>/dev/null").c_str(), "r");
    if (!f)
        return out;
    char line[1024];
    std::string name;
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == ' '))
            l.pop_back();
        const size_t n = l.find("Name: "), d = l.find("Description: ");
        if (n != std::string::npos && l.find_first_not_of(" \t") == n)
            name = l.substr(n + 6);
        else if (d != std::string::npos && !name.empty() && l.find_first_not_of(" \t") == d) {
            // a source that is a sink's monitor isn't an input
            if (name.size() < 8 || name.compare(name.size() - 8, 8, ".monitor") != 0)
                out.emplace_back(name, l.substr(d + 13));
            name.clear();
        }
    }
    pclose(f);
    return out;
}

std::string firstLine(const char *command)
{
    std::string out;
    if (FILE *f = popen(command, "r")) {
        char line[512];
        if (fgets(line, sizeof line, f))
            out = line;
        pclose(f);
    }
    while (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

} // namespace

AppleVolumeExtra::AppleVolumeExtra(const std::string &bundle) : MenuExtra(bundle)
{
    setView(new VolumeView(this));
    refresh();
    m_timer = App::shared().addTimer(3, true, [this] { refresh(); });
}

AppleVolumeExtra::~AppleVolumeExtra() { willUnload(); }

void AppleVolumeExtra::willUnload()
{
    if (m_timer)
        App::shared().removeTimer(m_timer);
    m_timer = 0;
    m_task.reset();
}

void AppleVolumeExtra::refresh()
{
    if (m_task && m_task->isRunning())
        return;
    m_task = std::make_unique<Task>();
    m_task->onExit = [this](int code, const std::string &out) {
        static const std::regex pct(R"((\d+)%)");
        std::smatch m;
        const bool available = code == 0 && !out.empty();
        const int level = std::regex_search(out, m, pct) ? std::stoi(m[1]) : m_level;
        const bool muted = out.find("Mute: yes") != std::string::npos;
        if (available != m_available || level != m_level || muted != m_muted) {
            m_available = available, m_level = level, m_muted = muted;
            setNeedsDisplay();
        }
    };
    m_task->launch({"sh", "-c", "pactl get-sink-volume @DEFAULT_SINK@ && pactl get-sink-mute @DEFAULT_SINK@"});
}

void AppleVolumeExtra::set(int percent)
{
    m_level = std::clamp(percent, 0, 100);
    m_muted = m_level == 0;
    Task::launchDetached({"pactl", "set-sink-volume", "@DEFAULT_SINK@", std::to_string(m_level) + "%"});
    Task::launchDetached({"pactl", "set-sink-mute", "@DEFAULT_SINK@", m_muted ? "1" : "0"});
    setNeedsDisplay();
}

std::vector<MenuItem> AppleVolumeExtra::menu()
{
    // Option-click: the devices (-buildDeviceMenu:, MenuItem.nib's _deviceMenu)
    if (modifierFlags() & Option) {
        std::vector<MenuItem> m;
        auto section = [&](const char *title, const char *kind, const char *getDefault, const char *setDefault) {
            m.emplace_back(title, false, false);
            const std::string current = firstLine(getDefault);
            for (const auto &[name, description] : devices(kind)) {
                MenuItem i(description, false, true, name == current);
                i.indentation = 1;
                const std::string n = name, set = setDefault;
                i.action = [n, set] { Task::launchDetached({"pactl", set, n}); };
                m.push_back(i);
            }
        };
        section("Output Device:", "sinks", "pactl get-default-sink 2>/dev/null", "set-default-sink");
        m.push_back(MenuItem::line());
        section("Input Device:", "sources", "pactl get-default-source 2>/dev/null", "set-default-source");
        m.push_back(MenuItem::line());
        MenuItem prefs("Sound Preferences…");
        prefs.action = [] { openPreferencePane("sound"); };
        m.push_back(prefs);
        return m;
    }
    MenuItem slider;
    slider.viewHeight = 104;
    slider.viewWidth = 30;
    slider.makeView = [this](double width) {
        auto *v = new SliderView(this);
        v->setFrame(Rect{std::floor((width - 30) / 2), 0, 30, 104});
        return v;
    };
    return {slider};
}

SYSTEMUI_REGISTER_EXTRA("AppleVolumeExtra", AppleVolumeExtra);
