// SPDX-License-Identifier: LGPL-2.1-or-later
#include "preferences.h"

#include "files.h"
#include "labels.h"
#include "sidebar.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"
#include "text.h"
#include "toolbar.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <memory>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";

bool inside(const View *v, const View *ancestor)
{
    for (; v; v = v->superview())
        if (v == ancestor)
            return true;
    return false;
}

// the window's toolbar: the four panes' icons (generalpref, labelpref, sidebarpref, advancedpref)
// with their names under them, the selected one on a darker rounded back (NSToolbar's selectable items)
class PaneBar : public View {
public:
    static constexpr double Height = Toolbar::Gradient + 1, ItemWidth = 64;
    std::vector<std::pair<std::string, Image>> items;
    int selected = 0;
    std::function<void(int)> onSelect;

    void draw(cairo_t *cr) override
    {
        const bool active = windowIsKey();
        // the unified title bar's gradient continued (the title bar's 22 rows above it)
        const int rows = int(Toolbar::Gradient);
        for (int r = 0; r < rows; ++r)
            fill(cr, Rect{0, double(r), frame().w, 1}, Toolbar::rowColor(r + 22, rows + 22, active));
        fill(cr, Rect{0, Height - 1, frame().w, 1}, active ? Color::gray(81) : Color::gray(135));
        const double start = 8;
        for (size_t i = 0; i < items.size(); ++i) {
            const double x = start + i * ItemWidth;
            if (int(i) == selected) {
                const Rect r{x + 2, 2, ItemWidth - 4, Height - 5};
                cairo_new_path(cr);
                const double rad = 4;
                cairo_arc(cr, r.right() - rad, r.y + rad, rad, -M_PI / 2, 0);
                cairo_arc(cr, r.right() - rad, r.bottom() - rad, rad, 0, M_PI / 2);
                cairo_arc(cr, r.x + rad, r.bottom() - rad, rad, M_PI / 2, M_PI);
                cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 3 * M_PI / 2);
                cairo_close_path(cr);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.13);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
                cairo_set_line_width(cr, 1);
                cairo_stroke(cr);
            }
            items[i].second.draw(cr, Rect{x + (ItemWidth - 32) / 2, 4, 32, 32});
            TextStyle t;
            t.font = Font::system(11);
            t.align = Align::Center;
            t.engraved = true;
            t.color = active ? Color::gray(0) : Color::gray(0, 0.5);
            drawText(cr, items[i].first, Rect{x, 38, ItemWidth, 14}, t);
        }
    }
    void mouseDown(Event &e) override
    {
        const int i = int(std::floor((e.location.x - 8) / ItemWidth));
        if (i >= 0 && i < int(items.size()) && onSelect)
            onSelect(i);
    }
};

