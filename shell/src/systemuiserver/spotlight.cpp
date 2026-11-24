// SPDX-License-Identifier: LGPL-2.1-or-later
#include "spotlight.h"

#include "coreui.h"
#include "foundation.h"
#include "catalog.h"
#include "launchservices.h"
#include "text.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <cmath>
#include <strings.h>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace AK;

namespace {

const char *const Bundle = "/System/Library/CoreServices/Search.bundle";

std::string tr(const char *key, const char *fallback) { return Assets::tr(Bundle, "MDMenuStrings", key, fallback); }

bool contains(const std::string &hay, const std::string &needle)
{
    if (needle.empty())
        return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (strncasecmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0)
            return true;
    return false;
}

class SpotlightView : public SystemUI::MenuExtraView {
public:
    explicit SpotlightView(SpotlightExtra *e) : MenuExtraView(e), spotlight(e) {}
    SpotlightExtra *spotlight;
    void draw(cairo_t *cr) override
    {
        const bool open = spotlight->isOpen();
        const Rect r{-4, 0, frame().w + 8, frame().h - 1};
        if (open) {
            // Spotlight's own blue square
            cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, r.h);
            cairo_pattern_add_color_stop_rgb(g, 0, 0x5d / 255.0, 0x8c / 255.0, 0xf3 / 255.0);
            cairo_pattern_add_color_stop_rgb(g, 1, 0x1a / 255.0, 0x52 / 255.0, 0xe0 / 255.0);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
        }
        static const Image black = m_extra->templateImage("MDSearchMenuIcon.pdf"), white = m_extra->templateImage("MDSearchMenuIcon.pdf", true);
        const Image &img = open ? white : black;
        if (!img.isNull())
            img.draw(cr, Point{std::floor((frame().w - img.width()) / 2), std::floor((frame().h - 1 - img.height()) / 2)});
    }
    void mouseDown(Event &) override { spotlight->openField(); }
};

} // namespace

// Spotlight's menu: the field, then the results
class SpotlightWindow {
public:
    static constexpr double FieldRow = 34, Row = 19;
    // MDMenuView -_calculateMaxGroupWidth (0x12c5f): the widest of the groups' titles and "Top Hit"
    // in the menu bar font of 11 (-_updateDrawingAttributes), the floor of it; the menu that and 230
    // wide (-sizeToFit: 180 + 8 + 8 + 16 + 5 + 13), the icons 8 + 13 after it, the names 16 + 5 further
    static double groupWidth()
    {
        static const double w = [] {
            double widest = 0;
            for (const char *key : {"APPLICATIONS", "DIRECTORIES", "MUSIC", "MOVIES", "PDF", "DOCUMENTS", "MESSAGES", "IMAGES",
                                    "CONTACT", "SYSTEM_PREFS", "EVENT_TODO", "FONTS", "PRESENTATIONS", "BOOKMARKS",
                                    "EXPRESSION", "DEFINITION", "TOP_HIT"})
                widest = std::max(widest, textWidth(Assets::tr(Bundle, "MDSimpleGrouping", key, key), Font::system(11)));
            return std::floor(widest);
        }();
        return w;
    }
    static double width() { return groupWidth() + 230; }
    static double column() { return groupWidth() + 14; }   // the left pattern's edge, between the titles and the icons
    static double iconX() { return groupWidth() + 8 + 13; }
    struct Result {
        std::string group, name;
        Image icon;
        LaunchServices::App app;
        std::string path;   // a file's
        std::string pane;   // a System Preferences pane's id
        long long used = 0;
        bool showAll = false, prefs = false;
    };

