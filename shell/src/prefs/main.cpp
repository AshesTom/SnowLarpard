// SPDX-License-Identifier: LGPL-2.1-or-later
// System Preferences.app, on AquaKit (no Qt): Show All and every pane of 10.6, from their nibs.
//   System Preferences [--pane dock|sound|desktop|screensaver|<identifier>...]
//   System Preferences --screensaver           the screen saver, full screen (Test, and the Finder when idle)
#include "controls.h"
#include "desktoppane.h"
#include "foundation.h"
#include "nibpane.h"
#include "showall.h"
#include "slideshow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace AK;

namespace {

const char *const AppBundle = "/Applications/System Preferences.app";

// System Preferences' toolbar, under the title bar in one gradient (measured on 10.6, points): its
// buttons 4 points down, a 97 line and a 188 row under it
class Toolbar : public View {
public:
    static constexpr double Gradient = 31, Height = Gradient + 2;
    SegmentedControl *nav = nullptr;
    Button *showAll = nullptr;
    SearchField *search = nullptr;

    Toolbar()
    {
        nav = add<SegmentedControl>(CoreUI::SegmentStyle::Textured, std::vector<std::string>{"", ""}, std::vector<double>{25, 25});
        nav->momentary = true;
        nav->setFrame({9, 4, 50, 24});
        nav->glyph = [](cairo_t *cr, int i, const Rect &r, bool enabled) {
            // NSImageNameGoLeftTemplate / GoRight: a solid triangle, 9 high
            const double cx = std::round(r.x + r.w / 2), cy = std::round(r.y + r.h / 2);
            Color::gray(0, enabled ? 0.85 : 0.3).set(cr);
            const double d = i == 0 ? -1 : 1;
            cairo_move_to(cr, cx + 4 * d, cy);
            cairo_line_to(cr, cx - 3.5 * d, cy - 4.5);
            cairo_line_to(cr, cx - 3.5 * d, cy + 4.5);
            cairo_close_path(cr);
            cairo_fill(cr);
        };
        showAll = add<Button>(Assets::tr(AppBundle, "Localizable", "SHOWALL_LABEL", "Show All"), Button::Textured);
        showAll->setFrame({69, 4, 79, 24});
        search = add<SearchField>();
        search->setFrame({511, 5, 150, 23});
    }

    // WindowServer's unified title bar: a 218 first row, then 207 down to 167 at the toolbar's last
    // row (measured on 10.6); inactive lighter
    static Color rowColor(int row, int total, bool active)
    {
        if (row <= 0)
            return active ? Color::gray(218) : Color::gray(236);
        const double t = std::clamp(double(row - 1) / std::max(1, total - 2), 0.0, 1.0);
        const double top = active ? 207 : 232, bottom = active ? 167 : 207;
        return Color::gray(int(std::lround(top + (bottom - top) * t)));
    }

    void draw(cairo_t *cr) override
    {
        const bool active = windowIsKey();
        const int total = 22 + int(Gradient);   // with the title bar's rows
        for (int y = 0; y < Gradient; ++y)
            fill(cr, Rect{0, double(y), frame().w, 1}, rowColor(22 + y, total, active));
        fill(cr, Rect{0, Gradient, frame().w, 1}, active ? Color::gray(97) : Color::gray(150));
        fill(cr, Rect{0, Gradient + 1, frame().w, 1}, Color::gray(188));
    }
};

// The window: the toolbar, then Show All or a pane; back and forward through the panes seen
class Prefs {
public:
    explicit Prefs(Window &w) : m_window(w)
    {
        m_toolbar = w.contentView()->add<Toolbar>();
        m_toolbar->setFrame({0, 0, ShowAll::Width, Toolbar::Height});
        m_toolbar->nav->onSelect = [this](int i) { go(i == 0 ? -1 : 1); };
        m_toolbar->showAll->action = [this] { show(""); };
        m_toolbar->search->onChange = [this](const std::string &text) {
            // the search is Show All's: it comes back to it
            if (!m_current.empty())
                show("");
            if (m_showAll)
                m_showAll->setSearch(text);
        };
        NibPane::openPane = [this](const std::string &id) { show(id); };
        m_toolbar->search->onEnter = [this] {
            // Return: the first pane found
            const auto found = searchPanes(m_toolbar->search->text);
            if (!found.empty() && m_window.firstResponder() == m_toolbar->search)
                App::shared().later([this, id = found.front()] { show(id); });
        };
    }