// a label's colour beside its name (TLabelSwatch: the gumdrop's colour, a darker rim)
class Swatch : public View {
public:
    int label = 0;
    void draw(cairo_t *cr) override
    {
        const Color c = Labels::color(label);
        const Rect r{4.5, 2.5, frame().w - 9, frame().h - 5};
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, r.y, 0, r.bottom());
        cairo_pattern_add_color_stop_rgb(g, 0, std::min(1.0, c.r + 0.15), std::min(1.0, c.g + 0.15), std::min(1.0, c.b + 0.15));
        cairo_pattern_add_color_stop_rgb(g, 1, c.r, c.g, c.b);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        cairo_set_source_rgba(cr, c.r * 0.6, c.g * 0.6, c.b * 0.6, 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    View *hitTest(Point) override { return nullptr; }
};

struct PrefsWindow {
    std::unique_ptr<Nib> nib;
    std::unique_ptr<Window> window;
    PaneBar *bar = nullptr;
    std::vector<View *> panes;    // General, Labels, Sidebar, Advanced
    std::vector<Size> sizes;
    int current = -1;
};
std::unique_ptr<PrefsWindow> s_prefs;

void changed()
{
    if (Preferences::onChange)
        Preferences::onChange();
}

void selectPane(int i)
{
    PrefsWindow &p = *s_prefs;
    if (i < 0 || i >= int(p.panes.size()))
        return;
    p.current = i;
    Settings("finder").set("PreferencesPane", i);
    for (size_t k = 0; k < p.panes.size(); ++k)
        p.panes[k]->setHidden(int(k) != i);
    const Size s = p.sizes[size_t(i)];
    p.window->setContentSize({std::max(s.w, 4 * PaneBar::ItemWidth + 16), s.h + PaneBar::Height});
    p.bar->setFrame({0, 0, p.window->size().w, PaneBar::Height});
    p.panes[size_t(i)]->setFrame({std::floor((p.window->size().w - s.w) / 2), PaneBar::Height, s.w, s.h});
    p.bar->selected = i;
    p.bar->setNeedsDisplay();
    p.window->setTitle(p.bar->items[size_t(i)].first);
}

void build()
{
    auto p = std::make_unique<PrefsWindow>();
    p->nib = std::make_unique<Nib>();
    const std::string res = std::string(Bundle) + "/Contents/Resources/";
    if (!p->nib->load(Assets::path(res + "English.lproj/PreferencesWindow.nib/keyedobjects.nib"), Assets::path(res)))
        return;
    Nib &nib = *p->nib;
    // each pane controller's _view; told apart by an outlet each one has
    std::vector<View *> views;
    std::vector<Size> sizes;
    for (int object : nib.outletObjects("_view")) {
        Size size;
        View *v = nib.instantiateWindow(object, &size);
        views.push_back(v);
        sizes.push_back(size);
    }
    const char *const markers[4] = {"_newFinderWindowsBtn", "_field1", "_homeBtn", "_showAllFileExtensionsBtn"};
    p->panes.assign(4, nullptr);
    p->sizes.assign(4, {});
    for (int k = 0; k < 4; ++k)
        if (View *m = nib.outlet(markers[k]))
            for (size_t i = 0; i < views.size(); ++i)
                if (inside(m, views[i])) {
                    p->panes[size_t(k)] = views[i];
                    p->sizes[size_t(k)] = sizes[i];
                }
    for (View *v : p->panes)
        if (!v)
            return;

    p->window = std::make_unique<Window>(Size{420, 300}, "General", Window::Titled);
    View *root = p->window->contentView();
    p->bar = root->add<PaneBar>();
    auto icon = [&](const char *name) { return Image::load(Assets::path(res + name), {32, 32}); };
    p->bar->items = {{"General", icon("generalpref.icns")},
                     {"Labels", icon("labelpref.icns")},
                     {"Sidebar", icon("sidebarpref.icns")},
                     {"Advanced", icon("advancedpref.icns")}};
    p->bar->onSelect = [](int i) { selectPane(i); };
    for (View *v : p->panes)
        root->addSubview(v);
    p->window->setUnifiedToolbarHeight(int(Toolbar::Gradient));
    p->window->onClose = [] { App::shared().later([] { s_prefs.reset(); }); };

    Settings conf("finder");
    auto check = [&](const char *outlet, const std::string &key, bool fallback, std::function<void()> after = {}) {
        if (auto *c = nib.outlet<CheckBox>(outlet)) {
            c->setChecked(Settings("finder").boolean(key, fallback));
            c->onToggle = [key, after](CheckBox *b) {
                Settings("finder").set(key, b->checked());
                if (after)
                    after();
                changed();
            };
        }
    };
    // General
    check("_hardDisksOnDesktopBtn", "ShowHardDrivesOnDesktop", true);
    check("_externalDisksOnDesktopBtn", "ShowExternalHardDrivesOnDesktop", true);
    check("_otherDisksOnDesktopBtn", "ShowRemovableMediaOnDesktop", true);
    check("_connectedServersOnDesktopBtn", "ShowMountedServersOnDesktop", false);
    check("_alwaysOpenNewWindowsBtn", "AlwaysOpenFoldersInNewWindow", false);
    check("_springLoadedBtn", "SpringingEnabled", true);
    if (auto *pop = nib.outlet<PopUpButton>("_newFinderWindowsBtn")) {
        // the computer, the startup disk, home, Desktop, Documents, Other…
        const std::string h = home();
        char host[256] = "Computer";
        gethostname(host, sizeof host - 1);
        std::string disk = displayName("/");
        pop->items = {{host}, {disk}, {fileName(h)}, {"Desktop"}, {"Documents"}, MenuItem::line(), {"Other…"}};
        static const char *const targets[] = {"PfCm", "PfVo", "PfHm", "PfDe", "PfDo", "", "PfLo"};
        const std::string target = conf.string("NewWindowTarget", "PfHm");
        for (int i = 0; i < 7; ++i)
            if (target == targets[i])
                pop->setSelected(i);
        pop->onSelect = [](int i) {
            static const char *const targets[] = {"PfCm", "PfVo", "PfHm", "PfDe", "PfDo", "", "PfLo"};
            if (i >= 0 && i < 7 && *targets[i])
                Settings("finder").set("NewWindowTarget", targets[i]);
        };
    }
    if (auto *s = nib.outlet<Slider>("_delayBtn")) {
        // the springing delay: 0 to 6 on its seven ticks (com.apple.springing.delay, seconds)
        s->setDoubleValue(conf.string("SpringingDelay", "0.5") == "0" ? 0 : std::atof(conf.string("SpringingDelay", "0.5").c_str()) * 6 / 1.5);
        s->onChange = [](double v) { Settings("finder").set("SpringingDelay", std::to_string(v * 1.5)); };
    }

    // Labels: the swatches and their names
    for (int i = 1; i <= 7; ++i) {
        const int index = Labels::Order[i - 1];
        if (View *sw = nib.outlet("_swatch" + std::to_string(i))) {
            auto *s = sw->add<Swatch>();
            s->label = index;
            s->setFrame(sw->bounds());
        }
        if (auto *f = nib.outlet<TextField>("_field" + std::to_string(i))) {
            f->setText(Labels::name(index));
            f->onChange = [index](const std::string &t) {
                Labels::setName(index, t);
                changed();
            };
        }
    }

    // Sidebar
    const struct {
        const char *outlet, *item;
    } sidebar[] = {{"_computerBtn", "Computer"},     {"_hardDisksBtn", "HardDisks"},   {"_externalDisksBtn", "ExternalDisks"},
                   {"_iDiskBtn", "iDisk"},           {"_ejectablesBtn", "Removable"},  {"_btmmBtn", "BackToMyMac"},
                   {"_serversBtn", "Servers"},       {"_bonjourBtn", "Bonjour"},       {"_homeBtn", "Home"},
                   {"_desktopBtn", "Desktop"},       {"_applicationsBtn", "Applications"}, {"_documentsBtn", "Documents"},
                   {"_todayBtn", "Today"},           {"_yesterdayBtn", "Yesterday"},   {"_pastWeekBtn", "PastWeek"},
                   {"_allImagesBtn", "AllImages"},   {"_allMoviesBtn", "AllMovies"},   {"_allDocumentsBtn", "AllDocuments"}};
    for (const auto &s : sidebar)
        check(s.outlet, std::string("SidebarShow") + s.item, true);

    // Advanced
    check("_showAllFileExtensionsBtn", "AppleShowAllExtensions", false);
    check("_showExtensionWarningBtn", "FXEnableExtensionChangeWarning", true);
    check("_showTrashWarningBtn", "WarnOnEmptyTrash", true);
    check("_emptyTrashSecurelyBtn", "EmptyTrashSecurely", false);
    if (auto *pop = nib.outlet<PopUpButton>("_defaultSearchLocationBtn")) {
        static const char *const scopes[] = {"SCev", "SCcf", "SCsp"};
        const std::string scope = conf.string("FXDefaultSearchScope", "SCev");
        for (int i = 0; i < 3; ++i)
            if (scope == scopes[i])
                pop->setSelected(i);
        pop->onSelect = [](int i) {
            if (i >= 0 && i < 3)
                Settings("finder").set("FXDefaultSearchScope", scopes[i]);
        };
    }
    s_prefs = std::move(p);
}

} // namespace

void Preferences::show(int pane)
{
    if (!s_prefs)
        build();
    if (!s_prefs)
        return;
    if (pane < 0)
        pane = s_prefs->current >= 0 ? s_prefs->current : std::clamp(Settings("finder").integer("PreferencesPane", 0), 0, 3);
    selectPane(pane);
    s_prefs->window->show();
}

} // namespace Finder