    SpotlightWindow(SpotlightExtra *extra, Window *bar, double right) : m_extra(extra)
    {
        m_window = std::make_unique<Window>(Size{width(), FieldRow}, "Spotlight", Window::Menu, bar, Point{right - width(), 21});
        m_view = m_window->contentView()->add<ResultsView>(this);
        m_view->setFrame(Rect{0, 0, width(), FieldRow});
        m_field = m_view->add<SearchField>();
        m_field->inToolbar = false;
        m_field->font = Font::system(13);
        m_field->setFrame(Rect{column() - 10, 6, width() - column() - 2, 22});
        m_field->onChange = [this](const std::string &t) { search(t); };
        m_field->onEnter = [this] { choose(m_selected); };
        m_field->onCancel = [this] { close(); };
        m_field->onKey = [this](Event &e) {
            if ((e.keysym == XKB_KEY_Down || e.keysym == XKB_KEY_Up) && !m_results.empty()) {
                m_selected = std::clamp(m_selected + (e.keysym == XKB_KEY_Down ? 1 : -1), 0, int(m_results.size()) - 1);
                m_view->setNeedsDisplay();
                return true;
            }
            if (e.keysym == XKB_KEY_Escape) {
                close();
                return true;
            }
            return false;
        };
        m_window->onOutsideClick = [this](Event &e) {
            if (e.type == Event::MouseDown)
                App::shared().later([this] { close(); });
        };
        m_window->show();
        m_window->grabInput();
        m_window->makeFirstResponder(m_field);
        layoutResults();
    }
    ~SpotlightWindow()
    {
        if (m_debounce)
            App::shared().removeTimer(m_debounce);
    }
    void close()
    {
        if (m_debounce)
            App::shared().removeTimer(m_debounce);
        m_debounce = 0;
        if (!m_window)
            return;
        m_window->close();
        App::shared().later([w = std::shared_ptr<Window>(std::move(m_window))] {});
        m_extra->setNeedsDisplay();
    }
    bool isOpen() const { return m_window != nullptr; }

private:
    class ResultsView : public View {
    public:
        explicit ResultsView(SpotlightWindow *s) : s(s) {}
        SpotlightWindow *s;
        void draw(cairo_t *cr) override
        {
            CoreUI::menuBackground(cr, bounds(), CoreUI::MenuShape::Pulldown);
            // the left column (MDMenu_LeftPattern) under the groups' names
            static const Image left = Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/MDMenu_LeftPattern.tiff"));
            const Rect col{0, FieldRow, column(), frame().h - FieldRow - 4};
            if (!s->m_results.empty()) {
                if (!left.isNull())
                    left.draw(cr, col);
                else
                    fill(cr, col, Color::gray(237));
            }
            TextStyle label;
            label.font = Font::boldSystem(13);
            label.align = Align::Right;
            drawText(cr, tr("MENU_SEARCH", "Spotlight"), Rect{0, 6, column() - 18, 22}, label);
            std::string lastGroup;
            for (size_t i = 0; i < s->m_results.size(); ++i) {
                const Result &r = s->m_results[i];
                const double y = FieldRow + i * Row;
                const bool sel = int(i) == s->m_selected;
                if (sel)
                    fill(cr, Rect{column(), y, frame().w - column(), Row}, Color::rgb(0x38, 0x75, 0xd7));
                if (r.group != lastGroup) {
                    TextStyle g;
                    g.font = Font::system(11);
                    g.color = Color::gray(110);
                    g.align = Align::Right;
                    drawText(cr, r.group, Rect{0, y, groupWidth() + 8, Row}, g);
                    lastGroup = r.group;
                }
                double x = iconX();
                if (!r.icon.isNull())
                    r.icon.draw(cr, Rect{x, y + 1.5, 16, 16});
                x += 16 + 5;
                TextStyle t;
                t.font = Font::system(12);   // menuBarFontOfSize:12
                t.color = sel ? Color::gray(255) : Color::gray(0);
                drawText(cr, r.name, Rect{x, y, frame().w - x - 8, Row}, t);
            }
        }
        void mouseDown(Event &e) override
        {
            const int i = int(std::floor((e.location.y - FieldRow) / Row));
            if (e.location.y >= FieldRow && e.location.x >= column() && i >= 0 && size_t(i) < s->m_results.size())
                App::shared().later([s = s, i] { s->choose(i); });
        }
    };

