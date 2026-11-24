// SPDX-License-Identifier: LGPL-2.1-or-later
#include "spotlight.h"

#include "coreui.h"
#include "foundation.h"
#include "launchservices.h"
#include "text.h"

#include <algorithm>
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
    static constexpr double Width = 330, FieldRow = 34, Row = 19, Column = 98;
    struct Result {
        std::string group, name;
        Image icon;
        LaunchServices::App app;
        std::string path;   // a file's
        bool showAll = false, prefs = false;
    };

    SpotlightWindow(SpotlightExtra *extra, Window *bar, double right) : m_extra(extra)
    {
        m_window = std::make_unique<Window>(Size{Width, FieldRow}, "Spotlight", Window::Menu, bar, Point{right - Width, 21});
        m_view = m_window->contentView()->add<ResultsView>(this);
        m_view->setFrame(Rect{0, 0, Width, FieldRow});
        m_field = m_view->add<SearchField>();
        m_field->inToolbar = false;
        m_field->font = Font::system(13);
        m_field->setFrame(Rect{Column - 10, 6, Width - Column - 2, 22});
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
            const Rect col{0, FieldRow, Column, frame().h - FieldRow - 4};
            if (!s->m_results.empty()) {
                if (!left.isNull())
                    left.draw(cr, col);
                else
                    fill(cr, col, Color::gray(237));
            }
            TextStyle label;
            label.font = Font::boldSystem(13);
            label.align = Align::Right;
            drawText(cr, tr("MENU_SEARCH", "Spotlight"), Rect{0, 6, Column - 18, 22}, label);
            std::string lastGroup;
            for (size_t i = 0; i < s->m_results.size(); ++i) {
                const Result &r = s->m_results[i];
                const double y = FieldRow + i * Row;
                const bool sel = int(i) == s->m_selected;
                if (sel)
                    fill(cr, Rect{Column, y, frame().w - Column, Row}, Color::rgb(0x38, 0x75, 0xd7));
                if (r.group != lastGroup) {
                    TextStyle g;
                    g.font = Font::system(11);
                    g.color = Color::gray(110);
                    g.align = Align::Right;
                    drawText(cr, r.group, Rect{0, y, Column - 8, Row}, g);
                    lastGroup = r.group;
                }
                double x = Column + 8;
                if (!r.icon.isNull()) {
                    r.icon.draw(cr, Rect{x, y + 1.5, 16, 16});
                    x += 20;
                }
                TextStyle t;
                t.font = Font::system(13);
                t.color = sel ? Color::gray(255) : Color::gray(0);
                drawText(cr, r.name, Rect{x, y, frame().w - x - 8, Row}, t);
            }
        }
        void mouseDown(Event &e) override
        {
            const int i = int(std::floor((e.location.y - FieldRow) / Row));
            if (e.location.y >= FieldRow && e.location.x >= Column && i >= 0 && size_t(i) < s->m_results.size())
                App::shared().later([s = s, i] { s->choose(i); });
        }
    };

    void search(const std::string &text)
    {
        m_results.clear();
        std::string trimmed = text;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        if (!trimmed.empty()) {
            int n = 0;
            for (const LaunchServices::App &a : LaunchServices::all()) {
                if (!a.shown)
                    continue;
                const LaunchServices::App shown = LaunchServices::forWindow(a.wmClass, a.id, 0);
                if (!contains(a.name, trimmed) && !contains(shown.name, trimmed))
                    continue;
                // one row an application, whichever of its entries found it
                if (std::any_of(m_results.begin(), m_results.end(), [&](const Result &r) { return r.name == shown.name; }))
                    continue;
                Result r;
                r.group = n == 0 ? "Top Hit" : "Applications";
                r.name = shown.name;
                r.icon = LaunchServices::icon(shown, 16);
                r.app = a;
                m_results.push_back(r);
                if (++n == 12)
                    break;
            }
            Result all;
            all.showAll = true;
            all.name = m_results.empty() ? tr("SHOW_ALL_RESULT_NONE_FOUND", "No Results Found") : tr("SHOW_ALL_NO_COUNT", "Show All");
            m_results.insert(m_results.begin(), all);
        }
        Result prefs;
        prefs.prefs = true;
        prefs.name = tr("MENU_PREFERENCES", "Spotlight Preferences…");
        if (!trimmed.empty())
            m_results.push_back(prefs);
        m_selected = m_results.size() > 1 ? 1 : 0;
        layoutResults();
        searchFiles(trimmed);
    }
    // The files: the desktop's index (Baloo), else plocate's in the home folder, a moment after the
    // typing stops; grouped as the Mac's menu groups them
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
                m_files->launch({"baloosearch6", "-l", "40", text});
            else
                m_files->launch({"plocate", "-i", "-l", "200", "--", text});
        });
    }
    void filesFound(const std::string &text, const std::string &out)
    {
        if (!m_window || !m_field || m_field->text.find(text) != 0)
            return;
        const std::string home = AK::home() + "/";
        std::map<std::string, std::vector<std::string>> groups;
        size_t from = 0, count = 0;
        while (from < out.size() && count < 9) {
            const size_t nl = out.find('\n', from);
            std::string path = out.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
            from = nl == std::string::npos ? out.size() : nl + 1;
            if (path.rfind(home, 0) != 0 || path.find("/.") != std::string::npos || !contains(fileName(path), text))
                continue;
            std::string ext = path.substr(path.find_last_of('.') + 1);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            const std::string group = isDir(path) ? "Folders" : ext == "pdf" ? "PDF Documents"
                                      : (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "tif" || ext == "tiff") ? "Images"
                                                                                                                                       : "Documents";
            groups[group].push_back(path);
            ++count;
        }
        if (groups.empty())
            return;
        static const Image folder = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns"), {16, 16}),
                           document = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericDocumentIcon.icns"), {16, 16});
        // before Spotlight Preferences…
        auto end = m_results.end();
        if (!m_results.empty() && m_results.back().prefs)
            --end;
        std::vector<Result> rows;
        for (const char *g : {"Documents", "Folders", "PDF Documents", "Images"})
            for (const std::string &path : groups[g]) {
                Result r;
                r.group = g;
                r.name = fileName(path);
                r.path = path;
                r.icon = (std::string(g) == "Folders" ? folder : document).scaled(16, 16);
                rows.push_back(r);
            }
        m_results.insert(end, rows.begin(), rows.end());
        if (!m_results.empty() && m_results.front().showAll)
            m_results.front().name = tr("SHOW_ALL_NO_COUNT", "Show All");
        if (m_selected == 0 && m_results.size() > 1)
            m_selected = 1;
        layoutResults();
    }
    void layoutResults()
    {
        const double h = FieldRow + m_results.size() * Row + (m_results.empty() ? 0 : 4);
        m_window->setContentSize(Size{Width, h});
        m_view->setFrame(Rect{0, 0, Width, h});
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
