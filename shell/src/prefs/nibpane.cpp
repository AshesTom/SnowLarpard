// SPDX-License-Identifier: LGPL-2.1-or-later
#include "nibpane.h"

#include "desktoppane.h"
#include "foundation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <spawn.h>

extern char **environ;

using namespace AK;

void then(View *v, std::function<void()> f)
{
    auto chain = [](std::function<void()> &slot, std::function<void()> f) {
        auto old = slot;
        slot = [old, f] {
            if (old)
                old();
            f();
        };
    };
    if (auto *c = dynamic_cast<class Control *>(v))
        chain(c->action, f);
    if (auto *s = dynamic_cast<Slider *>(v)) {
        auto old = s->onChange;
        s->onChange = [old, f](double x) {
            if (old)
                old(x);
            f();
        };
    }
    if (auto *g = dynamic_cast<RadioGroup *>(v)) {
        auto old = g->onSelect;
        g->onSelect = [old, f](int i) {
            if (old)
                old(i);
            f();
        };
    }
    if (auto *t = dynamic_cast<TextField *>(v))
        chain(t->onEnter, f);
    if (auto *t = dynamic_cast<TableView *>(v)) {
        auto old = t->onSelect;
        t->onSelect = [old, f](int i) {
            if (old)
                old(i);
            f();
        };
    }
}