    // MDSearchController's groups (MDSimpleGrouping.strings' keys), in com.apple.spotlight's
    // orderedItems (the Spotlight pane's -_defaultSpotlightItems when unset), those not disabled
    static std::vector<std::string> groupOrder()
    {
        Settings prefs("spotlight");
        std::vector<std::string> order = prefs.list("orderedItems");
        if (order.empty())
            order = {"APPLICATIONS", "SYSTEM_PREFS", "DOCUMENTS", "DIRECTORIES", "MESSAGES", "CONTACT", "EVENT_TODO",
                     "IMAGES",       "PDF",          "BOOKMARKS", "MUSIC",       "MOVIES",   "FONTS",   "PRESENTATIONS"};
        const std::vector<std::string> disabled = prefs.list("disabledItems");
        std::vector<std::string> out;
        for (const std::string &g : order)
            if (std::find(disabled.begin(), disabled.end(), g) == disabled.end())
                out.push_back(g == "EMAIL" ? "MESSAGES" : g);
        return out;
    }
    static std::string groupTitle(const std::string &key)
    {
        return Assets::tr(Bundle, "MDSimpleGrouping", key, key);
    }

    // _MDQueryCreateQueryStringToMatchDisplayName: the name's words, one starting with the text
    // (kMDItemDisplayName == "text*"cdw)
    static bool wordPrefix(const std::string &name, const std::string &text)
    {
        if (text.empty())
            return false;
        for (size_t i = 0; i + text.size() <= name.size(); ++i) {
            const bool start = i == 0 || !std::isalnum((unsigned char)name[i - 1]);
            if (start && strncasecmp(name.c_str() + i, text.c_str(), text.size()) == 0)
                return true;
        }
        return false;
    }

