// SPDX-License-Identifier: LGPL-2.1-or-later
// The Finder's windows, on AquaKit (no Qt): 10.6's browser window as BrowserWindow.nib and its parts'
// nibs lay it out (Toolbar, SidebarSplitter, Sidebar, StatusBar), in its four views. The desktop stays the
// Qt Finder's (src/finder/main.cpp); this program is its windows, one process for all of them: a
// second launch hands its folders to the first (an abstract socket) and quits.
//   FinderWindows [folder...]      (the home folder when none)
#include "files.h"
#include "columnview.h"
#include "dsstore.h"
#include "flowview.h"
#include "fileops.h"
#include "iconview.h"
#include "progress.h"
#include "infowindow.h"
#include "listview.h"
#include "viewoptions.h"
#include "sidebar.h"

#include "controls.h"
#include "foundation.h"
#include "toolbar.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

extern char **environ;

using namespace AK;
using namespace Finder;

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string core(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "LocalizableCore", key, fallback); }
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }

// "^0 items, ^1 available": the Finder's ^n placeholders
std::string format(std::string f, const std::vector<std::string> &values)
{
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string mark = "^" + std::to_string(i);
        for (size_t at; (at = f.find(mark)) != std::string::npos;)
            f.replace(at, mark.size(), values[i]);
    }
    return f;
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
    const bool ok = posix_spawnp(&pid, args[0], nullptr, &attr, args.data(), environ) == 0;
    posix_spawnattr_destroy(&attr);
    return ok;
}