std::string run(const std::string &command)
{
    std::string out;
    if (FILE *p = popen((command + " 2>/dev/null").c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0)
            out.append(buf, n);
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

void spawn(const std::vector<std::string> &argv)
{
    std::vector<std::string> copy = argv;
    std::vector<char *> args;
    for (std::string &a : copy)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid;
    posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ);
}

bool onX11() { return App::shared().platformName() == "x11"; }

NibPane::NibPane(const PaneInfo &i) : info(i)
{
    clipsToBounds = false;
    // the pane's nib: its main one (Mouse's is empty: the content is the mouse's own nib)
    std::string name = info.nib;
    if (info.id == "com.apple.preference.mouse")
        name = "RegularMouse";
    const std::string file = info.resources() + "/English.lproj/" + name + ".nib/keyedobjects.nib";
    if (!nib.load(file, info.resources()))
        return;
    View *root = nib.instantiate("_window");
    if (!root)
        root = nib.instantiate("view");
    if (!root)
        return;
    size = nib.size();
    setFrame({0, 0, size.w, size.h});
    addSubview(root);
    m_loaded = true;
    remember();
    connectPanels();
    wirePane(*this);
}

NibPane::~NibPane()
{
    for (int t : m_timers)
        App::shared().removeTimer(t);
}

void NibPane::every(double seconds, std::function<void()> f) { m_timers.push_back(App::shared().addTimer(seconds, true, std::move(f))); }

std::function<void(const std::string &)> NibPane::openPane;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(std::string s, const std::string &word)
{
    const size_t at = s.find(word);
    if (at != std::string::npos)
        s.erase(at, word.size());
    return s;
}

// "Open Language & Text…", "Bluetooth Preferences…", "Open Print Preferences…": the pane they name
const PaneInfo *paneNamedBy(std::string title)
{
    if (title.size() < 4 || title.compare(title.size() - 3, 3, "…") != 0)
        return nullptr;
    title = title.substr(0, title.size() - 3);
    title = trim(trim(trim(title, "Open "), " Preferences"), " Utility");
    if (title.empty())
        return nullptr;
    const std::string want = lower(title);
    for (const PaneGroup &g : catalog())
        for (const PaneInfo &p : g.panes)
            if (lower(p.name()).rfind(want, 0) == 0)
                return &p;
    return nullptr;
}

// the panels that are in nibs of their own, or that their window's title doesn't name
struct PanelSpec {
    const char *pane, *action, *nib, *outlet, *containsAction;
};
const PanelSpec Panels[] = {
    {"com.apple.preference.energysaver", "showScheduleSheet:", "", "", "setScheduleAndDismissSheet:"},
    {"com.apple.preference.keyboard", "advancedOptions:", "", "", "modifierMappingDidChange:"},
    {"com.apple.preference.mouse", "zoomOptions:", "SWZoomOptions", "mZoomOptionsWindow", ""},
    {"com.apple.preference.trackpad", "zoomOptions:", "SWZoomOptions", "mZoomOptionsWindow", ""},
    {"com.apple.preferences.Bluetooth", "handleShowAdvancedOptions:", "", "_advancedOptionsPanel", ""},
    {"com.apple.preference.printfax", "createPrinterPoolPressed:", "", "_createPrinterPoolSheet", ""},
};

// a nib's window by the outlet that names it, an action of its controls, or its title
int findWindow(const Nib &nib, const std::string &outlet, const std::string &action, const std::string &title)
{
    if (!outlet.empty() && nib.outletObject(outlet) >= 0)
        return nib.outletObject(outlet);
    for (const auto &w : nib.windows()) {
        if (!action.empty() && std::find(w.actions.begin(), w.actions.end(), action) != w.actions.end())
            return w.object;
    }
    if (!title.empty())
        for (const auto &w : nib.windows())
            if (w.outlet != "_window" && (lower(w.title).find(lower(title)) != std::string::npos ||
                                          lower(w.outlet).find(lower(trim(title, " "))) != std::string::npos))
                return w.object;
    return -1;
}

bool ends(const std::string &s, const char *suffix);

// "Options…" or "Options...": the title without its dots
std::string undotted(const std::string &title)
{
    if (ends(title, "\u2026"))
        return title.substr(0, title.size() - 3);
    if (ends(title, "..."))
        return title.substr(0, title.size() - 3);
    return {};
}

bool ends(const std::string &s, const char *suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

Nib *NibPane::loadPart(const std::string &name)
{
    auto other = std::make_unique<Nib>();
    if (!other->load(info.resources() + "/English.lproj/" + name + ".nib/keyedobjects.nib", info.resources()))
        return nullptr;
    parts.push_back(std::move(other));
    return parts.back().get();
}

void NibPane::openSheet(Nib &from, int panel, Size size)
{
    Size s;
    View *content = from.instantiateWindow(panel, &s);
    if (content)
        presentSheet(content, s.w > 0 ? s : size);
}

void NibPane::presentSheet(View *content, Size size)
{
    // the buttons that end it: OK, Cancel, Done... (its code would do what they say first)
    std::function<void(View *)> wire = [&](View *v) {
        if (auto *b = dynamic_cast<Button *>(v)) {
            static const char *const closing[] = {"OK", "Cancel", "Done", "Close", "Apply", "Save", "Create", "Continue", "Remove", "Set"};
            const std::string t = trim(b->title, "…");
            const bool closes = std::any_of(std::begin(closing), std::end(closing), [&](const char *c) { return t == c; });
            if (closes || b->isDefault) {
                Window *w = window();
                then(b, [w] { Sheet::end(w); });
            }
        }
        for (View *c : v->subviews())
            wire(c);
    };
    wire(content);
    Sheet::begin(window(), content, size, toWindow({0, 0}).y);
}

void NibPane::connectPanels()
{
    for (const auto &c : nib.controls()) {
        auto *button = dynamic_cast<Button *>(c.view);
        if (!button)
            continue;
        // a button that opens another pane
        if (const PaneInfo *other = paneNamedBy(button->title)) {
            if (other->id != info.id) {
                const std::string id = other->id;
                then(button, [id] {
                    if (openPane)
                        App::shared().later([id] { openPane(id); });
                });
                continue;
            }
        }
        if (c.action.empty())
            continue;
        // a button that opens a panel: in another nib, or one of this nib's windows
        const PanelSpec *spec = nullptr;
        for (const PanelSpec &p : Panels)
            if (info.id == p.pane && c.action == p.action)
                spec = &p;
        Nib *source = &nib;
        if (spec && *spec->nib) {
            source = loadPart(spec->nib);
            if (!source)
                continue;
        }
        const int window = spec ? findWindow(*source, spec->outlet, spec->containsAction, "")
                                : (!undotted(button->title).empty() ? findWindow(*source, "", "", trim(undotted(button->title), "Show ")) : -1);
        if (window < 0)
            continue;
        then(button, [this, source, window] {
            App::shared().later([this, source, window] { openSheet(*source, window, {}); });
        });
    }
}

void NibPane::remember()
{
    // each control's state by its place in the nib: c0, c1... (the nib doesn't change)
    auto settings = std::make_shared<Settings>("panes/" + info.id);
    const auto &controls = nib.controls();
    for (size_t i = 0; i < controls.size(); ++i) {
        View *v = controls[i].view;
        const std::string key = "c" + std::to_string(i);
        const std::string saved = settings->string(key);
        if (auto *b = dynamic_cast<CheckBox *>(v)) {
            if (!saved.empty())
                {
                b->setChecked(saved == "1");
                m_restored.insert(v);
            }
            then(b, [settings, key, b] { settings->set(key, std::string(b->checked() ? "1" : "0")); });
        } else if (auto *s = dynamic_cast<Slider *>(v)) {
            if (!saved.empty())
                {
                s->setDoubleValue(std::atof(saved.c_str()));
                m_restored.insert(v);
            }
            then(s, [settings, key, s] { settings->set(key, std::to_string(s->doubleValue())); });
        } else if (auto *p = dynamic_cast<PopUpButton *>(v)) {
            if (!saved.empty() && !p->pullDown)
                {
                p->setSelected(std::atoi(saved.c_str()));
                m_restored.insert(v);
            }
            then(p, [settings, key, p] { settings->set(key, p->selected()); });
        } else if (auto *g = dynamic_cast<RadioGroup *>(v)) {
            if (!saved.empty())
                {
                g->select(std::atoi(saved.c_str()));
                m_restored.insert(v);
            }
            then(g, [settings, key, g] { settings->set(key, g->selected()); });
        } else if (auto *t = dynamic_cast<TextField *>(v)) {
            if (!saved.empty() && t->editable && !t->secure)
                {
                t->setText(saved);
                m_restored.insert(v);
            }
            then(t, [settings, key, t] {
                if (!t->secure)
                    settings->set(key, t->text);
            });
        }
    }
}

View *makePane(const PaneInfo &info, Size *size)
{
    if (info.id == "com.apple.preference.desktopscreeneffect") {
        auto *d = new DesktopPane;
        *size = {DesktopPane::Width, DesktopPane::Height};
        return d;
    }
    if (View *v = handPane(info, size))
        return v;
    auto *p = new NibPane(info);
    if (!p->loaded()) {
        delete p;
        // 10.6's message when a pane can't load
        auto *v = new View;
        *size = {668, 120};
        v->setFrame({0, 0, size->w, size->h});
        auto *l = v->add<Label>(Assets::tr("/Applications/System Preferences.app", "Localizable", "CouldNotLoad", "Could not load %@ preference pane."));
        std::string text = l->text();
        const size_t at = text.find("%@");
        if (at != std::string::npos)
            text.replace(at, 2, info.name());
        l->setText(text);
        l->style.align = Align::Center;
        l->setFrame({20, 50, 628, 20});
        return v;
    }
    *size = p->size;
    return p;
}