    void search(const std::string &text)
    {
        std::string trimmed = text;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        m_query = trimmed;
        m_groups.clear();
        if (!trimmed.empty()) {
            // ApplicationsQuery (it can be the top hit)
            std::vector<Result> &apps = m_groups["APPLICATIONS"];
            for (const LaunchServices::App &a : LaunchServices::all()) {
                if (!a.shown)
                    continue;
                const LaunchServices::App shown = LaunchServices::forWindow(a.wmClass, a.id, 0);
                if (!contains(a.name, trimmed) && !contains(shown.name, trimmed))
                    continue;
                if (std::any_of(apps.begin(), apps.end(), [&](const Result &r) { return r.name == shown.name; }))
                    continue;   // one row an application, whichever of its entries found it
                Result r;
                r.group = "APPLICATIONS";
                r.name = shown.name;
                r.icon = LaunchServices::icon(shown, 16);
                r.app = a;
                apps.push_back(r);
                if (apps.size() > 20)   // a group stops past 20 (0x14)
                    break;
            }
            // System Preferences' panes (com.apple.systempreference.prefpane items)
            std::vector<Result> &panes = m_groups["SYSTEM_PREFS"];
            for (const std::string &id : searchPanes(trimmed)) {
                const PaneInfo *p = paneById(id);
                if (!p || panes.size() > 20)
                    continue;
                Result r;
                r.group = "SYSTEM_PREFS";
                r.name = p->name();
                r.icon = p->icon.isNull() ? Image() : p->icon.scaled(16, 16);
                r.pane = id;
                panes.push_back(r);
            }
        }
        compose();
        searchFiles(trimmed);
    }
    // The files: the desktop's index (Baloo), else plocate's in the home folder, a moment after the
    // typing stops
    void searchFiles(const std::string &text)
    {
        if (m_debounce)
            App::shared().removeTimer(m_debounce);
        m_debounce = 0;
        m_files.reset();
        if (text.empty())
            return;
        m_debounce = App::shared().addTimer(0.25, false, [this, text] {
            m_debounce = 0;
            m_files = std::make_unique<Task>();
            m_files->onExit = [this, text](int, const std::string &out) { filesFound(text, out); };
            if (SystemUI::MenuExtra::onPath("baloosearch6"))
                m_files->launch({"baloosearch6", "-l", "200", text});
            else
                m_files->launch({"plocate", "-i", "-l", "400", "--", text});
        });
    }
    // a file's group, by its kind (the content type tree's)
    static std::string fileGroup(const std::string &path)
    {
        if (isDir(path))
            return "DIRECTORIES";
        std::string ext = path.find('.') == std::string::npos ? std::string() : path.substr(path.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        static const std::map<std::string, const char *> kinds = {
            {"pdf", "PDF"},           {"png", "IMAGES"},        {"jpg", "IMAGES"},        {"jpeg", "IMAGES"},
            {"gif", "IMAGES"},        {"tif", "IMAGES"},        {"tiff", "IMAGES"},       {"bmp", "IMAGES"},
            {"webp", "IMAGES"},       {"heic", "IMAGES"},       {"svg", "IMAGES"},        {"mp3", "MUSIC"},
            {"m4a", "MUSIC"},         {"aac", "MUSIC"},         {"flac", "MUSIC"},        {"ogg", "MUSIC"},
            {"wav", "MUSIC"},         {"aif", "MUSIC"},         {"aiff", "MUSIC"},        {"opus", "MUSIC"},
            {"mp4", "MOVIES"},        {"m4v", "MOVIES"},        {"mov", "MOVIES"},        {"mkv", "MOVIES"},
            {"avi", "MOVIES"},        {"webm", "MOVIES"},       {"ttf", "FONTS"},         {"otf", "FONTS"},
            {"ttc", "FONTS"},         {"dfont", "FONTS"},       {"ppt", "PRESENTATIONS"}, {"pptx", "PRESENTATIONS"},
            {"key", "PRESENTATIONS"}, {"odp", "PRESENTATIONS"}, {"eml", "MESSAGES"},      {"emlx", "MESSAGES"},
            {"vcf", "CONTACT"},       {"ics", "EVENT_TODO"},    {"html", "BOOKMARKS"},    {"htm", "BOOKMARKS"},
            {"webloc", "BOOKMARKS"},  {"url", "BOOKMARKS"},     {"desktop", ""},          {"o", ""}};
        const auto it = kinds.find(ext);
        return it == kinds.end() ? "DOCUMENTS" : it->second;
    }
    void filesFound(const std::string &text, const std::string &out)
    {
        if (!m_window || !m_field || m_field->text.find(text) != 0)
            return;
        const std::string home = AK::home() + "/";
        size_t from = 0;
        static const Image folder = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns"), {16, 16}),
                           document = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericDocumentIcon.icns"), {16, 16});
        while (from < out.size()) {
            const size_t nl = out.find('\n', from);
            std::string path = out.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            from = nl == std::string::npos ? out.size() : nl + 1;
            if (path.rfind(home, 0) != 0 || path.find("/.") != std::string::npos || !contains(fileName(path), text))
                continue;
            const std::string g = fileGroup(path);
            if (g.empty())
                continue;
            std::vector<Result> &group = m_groups[g];
            if (group.size() > 20)
                continue;
            Result r;
            r.group = g;
            r.name = fileName(path);
            r.path = path;
            r.used = modified(path);   // kMDItemLastUsedDate's stand-in
            r.icon = (g == "DIRECTORIES" ? folder : document).scaled(16, 16);
            group.push_back(r);
        }
        compose();
    }

    // -simpleGroupedResultsWithUpperLimitWithOldGroupedResults:outUsedOldResults: (Search.bundle
    // 0x23ee5): the groups cut to 20 rows in all: the largest share c (from 20 down) with the sum of
    // min(count, c) at most 20, the rows left over one more to the first groups; the top hit (an
    // application matching the name's words, else an exact title, else the most recently used match)
    // in a group of its own, when there are two results or two groups
    void compose()
    {
        m_results.clear();
        if (!m_query.empty()) {
            const std::vector<std::string> order = groupOrder();
            std::vector<std::pair<std::string, std::vector<Result>>> groups;
            size_t total = 0;
            for (const std::string &key : order) {
                const auto it = m_groups.find(key);
                if (it != m_groups.end() && !it->second.empty()) {
                    groups.push_back(*it);
                    total += it->second.size();
                }
            }
            // the top hit
            const Result *top = nullptr;
            for (const auto &[key, rows] : groups)
                if (key == "APPLICATIONS")
                    for (const Result &r : rows)
                        if (!top && wordPrefix(r.name, m_query))
                            top = &r;
            const Result *exact = nullptr, *match = nullptr;
            for (const auto &[key, rows] : groups)
                for (const Result &r : rows) {
                    if (strcasecmp(r.name.c_str(), m_query.c_str()) == 0 || strcasecmp(baseName(r.name).c_str(), m_query.c_str()) == 0) {
                        if (!exact || r.used > exact->used)
                            exact = &r;
                    } else if (wordPrefix(r.name, m_query) && (!match || r.used > match->used)) {
                        match = &r;
                    }
                }
            if (!top)
                top = exact ? exact : match;
            Result topHit;
            const bool haveTop = top && (total >= 2 || groups.size() >= 2);
            if (haveTop) {
                topHit = *top;
                for (auto &[key, rows] : groups)
                    rows.erase(std::remove_if(rows.begin(), rows.end(),
                                              [&](const Result &r) { return r.name == topHit.name && r.path == topHit.path && r.pane == topHit.pane; }),
                               rows.end());
            }
            // the shares
            const long limit = haveTop ? 19 : 20;
            long share = 20, sum = 0;
            do {
                sum = 0;
                for (const auto &g : groups)
                    sum += std::min<long>(long(g.second.size()), share);
            } while (sum > limit && --share > 0);
            long left = limit - sum;
            Result all;
            all.showAll = true;
            all.name = total == 0 ? tr("SHOW_ALL_RESULT_NONE_FOUND", "No Results Found") : tr("SHOW_ALL_NO_COUNT", "Show All");
            m_results.push_back(all);
            if (haveTop) {
                topHit.group = groupTitle("TOP_HIT");
                m_results.push_back(topHit);
            }
            for (auto &[key, rows] : groups) {
                long cap = share;
                if (left > 0 && long(rows.size()) > share) {
                    ++cap;
                    --left;
                }
                if (long(rows.size()) > cap)
                    rows.resize(size_t(cap));
                for (Result r : rows) {
                    r.group = groupTitle(key);
                    m_results.push_back(r);
                }
            }
            Result prefs;
            prefs.prefs = true;
            prefs.name = tr("MENU_PREFERENCES", "Spotlight Preferences…");
            m_results.push_back(prefs);
        }
        m_selected = m_results.size() > 1 ? 1 : 0;
        layoutResults();
    }
    void layoutResults()
    {
        const double h = FieldRow + m_results.size() * Row + (m_results.empty() ? 0 : 4);
        m_window->setContentSize(Size{width(), h});
        m_view->setFrame(Rect{0, 0, width(), h});
        m_view->setNeedsDisplay();
    }
    void choose(int i)
    {
        if (i < 0 || size_t(i) >= m_results.size())
            return;
        const Result r = m_results[size_t(i)];
        close();
        if (!r.path.empty())
            LaunchServices::open(r.path);
        else if (!r.pane.empty())
            Task::launchDetached({LaunchServices::appsDir() + "/System Preferences.app/Contents/MacOS/System Preferences", "--pane", r.pane});
        else if (r.prefs)
            Task::launchDetached({LaunchServices::appsDir() + "/System Preferences.app/Contents/MacOS/System Preferences", "--pane",
                                  "com.apple.preference.spotlight"});
        else if (!r.showAll)
            LaunchServices::launch(r.app);
    }

    SpotlightExtra *m_extra;
    std::unique_ptr<Window> m_window;
    ResultsView *m_view = nullptr;
    SearchField *m_field = nullptr;
    std::vector<Result> m_results;
    std::map<std::string, std::vector<Result>> m_groups;   // by MDSimpleGrouping key
    std::string m_query;
    int m_selected = 0;
    int m_debounce = 0;
    std::unique_ptr<Task> m_files;
};

SpotlightExtra::SpotlightExtra(const std::string &bundle) : MenuExtra(bundle)
{
    setLength(22);
    setView(new SpotlightView(this));
}

SpotlightExtra::~SpotlightExtra() = default;

bool SpotlightExtra::isOpen() const { return m_window && m_window->isOpen(); }

void SpotlightExtra::openField()
{
    if (isOpen()) {
        m_window->close();
        return;
    }
    Window *bar = view()->window();
    if (!bar)
        return;
    const Point o = view()->toWindow(Point{0, 0});
    m_window = std::make_unique<SpotlightWindow>(this, bar, o.x + view()->frame().w + 4);
    setNeedsDisplay();
}