// A document: PDFs and pictures to Preview (ours, beside this program's bundle), applications started,
// the rest to the desktop's own choice
void openDocument(const Item &item)
{
    if (item.application) {
        spawn({"gio", "launch", item.path});
        return;
    }
    std::string ext = item.name.substr(item.name.rfind('.') == std::string::npos ? item.name.size() : item.name.rfind('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char *const preview[] = {"pdf", "png", "jpg", "jpeg", "tif", "tiff", "gif", "bmp", "icns"};
    // .../apps/Finder.app/Contents/MacOS/FinderWindows -> .../apps/Preview.app/Contents/MacOS/Preview
    const std::string apps = dirName(dirName(dirName(dirName(executablePath()))));
    const std::string previewBin = apps + "/Preview.app/Contents/MacOS/Preview";
    if (std::find(std::begin(preview), std::end(preview), ext) != std::end(preview) && isFile(previewBin)) {
        spawn({previewBin, item.path});
        return;
    }
    spawn({"xdg-open", item.path});
}

// ---- the status bar (StatusBar.nib's TStatusBar: 24 high, its text centred in Lucida Grande 11)

class StatusBar : public View {
public:
    static constexpr double Height = 24;
    std::string text;
    void draw(cairo_t *cr) override
    {
        // the window's bottom border: the toolbar's gradient continued, a line above it
        const bool active = windowIsKey();
        const Color top = active ? Color::gray(196) : Color::gray(228), bottom = active ? Color::gray(158) : Color::gray(207);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, Height);
        cairo_pattern_add_color_stop_rgb(g, 0, top.r, top.g, top.b);
        cairo_pattern_add_color_stop_rgb(g, 1, bottom.r, bottom.g, bottom.b);
        cairo_rectangle(cr, 0, 0, frame().w, Height);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        fill(cr, Rect{0, 0, frame().w, 1}, active ? Color::gray(81) : Color::gray(135));
        TextStyle t;
        t.font = Font::system(11);
        t.align = Align::Center;
        t.engraved = true;
        t.engravedAlpha = 0.45;
        t.color = active ? Color::gray(0) : Color::gray(0, 0.5);
        drawText(cr, text, Rect{8, 5, frame().w - 16, 15}, t);
    }
};

// The icon view's size slider in the status bar, from the Finder's own pictures (trackCapLeft, trackFill,
// trackCapRight, knobNormal / knobHover / knobDisabled): its track 77 long, 25 from the window's right edge
// (measured on a 10.6 screenshot); 16 to 512 points, by their square root (64 a fifth along, as on 10.6)
class IconSizeSlider : public View {
public:
    static constexpr double Width = 77, Right = 25, MinSize = 16, MaxSize = 512, Knob = 12;
    int value = 64;
    std::function<void(int)> onChange;

    IconSizeSlider()
    {
        auto res = [](const char *name) { return Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/" + name)); };
        m_left = res("trackCapLeft.png");
        m_fill = res("trackFill.png");
        m_right = res("trackCapRight.png");
        m_knob = res("knobNormal.png");
        m_knobHover = res("knobHover.png");
        m_knobDisabled = res("knobDisabled.png");
    }

    void draw(cairo_t *cr) override
    {
        const double ty = std::floor((frame().h - m_fill.height()) / 2);
        m_left.draw(cr, Point{0, ty});
        m_fill.draw(cr, Rect{double(m_left.width()), ty, Width - m_left.width() - m_right.width(), double(m_fill.height())});
        m_right.draw(cr, Point{Width - m_right.width(), ty});
        const Image &knob = !windowIsKey() ? m_knobDisabled : (m_hover || m_dragging) ? m_knobHover : m_knob;
        knob.draw(cr, Point{std::round(knobX()), std::floor((frame().h - Knob) / 2)});
    }
    void mouseDown(Event &e) override
    {
        m_dragging = true;
        track(e.location.x);
    }
    void mouseDragged(Event &e) override { track(e.location.x); }
    void mouseUp(Event &) override
    {
        m_dragging = false;
        setNeedsDisplay();
    }
    void mouseMoved(Event &e) override
    {
        const bool over = std::abs(e.location.x - knobX() - Knob / 2) <= Knob / 2;
        if (over != m_hover) {
            m_hover = over;
            setNeedsDisplay();
        }
    }
    void mouseExited(Event &) override
    {
        m_hover = false;
        setNeedsDisplay();
    }

private:
    double knobX() const
    {
        const double t = (std::sqrt(double(value)) - std::sqrt(MinSize)) / (std::sqrt(MaxSize) - std::sqrt(MinSize));
        return t * (Width - Knob);
    }
    void track(double x)
    {
        const double t = std::clamp((x - Knob / 2) / (Width - Knob), 0.0, 1.0);
        const double root = std::sqrt(MinSize) + t * (std::sqrt(MaxSize) - std::sqrt(MinSize));
        const int size = std::clamp(int(std::lround(root * root / 4)) * 4, int(MinSize), int(MaxSize));   // by fours
        if (size == value)
            return;
        value = size;
        setNeedsDisplay();
        if (onChange)
            onChange(size);
    }

    Image m_left, m_fill, m_right, m_knob, m_knobHover, m_knobDisabled;
    bool m_hover = false, m_dragging = false;
};

// The sidebar's divider (TSidebarSplitter's thin style): 1 point, dragged a few points around it
class Divider : public View {
public:
    std::function<void(double dx, bool start)> onDrag;
    void draw(cairo_t *cr) override { fill(cr, Rect{std::floor(bounds().w / 2), 0, 1, bounds().h}, Color::gray(141)); }
    void mouseDown(Event &e) override
    {
        m_from = e.screen.x;
        if (onDrag)
            onDrag(0, true);
    }
    void mouseDragged(Event &e) override
    {
        if (onDrag)
            onDrag(e.screen.x - m_from, false);
    }

private:
    double m_from = 0;
};

// A textured toolbar button showing a template image (Quick Look's eye); the Action menu's
// (TActionToolbarItem) adds the pull-down's arrow after its gear
class TemplateButton : public Button {
public:
    explicit TemplateButton(const Image &glyph, bool arrow = false) : Button({}, Textured), m_glyph(glyph), m_arrow(arrow) {}
    void draw(cairo_t *cr) override
    {
        Button::draw(cr);
        const bool active = windowIsKey();
        const double shift = m_arrow ? 4 : 0;
        drawTemplate(cr, m_glyph, Rect{std::floor((frame().w - m_glyph.width()) / 2) - shift, std::floor((frame().h - 1 - m_glyph.height()) / 2),
                                       double(m_glyph.width()), double(m_glyph.height())}, isEnabled(), active, m_pressed);
        if (!m_arrow)
            return;
        // the arrow: a small triangle pointing down
        const double x = frame().w - 11, y = std::floor(frame().h / 2) - 1;
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + 5, y);
        cairo_line_to(cr, x + 2.5, y + 3);
        cairo_close_path(cr);
        const double gray = m_pressed ? 1 : 0.19;
        cairo_set_source_rgba(cr, gray, gray, gray, isEnabled() ? (active ? 1 : 0.6) : 0.35);
        cairo_fill(cr);
    }

private:
    Image m_glyph;
    bool m_arrow;
};

// ---- a Finder window

class BrowserWindow {
public:
    // BrowserWindow.nib: its content 770 x 383; the toolbar above it (icon only); TSidebarViewController's
    // +defaultSidebarWidth 192
    static constexpr double ContentWidth = 770, ContentHeight = 383, DefaultSidebarWidth = 192, MinimumSidebarWidth = 120;

    explicit BrowserWindow(const std::string &folder)
    {
        m_toolbar = new Toolbar(false);
        const Size size{ContentWidth, ContentHeight + m_toolbar->height()};
        m_window = std::make_unique<Window>(size, displayName(folder), Window::Titled);
        View *root = m_window->contentView();
        root->addSubview(m_toolbar);
        buildToolbar();
        m_sidebar = root->add<Sidebar>();
        m_sidebar->reload();
        m_sidebar->onSelect = [this](const Sidebar::Row &r) {
            if (r.path.rfind("search:", 0) == 0)
                return;   // the searches: later (Spotlight's queries)
            go(r.path);
        };
        m_scroll = root->add<ScrollView>();
        m_icons = m_scroll->document()->add<IconView>();
        m_icons->onOpen = [this](const Item &item) {
            if (item.folder)
                go(item.path);
            else
                openDocument(item);
        };
        m_icons->onSelectionChanged = [this] { updateStatus(); };
        m_icons->onBack = [this] { goUp(); };
        m_list = root->add<ListView>();
        m_list->onOpen = m_icons->onOpen;
        m_list->onSelectionChanged = [this] { updateStatus(); };
        m_list->onBack = [this] { goUp(); };
        m_list->setHidden(true);
        m_columnsView = root->add<ColumnView>();
        m_columnsView->onOpen = m_icons->onOpen;
        m_columnsView->onChanged = [this] {
            if (m_view == 2)
                setTitle(m_columnsView->currentFolder());
            updateStatus();
        };
        m_columnsView->setHidden(true);
        m_flowView = root->add<FlowView>();
        m_flowView->onOpen = m_icons->onOpen;
        m_flowView->onSelectionChanged = [this] { updateStatus(); };
        m_flowView->list()->onBack = [this] { goUp(); };
        m_flowView->setHidden(true);
        m_status = root->add<StatusBar>();
        m_iconSize = root->add<IconSizeSlider>();
        m_iconSize->onChange = [this](int size) {
            ViewSettings s = m_settings;
            s.iconSize = size;
            settingsChanged(s);
        };
        m_icons->onMoved = [this](const std::vector<std::pair<std::string, Point>> &moved) {
            // the icons' new places, and the folder kept unarranged (icvp's arrangeBy "none")
            if (!m_store)
                return;
            for (const auto &m : moved)
                m_store->set(DSStore::iconLocation(m.first, int(m.second.x), int(m.second.y)));
            PList icvp = storedPList("icvp");
            if (!icvp.get("arrangeBy"))
                icvp.set("arrangeBy", PList::string("none"));
            storePList("icvp", icvp);
            layoutIcons();
        };
        auto sorted = [this](ListView *list) {
            // lsvp: the sort column, and that column's order in its own dictionary
            PList lsvp = storedPList("lsvp");
            lsvp.set("sortColumn", PList::string(list->sortColumn()));
            PList columns = lsvp.get("columns") ? *lsvp.get("columns") : PList::dictionary();
            PList column = columns.get(list->sortColumn()) ? *columns.get(list->sortColumn()) : PList::dictionary();
            column.set("ascending", PList::boolean(list->ascending()));
            columns.set(list->sortColumn(), column);
            lsvp.set("columns", columns);
            storePList("lsvp", lsvp);
        };
        m_list->onSortChanged = [this, sorted] { sorted(m_list); };
        m_flowView->list()->onSortChanged = [this, sorted] { sorted(m_flowView->list()); };
        // dragged items: onto a folder here, or anywhere else (the sidebar, another window)
        m_icons->onDropOnFolder = [this](const Item &folder, bool option) { transfer(m_icons->selectedItems(), folder.path, option); };
        m_icons->onDropOutside = [this](Point screen, bool option) {
            const std::string target = dropTargetAt(screen);
            if (!target.empty())
                transfer(m_icons->selectedItems(), target, option);
        };
        s_windows.push_back(this);
        // the divider over the views' edges: last, so it gets the clicks
        m_divider = root->add<Divider>();
        m_divider->onDrag = [this](double dx, bool start) {
            if (start)
                m_dragFrom = m_sidebarWidth;
            m_sidebarWidth = std::clamp(m_dragFrom + dx, 0.0, std::max(MinimumSidebarWidth, m_window->size().w - 200));
            // dragged under half the minimum: closed
            m_sidebarWidth = m_sidebarWidth < MinimumSidebarWidth / 2 ? 0 : std::max(m_sidebarWidth, MinimumSidebarWidth);
            layout();
        };
        m_window->setUnifiedToolbarHeight(int(m_toolbar->gradient()));
        m_window->setRoundedBottomCorners(true);   // a textured window (TBrowserWindow)
        m_window->setShowsToolbarButton(true);
        m_window->onToggleToolbar = [this] { toggleToolbar(); };
        m_window->setIcon(Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/Finder.icns"), {128, 128}));
        m_window->onResize = [this](Size) { layout(); };
        // Command-J: View > Show View Options; the panel follows the front window
        m_window->onUnhandledKey = [this](Event &e) {
            if ((e.modifiers & Command) && e.keysym == XKB_KEY_j)
                toggleViewOptions();
            else if ((e.modifiers & Command) && e.keysym == XKB_KEY_i)
                getInfo();
            else if ((e.modifiers & Command) && e.keysym == XKB_KEY_c)
                copySelection();
            else if ((e.modifiers & Command) && e.keysym == XKB_KEY_v)
                paste();
            else if ((e.modifiers & Command) && e.keysym == XKB_KEY_d)
                duplicate();
            else if ((e.modifiers & Command) && (e.modifiers & Shift) && e.keysym == XKB_KEY_BackSpace)
                emptyTrash();
            else if ((e.modifiers & Command) && e.keysym == XKB_KEY_BackSpace)
                moveToTrash();
        };
        m_window->onKeyChanged = [this](bool key) {
            if (key)
                targetViewOptions();
        };
        m_window->onClose = [this] {
            s_windows.erase(std::remove(s_windows.begin(), s_windows.end(), this), s_windows.end());
            if (s_front == this)
                s_front = nullptr;
            saveWindowSettings();
            if (onClose)
                onClose(this);
        };
        m_window->setResizable({460, 200 + m_toolbar->height()});
        go(folder, false);
        applyWindowSettings();
        layout();
        m_window->makeFirstResponder(m_icons);
        m_window->show();
    }

    // Command-1, Command-2: the views (the View menu's As Icons, As List)
    // the title bar's toolbar button (View > Hide Toolbar): the toolbar and the sidebar go together, as
    // in 10.6's browser windows
    void toggleToolbar()
    {
        m_toolbarShown = !m_toolbarShown;
        m_toolbar->setHidden(!m_toolbarShown);
        m_window->setUnifiedToolbarHeight(m_toolbarShown ? int(m_toolbar->gradient()) : 0);
        layout();
    }

    // a view chosen by the user is the folder's from then on (.DS_Store's vstl)
    // In a window with a toolbar the view is the window's: the folder keeps one only when "Always open
    // in icon view" is checked in its View Options (its vstl)
    void setView(int view)
    {
        static const char *const styles[] = {"icnv", "Nlsv", "clmv", "Flwv"};
        const DSStore::Record *vstl = m_store ? m_store->get(".", "vstl") : nullptr;
        m_settings.view = view;
        m_settings.alwaysOpen = vstl && view >= 0 && view < 4 && vstl->data == styles[view];
        if (s_front == this)
            ViewOptions::shared().update(m_settings);
        m_view = view;
        m_views->setSelected(view);
        m_scroll->setHidden(view != 0);
        m_list->setHidden(view != 1);
        m_columnsView->setHidden(view != 2);
        m_flowView->setHidden(view != 3);
        layout();
        setTitle(view == 2 ? m_columnsView->currentFolder() : m_folder);
        if (view == 0)
            m_window->makeFirstResponder(m_icons);
        else if (view == 1)
            m_list->focus();
        else if (view == 2)
            m_window->makeFirstResponder(m_columnsView);
        else
            m_flowView->focus();
        updateStatus();
    }

    std::function<void(BrowserWindow *)> onClose;
    Window *window() const { return m_window.get(); }
    // its folder's items again (a rename elsewhere)
    void refresh(const std::string &folder)
    {
        if (folder == m_folder)
            show();
    }

    void go(const std::string &folder, bool remember = true)
    {
        if (!isDir(folder))
            return;
        if (remember && !m_folder.empty() && folder != m_folder) {
            m_back.push_back(m_folder);
            m_forward.clear();
        }
        m_folder = folder;
        show();
    }

private:
    // the folder's name, its icon before it (the proxy icon)
    void setTitle(const std::string &folder)
    {
        m_window->setTitle(displayName(folder));
        m_window->setRepresentedIcon(icon(Item{fileName(folder), folder, true}, 16));
    }

    void goUp()
    {
        if (m_folder != "/")
            go(dirName(m_folder).empty() ? "/" : dirName(m_folder));
    }

    // ---- the folder's .DS_Store

    PList storedPList(const std::string &code) const
    {
        PList p;
        const DSStore::Record *r = m_store ? m_store->get(".", code) : nullptr;
        if (!r || r->type != "blob" || !PList::parse(r->data, p) || p.type != PList::Dict)
            p = PList::dictionary();
        return p;
    }

    void storePList(const std::string &code, const PList &p)
    {
        if (!m_store)
            return;
        m_store->set(DSStore::blob(".", code, p.serialize()));
        m_store->save();
    }

    // ---- the view settings: the user's defaults (finder.conf, as com.apple.finder's
    // StandardViewSettings), then the folder's icvp and lsvp; the column view's are the user's alone

    static ViewSettings defaults()
    {
        Settings conf("finder");
        ViewSettings s;
        s.iconSize = conf.integer("IconViewIconSize", s.iconSize);
        s.gridSpacing = conf.integer("IconViewGridSpacing", s.gridSpacing);
        s.iconTextSize = conf.integer("IconViewTextSize", s.iconTextSize);
        s.labelOnBottom = conf.boolean("IconViewLabelOnBottom", s.labelOnBottom);
        s.showItemInfo = conf.boolean("IconViewShowItemInfo", s.showItemInfo);
        s.iconPreview = conf.boolean("IconViewShowIconPreview", s.iconPreview);
        s.arrangeBy = conf.string("IconViewArrangeBy", s.arrangeBy);
        s.listTextSize = conf.integer("ListViewTextSize", s.listTextSize);
        s.listIconSize = conf.integer("ListViewIconSize", s.listIconSize);
        s.relativeDates = conf.boolean("ListViewUseRelativeDates", s.relativeDates);
        s.calculateAllSizes = conf.boolean("ListViewCalculateAllSizes", s.calculateAllSizes);
        s.listIconPreview = conf.boolean("ListViewShowIconPreview", s.listIconPreview);
        if (!conf.list("ListViewColumns").empty())
            s.listColumns = conf.list("ListViewColumns");
        s.columnTextSize = conf.integer("ColumnViewTextSize", s.columnTextSize);
        s.columnShowIcons = conf.boolean("ColumnViewShowIcons", s.columnShowIcons);
        s.columnIconPreview = conf.boolean("ColumnViewShowIconPreview", s.columnIconPreview);
        s.columnPreview = conf.boolean("ColumnViewShowPreviewColumn", s.columnPreview);
        return s;
    }

    // Use as Defaults: the folder's settings for every folder without its own
    void useAsDefaults()
    {
        Settings conf("finder");
        const ViewSettings &s = m_settings;
        conf.set("IconViewIconSize", s.iconSize);
        conf.set("IconViewGridSpacing", s.gridSpacing);
        conf.set("IconViewTextSize", s.iconTextSize);
        conf.set("IconViewLabelOnBottom", s.labelOnBottom);
        conf.set("IconViewShowItemInfo", s.showItemInfo);
        conf.set("IconViewShowIconPreview", s.iconPreview);
        conf.set("IconViewArrangeBy", s.arrangeBy);
        conf.set("ListViewTextSize", s.listTextSize);
        conf.set("ListViewIconSize", s.listIconSize);
        conf.set("ListViewUseRelativeDates", s.relativeDates);
        conf.set("ListViewCalculateAllSizes", s.calculateAllSizes);
        conf.set("ListViewShowIconPreview", s.listIconPreview);
        conf.set("ListViewColumns", s.listColumns);
    }

    // what the folder's .DS_Store says of its view: the style (vstl, else the older fwi0's), the
    // icons' places (Iloc), the icon view's options (icvp, else the older icvo's size), the list's (lsvp)
    void applyFolderSettings(const std::vector<Item> &items)
    {
        m_store = std::make_unique<DSStore::Store>(m_folder);
        ViewSettings s = defaults();
        s.folderName = displayName(m_folder);
        s.view = m_view;
        std::string style;
        if (const DSStore::Record *v = m_store->get(".", "vstl"))
            style = v->data;
        else if (const DSStore::Record *f = m_store->get(".", "fwi0"); f && f->data.size() >= 12)
            style = f->data.substr(8, 4);
        static const char *const styles[] = {"icnv", "Nlsv", "clmv", "Flwv"};
        for (int k = 0; k < 4; ++k)
            if (style == styles[k])
                s.view = k;
        s.alwaysOpen = m_store->get(".", "vstl") != nullptr;

        std::map<std::string, Point> places;
        for (const Item &item : items)
            if (const DSStore::Record *r = m_store->get(item.name, "Iloc")) {
                int x, y;
                if (DSStore::iconLocation(*r, x, y))
                    places[item.name] = {double(x), double(y)};
            }
        m_icons->setPositions(std::move(places));

        const PList icvp = storedPList("icvp");
        if (const PList *v = icvp.get("iconSize"))
            s.iconSize = std::clamp(int(v->number(64)), 16, 512);
        else if (const DSStore::Record *icvo = m_store->get(".", "icvo"); icvo && icvo->data.size() >= 6 && icvo->data.compare(0, 4, "icv4") == 0)
            s.iconSize = std::clamp((static_cast<unsigned char>(icvo->data[4]) << 8) | static_cast<unsigned char>(icvo->data[5]), 16, 512);
        if (const PList *v = icvp.get("gridSpacing"))
            s.gridSpacing = std::clamp(int(v->number(54)), 1, 100);
        if (const PList *v = icvp.get("textSize"))
            s.iconTextSize = std::clamp(int(v->number(12)), 10, 16);
        if (const PList *v = icvp.get("labelOnBottom"))
            s.labelOnBottom = v->number(1) != 0;
        if (const PList *v = icvp.get("showItemInfo"))
            s.showItemInfo = v->number(0) != 0;
        if (const PList *v = icvp.get("showIconPreview"))
            s.iconPreview = v->number(1) != 0;
        if (const PList *v = icvp.get("arrangeBy"); v && v->type == PList::String)
            s.arrangeBy = v->s;

        const PList lsvp = storedPList("lsvp");
        if (const PList *v = lsvp.get("textSize"))
            s.listTextSize = std::clamp(int(v->number(12)), 10, 16);
        if (const PList *v = lsvp.get("iconSize"))
            s.listIconSize = v->number(16) >= 32 ? 32 : 16;
        if (const PList *v = lsvp.get("useRelativeDates"))
            s.relativeDates = v->number(1) != 0;
        if (const PList *v = lsvp.get("calculateAllSizes"))
            s.calculateAllSizes = v->number(0) != 0;
        if (const PList *v = lsvp.get("showIconPreview"))
            s.listIconPreview = v->number(1) != 0;
        if (const PList *columns = lsvp.get("columns"); columns && columns->type == PList::Dict) {
            s.listColumns.clear();
            for (const auto &[id, c] : columns->dict)
                if (const PList *visible = c.get("visible"); visible && visible->b && id != "name")
                    s.listColumns.push_back(id);
        }
        if (const PList *column = lsvp.get("sortColumn")) {
            bool ascending = column->s.rfind("date", 0) != 0;
            if (const PList *columns = lsvp.get("columns"))
                if (const PList *c = columns->get(column->s))
                    if (const PList *a = c->get("ascending"))
                        ascending = a->b;
            m_list->setSort(column->s, ascending);
            m_flowView->list()->setSort(column->s, ascending);
        }
        const int view = s.view;
        m_settings = s;
        applySettings();
        if (view != m_view)
            setView(view);
        if (s_front == this)
            ViewOptions::shared().update(m_settings);
    }

    // the settings into the views
    void applySettings()
    {
        const ViewSettings &s = m_settings;
        m_icons->iconSize = s.iconSize;
        m_icons->gridSpacing = s.gridSpacing;
        m_icons->font = Font::system(s.iconTextSize);
        m_icons->labelOnBottom = s.labelOnBottom;
        m_icons->showItemInfo = s.showItemInfo;
        m_icons->showIconPreview = s.iconPreview;
        m_icons->arrangeBy = s.arrangeBy;
        m_icons->relayout();
        m_iconSize->value = s.iconSize;
        m_iconSize->setNeedsDisplay();
        for (ListView *l : {m_list, m_flowView->list()}) {
            l->setColumnsShown(s.listColumns);
            l->setOptions(s.listTextSize, s.listIconSize, s.relativeDates, s.listIconPreview);
        }
        m_columnsView->textSize = s.columnTextSize;
        m_columnsView->showIcons = s.columnShowIcons;
        m_columnsView->showIconPreview = s.columnIconPreview;
        m_columnsView->showPreviewColumn = s.columnPreview;
        m_columnsView->setNeedsDisplay();
        layoutIcons();
    }

    // a change from the View Options (or the status bar's slider): into the views, the folder's
    // .DS_Store (icvp, lsvp, vstl) and the user's preferences (the column view's)
    void settingsChanged(const ViewSettings &next)
    {
        const ViewSettings before = m_settings;
        m_settings = next;
        applySettings();
        const ViewSettings &s = m_settings;
        if (s.iconSize != before.iconSize || s.gridSpacing != before.gridSpacing || s.iconTextSize != before.iconTextSize ||
            s.labelOnBottom != before.labelOnBottom || s.showItemInfo != before.showItemInfo || s.iconPreview != before.iconPreview ||
            s.arrangeBy != before.arrangeBy) {
            PList icvp = storedPList("icvp");
            icvp.set("iconSize", PList::real(s.iconSize));
            icvp.set("gridSpacing", PList::real(s.gridSpacing));
            icvp.set("textSize", PList::real(s.iconTextSize));
            icvp.set("labelOnBottom", PList::boolean(s.labelOnBottom));
            icvp.set("showItemInfo", PList::boolean(s.showItemInfo));
            icvp.set("showIconPreview", PList::boolean(s.iconPreview));
            icvp.set("arrangeBy", PList::string(s.arrangeBy));
            if (!icvp.get("viewOptionsVersion"))
                icvp.set("viewOptionsVersion", PList::integer(0));
            storePList("icvp", icvp);
        }
        if (s.listTextSize != before.listTextSize || s.listIconSize != before.listIconSize || s.relativeDates != before.relativeDates ||
            s.calculateAllSizes != before.calculateAllSizes || s.listIconPreview != before.listIconPreview || s.listColumns != before.listColumns) {
            PList lsvp = storedPList("lsvp");
            lsvp.set("textSize", PList::real(s.listTextSize));
            lsvp.set("iconSize", PList::real(s.listIconSize));
            lsvp.set("useRelativeDates", PList::boolean(s.relativeDates));
            lsvp.set("calculateAllSizes", PList::boolean(s.calculateAllSizes));
            lsvp.set("showIconPreview", PList::boolean(s.listIconPreview));
            PList columns = lsvp.get("columns") ? *lsvp.get("columns") : PList::dictionary();
            for (const char *id : {"name", "dateModified", "dateCreated", "size", "kind", "version", "comments", "label", "dateLastOpened"}) {
                PList c = columns.get(id) ? *columns.get(id) : PList::dictionary();
                const bool shown = std::string(id) == "name" || std::find(s.listColumns.begin(), s.listColumns.end(), id) != s.listColumns.end();
                c.set("visible", PList::boolean(shown));
                columns.set(id, c);
            }
            lsvp.set("columns", columns);
            if (!lsvp.get("viewOptionsVersion"))
                lsvp.set("viewOptionsVersion", PList::integer(0));
            storePList("lsvp", lsvp);
        }
        if (s.columnTextSize != before.columnTextSize || s.columnShowIcons != before.columnShowIcons ||
            s.columnIconPreview != before.columnIconPreview || s.columnPreview != before.columnPreview) {
            Settings conf("finder");
            conf.set("ColumnViewTextSize", s.columnTextSize);
            conf.set("ColumnViewShowIcons", s.columnShowIcons);
            conf.set("ColumnViewShowIconPreview", s.columnIconPreview);
            conf.set("ColumnViewShowPreviewColumn", s.columnPreview);
        }
        if (s.alwaysOpen != before.alwaysOpen && m_store) {
            static const char *const styles[] = {"icnv", "Nlsv", "clmv", "Flwv"};
            if (s.alwaysOpen)
                m_store->set(DSStore::fourCC(".", "vstl", styles[std::clamp(m_view, 0, 3)]));
            else
                m_store->remove(".", "vstl");
            m_store->save();
        }
        if (s_front == this)
            ViewOptions::shared().update(m_settings);
    }

    // the View Options panel shows the front window's settings and changes them
    void targetViewOptions()
    {
        s_front = this;
        ViewOptions &panel = ViewOptions::shared();
        panel.onChange = [](const ViewSettings &s) {
            if (s_front)
                s_front->settingsChanged(s);
        };
        panel.onUseAsDefaults = [] {
            if (s_front)
                s_front->useAsDefaults();
        };
        panel.update(m_settings);
    }

    // File > Get Info: a window for each selected item, the folder's own when nothing is selected
    std::vector<Item> selectedItems() const
    {
        if (m_view == 1)
            return m_list->selectedItems();
        if (m_view == 3)
            return m_flowView->list()->selectedItems();
        if (m_view == 2)
            return m_columnsView->selectedItems();
        std::vector<Item> out;
        for (int i : m_icons->selection())
            out.push_back(m_icons->items()[size_t(i)]);
        return out;
    }

    void getInfo()
    {
        std::vector<Item> items = selectedItems();
        if (items.empty())
            items.push_back(Item{displayName(m_folder), m_folder, true});
        for (const Item &item : items)
            InfoWindow::show(item);
    }

    void toggleViewOptions()
    {
        targetViewOptions();
        ViewOptions::shared().toggle(m_settings);
    }

    // bwsp, when the window opens on the folder: its size, the sidebar's width, the toolbar
    void applyWindowSettings()
    {
        const PList bwsp = storedPList("bwsp");
        if (const PList *bounds = bwsp.get("WindowBounds")) {
            double x, y, w, h;
            if (std::sscanf(bounds->s.c_str(), "{{%lf, %lf}, {%lf, %lf}}", &x, &y, &w, &h) == 4 && w >= 460 && h > 222)
                m_window->setContentSize({w, h - 22});   // the frame's height less the title bar
        }
        if (const PList *sidebar = bwsp.get("SidebarWidth"))
            m_sidebarWidth = std::max(0.0, sidebar->number(DefaultSidebarWidth));
        if (const PList *shown = bwsp.get("ShowToolbar"); shown && !shown->b && m_toolbarShown)
            toggleToolbar();
        layout();
    }

    void saveWindowSettings()
    {
        // the frame as Cocoa has it: its origin from the screen's bottom left, the title bar included
        PList bwsp = storedPList("bwsp");
        const Size size = m_window->size();
        const Rect screen = App::shared().screenFrame();
        const Point topLeft = m_window->toScreen({0, 0});
        const double frameH = size.h + 22;
        char bounds[96];
        std::snprintf(bounds, sizeof bounds, "{{%d, %d}, {%d, %d}}", int(topLeft.x), int(screen.h - (topLeft.y - 22) - frameH), int(size.w),
                      int(frameH));
        bwsp.set("WindowBounds", PList::string(bounds));
        bwsp.set("SidebarWidth", PList::integer(int64_t(m_sidebarWidth)));
        bwsp.set("ShowSidebar", PList::boolean(m_sidebarWidth > 0 && m_toolbarShown));
        bwsp.set("ShowToolbar", PList::boolean(m_toolbarShown));
        bwsp.set("ShowStatusBar", PList::boolean(true));
        bwsp.set("ShowPathbar", PList::boolean(false));
        storePList("bwsp", bwsp);
    }

    void show()
    {
        setTitle(m_folder);
        std::vector<Item> items = list(m_folder);
        m_list->setItems(items);
        m_columnsView->setRoot(m_folder);
        m_flowView->setItems(items);
        applyFolderSettings(items);
        m_icons->setItems(std::move(items));
        m_sidebar->selectPath(m_folder);
        m_scroll->scrollTo(0);
        layoutIcons();
        updateToolbar();
        updateStatus();
    }

    void buildToolbar()
    {
        // Toolbar.nib's views; 10.6's default set (the Finder's table of four-char kinds at 0x1002784c0:
        // BACK flxs SWCH sSpc QUIK ACTN flxs flxs SRCH; sSpc and spac are both NSToolbarSpaceItem). The
        // first flexible space tracks the sidebar's divider (fixUpFlexibleSpacersForItems:,
        // setTrackedSplitView:), so View starts by it; TSearchToolbarItem grows from 60 to 250.
        m_backForward = toolbarSegments({CoreUI::named("GoLeft"), CoreUI::named("GoRight")}, {23, 23}, true, true);
        m_backForward->onSelect = [this](int i) {
            if (i == 0 && !m_back.empty()) {
                m_forward.push_back(m_folder);
                m_folder = m_back.back();
                m_back.pop_back();
                show();
            } else if (i == 1 && !m_forward.empty()) {
                m_back.push_back(m_folder);
                m_folder = m_forward.back();
                m_forward.pop_back();
                show();
            }
        };
        m_toolbar->addItem("com.apple.finder.BACK", core("TL9", "Back"), m_backForward, 53);
        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);

        m_views = toolbarSegments({CoreUI::named("IconView"), CoreUI::named("ListView"), CoreUI::named("ColumnView"), CoreUI::named("FlowView")},
                                  {25, 26, 26, 25}, false, true);
        m_views->setSelected(0);
        m_views->onSelect = [this](int i) { setView(i); };
        m_toolbar->addItem("com.apple.finder.SWCH", core("TL8", "View"), m_views, 111);
        m_toolbar->addItem("NSToolbarSpaceItem", {}, nullptr, 0);

        m_quickLook = new TemplateButton(CoreUI::named("QuickLook"));
        m_toolbar->addItem("com.apple.finder.QUIK", core("TL14", "Quick Look"), m_quickLook, 40);

        auto *action = new TemplateButton(CoreUI::named("Action"), true);
        action->action = [this, action] { showActionMenu(action); };
        m_toolbar->addItem("com.apple.finder.ACTN", core("TL26", "Action"), action, 40);

        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);
        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);

        auto *search = new SearchField();
        search->setFrame({0, 0, 112, 22});
        m_toolbar->addItem("com.apple.finder.SRCH", core("TL23", "Search"), search, 60, 250);
    }

    void updateToolbar()
    {
        m_backForward->segmentEnabled = {!m_back.empty(), !m_forward.empty()};
        m_backForward->setNeedsDisplay();
    }

    void showActionMenu(View *button)
    {
        // TActionToolbarItem's menu: the selection's commands, else the folder's
        const std::vector<Item> sel = selectedItems();
        std::vector<MenuItem> items;
        std::vector<std::function<void()>> actions;
        auto add = [&](const std::string &title, bool enabled, std::function<void()> f) {
            items.push_back({title, false, enabled, false});
            actions.push_back(std::move(f));
        };
        auto line = [&] {
            items.push_back(MenuItem::line());
            actions.push_back({});
        };
        if (!sel.empty()) {
            add(tr("N151", "Open"), true, [this, sel] {
                for (const Item &i : sel)
                    i.folder ? go(i.path) : openDocument(i);
            });
            line();
            add(tr("N153", "Move to Trash"), !inTrash(), [this] { moveToTrash(); });
            line();
            add(tr("N165", "Get Info"), true, [this] { getInfo(); });
            add(tr("N154", "Duplicate"), true, [this] { duplicate(); });
            line();
            add(sel.size() == 1 ? format(core("N47", "Copy “^0”"), {sel.front().name}) : format(tr("N48", "Copy ^0 Items"), {std::to_string(sel.size())}),
                true, [this] { copySelection(); });
        } else {
            add(tr("N156", "New Folder"), !inTrash(), [this] { newFolder(); });
            line();
            add(tr("N165", "Get Info"), true, [this] { getInfo(); });
            if (inTrash())
                add(core("A3", "Empty Trash…"), !list(m_folder).empty(), [this] { emptyTrash(); });
            line();
            add(s_clipboard.size() == 1 ? tr("N49", "Paste Item") : tr("N50", "Paste Items"), !s_clipboard.empty() && !inTrash(),
                [this] { paste(); });
            line();
            add(core("N35", "Show View Options"), true, [this] { toggleViewOptions(); });
        }
        const Point at = button->toWindow({0, button->frame().h + 2});
        Menu::popUp(m_window.get(), at, items, 0, Font::system(14), [actions](int i) {
            if (i >= 0 && size_t(i) < actions.size() && actions[size_t(i)])
                actions[size_t(i)]();
        });
    }

    // ---- the file operations (fileops.h), followed in the Copy window (progress.h)

    bool inTrash() const { return m_folder == Operation::trashFiles(); }

    static std::vector<std::string> paths(const std::vector<Item> &items)
    {
        std::vector<std::string> out;
        for (const Item &i : items)
            out.push_back(i.path);
        return out;
    }

    // a drop: on the same disk a move, on another a copy; Option down, a copy
    static void transfer(const std::vector<Item> &items, const std::string &folder, bool option)
    {
        if (items.empty() || folder.empty())
            return;
        struct stat to {}, from {};
        stat(folder.c_str(), &to);
        stat(items.front().path.c_str(), &from);
        const bool copy = option || to.st_dev != from.st_dev;
        std::vector<std::string> sources;
        for (const Item &i : items)
            if (copy || dirName(i.path) != folder)   // a move into its own folder: nothing to do
                sources.push_back(i.path);
        if (sources.empty())
            return;
        if (folder == Operation::trashFiles() && !copy)
            Progress::follow(Operation::start(Operation::Trash, sources));
        else
            Progress::follow(Operation::start(copy ? Operation::Copy : Operation::Move, sources, folder));
    }

    // where a drop at a point of the screen goes: a Finder window's sidebar place, its folder under
    // the pointer, else the window's own folder
    static std::string dropTargetAt(Point screen)
    {
        for (BrowserWindow *w : s_windows) {
            const Point origin = w->m_window->toScreen({0, 0});
            const Size size = w->m_window->size();
            const Point p{screen.x - origin.x, screen.y - origin.y};
            if (p.x < 0 || p.y < 0 || p.x >= size.w || p.y >= size.h)
                continue;
            if (!w->m_sidebar->isHidden() && w->m_sidebar->frame().contains(p))
                return w->m_sidebar->pathAt(w->m_sidebar->fromWindow(p));
            if (w->m_view == 0 && w->m_scroll->frame().contains(p))
                if (const Item *item = w->m_icons->itemAtPoint(w->m_icons->fromWindow(p)); item && item->folder)
                    return item->path;
            return w->m_folder;
        }
        return {};
    }

    void copySelection()
    {
        // Edit > Copy: the items to paste later (the Finder has no Cut)
        const std::vector<Item> sel = selectedItems();
        if (!sel.empty())
            s_clipboard = paths(sel);
    }

    void paste()
    {
        if (s_clipboard.empty() || inTrash())
            return;
        Progress::follow(Operation::start(Operation::Copy, s_clipboard, m_folder));
    }

    void duplicate()
    {
        const std::vector<Item> sel = selectedItems();
        if (!sel.empty() && !inTrash())
            Progress::follow(Operation::start(Operation::Duplicate, paths(sel)));
    }

    void moveToTrash()
    {
        const std::vector<Item> sel = selectedItems();
        if (!sel.empty() && !inTrash())
            Progress::follow(Operation::start(Operation::Trash, paths(sel)));
    }

    void emptyTrash()
    {
        // "Are you sure you want to permanently erase the items in the Trash?" (A15, A16): a sheet,
        // Cancel and OK
        const Size size{420, 126};
        auto *content = new View;
        content->setFrame({0, 0, size.w, size.h});
        auto *iconView = new ImageView;
        iconView->image = Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/Finder.icns"), {64, 64});
        iconView->setFrame({24, 15, 64, 64});
        content->addSubview(iconView);
        auto *message = new Label(tr("A15", "Are you sure you want to permanently erase the items in the Trash?"), Font::boldSystem(13));
        message->wrap = true;
        message->setFrame({104, 15, size.w - 104 - 20, 34});
        content->addSubview(message);
        auto *info = new Label(tr("A16", "You can’t undo this action."), Font::system(11));
        info->setFrame({104, 55, size.w - 104 - 20, 14});
        content->addSubview(info);
        auto *cancel = new Button(tr("AL1", "Cancel"));
        cancel->setFrame({size.w - 20 - 82 - 82 - 12, size.h - 20 - 32, 82, 32});
        cancel->action = [this] { Sheet::end(m_window.get()); };
        content->addSubview(cancel);
        auto *ok = new Button(tr("AL4", "OK"));
        ok->isDefault = true;
        ok->setFrame({size.w - 20 - 82 - 6, size.h - 20 - 32, 82, 32});
        ok->action = [this] {
            Sheet::end(m_window.get());
            Progress::follow(Operation::start(Operation::EmptyTrash, {}));
        };
        content->addSubview(ok);
        Sheet::begin(m_window.get(), content, size, m_toolbarShown ? m_toolbar->height() : 0);
    }

    void newFolder()
    {
        // "untitled folder", then "untitled folder 2"... (DesktopServicesPriv's TUniqueNamer)
        const std::string name = tr("N2", "untitled folder");
        std::string path = m_folder + "/" + name;
        for (int n = 2; isDir(path) || isFile(path); ++n)
            path = m_folder + "/" + name + " " + std::to_string(n);
        if (mkdir(path.c_str(), 0755) == 0)
            show();
    }

    void updateStatus()
    {
        // "^0 items, ^1 available" (LocalizableCore SB1, SB_COUNT_*), "^0 of ^1 selected" (SB18)
        const ListView *list = m_view == 3 ? m_flowView->list() : m_list;
        const size_t n = m_view == 2 ? m_columnsView->itemCount() : m_view != 0 ? list->itemCount() : m_icons->items().size();
        const size_t sel = m_view == 2 ? m_columnsView->selectedCount() : m_view != 0 ? list->selectedCount() : m_icons->selection().size();
        std::string count;
        if (sel > 0)
            count = sel == 1 && n == 1 ? core("SB17", "1 of 1 selected")
                                       : format(core("SB18", "^0 of ^1 selected"), {std::to_string(sel), std::to_string(n)});
        else
            count = n == 0 ? core("SB_COUNT_V1", "0 items")
                    : n == 1 ? core("SB_COUNT_V2", "1 item")
                             : format(core("SB_COUNT_V3", "^0 items"), {std::to_string(n)});
        const long long free = freeSpace(m_folder);
        m_status->text = free >= 0 ? format(core("SB1", "^0, ^1 available"), {count, formatBytes(free)}) : count;
        m_status->setNeedsDisplay();
    }

    void layout()
    {
        const Size s = m_window->size();
        const double top = m_toolbarShown ? m_toolbar->height() : 0, bottom = s.h - StatusBar::Height;
        const double sw = m_toolbarShown ? std::min(m_sidebarWidth, std::max(0.0, s.w - 200)) : 0;
        m_toolbar->items[1].alignTo = sw + 1;   // the divider's right edge
        m_toolbar->setFrame({0, 0, s.w, m_toolbar->height()});
        m_sidebar->setHidden(sw <= 0);
        m_sidebar->setFrame({0, top, sw, bottom - top});
        m_divider->setFrame({sw - 3, top, 7, bottom - top});
        m_divider->setHidden(!m_toolbarShown);
        m_scroll->setFrame({sw + 1, top, s.w - sw - 1, bottom - top});
        m_list->setFrame({sw + 1, top, s.w - sw - 1, bottom - top});
        m_columnsView->setFrame({sw + 1, top, s.w - sw - 1, bottom - top});
        m_flowView->setFrame({sw + 1, top, s.w - sw - 1, bottom - top});
        m_status->setFrame({0, bottom, s.w, StatusBar::Height});
        m_iconSize->setFrame({s.w - IconSizeSlider::Right - IconSizeSlider::Width, bottom + 1, IconSizeSlider::Width, StatusBar::Height - 1});
        m_iconSize->setHidden(m_view != 0);
        layoutIcons();
    }

    void layoutIcons()
    {
        const double w = m_scroll->frame().w;
        m_scroll->setDocumentHeight(m_icons->contentHeight(w - CoreUI::ScrollerWidth));
        if (m_icons->contentHeight(w) <= m_scroll->frame().h)
            m_scroll->setDocumentHeight(m_icons->contentHeight(w));
        m_icons->setFrame(m_scroll->document()->bounds());
        m_icons->setNeedsDisplay();
    }

    std::unique_ptr<Window> m_window;
    Toolbar *m_toolbar = nullptr;
    Sidebar *m_sidebar = nullptr;
    Divider *m_divider = nullptr;
    ScrollView *m_scroll = nullptr;
    IconView *m_icons = nullptr;
    ListView *m_list = nullptr;
    ColumnView *m_columnsView = nullptr;
    FlowView *m_flowView = nullptr;
    int m_view = 0;   // 0 icons, 1 list, 2 columns, 3 Cover Flow
    bool m_toolbarShown = true;
    StatusBar *m_status = nullptr;
    IconSizeSlider *m_iconSize = nullptr;
    SegmentedControl *m_backForward = nullptr, *m_views = nullptr;
    Button *m_quickLook = nullptr;
    double m_sidebarWidth = DefaultSidebarWidth, m_dragFrom = 0;
    std::string m_folder;
    std::unique_ptr<DSStore::Store> m_store;   // the folder's .DS_Store
    ViewSettings m_settings;                   // its view settings
    static inline BrowserWindow *s_front = nullptr;   // the window the View Options panel follows
    static inline std::vector<BrowserWindow *> s_windows;      // every Finder window (a drop's targets)
    static inline std::vector<std::string> s_clipboard;        // Edit > Copy's items
    std::vector<std::string> m_back, m_forward;
};