    void show(const std::string &id, bool record = true)
    {
        if (m_pending) {
            // a pane still waiting for the window to fit: it goes
            delete m_pending;
            m_pending = nullptr;
            m_view = nullptr;
            m_showAll = nullptr;
        }
        if (m_view) {
            m_window.contentView()->removeSubview(m_view);
            m_view = nullptr;
            m_showAll = nullptr;
        }
        const PaneInfo *pane = id.empty() ? nullptr : paneById(id);
        Size size{ShowAll::Width, 0};
        if (!pane) {
            m_showAll = new ShowAll;
            m_showAll->setSearch(m_toolbar->search->text);
            m_showAll->onOpen = [this](const std::string &paneId) { App::shared().later([this, paneId] { show(paneId); }); };
            size.h = m_showAll->height();
            m_view = m_showAll;
            m_window.setTitle(Assets::tr(AppBundle, "Localizable", "System Preferences", "System Preferences"));
        } else {
            m_view = makePane(*pane, &size);
            m_window.setTitle(pane->name());
        }
        m_current = pane ? pane->id : std::string();
        // the window keeps its width; the pane in its middle
        m_view->setFrame({std::floor((ShowAll::Width - size.w) / 2), Toolbar::Height, size.w, size.h});
        resizeTo(Toolbar::Height + size.h, m_view);
        if (record) {
            m_history.resize(size_t(m_at + 1));
            if (m_history.empty() || m_history.back() != m_current)
                m_history.push_back(m_current);
            m_at = int(m_history.size()) - 1;
        }
        m_toolbar->nav->segmentEnabled = {m_at > 0, m_at + 1 < int(m_history.size())};
        m_toolbar->setNeedsDisplay();
    }

    // -[NSWindow setFrame:display:animate:]: the window grows or shrinks to the pane's height (its top
    // stays), eased over about 0.2 seconds, the pane shown once it fits
    void resizeTo(double height, View *view)
    {
        if (m_animation)
            App::shared().removeTimer(m_animation);
        m_animation = 0;
        const double from = m_window.size().h;
        if (!m_window.isVisible() || std::abs(from - height) < 1) {
            m_window.setContentSize({ShowAll::Width, height});
            m_window.contentView()->addSubview(view);
            return;
        }
        const double start = now(), duration = std::min(0.25, 0.1 + std::abs(height - from) / 2000);
        m_pending = view;
        m_animation = App::shared().addTimer(1.0 / 60, true, [this, from, height, start, duration] {
            const double t = std::min(1.0, (now() - start) / duration);
            const double eased = t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
            m_window.setContentSize({ShowAll::Width, std::round(from + (height - from) * eased)});
            if (t < 1)
                return;
            App::shared().removeTimer(m_animation);
            m_animation = 0;
            m_window.contentView()->addSubview(m_pending);
            m_pending = nullptr;
        });
    }

    void go(int step)
    {
        const int to = m_at + step;
        if (to < 0 || to >= int(m_history.size()))
            return;
        m_at = to;
        App::shared().later([this, id = m_history[size_t(to)]] { show(id, false); });
    }

    View *view() const { return m_view; }
    Toolbar *toolbar() const { return m_toolbar; }

private:
    Window &m_window;
    Toolbar *m_toolbar = nullptr;
    View *m_view = nullptr;
    ShowAll *m_showAll = nullptr;
    std::string m_current;
    std::vector<std::string> m_history;
    int m_at = -1;
    int m_animation = 0;
    View *m_pending = nullptr;   // the pane shown when the window has its size
};

int screenSaver()
{
    // full screen over everything, the pointer hidden, every key and click its own
    App &app = App::shared();
    const Rect screen = app.screenFrame();
    Window window({screen.w, screen.h}, "Screen Saver", Window::FullScreen);
    auto *saver = window.contentView()->add<Slideshow>(true);
    saver->setFrame({0, 0, screen.w, screen.h});
    window.makeFirstResponder(saver);
    window.show();
    window.grabInput();
    app.later([saver] { saver->setFolder(ScreenSaver::folder()); });
    return app.run();
}

} // namespace

int main(int argc, char **argv)
{
    App &app = App::shared();
    if (!app.open()) {
        std::fprintf(stderr, "System Preferences: no display (Wayland or X11)\n");
        return 1;
    }
    std::string pane;
    bool asked = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--screensaver"))
            return screenSaver();
        if (!std::strcmp(argv[i], "--pane") && i + 1 < argc) {
            pane = argv[++i];
            asked = true;
        }
    }

    Window window({ShowAll::Width, Toolbar::Height + 400}, "System Preferences");
    Prefs prefs(window);
    // the pane asked for, else Show All (-[NSPrefPanesManager applicationDidFinishLaunching:] sends
    // showAll: unless a pane's window is already up)
    const PaneInfo *first = asked ? paneByShortName(pane) : nullptr;
    prefs.show(first ? first->id : std::string());
    if (asked && pane == "screensaver")
        if (auto *d = dynamic_cast<DesktopPane *>(prefs.view()))
            d->showTab(1);
    window.setUnifiedToolbarHeight(int(Toolbar::Gradient));
    window.setIcon(Image::load(Assets::path(std::string(AppBundle) + "/Contents/Resources/PrefApp.icns"), {128, 128}));
    window.makeFirstResponder(prefs.toolbar()->search);   // as 10.6 opens: the search field has the keyboard
    window.onClose = [&app] { app.quit(); };
    window.show();
    return app.run();
}