// ---- one process for every window: an abstract socket (no file), by user and display

sockaddr_un socketAddress(socklen_t &length)
{
    const char *display = std::getenv("DISPLAY");
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    const std::string name = "snowleopard-finder-" + std::to_string(getuid()) + "-" +
                             (wayland && *wayland ? wayland : display ? display : "");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t n = std::min(name.size(), sizeof addr.sun_path - 1);
    std::memcpy(addr.sun_path + 1, name.data(), n);   // sun_path[0] = 0: abstract
    length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
    return addr;
}

// A folder to open from the command line: "trash:///" (the Dock's Trash) is the freedesktop Trash's
// files; a file opens its folder; anything else the home folder
std::string folderToOpen(const std::string &p)
{
    if (p.rfind("trash:", 0) == 0) {
        const char *data = std::getenv("XDG_DATA_HOME");
        const std::string trash = (data && *data ? std::string(data) : home() + "/.local/share") + "/Trash/files";
        makePath(trash);
        return trash;
    }
    char real[4096];
    std::string path = p.rfind("file://", 0) == 0 ? p.substr(7) : p;
    path = realpath(path.c_str(), real) ? std::string(real) : path;
    if (isFile(path))
        path = dirName(path);
    return isDir(path) ? path : home();
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> folders;
    for (int i = 1; i < argc; ++i)
        folders.push_back(folderToOpen(argv[i]));
    if (folders.empty())
        folders.push_back(home());

    // a Finder already running opens them
    socklen_t addrLength = 0;
    const sockaddr_un addr = socketAddress(addrLength);
    {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, reinterpret_cast<const sockaddr *>(&addr), addrLength) == 0) {
            std::string msg;
            for (const std::string &f : folders)
                msg += f + '\n';
            const bool sent = write(fd, msg.data(), msg.size()) == ssize_t(msg.size());
            close(fd);
            if (sent)
                return 0;
        }
        if (fd >= 0)
            close(fd);
    }

    setProgramName("Finder");
    Assets::registerFonts();
    App &app = App::shared();
    if (!app.open()) {
        std::fprintf(stderr, "FinderWindows: no display\n");
        return 1;
    }
    std::vector<std::unique_ptr<BrowserWindow>> windows;
    std::function<void(const std::string &)> open = [&](const std::string &folder) {
        auto w = std::make_unique<BrowserWindow>(folder);
        w->onClose = [&](BrowserWindow *closed) {
            app.later([&, closed] {
                windows.erase(std::remove_if(windows.begin(), windows.end(), [closed](const auto &p) { return p.get() == closed; }),
                              windows.end());
                if (windows.empty())
                    app.quit(0);
            });
        };
        windows.push_back(std::move(w));
    };
    for (const std::string &f : folders)
        open(f);
    InfoWindow::onRenamed = [&](const std::string &folder) {
        for (auto &w : windows)
            w->refresh(folder);
    };
    // an operation ended: the windows showing its folders (its destination, its items' folders, the
    // Trash) refresh
    Progress::onFinished = [&](const Operation &op) {
        std::vector<std::string> folders = {op.destination(), Operation::trashFiles()};
        for (const std::string &s : op.sources())
            folders.push_back(dirName(s).empty() ? "/" : dirName(s));
        for (auto &w : windows)
            for (const std::string &f : folders)
                w->refresh(f);
    };

    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server >= 0 && bind(server, reinterpret_cast<const sockaddr *>(&addr), addrLength) == 0 && listen(server, 4) == 0) {
        app.addTimer(0.2, true, [&] {
            for (int c; (c = accept(server, nullptr, nullptr)) >= 0;) {
                std::string msg;
                char buf[4096];
                for (ssize_t n; (n = read(c, buf, sizeof buf)) > 0;)
                    msg.append(buf, size_t(n));
                close(c);
                for (size_t start = 0, end; (end = msg.find('\n', start)) != std::string::npos; start = end + 1)
                    if (end > start)
                        open(msg.substr(start, end - start));
            }
        });
    }
    const int code = app.run();
    if (server >= 0)
        close(server);
    return code;
}
