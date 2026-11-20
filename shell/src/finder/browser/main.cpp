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
#include "goto.h"
#include "labels.h"
#include "preferences.h"
#include "quicklook.h"
#include "search.h"

#include "controls.h"
#include "foundation.h"
#include "toolbar.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>

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

// A document: PDFs and pictures to Preview, songs to iTunes (ours, beside this program's bundle: 10.6's
// LaunchServices defaults), applications started, the rest to the desktop's own choice
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
    static const char *const music[] = {"mp3", "m4a", "m4p", "aac", "aif", "aiff", "wav", "flac", "ogg", "oga", "opus"};
    const std::string itunesBin = apps + "/iTunes.app/Contents/MacOS/iTunes";
    if (std::find(std::begin(music), std::end(music), ext) != std::end(music) && isFile(itunesBin)) {
        spawn({itunesBin, item.path});
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
        // the window's bottom border, a line above it: 202 down to 167 under the line (measured on a
        // 10.6.8 Finder window); a window that isn't key: lighter (estimated)
        const bool active = windowIsKey();
        const Color top = active ? Color::gray(202) : Color::gray(228), bottom = active ? Color::gray(167) : Color::gray(207);
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

// The sidebar's divider (TSidebarSplitter's thin style): 1 point, dragged a few points around it; grey
// 206, the sidebar's last column darkened under it (a 10.6.8 Finder: 222/228/234 there, 206/210/214)
class Divider : public View {
public:
    std::function<void(double dx, bool start)> onDrag;
    void draw(cairo_t *cr) override { fill(cr, Rect{std::floor(bounds().w / 2), 0, 1, bounds().h}, Color::gray(206)); }
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


// ---- Open With: the desktop's applications for the item's type (xdg's mimeinfo.cache), its default first

struct Application {
    std::string id, name, file;   // "org.gnome.TextEditor.desktop", its Name, its .desktop's path
};

std::string capture(const std::vector<std::string> &argv)
{
    int fds[2];
    if (pipe(fds) != 0)
        return {};
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0)
            dup2(null, 2);
        std::vector<char *> args;
        for (const std::string &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
        out.append(buf, size_t(n));
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

Application application(const std::string &id)
{
    for (const std::string &d : applicationDirs()) {
        const std::string file = d + "/" + id;
        std::ifstream in(file);
        if (!in)
            continue;
        Application a{id, id, file};
        std::string line;
        bool entry = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '[')
                entry = line == "[Desktop Entry]";
            else if (entry && line.rfind("Name=", 0) == 0) {
                a.name = line.substr(5);
                break;
            }
        }
        return a;
    }
    return {};
}

// the applications for the file's type: the default one first, then the others by name
std::vector<Application> applicationsFor(const std::string &path)
{
    std::string mime = capture({"xdg-mime", "query", "filetype", path});
    std::vector<Application> out;
    if (mime.empty())
        return out;
    // a text type nothing opens (text/x-todo-txt): its parent's, text/plain
    std::string def = capture({"xdg-mime", "query", "default", mime});
    if (def.empty() && mime.rfind("text/", 0) == 0 && mime != "text/plain") {
        mime = "text/plain";
        def = capture({"xdg-mime", "query", "default", mime});
    }
    std::set<std::string> seen;
    if (!def.empty())
        if (Application a = application(def); !a.file.empty()) {
            out.push_back(a);
            seen.insert(def);
        }
    std::vector<Application> others;
    for (const std::string &d : applicationDirs()) {
        std::ifstream in(d + "/mimeinfo.cache");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind(mime + "=", 0) != 0)
                continue;
            std::string ids = line.substr(mime.size() + 1);
            for (size_t start = 0; start < ids.size();) {
                const size_t end = ids.find(';', start);
                const std::string id = ids.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!id.empty() && seen.insert(id).second)
                    if (Application a = application(id); !a.file.empty())
                        others.push_back(a);
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
    }
    std::sort(others.begin(), others.end(), [](const Application &a, const Application &b) { return finderLess(a.name, b.name); });
    out.insert(out.end(), others.begin(), others.end());
    return out;
}

// ---- the path bar (View > Show Path Bar, TPathBarView in BottomBar.nib): the folder's path from the
// startup disk, each folder with its icon, ▸ between them; a click opens that folder

class PathBar : public View {
public:
    static constexpr double Height = 22;
    std::string folder;
    std::function<void(const std::string &)> onGo;

    std::vector<std::string> components() const
    {
        std::vector<std::string> out;
        if (folder == ComputerFolder) {
            out.push_back(ComputerFolder);
            return out;
        }
        for (std::string p = folder; !p.empty(); p = p == "/" ? std::string() : (dirName(p).empty() ? "/" : dirName(p)))
            out.insert(out.begin(), p);
        return out;
    }
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), windowIsKey() ? Color::gray(232) : Color::gray(244));
        fill(cr, Rect{0, 0, frame().w, 1}, Color::gray(170));
        TextStyle t;
        t.font = Font::system(11);
        t.color = windowIsKey() ? Color::gray(0) : Color::gray(0, 0.6);
        double x = 8;
        m_hits.clear();
        const std::vector<std::string> parts = components();
        for (size_t i = 0; i < parts.size(); ++i) {
            const std::string name = displayName(parts[i]);
            const Image img = icon(Item{name, parts[i], true}, 16);
            const double w = 20 + std::ceil(textWidth(name, t.font));
            img.draw(cr, Rect{x, 3, 16, 16});
            drawText(cr, name, Rect{x + 19, 3, w - 19, 16}, t);
            m_hits.push_back({x, x + w, parts[i]});
            x += w + 6;
            if (i + 1 < parts.size()) {
                // the ▸ between folders
                cairo_move_to(cr, x, 8);
                cairo_line_to(cr, x + 5, 11);
                cairo_line_to(cr, x, 14);
                cairo_close_path(cr);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
                cairo_fill(cr);
                x += 12;
            }
        }
    }
    void mouseDown(Event &e) override
    {
        for (const auto &h : m_hits)
            if (e.location.x >= h.from && e.location.x < h.to && onGo)
                onGo(h.path);
    }

private:
    struct Hit {
        double from, to;
        std::string path;
    };
    std::vector<Hit> m_hits;
};

// the field renaming an item in place (the Finder's field editor: white, a thin grey frame)
class RenameField : public TextField {
public:
    RenameField() { style = Plain; }
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(255));
        cairo_rectangle(cr, 0.5, 0.5, frame().w - 1, frame().h - 1);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        TextField::draw(cr);
    }
};

// ---- the contextual menu's table (TContextMenu -buildContextMenu:forContext:target:maxItems:
// addServices:, the 0x28-byte entries at 0x10042ae00): the contexts it is in (1 items, 2 a window's
// background, 4 the desktop's, 8 a sidebar item, 0x20 the path bar), its command, its title's key
// (Localizable, else the command's own title, made when the menu is); none a separator. The Time
// Machine, iDisk, burning and search slices' entries aren't here (nothing here has them).

struct ContextEntry {
    unsigned mask;
    const char *command;
};
const ContextEntry ContextTable[] = {
    {0x06, "cmdNewFolderAtMouseLocation:"}, {0x3d, "cmdOpen:"},           {0x2d, "cmdOpenWith:"},        {0x00, nullptr},
    {0x05, "cmdShowOriginal:"},             {0x28, "cmdOpenEnclosingFolder:"}, {0x05, "cmdMoveToTrash:"}, {0x00, nullptr},
    {0x07, "cmdEmptyTrash:"},               {0x07, "cmdEmptyTrashSecure:"}, {0x2f, "cmdEject:"},          {0x00, nullptr},
    {0x3f, "cmdShowStaticInfo:"},           {0x05, "cmdArchive:"},         {0x05, "cmdDuplicate:"},       {0x05, "cmdMakeAlias:"},
    {0x05, "cmdQuickLook:"},                {0x00, nullptr},               {0x05, "copy:"},               {0x07, "paste:"},
    {0x00, nullptr},                        {0x0e, "cmdChangeDesktopBackground:"}, {0x02, "cmdViewAsSubmenu:"}, {0x07, "placeholderViewSpecificStuff"},
    {0x07, "cmdToggleViewOptions:"},        {0x00, nullptr},               {0x18, "cmdRemoveFromSidebar:"}, {0x00, nullptr},
    {0x18, "cmdShowSidebarPrefs:"},         {0x05, "cmdSetLabel:"},
};

// ---- the key equivalents (MenuBar.nib's items: their key, their modifier mask), each a command

struct KeyEquivalent {
    uint32_t key;
    unsigned modifiers;
    const char *command;
};
const KeyEquivalent KeyTable[] = {
    {XKB_KEY_n, Command, "cmdNewFileViewer:"},          {XKB_KEY_n, Command | Shift, "cmdNewFolder:"},
    {XKB_KEY_n, Command | Option, "cmdNewSmartFolder:"}, {XKB_KEY_o, Command, "cmdOpen:"},
    {XKB_KEY_o, Command | Control, "cmdOpenAltBrowse:"}, {XKB_KEY_o, Command | Option, "cmdOpenOptionBrowse:"},
    {XKB_KEY_w, Command, "performClose:"},             {XKB_KEY_w, Command | Option, "cmdCloseAllWindows:"},
    {XKB_KEY_i, Command, "cmdShowStaticInfo:"},        {XKB_KEY_i, Command | Option, "cmdShowInspector:"},
    {XKB_KEY_i, Command | Control, "cmdShowStaticInfoSummary:"},
    {XKB_KEY_d, Command, "cmdDuplicate:"},             {XKB_KEY_l, Command, "cmdMakeAlias:"},
    {XKB_KEY_y, Command, "cmdQuickLook:"},             {XKB_KEY_y, Command | Option, "cmdSlideshow:"},
    {XKB_KEY_r, Command, "cmdShowOriginal:"},          {XKB_KEY_t, Command, "cmdAddToSidebar:"},
    {XKB_KEY_BackSpace, Command, "cmdMoveToTrash:"},   {XKB_KEY_BackSpace, Command | Option, "cmdMoveToTrashSilently:"},
    {XKB_KEY_e, Command, "cmdEject:"},                 {XKB_KEY_f, Command, "cmdFindFile:"},
    {XKB_KEY_f, Command | Shift, "cmdAlternateFindFile:"}, {XKB_KEY_f, Command | Option, "cmdSetFocusToSearchField:"},
    {XKB_KEY_z, Command, "undo:"},                     {XKB_KEY_c, Command, "copy:"},
    {XKB_KEY_v, Command, "paste:"},                    {XKB_KEY_a, Command, "selectAll:"},
    {XKB_KEY_a, Command | Option, "cmdDeselectAll:"},
    {XKB_KEY_1, Command, "cmdViewAsIcons:"},           {XKB_KEY_2, Command, "cmdViewAsList:"},
    {XKB_KEY_3, Command, "cmdViewAsColumns:"},         {XKB_KEY_4, Command, "cmdViewAsFlow:"},
    {XKB_KEY_1, Command | Control, "cmdArrangeByName:"}, {XKB_KEY_2, Command | Control, "cmdArrangeByModDate:"},
    {XKB_KEY_3, Command | Control, "cmdArrangeByCreationDate:"}, {XKB_KEY_4, Command | Control, "cmdArrangeByPhysicalSize:"},
    {XKB_KEY_5, Command | Control, "cmdArrangeByKind:"}, {XKB_KEY_6, Command | Control, "cmdArrangeByLabel:"},
    {XKB_KEY_1, Command | Control | Option, "cmdKeepArrangedByName:"}, {XKB_KEY_2, Command | Control | Option, "cmdKeepArrangedByModDate:"},
    {XKB_KEY_3, Command | Control | Option, "cmdKeepArrangedByCreationDate:"},
    {XKB_KEY_4, Command | Control | Option, "cmdKeepArrangedByPhysicalSize:"},
    {XKB_KEY_5, Command | Control | Option, "cmdKeepArrangedByKind:"}, {XKB_KEY_6, Command | Control | Option, "cmdKeepArrangedByLabel:"},
    {XKB_KEY_s, Command | Option, "cmdToggleSidebar:"}, {XKB_KEY_t, Command | Option, "cmdToggleToolbar:"},
    {XKB_KEY_j, Command, "cmdToggleViewOptions:"},
    {XKB_KEY_bracketleft, Command, "cmdBack:"},        {XKB_KEY_bracketright, Command, "cmdForward:"},
    {XKB_KEY_Up, Command, "cmdOpenParent:"},           {XKB_KEY_Up, Command | Option, "cmdOpenParent:"},
    {XKB_KEY_Up, Command | Control, "cmdOpenParentAltBrowse:"}, {XKB_KEY_Up, Command | Shift, "cmdSelectStartupDisk:"},
    {XKB_KEY_Down, Command, "cmdOpenWithDownArrow:"},  {XKB_KEY_Down, Command | Option, "cmdOpenWithDownArrow:"},
    {XKB_KEY_c, Command | Shift, "cmdGoToComputer:"},  {XKB_KEY_h, Command | Shift, "cmdGoHome:"},
    {XKB_KEY_d, Command | Shift, "cmdGoToDesktop:"},   {XKB_KEY_k, Command | Shift, "cmdGoToNetwork:"},
    {XKB_KEY_a, Command | Shift, "cmdGoToApplications:"}, {XKB_KEY_o, Command | Shift, "cmdGoToDocuments:"},
    {XKB_KEY_u, Command | Shift, "cmdGoToUtilities:"},  {XKB_KEY_g, Command | Shift, "cmdGoTo:"},
    {XKB_KEY_k, Command, "cmdConnectTo:"},             {XKB_KEY_grave, Command, "cmdCycleWindows:"},
    {XKB_KEY_comma, Command, "cmdPreferences:"},
    {XKB_KEY_BackSpace, Command | Shift, "cmdEmptyTrash:"}, {XKB_KEY_BackSpace, Command | Shift | Option, "cmdEmptyTrashSilently:"},
};

// a key event's command (its letters' case set aside: Shift is in the modifiers)
const char *commandForKey(const Event &e)
{
    uint32_t key = e.keysym;
    if (key >= XKB_KEY_A && key <= XKB_KEY_Z)
        key = key - XKB_KEY_A + XKB_KEY_a;
    // the shifted digits and brackets come as their own keysyms
    switch (key) {
    case XKB_KEY_braceleft: key = XKB_KEY_bracketleft; break;
    case XKB_KEY_braceright: key = XKB_KEY_bracketright; break;
    case XKB_KEY_asciitilde: key = XKB_KEY_grave; break;
    case XKB_KEY_exclam: key = XKB_KEY_1; break;
    case XKB_KEY_at: key = XKB_KEY_2; break;
    case XKB_KEY_numbersign: key = XKB_KEY_3; break;
    case XKB_KEY_dollar: key = XKB_KEY_4; break;
    case XKB_KEY_percent: key = XKB_KEY_5; break;
    case XKB_KEY_asciicircum: key = XKB_KEY_6; break;
    default: break;
    }
    const unsigned mods = e.modifiers & (Command | Shift | Option | Control);
    for (const KeyEquivalent &k : KeyTable)
        if (k.key == key && k.modifiers == mods)
            return k.command;
    return nullptr;
}

// the state the menu bar reads (SystemUIServer builds the Finder's menus from MenuBar.nib and asks the
// front window what is enabled): $XDG_RUNTIME_DIR/snowleopard-finder-<uid>-<display>.state
std::string stateFile()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const char *display = std::getenv("DISPLAY");
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    std::string d = wayland && *wayland ? wayland : display ? display : "";
    for (char &c : d)
        if (c == '/')
            c = '_';
    return std::string(runtime && *runtime ? runtime : "/tmp") + "/snowleopard-finder-" + std::to_string(getuid()) + "-" + d + ".state";
}

// the last folders opened (Go > Recent Folders, FXRecentFolders: ten)
void rememberFolder(const std::string &folder)
{
    if (folder == ComputerFolder)
        return;
    Settings conf("finder");
    std::vector<std::string> recent = conf.list("FXRecentFolders");
    recent.erase(std::remove(recent.begin(), recent.end(), folder), recent.end());
    recent.insert(recent.begin(), folder);
    if (recent.size() > 10)
        recent.resize(10);
    conf.set("FXRecentFolders", recent);
}

// Edit > Undo: the last action, as the Finder keeps it (one, its title "Undo Rename"...)
struct UndoAction {
    std::string title;
    std::function<void()> undo;
};
UndoAction s_undo;

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
            if (r.path.rfind("search:", 0) == 0) {
                startSearch({}, r.path.substr(7), r.title);
                return;
            }
            go(r.path);
        };
        m_sidebar->onEject = [](const Sidebar::Row &r) { eject(r.path); };
        m_sidebar->onContextMenu = [this](const Sidebar::Row &r, Point at) { showSidebarMenu(r, at); };
        m_scroll = root->add<ScrollView>();
        m_icons = m_scroll->document()->add<IconView>();
        m_icons->onOpen = [this](const Item &item) { openItems({item}, false); };
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
        m_pathBar = root->add<PathBar>();
        m_pathBar->onGo = [this](const std::string &f) { go(f); };
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
            std::string target = dropTargetAt(screen);
            const std::vector<Item> sel = m_icons->selectedItems();
            if (target.empty()) {
                // on the desktop: into the Desktop folder, the icons where they were dropped
                target = userDir("DESKTOP", home() + "/Desktop");
                std::string names;
                for (const Item &i : sel)
                    names += "\t" + i.name;
                tellDesktop("place:" + std::to_string(int(screen.x)) + "," + std::to_string(int(screen.y)) + names);
            }
            transfer(sel, target, option);
        };
        // the contextual menus (the right button, Control-click)
        auto context = [this](Point at, bool onItem) { showContextMenu(at, onItem ? 1 : 2); };
        m_icons->onContextMenu = context;
        m_list->onContextMenu = context;
        m_flowView->list()->onContextMenu = context;
        m_columnsView->onContextMenu = context;
        // a click on the name of the item selected alone: renaming after a moment
        m_icons->onClickName = [this](const Item &item) { scheduleRename(item); };
        m_list->onClickName = m_icons->onClickName;
        m_flowView->list()->onClickName = m_icons->onClickName;
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
        // the keys: MenuBar.nib's key equivalents (commandForKey), Return renames, the space bar is
        // Quick Look, Escape ends a search
        m_window->onUnhandledKey = [this](Event &e) {
            if (const char *command = commandForKey(e)) {
                perform(command);
                return;
            }
            if (e.modifiers & (Command | Control))
                return;
            if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter) {
                const std::vector<Item> sel = selectedItems();
                if (sel.size() == 1)
                    startRename(sel.front());
            } else if (e.keysym == XKB_KEY_space) {
                perform("cmdQuickLook:");
            } else if (e.keysym == XKB_KEY_Escape && m_search) {
                endSearch(true);
            }
        };
        m_window->onKeyChanged = [this](bool key) {
            if (key) {
                targetViewOptions();
                writeState();
            }
        };
        m_window->onClose = [this] {
            s_windows.erase(std::remove(s_windows.begin(), s_windows.end(), this), s_windows.end());
            if (s_front == this)
                s_front = s_windows.empty() ? nullptr : s_windows.back();
            endSearch(false);
            cancelRename();
            saveWindowSettings();
            if (onClose)
                onClose(this);
        };
        m_window->setResizable({460, 200 + m_toolbar->height()});
        Settings conf("finder");
        m_statusShown = conf.boolean("ShowStatusBar", true);
        m_pathBarShown = conf.boolean("ShowPathbar", false);
        s_front = this;
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
        if (m_search)
            return;
        if (m_view == 2 && m_columnsView->shows(folder) && folder != m_folder) {
            m_columnsView->refresh();
            return;
        }
        if (folder == m_folder || (m_folder == ComputerFolder && folder == "/")) {
            const std::vector<std::string> sel = paths(selectedItems());
            show();
            selectPaths(sel);
        }
    }

    void go(const std::string &folder, bool remember = true)
    {
        if (!isDir(folder) && folder != ComputerFolder)
            return;
        endSearch(false);
        cancelRename();
        if (remember && !m_folder.empty() && folder != m_folder) {
            m_back.push_back(m_folder);
            m_forward.clear();
        }
        m_folder = folder;
        rememberFolder(folder);
        show();
    }

    // ---- the commands: MenuBar.nib's selectors, from the keys, the menu bar (over the socket), the
    // contextual menus and the toolbar

    static BrowserWindow *front() { return s_front; }
    static const std::vector<BrowserWindow *> &all() { return s_windows; }
    const std::string &folder() const { return m_folder; }
    // a new window (main's), and the item selected in it
    static inline std::function<BrowserWindow *(const std::string &)> openWindow;

    void selectPaths(const std::vector<std::string> &paths)
    {
        if (m_view == 0)
            m_icons->selectPaths(paths);
        else if (m_view == 1)
            m_list->selectPaths(paths);
        else if (m_view == 2)
            m_columnsView->selectPaths(paths);
        else
            m_flowView->list()->selectPaths(paths);
    }

    bool perform(const std::string &cmd)
    {
        const std::vector<Item> sel = selectedItems();
        auto selectedPaths = [&] { return paths(sel); };
        if (cmd == "cmdNewFileViewer:") {
            newWindowAtTarget();
        } else if (cmd == "cmdNewFolder:" || cmd == "cmdNewFolderAtMouseLocation:") {
            newFolder();
        } else if (cmd == "cmdNewSmartFolder:" || cmd == "cmdFindFile:" || cmd == "cmdAlternateFindFile:" || cmd == "cmdSetFocusToSearchField:") {
            if (!m_toolbarShown)
                toggleToolbar();
            m_window->makeFirstResponder(m_searchField);
            m_searchField->selectAll();
        } else if (cmd == "cmdOpen:" || cmd == "cmdOpenWithDownArrow:") {
            openItems(sel, false);
        } else if (cmd == "cmdOpenAltBrowse:" || cmd == "cmdOpenOptionBrowse:") {
            openItems(sel, true);
            if (cmd == "cmdOpenOptionBrowse:")
                m_window->close();
        } else if (cmd == "performClose:") {
            m_window->close();
        } else if (cmd == "cmdCloseAllWindows:") {
            const std::vector<BrowserWindow *> windows = s_windows;
            for (BrowserWindow *w : windows)
                w->m_window->close();
        } else if (cmd == "cmdShowStaticInfo:" || cmd == "cmdShowInspector:" || cmd == "cmdShowStaticInfoSummary:") {
            getInfo();
        } else if (cmd == "cmdArchive:") {
            compress();
        } else if (cmd == "cmdDuplicate:") {
            duplicate();
        } else if (cmd == "cmdMakeAlias:") {
            makeAlias();
        } else if (cmd == "cmdQuickLook:" || cmd == "cmdQuickLookFromToolbar:" || cmd == "cmdSlideshow:") {
            QuickLook::onShowItem = [this](const Item &item) { selectPaths({item.path}); };
            std::vector<Item> shown = sel;
            if (shown.empty() && cmd == "cmdQuickLookFromToolbar:")
                shown.push_back(Item{displayName(m_folder), m_folder, true});
            QuickLook::toggle(shown);
        } else if (cmd == "cmdShowOriginal:") {
            showOriginal();
        } else if (cmd == "cmdAddToSidebar:" || cmd == "cmdAddToFavorites:") {
            std::vector<Item> added = sel;
            if (added.empty())
                added.push_back(Item{displayName(m_folder), m_folder, true});
            for (const Item &i : added)
                Sidebar::add(i.path);
            reloadSidebars();
        } else if (cmd == "cmdMoveToTrash:" || cmd == "cmdMoveToTrashSilently:") {
            if (inTrash())
                putBack();
            else
                moveToTrash();
        } else if (cmd == "cmdEject:" || cmd == "cmdEjectOne:" || cmd == "cmdEjectAll:") {
            for (const Item &i : sel)
                eject(i.path);
        } else if (cmd == "undo:") {
            if (s_undo.undo) {
                auto u = std::move(s_undo.undo);
                s_undo = {};
                u();
            }
        } else if (cmd == "copy:") {
            copySelection();
        } else if (cmd == "paste:") {
            paste();
        } else if (cmd == "selectAll:") {
            if (m_view == 0)
                m_icons->selectAll();
            else if (m_view == 1)
                m_list->selectAll();
            else if (m_view == 3)
                m_flowView->list()->selectAll();
        } else if (cmd == "cmdDeselectAll:") {
            selectPaths({});
        } else if (cmd == "cmdViewAsIcons:" || cmd == "cmdViewAsList:" || cmd == "cmdViewAsColumns:" || cmd == "cmdViewAsFlow:") {
            static const char *const views[] = {"cmdViewAsIcons:", "cmdViewAsList:", "cmdViewAsColumns:", "cmdViewAsFlow:"};
            for (int v = 0; v < 4; ++v)
                if (cmd == views[v] && !(m_search && v == 2))
                    setView(v);
        } else if (cmd == "cmdCleanUp:" || cmd == "cmdCleanUpSelection:") {
            cleanUp(cmd == "cmdCleanUpSelection:");
        } else if (cmd.rfind("cmdArrangeBy", 0) == 0 || cmd.rfind("cmdKeepArrangedBy", 0) == 0) {
            const bool keep = cmd.rfind("cmdKeepArrangedBy", 0) == 0;
            const std::string what = cmd.substr(keep ? 17 : 12);
            static const std::map<std::string, std::string> by = {{"Name:", "name"}, {"ModDate:", "dateModified"},
                                                                  {"CreationDate:", "dateCreated"}, {"PhysicalSize:", "size"},
                                                                  {"Kind:", "kind"}, {"Label:", "label"}, {"None:", "none"}};
            auto it = by.find(what);
            if (it != by.end())
                arrange(it->second, keep);
        } else if (cmd == "cmdToggleBottombar:") {
            m_pathBarShown = !m_pathBarShown;
            Settings("finder").set("ShowPathbar", m_pathBarShown);
            layout();
        } else if (cmd == "cmdToggleStatusBar:") {
            m_statusShown = !m_statusShown;
            Settings("finder").set("ShowStatusBar", m_statusShown);
            layout();
        } else if (cmd == "cmdToggleSidebar:") {
            if (m_sidebarWidth > 0) {
                m_hiddenSidebarWidth = m_sidebarWidth;
                m_sidebarWidth = 0;
            } else {
                m_sidebarWidth = m_hiddenSidebarWidth > 0 ? m_hiddenSidebarWidth : DefaultSidebarWidth;
            }
            layout();
        } else if (cmd == "cmdToggleToolbar:") {
            toggleToolbar();
        } else if (cmd == "cmdToggleViewOptions:") {
            toggleViewOptions();
        } else if (cmd == "cmdBack:") {
            m_backForward->onSelect(0);
        } else if (cmd == "cmdForward:") {
            m_backForward->onSelect(1);
        } else if (cmd == "cmdOpenParent:") {
            goUp();
        } else if (cmd == "cmdOpenParentAltBrowse:") {
            if (m_folder != "/" && m_folder != ComputerFolder && openWindow)
                openWindow(dirName(m_folder).empty() ? "/" : dirName(m_folder));
        } else if (cmd == "cmdSelectStartupDisk:") {
            go(ComputerFolder);
            selectPaths({"/"});
        } else if (const std::string place = placeFor(cmd); !place.empty()) {
            go(place);
        } else if (cmd == "cmdGoTo:") {
            GoTo::show(m_window.get(), m_toolbarShown ? m_toolbar->height() : 0, [this](const std::string &f) { go(f); });
        } else if (cmd == "cmdConnectTo:") {
            ConnectToServer::show([](const std::string &f) {
                if (openWindow)
                    openWindow(f);
            });
        } else if (cmd.rfind("cmdRecentFoldersItem:", 0) == 0) {
            go(cmd.substr(21));
        } else if (cmd == "cmdClearRecentFolders:") {
            Settings("finder").set("FXRecentFolders", std::vector<std::string>{});
        } else if (cmd == "cmdCycleWindows:") {
            if (s_windows.size() > 1) {
                auto it = std::find(s_windows.begin(), s_windows.end(), this);
                BrowserWindow *next = it == s_windows.end() || it + 1 == s_windows.end() ? s_windows.front() : *(it + 1);
                next->m_window->show();
            }
        } else if (cmd == "cmdBringAllToFront:") {
            for (BrowserWindow *w : s_windows)
                w->m_window->show();
        } else if (cmd == "cmdPreferences:") {
            Preferences::show();
        } else if (cmd == "cmdShowSidebarPrefs:") {
            Preferences::show(2);
        } else if (cmd == "cmdEmptyTrash:" || cmd == "cmdEmptyTrashSilently:" || cmd == "cmdEmptyTrashSecure:" || cmd == "cmdEmptyTrashSecureSilently:") {
            const bool secure = cmd.find("Secure") != std::string::npos || Settings("finder").boolean("EmptyTrashSecurely", false);
            const bool ask = cmd.find("Silently") == std::string::npos && Settings("finder").boolean("WarnOnEmptyTrash", true);
            emptyTrash(secure, ask);
        } else if (cmd.rfind("cmdSetLabel", 0) == 0 && cmd.size() > 11 && std::isdigit(static_cast<unsigned char>(cmd[11]))) {
            setLabel(cmd[11] - '0');
        } else if (cmd == "cmdOpenEnclosingFolder:") {
            if (!sel.empty() && openWindow)
                if (BrowserWindow *w = openWindow(dirName(sel.front().path).empty() ? "/" : dirName(sel.front().path)))
                    w->selectPaths({sel.front().path});
        } else if (cmd == "cmdChangeDesktopBackground:") {
            spawn({dirName(dirName(dirName(dirName(executablePath())))) + "/System Preferences.app/Contents/MacOS/System Preferences", "desktop"});
        } else if (cmd == "cmdRename:") {
            if (sel.size() == 1)
                startRename(sel.front());
        } else {
            return false;
        }
        (void)selectedPaths;
        writeState();
        return true;
    }

    // what a command can do now (the menus' items enabled), its title when the Finder makes it
    bool validate(const std::string &cmd) const
    {
        const std::vector<Item> sel = selectedItems();
        const bool any = !sel.empty(), trash = inTrash(), searching = m_search != nullptr;
        if (cmd == "cmdNewFolder:" || cmd == "cmdNewFolderAtMouseLocation:")
            return !trash && !searching && m_folder != ComputerFolder && access(m_folder.c_str(), W_OK) == 0;
        if (cmd == "cmdOpen:" || cmd == "cmdOpenWithDownArrow:" || cmd == "cmdOpenAltBrowse:" || cmd == "cmdShowStaticInfoSummary:")
            return any;
        if (cmd == "cmdOpenOptionBrowse:")
            return any;
        if (cmd == "cmdArchive:" || cmd == "cmdDuplicate:" || cmd == "cmdMakeAlias:")
            return any && !trash && m_folder != ComputerFolder;
        if (cmd == "cmdShowOriginal:") {
            struct stat st;
            return sel.size() == 1 && lstat(sel.front().path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
        }
        if (cmd == "cmdMoveToTrash:" || cmd == "cmdMoveToTrashSilently:")
            return any && m_folder != ComputerFolder;
        if (cmd == "cmdEject:")
            return any && std::all_of(sel.begin(), sel.end(), [](const Item &i) { return isEjectable(i.path); });
        if (cmd == "undo:")
            return bool(s_undo.undo);
        if (cmd == "copy:" || cmd == "cmdQuickLook:" || cmd == "cmdSlideshow:")
            return any;
        if (cmd == "paste:")
            return !s_clipboard.empty() && !trash && !searching && m_folder != ComputerFolder;
        if (cmd == "cmdCleanUp:" || cmd == "cmdArrange:" || cmd.rfind("cmdArrangeBy", 0) == 0 || cmd.rfind("cmdKeepArrangedBy", 0) == 0)
            return m_view == 0 && !searching;
        if (cmd == "cmdCleanUpSelection:")
            return m_view == 0 && any && !searching;
        if (cmd == "cmdBack:")
            return !m_back.empty();
        if (cmd == "cmdForward:")
            return !m_forward.empty();
        if (cmd == "cmdOpenParent:" || cmd == "cmdOpenParentAltBrowse:")
            return m_folder != ComputerFolder;
        if (cmd == "cmdViewAsColumns:")
            return !searching;
        if (cmd == "cmdEmptyTrash:" || cmd == "cmdEmptyTrashSecure:")
            return !list(Operation::trashFiles()).empty();
        if (cmd == "cmdSetLabel:")
            return any && !trash;
        if (cmd == "cmdOpenEnclosingFolder:")
            return searching && sel.size() == 1;
        if (cmd == "cmdChangeDesktopBackground:")
            return false;   // the desktop's own menu
        if (cmd == "cmdRemoveFromSidebar:" || cmd == "cmdShowSidebarPrefs:")
            return true;
        if (!placeFor(cmd).empty() || cmd == "cmdGoToComputer:")
            return true;
        if (cmd == "cmdGoToNetwork:" || cmd == "cmdGoToUtilities:")
            return false;
        return true;
    }

    std::string title(const std::string &cmd) const
    {
        // the titles the Finder makes for its selection ("Compress “notes.txt”", "Put Back")
        const std::vector<Item> sel = selectedItems();
        const std::string one = sel.size() == 1 ? sel.front().name : std::string(), n = std::to_string(sel.size());
        if (cmd == "cmdArchive:")
            return sel.size() == 1 ? format(tr("N168", "Compress “^0”"), {one}) : sel.size() > 1 ? format(tr("N169", "Compress ^0 Items"), {n})
                                                                                                    : core("N168.1", "Compress");
        if (cmd == "cmdQuickLook:")
            return sel.size() == 1 ? format(core("N169.18", "Quick Look “^0”"), {one}) : sel.size() > 1 ? format(tr("N169.19", "Quick Look ^0 Items"), {n})
                                                                                                          : core("TL14", "Quick Look");
        if (cmd == "cmdMoveToTrash:")
            return inTrash() ? tr("N153.1", "Put Back") : tr("N153", "Move to Trash");
        if (cmd == "copy:")
            return sel.size() == 1 ? format(core("N47", "Copy “^0”"), {one}) : sel.size() > 1 ? format(tr("N48", "Copy ^0 Items"), {n}) : "Copy";
        if (cmd == "paste:")
            return s_clipboard.size() > 1 ? tr("N50", "Paste Items") : tr("N49", "Paste Item");
        if (cmd == "cmdEject:")
            return sel.size() == 1 ? format(core("N57", "Eject “^0”"), {one}) : core("N58", "Eject");
        if (cmd == "undo:")
            return s_undo.undo ? s_undo.title : core("ME12", "Can’t Undo");
        if (cmd == "cmdToggleBottombar:")
            return m_pathBarShown ? core("FV11", "Hide Path Bar") : core("FV12", "Show Path Bar");
        if (cmd == "cmdToggleStatusBar:")
            return m_statusShown ? core("FV5", "Hide Status Bar") : core("FV6", "Show Status Bar");
        if (cmd == "cmdToggleSidebar:")
            return m_sidebarWidth > 0 ? tr("FV14", "Hide Sidebar") : tr("FV13", "Show Sidebar");
        if (cmd == "cmdToggleToolbar:")
            return m_toolbarShown ? core("FV1", "Hide Toolbar") : "Show Toolbar";
        if (cmd == "cmdToggleViewOptions:")
            return ViewOptions::shared().isOpen() ? tr("N36", "Hide View Options") : core("N35", "Show View Options");
        return {};
    }

    // the state file for the menu bar: "E cmd" enabled, "C cmd" checked, "T cmd<tab>title"
    void writeState() const
    {
        if (s_front != this)
            return;
        static const char *const commands[] = {
            "cmdNewFileViewer:", "cmdNewFolder:", "cmdNewSmartFolder:", "cmdOpen:", "cmdOpenAltBrowse:", "cmdOpenOptionBrowse:", "performClose:",
            "cmdCloseAllWindows:", "cmdShowStaticInfo:", "cmdShowInspector:", "cmdShowStaticInfoSummary:", "cmdArchive:", "cmdDuplicate:",
            "cmdMakeAlias:", "cmdQuickLook:", "cmdSlideshow:", "cmdShowOriginal:", "cmdAddToSidebar:", "cmdMoveToTrash:", "cmdEject:",
            "cmdFindFile:", "undo:", "copy:", "paste:", "selectAll:", "cmdDeselectAll:", "cmdViewAsIcons:", "cmdViewAsList:",
            "cmdViewAsColumns:", "cmdViewAsFlow:", "cmdCleanUp:", "cmdCleanUpSelection:", "cmdArrange:", "cmdArrangeByName:",
            "cmdArrangeByModDate:", "cmdArrangeByCreationDate:", "cmdArrangeByPhysicalSize:", "cmdArrangeByKind:", "cmdArrangeByLabel:",
            "cmdKeepArrangedBy:", "cmdToggleBottombar:", "cmdToggleStatusBar:", "cmdToggleSidebar:", "cmdToggleToolbar:",
            "cmdToggleViewOptions:", "cmdBack:", "cmdForward:", "cmdOpenParent:", "cmdOpenWithDownArrow:", "cmdGoToComputer:", "cmdGoHome:",
            "cmdGoToDesktop:", "cmdGoToNetwork:", "cmdGoToApplications:", "cmdGoToDocuments:", "cmdGoToUtilities:", "cmdGoTo:",
            "cmdConnectTo:", "cmdCycleWindows:", "cmdBringAllToFront:", "cmdPreferences:", "cmdEmptyTrash:", "cmdEmptyTrashSecure:",
            "cmdSetLabel:"};
        std::string out = "pid " + std::to_string(getpid()) + "\n";
        for (const char *c : commands) {
            if (validate(c))
                out += std::string("E ") + c + "\n";
            const std::string t = title(c);
            if (!t.empty())
                out += std::string("T ") + c + "\t" + t + "\n";
        }
        static const char *const views[] = {"cmdViewAsIcons:", "cmdViewAsList:", "cmdViewAsColumns:", "cmdViewAsFlow:"};
        out += std::string("C ") + views[std::clamp(m_view, 0, 3)] + "\n";
        if (m_view == 0 && m_settings.arrangeBy != "none" && m_settings.arrangeBy != "grid") {
            static const std::map<std::string, std::string> keep = {{"name", "Name"}, {"dateModified", "ModDate"}, {"dateCreated", "CreationDate"},
                                                                    {"size", "PhysicalSize"}, {"kind", "Kind"}, {"label", "Label"}};
            if (auto it = keep.find(m_settings.arrangeBy); it != keep.end())
                out += "C cmdKeepArrangedBy" + it->second + ":\n";
        }
        const std::vector<Item> sel = selectedItems();
        if (!sel.empty())
            out += "L " + std::to_string(Labels::get(sel.front().path)) + "\n";
        const std::string file = stateFile();
        const std::string temp = file + ".new";
        if (FILE *f = std::fopen(temp.c_str(), "w")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            std::rename(temp.c_str(), file.c_str());
        }
    }

    // the desktop's items (the Qt desktop sends "items:<command>" with their paths): the commands
    // that need no window, on those items
    static void performOnItems(const std::string &cmd, const std::vector<std::string> &itemPaths)
    {
        std::vector<Item> items;
        for (const std::string &p : itemPaths)
            if (!p.empty())
                items.push_back(Item{displayName(p), p, isDir(p)});
        if (cmd == "cmdShowStaticInfo:") {
            for (const Item &i : items)
                InfoWindow::show(i);
        } else if (cmd == "cmdQuickLook:") {
            QuickLook::onShowItem = {};
            QuickLook::toggle(items);
        } else if (cmd == "cmdMoveToTrash:" && !items.empty()) {
            follow(Operation::start(Operation::Trash, itemPaths), [](const Operation &op) {
                const std::vector<std::string> trashed = op.results();
                if (!trashed.empty())
                    s_undo = {"Undo Move to Trash", [trashed] { follow(Operation::start(Operation::PutBack, trashed), {}); }};
            });
        } else if (cmd == "cmdDuplicate:" && !items.empty()) {
            follow(Operation::start(Operation::Duplicate, itemPaths), [](const Operation &op) {
                undoByTrashing(tr("DU2", "Undo Duplicate"), op.results());
            });
        } else if (cmd == "cmdArchive:" && !items.empty()) {
            follow(Operation::start(Operation::Compress, itemPaths, dirName(itemPaths.front())), [](const Operation &op) {
                undoByTrashing("Undo Compress", op.results());
            });
        } else if (cmd == "copy:") {
            s_clipboard = itemPaths;
        } else if (cmd == "paste:" && !items.empty() && !s_clipboard.empty()) {
            follow(Operation::start(Operation::Copy, s_clipboard, itemPaths.front()), [](const Operation &op) {
                undoByTrashing(tr("ME16", "Undo Paste"), op.results());
            });
        } else if (cmd.rfind("transfer", 0) == 0 && items.size() > 1) {
            // "transfer" / "transferCopy": the items after the first into the first (a desktop drag)
            std::vector<Item> moved(items.begin() + 1, items.end());
            transfer(moved, items.front().path, cmd == "transferCopy");
        } else if (cmd == "undo:") {
            if (s_undo.undo) {
                auto u = std::move(s_undo.undo);
                s_undo = {};
                u();
            }
        } else if (cmd == "cmdOpen:") {
            for (Item item : items) {
                char real[4096];
                if (realpath(item.path.c_str(), real)) {
                    item.path = real;
                    item.folder = isDir(real);
                }
                if (item.folder) {
                    if (openWindow)
                        openWindow(item.path);
                } else {
                    openDocument(item);
                }
            }
        } else if (cmd == "cmdDesktopViewOptions:") {
            // View > Show View Options on the desktop: the icon view's page, the desktop's settings
            // (com.apple.finder's DesktopViewSettings: finder.conf's Desktop keys, the desktop reads them)
            Settings conf("finder");
            ViewSettings d;
            d.folderName = "Desktop";
            d.desktop = true;
            d.view = 0;
            d.iconSize = conf.integer("DesktopIconSize", 64);
            d.gridSpacing = conf.integer("DesktopGridSpacing", 54);
            d.iconTextSize = conf.integer("DesktopTextSize", 12);
            d.labelOnBottom = conf.boolean("DesktopLabelOnBottom", true);
            d.showItemInfo = conf.boolean("DesktopShowItemInfo", false);
            d.iconPreview = conf.boolean("DesktopShowIconPreview", true);
            d.arrangeBy = conf.string("DesktopArrangeBy", "none");
            s_front = nullptr;
            ViewOptions &panel = ViewOptions::shared();
            panel.onChange = [](const ViewSettings &v) {
                Settings conf("finder");
                conf.set("DesktopIconSize", v.iconSize);
                conf.set("DesktopGridSpacing", v.gridSpacing);
                conf.set("DesktopTextSize", v.iconTextSize);
                conf.set("DesktopLabelOnBottom", v.labelOnBottom);
                conf.set("DesktopShowItemInfo", v.showItemInfo);
                conf.set("DesktopShowIconPreview", v.iconPreview);
                conf.set("DesktopArrangeBy", v.arrangeBy);
                ViewOptions::shared().update(v);
            };
            panel.onUseAsDefaults = {};
            if (panel.isOpen())
                panel.update(d);
            else
                panel.toggle(d);
        } else if (cmd == "cmdShowOriginal:" && items.size() == 1) {
            char real[4096];
            if (realpath(items.front().path.c_str(), real) && openWindow) {
                const std::string original = real;
                if (BrowserWindow *w = openWindow(dirName(original).empty() ? "/" : dirName(original)))
                    w->selectPaths({original});
            }
        }
    }

    // where the desktop is told of items dropped on it (its socket, the Qt desktop's)
    static void tellDesktop(const std::string &line)
    {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        const char *display = std::getenv("DISPLAY");
        std::string d = display ? display : "";
        for (char &c : d)
            if (c == '/' || c == ':')
                c = '_';
        (void)runtime;
        // the desktop's abstract socket ("snowleopard-desktop-<uid>-<display>", sun_path[0] = 0)
        const std::string name = "snowleopard-desktop-" + std::to_string(getuid()) + "-" + d;
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const size_t n = std::min(name.size(), sizeof addr.sun_path - 1);
        std::memcpy(addr.sun_path + 1, name.data(), n);
        const socklen_t length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), length) == 0) {
            const std::string msg = line + "\n";
            if (write(fd, msg.data(), msg.size()) < 0) {
            }
        }
        close(fd);
    }

    // Go's places (TGlobalWindowController -cmdGoTo...:)
    static std::string placeFor(const std::string &cmd)
    {
        const std::string h = home();
        if (cmd == "cmdGoToComputer:")
            return ComputerFolder;
        if (cmd == "cmdGoHome:")
            return h;
        if (cmd == "cmdGoToDesktop:")
            return userDir("DESKTOP", h + "/Desktop");
        if (cmd == "cmdGoToDocuments:")
            return userDir("DOCUMENTS", h + "/Documents");
        if (cmd == "cmdGoToApplications:")
            return "/usr/share/applications";
        if (cmd == "cmdGoToNetwork:") {
            const std::string gvfs = "/run/user/" + std::to_string(getuid()) + "/gvfs";
            return isDir(gvfs) ? gvfs : std::string();
        }
        return {};
    }

    // File > New Finder Window: Preferences' "New Finder windows open" (NewWindowTarget)
    static void newWindowAtTarget()
    {
        Settings conf("finder");
        const std::string t = conf.string("NewWindowTarget", "PfHm"), h = home();
        std::string folder = h;
        if (t == "PfCm")
            folder = ComputerFolder;
        else if (t == "PfVo")
            folder = "/";
        else if (t == "PfDe")
            folder = userDir("DESKTOP", h + "/Desktop");
        else if (t == "PfDo")
            folder = userDir("DOCUMENTS", h + "/Documents");
        else if (t == "PfLo" && isDir(conf.string("NewWindowTargetPath", h)))
            folder = conf.string("NewWindowTargetPath", h);
        if (openWindow)
            openWindow(folder);
    }

    // a disk, a mounted server: GIO unmounts it (and ejects a removable one), the sidebars follow
    static bool isEjectable(const std::string &path)
    {
        for (const Volume &v : volumes())
            if (v.path == path)
                return v.kind != Volume::Internal;
        return path.rfind("/run/user/", 0) == 0 && path.find("/gvfs/") != std::string::npos;
    }
    static void eject(const std::string &path)
    {
        if (!isEjectable(path))
            return;
        spawn({"gio", "mount", "-e", path});
        for (double t : {1.0, 3.0})
            App::shared().addTimer(t, false, [] {
                reloadSidebars();
                for (BrowserWindow *w : s_windows)
                    if (w->m_folder == ComputerFolder || !isDir(w->m_folder))
                        w->go(w->m_folder == ComputerFolder ? ComputerFolder : home());
            });
    }
    static void reloadSidebars()
    {
        for (BrowserWindow *w : s_windows) {
            w->m_sidebar->reload();
            w->m_sidebar->selectPath(w->m_folder);
        }
    }

    // after a file operation: the new items selected in the window it was made from, its undo kept
    struct Pending {
        std::shared_ptr<Operation> op;
        std::function<void(const Operation &)> done;
    };
    static inline std::vector<Pending> s_pending;
    static void follow(std::shared_ptr<Operation> op, std::function<void(const Operation &)> done)
    {
        s_pending.push_back({op, std::move(done)});
        Progress::follow(op);
    }
    static bool idle() { return s_pending.empty() && !s_undo.undo; }
    static void finished(const Operation &op)
    {
        for (auto it = s_pending.begin(); it != s_pending.end(); ++it)
            if (it->op.get() == &op) {
                auto done = std::move(it->done);
                s_pending.erase(it);
                if (done)
                    done(op);
                break;
            }
        for (BrowserWindow *w : s_windows)
            w->writeState();
    }
    // the windows showing these folders list them again
    static void refreshFolders(const std::vector<std::string> &folders)
    {
        for (BrowserWindow *w : s_windows)
            for (const std::string &f : folders)
                w->refresh(f);
    }
    BrowserWindow *windowShowing(const std::string &folder)
    {
        if (m_folder == folder)
            return this;
        for (BrowserWindow *w : s_windows)
            if (w->m_folder == folder)
                return w;
        return nullptr;
    }
    // undo by putting items in the Trash (a copy, a duplicate, an alias, a new folder)
    static void undoByTrashing(const std::string &title, const std::vector<std::string> &made)
    {
        if (made.empty())
            return;
        s_undo = {title, [made] { Progress::follow(Operation::start(Operation::Trash, made)); }};
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
        // Computer has no .DS_Store: the user's defaults
        m_store = std::make_unique<DSStore::Store>(m_folder == ComputerFolder ? std::string("/nonexistent-computer") : m_folder);
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
        if (const PList *shown = bwsp.get("ShowStatusBar"))
            m_statusShown = shown->b;
        if (const PList *shown = bwsp.get("ShowPathbar"))
            m_pathBarShown = shown->b;
        if (const PList *shown = bwsp.get("ShowSidebar"); shown && !shown->b && m_toolbarShown && m_sidebarWidth > 0) {
            m_hiddenSidebarWidth = m_sidebarWidth;
            m_sidebarWidth = 0;
        }
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
        bwsp.set("ShowStatusBar", PList::boolean(m_statusShown));
        bwsp.set("ShowPathbar", PList::boolean(m_pathBarShown));
        storePList("bwsp", bwsp);
    }

    void show()
    {
        setTitle(m_folder);
        m_pathBar->folder = m_folder;
        m_pathBar->setNeedsDisplay();
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
        m_quickLook->action = [this] { perform("cmdQuickLookFromToolbar:"); };
        m_toolbar->addItem("com.apple.finder.QUIK", core("TL14", "Quick Look"), m_quickLook, 40);

        auto *action = new TemplateButton(CoreUI::named("Action"), true);
        action->action = [this, action] { showActionMenu(action); };
        m_toolbar->addItem("com.apple.finder.ACTN", core("TL26", "Action"), action, 40);

        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);
        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);

        auto *search = new SearchField();
        search->setFrame({0, 0, 112, 22});
        // typing searches after a moment (TSearchToolbarItem), Return at once; emptied, the folder again
        m_searchField = search;
        search->onChange = [this](const std::string &text) {
            if (m_searchDelay)
                App::shared().removeTimer(m_searchDelay);
            m_searchDelay = App::shared().addTimer(0.4, false, [this, text] {
                m_searchDelay = 0;
                if (text.empty())
                    endSearch(true);
                else
                    startSearch(text, {}, {});
            });
        };
        search->onEnter = [this, search] {
            if (m_searchDelay) {
                App::shared().removeTimer(m_searchDelay);
                m_searchDelay = 0;
            }
            if (!search->text.empty() && (!m_search || m_searchText != search->text))
                startSearch(search->text, {}, {});
        };
        m_toolbar->addItem("com.apple.finder.SRCH", core("TL23", "Search"), search, 60, 250);
    }

    void updateToolbar()
    {
        m_backForward->segmentEnabled = {!m_back.empty(), !m_forward.empty()};
        m_backForward->setNeedsDisplay();
    }

    void showActionMenu(View *button)
    {
        // TContextMenu -populateActionMenu:forWindowController: the selection's menu, else the window's
        const Point at = button->toWindow({0, button->frame().h + 2});
        showContextMenu(at, selectedItems().empty() ? 2 : 1);
    }

    void showOldActionMenu(View *button)
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

    // ---- opening: folders here (or in a window of their own: Preferences' "Always open folders in a
    // new window", a window without its toolbar, Command-Control), aliases to their original, the rest
    // to their applications

    void openItems(const std::vector<Item> &items, bool newWindow)
    {
        const bool own = newWindow || !m_toolbarShown || Settings("finder").boolean("AlwaysOpenFoldersInNewWindow", false);
        for (Item item : items) {
            struct stat st;
            if (lstat(item.path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
                // an alias: its original ("The alias “^0” can’t be opened because the original item can’t be found." N6)
                char real[4096];
                if (!realpath(item.path.c_str(), real)) {
                    alert(format(tr("N6", "The alias “^0” can’t be opened because the original item can’t be found."), {item.name}), {},
                          {tr("AL4", "OK")}, {});
                    continue;
                }
                item.path = real;
                item.folder = isDir(real);
            }
            if (item.folder) {
                if (own && openWindow)
                    openWindow(item.path);
                else if (items.size() == 1)
                    go(item.path);
                else if (openWindow)
                    openWindow(item.path);
            } else {
                openDocument(item);
            }
        }
    }

    void openWith(const Item &item, const Application &app)
    {
        spawn({"gio", "launch", app.file, item.path});
    }

    // ---- an alert sheet (NSAlert's: the Finder's icon, the message in bold, the information, the
    // buttons from the right, the first the default one)
    void alert(const std::string &message, const std::string &info, const std::vector<std::string> &buttons, std::function<void(int)> done)
    {
        if (Sheet::isOpen(m_window.get()))
            return;
        const double width = 420;
        const double textW = width - 104 - 20;
        const double mh = std::max(17.0, wrappedHeight(message, textW, Font::boldSystem(13)));
        const double ih = info.empty() ? 0 : wrappedHeight(info, textW, Font::system(11)) + 8;
        const Size size{width, std::max(126.0, 15 + mh + ih + 20 + 32 + 20)};
        auto *content = new View;
        content->setFrame({0, 0, size.w, size.h});
        auto *iconView = new ImageView;
        iconView->image = Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/Finder.icns"), {64, 64});
        iconView->setFrame({24, 15, 64, 64});
        content->addSubview(iconView);
        auto *m = new Label(message, Font::boldSystem(13));
        m->wrap = true;
        m->setFrame({104, 15, textW, mh});
        content->addSubview(m);
        if (!info.empty()) {
            auto *i = new Label(info, Font::system(11));
            i->wrap = true;
            i->setFrame({104, 15 + mh + 8, textW, ih});
            content->addSubview(i);
        }
        double right = size.w - 20;
        for (size_t b = 0; b < buttons.size(); ++b) {
            const double w = std::max(82.0, std::ceil(textWidth(buttons[b], Font::system(13))) + 32);
            auto *button = new Button(buttons[b]);
            button->isDefault = b == 0;
            button->setFrame({right - w, size.h - 20 - 32, w, 32});
            button->action = [this, b, done] {
                Sheet::end(m_window.get());
                if (done)
                    done(int(b));
            };
            content->addSubview(button);
            right -= w + 6;
        }
        Sheet::begin(m_window.get(), content, size, m_toolbarShown ? m_toolbar->height() : 0);
    }

    // ---- renaming in place (Return, or a click on the selected item's name): the name selected
    // without its extension; Return or a click elsewhere renames, Escape gives up

    void scheduleRename(const Item &item)
    {
        if (m_renameTimer)
            App::shared().removeTimer(m_renameTimer);
        m_renameTimer = App::shared().addTimer(0.7, false, [this, item] {
            m_renameTimer = 0;
            const std::vector<Item> sel = selectedItems();
            if (sel.size() == 1 && sel.front().path == item.path)
                startRename(item);
        });
    }

    void startRename(const Item &item)
    {
        cancelRename();
        if (inTrash() || m_folder == ComputerFolder || access(dirName(item.path).c_str(), W_OK) != 0)
            return;
        Rect r = m_view == 0   ? m_icons->nameRectInWindow(item.path)
                 : m_view == 1 ? m_list->nameRectInWindow(item.path)
                 : m_view == 2 ? m_columnsView->nameRectInWindow(item.path)
                               : m_flowView->list()->nameRectInWindow(item.path);
        if (r.empty())
            return;
        const int textSize = m_view == 0 ? m_settings.iconTextSize : m_view == 2 ? m_settings.columnTextSize : m_settings.listTextSize;
        auto *field = new RenameField;
        field->font = Font::system(textSize);
        const double w = std::max(r.w, std::ceil(textWidth(item.name, field->font)) + 16);
        if (m_view == 0 && m_settings.labelOnBottom)
            r.x -= (w - r.w) / 2;   // centred under the icon
        r.w = std::min(w, m_window->size().w - r.x - 4);
        field->setFrame({std::round(r.x), std::round(r.y), std::round(r.w), std::max(r.h, textSize + 6.0)});
        field->align = m_view == 0 && m_settings.labelOnBottom ? Align::Center : Align::Left;
        m_window->contentView()->addSubview(field);
        field->setText(item.name);
        m_renameField = field;
        m_renameItem = item;
        field->onEnter = [this] { endRename(true); };
        field->onCancel = [this] { endRename(false); };
        m_window->makeFirstResponder(field);
        // the name without its extension (a folder's whole name)
        const size_t dot = item.name.rfind('.');
        field->selectRange(0, !item.folder && dot != std::string::npos && dot > 0 ? dot : item.name.size());
    }

    void cancelRename()
    {
        if (m_renameTimer) {
            App::shared().removeTimer(m_renameTimer);
            m_renameTimer = 0;
        }
        if (m_renameField)
            endRename(false);
    }

    void endRename(bool commit)
    {
        TextField *field = m_renameField;
        if (!field)
            return;
        m_renameField = nullptr;
        const std::string typed = field->text;
        field->onEnter = nullptr;
        field->onCancel = nullptr;
        field->setHidden(true);
        View *root = m_window->contentView();
        App::shared().later([root, field] { root->removeSubview(field); });
        focusView();
        if (commit)
            renameItem(m_renameItem, typed);
    }

    void renameItem(const Item &item, std::string name, bool checked = false)
    {
        // a "/" is a ":" on the disk (as HFS+ shows it); spaces at the ends kept, as the Finder does
        std::replace(name.begin(), name.end(), '/', ':');
        if (name.empty() || name == item.name)
            return;
        if (name[0] == '.') {
            alert(tr("RN4", "You can’t use a name that begins with a dot “.”, because these names are reserved for the system. Please choose another name."),
                  {}, {tr("AL4", "OK")}, {});
            return;
        }
        if (name.size() > 255) {
            alert(format(tr("RN6", "The name “^0” can’t be used because it has too many characters."), {name}), {}, {tr("AL4", "OK")}, {});
            return;
        }
        const std::string folder = dirName(item.path).empty() ? "/" : dirName(item.path);
        const std::string to = folder == "/" ? "/" + name : folder + "/" + name;
        struct stat st;
        if (lstat(to.c_str(), &st) == 0 && strcasecmp(name.c_str(), item.name.c_str()) != 0) {
            alert(format(tr("RN17", "The name “^0” is already taken. Please choose a different name."), {name}), {}, {tr("AL4", "OK")}, {});
            return;
        }
        // the extension changed: asked first (FXEnableExtensionChangeWarning; RN12, RN19, RN20)
        auto ext = [](const std::string &n) {
            const size_t d = n.rfind('.');
            return d == std::string::npos || d == 0 ? std::string() : n.substr(d + 1);
        };
        const std::string before = ext(item.name), after = ext(name);
        if (!checked && !item.folder && before != after && Settings("finder").boolean("FXEnableExtensionChangeWarning", true)) {
            std::string message, info = tr("RN13", "If you make this change, your document may open in a different application.");
            std::vector<std::string> buttons;
            if (!before.empty() && !after.empty()) {
                message = format(tr("RN12", "Are you sure you want to change the extension from “^0” to “^1”?"), {"." + before, "." + after});
                buttons = {format(tr("RN20", "Keep ^0"), {"." + before}), format(tr("RN19", "Use ^0"), {"." + after})};
            } else if (after.empty()) {
                message = format(tr("RN14", "Are you sure you want to remove the extension “^0”?"), {"." + before});
                buttons = {format(tr("RN20", "Keep ^0"), {"." + before}), tr("RN24", "Remove")};
            } else {
                message = format(tr("RN8", "Are you sure you want to add the extension “^0” to the end of the name?"), {"." + after});
                buttons = {tr("RN22", "Don’t Add"), tr("RN21", "Add")};
            }
            alert(message, info, buttons, [this, item, name](int b) {
                if (b == 1)
                    renameItem(item, name, true);
            });
            return;
        }
        if (std::rename(item.path.c_str(), to.c_str()) != 0)
            return;
        // what the folder's .DS_Store kept under the old name follows it (its place, its comments)
        if (BrowserWindow *w = windowShowing(folder); w && w->m_store) {
            for (const char *code : {"Iloc", "cmmt"})
                if (const DSStore::Record *rec = w->m_store->get(item.name, code)) {
                    DSStore::Record moved = *rec;
                    moved.name = name;
                    w->m_store->remove(item.name, code);
                    w->m_store->set(moved);
                }
            w->m_store->save();
        }
        Labels::forget(item.path);
        const Item renamed{name, to, item.folder, item.application};
        s_undo = {tr("RN2", "Undo Rename"), [renamed, item] {
                      if (std::rename(renamed.path.c_str(), item.path.c_str()) == 0)
                          refreshFolders({dirName(item.path).empty() ? "/" : dirName(item.path)});
                  }};
        refreshFolders({folder});
        selectPaths({to});
    }

    // the view with the keyboard
    void focusView()
    {
        if (m_view == 0)
            m_window->makeFirstResponder(m_icons);
        else if (m_view == 1)
            m_list->focus();
        else if (m_view == 2)
            m_window->makeFirstResponder(m_columnsView);
        else
            m_flowView->focus();
    }

    // ---- the contextual menu (TContextMenu): the table's entries for the context, the ones that
    // don't apply left out, the separators condensed

    void showContextMenu(Point at, unsigned context)
    {
        cancelRename();
        s_front = this;
        const std::vector<Item> sel = context == 1 ? selectedItems() : std::vector<Item>{};
        std::vector<MenuItem> items;
        std::vector<std::function<void(int)>> actions;   // the subitem chosen, -1 the item itself
        auto add = [&](MenuItem item, std::function<void(int)> f) {
            items.push_back(std::move(item));
            actions.push_back(std::move(f));
        };
        auto line = [&] {
            if (!items.empty() && !items.back().separator)
                add(MenuItem::line(), {});
        };
        auto cmd = [this](const std::string &c) { return [this, c](int) { perform(c); }; };
        std::vector<Application> apps;
        for (const ContextEntry &e : ContextTable) {
            if (!e.command) {
                line();   // the table's separators are in every context (-_setCondensesSeparators: condensed)
                continue;
            }
            if (!(e.mask & context))
                continue;
            const std::string c = e.command;
            if (c == "placeholderViewSpecificStuff") {
                // TIconViewController: Clean Up, Arrange By ▸ (the window's background only)
                if (context == 2 && m_view == 0 && !m_search) {
                    add({tr("N169.3", "Clean Up")}, cmd("cmdCleanUp:"));
                    MenuItem arrangeMenu{tr("N148", "Arrange By")};
                    static const char *const by[][2] = {{"None", "none"}, {"Name", "name"}, {"Date Modified", "dateModified"},
                                                        {"Date Created", "dateCreated"}, {"Size", "size"}, {"Kind", "kind"}, {"Label", "label"}};
                    for (const auto &b : by)
                        arrangeMenu.submenu.push_back(MenuItem(b[0], false, true, m_settings.arrangeBy == b[1]));
                    add(arrangeMenu, [this](int sub) {
                        static const char *const keys[] = {"none", "name", "dateModified", "dateCreated", "size", "kind", "label"};
                        if (sub >= 0 && sub < 7)
                            arrange(keys[sub], true);
                    });
                }
                continue;
            }
            if (c == "cmdViewAsSubmenu:") {
                if (!m_toolbarShown) {
                    MenuItem view{"View"};
                    static const char *const v[] = {"as Icons", "as List", "as Columns", "as Cover Flow"};
                    for (int k = 0; k < 4; ++k)
                        view.submenu.push_back(MenuItem(v[k], false, true, m_view == k));
                    add(view, [this](int sub) {
                        if (sub >= 0)
                            setView(sub);
                    });
                }
                continue;
            }
            if (c == "cmdOpenWith:") {
                if (sel.size() != 1 || sel.front().folder || sel.front().application)
                    continue;
                apps = applicationsFor(sel.front().path);
                if (apps.empty())
                    continue;
                MenuItem with{tr("N152", "Open With")};
                for (size_t k = 0; k < apps.size(); ++k)
                    with.submenu.push_back(MenuItem(k == 0 ? apps[k].name + " (default)" : apps[k].name));
                const Item item = sel.front();
                add(with, [this, apps, item](int sub) {
                    if (sub >= 0 && size_t(sub) < apps.size())
                        openWith(item, apps[size_t(sub)]);
                });
                continue;
            }
            if (c == "cmdSetLabel:") {
                if (sel.empty() || inTrash())
                    continue;
                line();
                MenuItem label{core("LB1", "Label:")};
                label.swatches.push_back({});
                const int current = Labels::get(sel.front().path);
                for (int k = 0; k < 7; ++k) {
                    label.swatches.push_back(Labels::color(Labels::Order[k]));
                    if (Labels::Order[k] == current)
                        label.swatchChecked = k + 1;
                }
                if (current == 0)
                    label.swatchChecked = 0;
                add(label, [this](int sub) {
                    if (sub >= 0)
                        setLabel(sub == 0 ? 0 : Labels::Order[sub - 1]);
                });
                continue;
            }
            if (c == "cmdRemoveFromSidebar:" || c == "cmdShowSidebarPrefs:")
                continue;   // the sidebar's menu
            if (c == "cmdEmptyTrashSecure:" && !Settings("finder").boolean("EmptyTrashSecurely", false))
                continue;
            if (c == "cmdEmptyTrash:" && (!inTrash() || context != 2))
                continue;
            if (c == "cmdEmptyTrashSecure:" && (!inTrash() || context != 2))
                continue;
            if (c == "cmdEmptyTrash:" && Settings("finder").boolean("EmptyTrashSecurely", false))
                continue;
            if (c == "cmdOpenEnclosingFolder:" && !m_search)
                continue;
            if ((c == "cmdShowOriginal:" || c == "cmdEject:" || c == "cmdChangeDesktopBackground:") && !validate(c))
                continue;
            if (c == "paste:" && s_clipboard.empty())
                continue;
            if (context == 1 && (c == "cmdArchive:" || c == "cmdDuplicate:" || c == "cmdMakeAlias:") && inTrash())
                continue;
            std::string t = title(c);
            if (t.empty()) {
                static const std::map<std::string, std::pair<const char *, const char *>> keys = {
                    {"cmdNewFolderAtMouseLocation:", {"N156", "New Folder"}}, {"cmdOpen:", {"N151", "Open"}},
                    {"cmdShowOriginal:", {"N163", "Show Original"}},          {"cmdOpenEnclosingFolder:", {"N162", "Open Enclosing Folder"}},
                    {"cmdEmptyTrash:", {"N157", "Empty Trash"}},              {"cmdEmptyTrashSecure:", {"N157.1", "Secure Empty Trash"}},
                    {"cmdShowStaticInfo:", {"N165", "Get Info"}},             {"cmdDuplicate:", {"N154", "Duplicate"}},
                    {"cmdMakeAlias:", {"N155", "Make Alias"}},                {"cmdChangeDesktopBackground:", {"N164", "Change Desktop Background…"}}};
                auto it = keys.find(c);
                t = it == keys.end() ? c : (c == "cmdShowOriginal:" ? core(it->second.first, it->second.second) : tr(it->second.first, it->second.second));
            }
            add(MenuItem(t, false, validate(c)), cmd(c));
        }
        while (!items.empty() && items.back().separator) {
            items.pop_back();
            actions.pop_back();
        }
        if (items.empty())
            return;
        Menu::popUp(m_window.get(), at, items, 0, Font::system(14), [actions](int choice) {
            int item, sub;
            Menu::split(choice, item, sub);
            if (item >= 0 && size_t(item) < actions.size() && actions[size_t(item)])
                actions[size_t(item)](sub);
        });
    }

    // a sidebar item's menu (contexts 8): Open, Get Info, Eject, Remove from Sidebar, Open Sidebar Preferences…
    void showSidebarMenu(const Sidebar::Row &r, Point at)
    {
        std::vector<MenuItem> items;
        std::vector<std::function<void()>> actions;
        const std::string path = r.path;
        const bool search = path.rfind("search:", 0) == 0;
        if (!search) {
            items.push_back({tr("N151", "Open")});
            actions.push_back([this, path] { go(path); });
            items.push_back(MenuItem::line());
            actions.push_back({});
            items.push_back({tr("N165", "Get Info")});
            actions.push_back([path] { InfoWindow::show(Item{displayName(path), path, isDir(path)}); });
        }
        if (r.ejectable) {
            items.push_back({format(core("N57", "Eject “^0”"), {r.title})});
            actions.push_back([path] { eject(path); });
        }
        if (r.added) {
            items.push_back(MenuItem::line());
            actions.push_back({});
            items.push_back({tr("N169.2", "Remove from Sidebar")});
            actions.push_back([path] {
                Sidebar::remove(path);
                reloadSidebars();
            });
        }
        items.push_back(MenuItem::line());
        actions.push_back({});
        items.push_back({tr("N169.2.1", "Open Sidebar Preferences…")});
        actions.push_back([] { Preferences::show(2); });
        Menu::popUp(m_window.get(), at, items, 0, Font::system(14), [actions](int i) {
            if (i >= 0 && size_t(i) < actions.size() && actions[size_t(i)])
                actions[size_t(i)]();
        });
    }

    // ---- File's commands

    void makeAlias()
    {
        // "notes.txt alias" (N3) beside the original, a symbolic link here; selected after
        std::vector<std::string> made;
        for (const Item &i : selectedItems()) {
            const std::string folder = dirName(i.path).empty() ? "/" : dirName(i.path);
            const std::string name = Operation::freeName(folder, format(tr("N3", "^0 alias"), {i.name}), true);
            const std::string to = folder == "/" ? "/" + name : folder + "/" + name;
            if (symlink(i.path.c_str(), to.c_str()) == 0)
                made.push_back(to);
            else
                alert(format(tr("N23", "An alias to the item “^0” can’t be made."), {i.name}), {}, {tr("AL4", "OK")}, {});
        }
        if (made.empty())
            return;
        undoByTrashing("Undo Make Alias", made);
        refreshFolders({m_folder});
        selectPaths(made);
    }

    void showOriginal()
    {
        const std::vector<Item> sel = selectedItems();
        if (sel.size() != 1)
            return;
        char real[4096];
        if (!realpath(sel.front().path.c_str(), real)) {
            alert(format(tr("N6", "The alias “^0” can’t be opened because the original item can’t be found."), {sel.front().name}), {},
                  {tr("AL4", "OK")}, {});
            return;
        }
        const std::string original = real, folder = dirName(original).empty() ? "/" : dirName(original);
        BrowserWindow *w = windowShowing(folder);
        if (!w && openWindow)
            w = openWindow(folder);
        if (w) {
            w->m_window->show();
            w->selectPaths({original});
        }
    }

    void compress()
    {
        const std::vector<Item> sel = selectedItems();
        if (sel.empty() || inTrash())
            return;
        const std::string folder = dirName(sel.front().path).empty() ? "/" : dirName(sel.front().path);
        follow(Operation::start(Operation::Compress, paths(sel), folder), [this](const Operation &op) {
            const std::vector<std::string> made = op.results();
            undoByTrashing("Undo Compress", made);
            if (std::find(s_windows.begin(), s_windows.end(), this) != s_windows.end())
                selectPaths(made);
        });
    }

    void putBack()
    {
        const std::vector<Item> sel = selectedItems();
        if (sel.empty())
            return;
        follow(Operation::start(Operation::PutBack, paths(sel)), {});
    }

    void setLabel(int index)
    {
        for (const Item &i : selectedItems())
            Labels::set(i.path, index);
        for (BrowserWindow *w : s_windows) {
            w->m_icons->setNeedsDisplay();
            w->m_list->setNeedsDisplay();
            w->m_flowView->setNeedsDisplay();
        }
        refreshFolders({m_folder});
    }

    // ---- View's: Clean Up, Arrange By (once), Keep Arranged By (the folder's icvp arrangeBy)

    void cleanUp(bool selectionOnly)
    {
        if (m_view != 0)
            return;
        placeIcons(m_icons->cleanUp(selectionOnly));
    }

    void arrange(const std::string &by, bool keep)
    {
        if (m_view == 1 || m_view == 3) {
            // the list: sorted by that column
            ListView *l = m_view == 1 ? m_list : m_flowView->list();
            if (by != "none" && by != "label") {
                l->setSort(by, by.rfind("date", 0) != 0 && by != "size");
                if (l->onSortChanged)
                    l->onSortChanged();
            }
            return;
        }
        if (m_view != 0)
            return;
        if (keep) {
            ViewSettings s = m_settings;
            s.arrangeBy = m_settings.arrangeBy == by && by != "none" ? "none" : by;   // chosen again: no longer kept
            settingsChanged(s);
            return;
        }
        if (m_settings.arrangeBy != "none" && m_settings.arrangeBy != "grid") {
            ViewSettings s = m_settings;
            s.arrangeBy = "none";
            settingsChanged(s);
        }
        placeIcons(m_icons->arrangedPlaces(by));
    }

    void placeIcons(const std::vector<std::pair<std::string, Point>> &places)
    {
        if (places.empty())
            return;
        if (m_icons->onMoved)
            m_icons->onMoved(places);
        std::map<std::string, Point> all;
        for (const Item &item : m_icons->items())
            if (const DSStore::Record *r = m_store ? m_store->get(item.name, "Iloc") : nullptr) {
                int x, y;
                if (DSStore::iconLocation(*r, x, y))
                    all[item.name] = {double(x), double(y)};
            }
        for (const auto &p : places)
            all[p.first] = p.second;
        m_icons->setPositions(std::move(all));
        layoutIcons();
    }

    // ---- searching: the results in the list (or the icons, Cover Flow), the window's title the search's

    void startSearch(const std::string &text, const std::string &canned, const std::string &cannedTitle)
    {
        endSearch(false);
        cancelRename();
        std::string scope;
        if (canned.empty() && Settings("finder").string("FXDefaultSearchScope", "SCev") == "SCcf" && m_folder != ComputerFolder)
            scope = m_folder;
        m_search = Search::start(text, canned, scope);
        m_searchText = text;
        m_searchItems.clear();
        m_searchTitle = !canned.empty() ? cannedTitle
                                        : format(tr("FF31", "Searching “^0”"), {scope.empty() ? tr("FF22", "This Mac") : displayName(scope)});
        if (m_view == 2)
            setView(1);
        m_window->setTitle(m_searchTitle);
        m_window->setRepresentedIcon(coreTypesIcon("SmartFolderIcon", 16));
        if (!canned.empty())
            m_sidebar->selectPath("search:" + canned);
        else
            m_sidebar->selectPath({});
        showSearchItems();
        m_searchTimer = App::shared().addTimer(0.3, true, [this] {
            if (!m_search)
                return;
            std::vector<Item> found = m_search->take();
            const bool done = m_search->finished();
            if (!found.empty()) {
                m_searchItems.insert(m_searchItems.end(), found.begin(), found.end());
                showSearchItems();
            }
            if (done && m_searchTimer) {
                App::shared().removeTimer(m_searchTimer);
                m_searchTimer = 0;
            }
        });
    }

    void showSearchItems()
    {
        const std::vector<Item> sel = selectedItems();
        m_list->setItems(m_searchItems);
        m_flowView->setItems(m_searchItems);
        m_icons->setPositions({});
        m_icons->setItems(m_searchItems);
        selectPaths(paths(sel));
        layoutIcons();
        updateStatus();
    }

    void endSearch(bool restore)
    {
        if (m_searchTimer) {
            App::shared().removeTimer(m_searchTimer);
            m_searchTimer = 0;
        }
        if (!m_search)
            return;
        m_search->cancel();
        m_search.reset();
        m_searchItems.clear();
        if (m_searchField && restore)
            m_searchField->setText({});
        if (restore)
            show();
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
        if (folder == ComputerFolder)
            return;
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
        if (folder == Operation::trashFiles() && !copy) {
            follow(Operation::start(Operation::Trash, sources), [](const Operation &op) {
                const std::vector<std::string> trashed = op.results();
                if (!trashed.empty())
                    s_undo = {"Undo Move to Trash", [trashed] { follow(Operation::start(Operation::PutBack, trashed), {}); }};
            });
        } else if (copy) {
            follow(Operation::start(Operation::Copy, sources, folder), [](const Operation &op) {
                undoByTrashing(op.sources().size() == 1 ? format(tr("CP2", "Undo Copy of “^0”"), {fileName(op.sources().front())})
                                                        : format(tr("CP_UNDO_V1", "Undo Copy of ^0 Items"), {std::to_string(op.sources().size())}),
                               op.results());
            });
        } else {
            follow(Operation::start(Operation::Move, sources, folder), [](const Operation &op) {
                // undo: each item back to the folder it came from
                const std::vector<std::string> moved = op.results(), from = op.sources();
                if (moved.empty())
                    return;
                s_undo = {from.size() == 1 ? format(tr("MV2", "Undo Move of “^0”"), {fileName(from.front())})
                                           : format(tr("MV_UNDO_V1", "Undo Move of ^0 Items"), {std::to_string(from.size())}),
                          [moved, from] {
                              std::map<std::string, std::vector<std::string>> back;
                              for (size_t i = 0; i < moved.size() && i < from.size(); ++i)
                                  back[dirName(from[i]).empty() ? "/" : dirName(from[i])].push_back(moved[i]);
                              for (auto &[dir, items] : back)
                                  follow(Operation::start(Operation::Move, items, dir), {});
                          }};
            });
        }
    }

    // where a drop at a point of the screen goes: a Finder window's sidebar place, its folder under
    // the pointer, else the window's own folder
public:
    static std::string dropTarget(Point screen) { return dropTargetAt(screen); }

private:
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
        if (s_clipboard.empty() || inTrash() || m_folder == ComputerFolder || m_search)
            return;
        follow(Operation::start(Operation::Copy, s_clipboard, m_folder), [this](const Operation &op) {
            undoByTrashing(tr("ME16", "Undo Paste"), op.results());
            if (std::find(s_windows.begin(), s_windows.end(), this) != s_windows.end())
                selectPaths(op.results());
        });
    }

    void duplicate()
    {
        const std::vector<Item> sel = selectedItems();
        if (sel.empty() || inTrash())
            return;
        follow(Operation::start(Operation::Duplicate, paths(sel)), [this](const Operation &op) {
            undoByTrashing(op.sources().size() == 1 ? format(tr("DU2", "Undo Duplicate of “^0”"), {fileName(op.sources().front())})
                                                    : format(tr("DU_UNDO_V1", "Undo Duplicate of ^0 Items"), {std::to_string(op.sources().size())}),
                           op.results());
            // the copies selected, as the Finder selects them
            if (std::find(s_windows.begin(), s_windows.end(), this) != s_windows.end())
                selectPaths(op.results());
        });
    }

    void moveToTrash()
    {
        const std::vector<Item> sel = selectedItems();
        if (sel.empty() || inTrash() || m_folder == ComputerFolder)
            return;
        follow(Operation::start(Operation::Trash, paths(sel)), [](const Operation &op) {
            const std::vector<std::string> trashed = op.results();
            if (!trashed.empty())
                s_undo = {"Undo Move to Trash", [trashed] { follow(Operation::start(Operation::PutBack, trashed), {}); }};
        });
    }

    void emptyTrash(bool secure = false, bool ask = true)
    {
        if (list(Operation::trashFiles()).empty())
            return;
        if (!ask) {
            Progress::follow(Operation::start(secure ? Operation::SecureEmptyTrash : Operation::EmptyTrash, {}));
            return;
        }
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
        ok->action = [this, secure] {
            Sheet::end(m_window.get());
            Progress::follow(Operation::start(secure ? Operation::SecureEmptyTrash : Operation::EmptyTrash, {}));
        };
        content->addSubview(ok);
        Sheet::begin(m_window.get(), content, size, m_toolbarShown ? m_toolbar->height() : 0);
    }

    void newFolder()
    {
        // "untitled folder", then "untitled folder 2"... (DesktopServicesPriv's TUniqueNamer), selected
        // and its name ready to type (TBrowserViewController -cmdNewFolder: then the rename)
        if (!validate("cmdNewFolder:"))
            return;
        const std::string name = tr("N2", "untitled folder");
        std::string path = m_folder + "/" + name;
        for (int n = 2; isDir(path) || isFile(path); ++n)
            path = m_folder + "/" + name + " " + std::to_string(n);
        if (mkdir(path.c_str(), 0755) != 0)
            return;
        undoByTrashing(tr("NF2", "Undo New Folder"), {path});
        refreshFolders({m_folder});
        selectPaths({path});
        startRename(Item{fileName(path), path, true});
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
        const long long free = m_search || m_folder == ComputerFolder ? -1 : freeSpace(m_folder);
        m_status->text = free >= 0 ? format(core("SB1", "^0, ^1 available"), {count, formatBytes(free)}) : count;
        m_status->setNeedsDisplay();
        if (s_front == this) {
            QuickLook::follow(selectedItems());
            writeState();
        }
    }

    void layout()
    {
        const Size s = m_window->size();
        const double statusH = m_statusShown ? StatusBar::Height : 0, pathH = m_pathBarShown ? PathBar::Height : 0;
        const double top = m_toolbarShown ? m_toolbar->height() : 0, bottom = s.h - statusH - pathH;
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
        m_pathBar->setHidden(!m_pathBarShown);
        m_pathBar->setFrame({0, bottom, s.w, PathBar::Height});
        m_status->setHidden(!m_statusShown);
        m_status->setFrame({0, bottom + pathH, s.w, StatusBar::Height});
        m_iconSize->setFrame({s.w - IconSizeSlider::Right - IconSizeSlider::Width, bottom + pathH + 1, IconSizeSlider::Width, StatusBar::Height - 1});
        m_iconSize->setHidden(m_view != 0 || !m_statusShown);
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
    PathBar *m_pathBar = nullptr;
    bool m_statusShown = true, m_pathBarShown = false;
    double m_hiddenSidebarWidth = 0;
    SearchField *m_searchField = nullptr;
    int m_searchDelay = 0, m_searchTimer = 0;
    std::shared_ptr<Search> m_search;
    std::string m_searchText, m_searchTitle;
    std::vector<Item> m_searchItems;
    TextField *m_renameField = nullptr;
    Item m_renameItem;
    int m_renameTimer = 0;
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
    // FinderWindows --command <selector>: the menu bar's command for the running Finder (the windows'
    // process), else a window for the ones that open one
    std::vector<std::string> folders, commands;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--command" && i + 1 < argc)
            commands.push_back(std::string("cmd:") + argv[++i]);
        else if (std::string(argv[i]) == "--line" && i + 1 < argc)
            commands.push_back(argv[++i]);   // the desktop's "items:..." and "drop:..."
        else
            folders.push_back(folderToOpen(argv[i]));
    }
    if (folders.empty() && commands.empty())
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
            for (const std::string &c : commands)
                msg += c + '\n';
            const bool sent = write(fd, msg.data(), msg.size()) == ssize_t(msg.size());
            close(fd);
            if (sent)
                return 0;
        }
        if (fd >= 0)
            close(fd);
    }

    setProgramName("Finder");
    FontManager::registerSystemFonts();
    App &app = App::shared();
    if (!app.open()) {
        std::fprintf(stderr, "FinderWindows: no display\n");
        return 1;
    }
    std::vector<std::unique_ptr<BrowserWindow>> windows;
    std::function<BrowserWindow *(const std::string &)> open = [&](const std::string &folder) -> BrowserWindow * {
        auto w = std::make_unique<BrowserWindow>(folder);
        w->onClose = [&](BrowserWindow *closed) {
            app.later([&, closed] {
                windows.erase(std::remove_if(windows.begin(), windows.end(), [closed](const auto &p) { return p.get() == closed; }),
                              windows.end());
                if (windows.empty()) {
                    std::remove(stateFile().c_str());
                    app.quit(0);
                }
            });
        };
        BrowserWindow *raw = w.get();
        windows.push_back(std::move(w));
        return raw;
    };
    BrowserWindow::openWindow = open;
    for (const std::string &f : folders)
        open(f);
    InfoWindow::onRenamed = [&](const std::string &folder) {
        for (auto &w : windows)
            w->refresh(folder);
    };
    // an operation ended: the windows showing its folders (its destination, its items' folders, where
    // they went, the Trash) refresh; then its own end (the new items selected, its undo)
    Progress::onFinished = [&](const Operation &op) {
        std::vector<std::string> folders = {op.destination(), Operation::trashFiles()};
        for (const std::string &s : op.sources())
            folders.push_back(dirName(s).empty() ? "/" : dirName(s));
        for (const std::string &s : op.results())
            folders.push_back(dirName(s).empty() ? "/" : dirName(s));
        for (auto &w : windows)
            for (const std::string &f : folders)
                w->refresh(f);
        BrowserWindow::finished(op);
    };
    // Preferences: the sidebars, the labels' names and colours, the windows redrawn
    Preferences::onChange = [&] {
        BrowserWindow::reloadSidebars();
        for (auto &w : windows)
            w->window()->invalidate();
    };
    // the menu bar's commands ("cmd:<selector>"), for the front window; a folder opens a window
    auto command = [&](const std::string &line) {
        auto fields = [](const std::string &l) {
            std::vector<std::string> out;
            for (size_t start = 0;;) {
                const size_t tab = l.find('\t', start);
                out.push_back(l.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos)
                    break;
                start = tab + 1;
            }
            return out;
        };
        if (line.rfind("items:", 0) == 0) {
            std::vector<std::string> f = fields(line.substr(6));
            const std::string cmd = f.front();
            f.erase(f.begin());
            BrowserWindow::performOnItems(cmd, f);
            return;
        }
        if (line.rfind("drop:", 0) == 0) {
            // the desktop's icons dropped at a point of the screen: a Finder window's folder there
            std::vector<std::string> f = fields(line.substr(5));
            int x = 0, y = 0, option = 0;
            std::sscanf(f.front().c_str(), "%d,%d,%d", &x, &y, &option);
            f.erase(f.begin());
            const std::string target = BrowserWindow::dropTarget({double(x), double(y)});
            if (!target.empty()) {
                f.insert(f.begin(), target);
                BrowserWindow::performOnItems(option ? "transferCopy" : "transfer", f);
            }
            return;
        }
        if (line.rfind("cmd:", 0) != 0) {
            open(folderToOpen(line));
            return;
        }
        const std::string cmd = line.substr(4);
        BrowserWindow *front = BrowserWindow::front();
        if (!front && !windows.empty())
            front = windows.back().get();
        if (front) {
            front->perform(cmd);
            return;
        }
        // no window: the commands that need none
        if (cmd == "cmdNewFileViewer:")
            BrowserWindow::newWindowAtTarget();
        else if (const std::string place = BrowserWindow::placeFor(cmd); !place.empty())
            open(place);
        else if (cmd.rfind("cmdRecentFoldersItem:", 0) == 0)
            open(cmd.substr(21));
        else if (cmd == "cmdPreferences:")
            Preferences::show();
        else if (cmd == "cmdConnectTo:")
            ConnectToServer::show([&](const std::string &f) { open(f); });
        else if (cmd == "cmdGoTo:")
            GoTo::show(nullptr, 0, [&](const std::string &f) { open(f); });
        else if (cmd.rfind("cmdEmptyTrash", 0) == 0)
            Progress::follow(Operation::start(cmd.find("Secure") != std::string::npos || Settings("finder").boolean("EmptyTrashSecurely", false)
                                                  ? Operation::SecureEmptyTrash
                                                  : Operation::EmptyTrash,
                                              {}));
    };
    for (const std::string &c : commands)
        command(c);
    // started for the desktop's items or a panel: gone once nothing of it is left on the screen
    // (a minute without a window, an operation or anything to undo, as the desktop's Edit > Undo
    // needs this process's memory of it)
    app.addTimer(5, true, [&] {
        static int quiet = 0;
        quiet = windows.empty() && app.visibleWindowCount() == 0 && BrowserWindow::idle() ? quiet + 1 : 0;
        if (quiet >= 12) {
            std::remove(stateFile().c_str());
            app.quit(0);
        }
    });

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
                        command(msg.substr(start, end - start));
            }
        });
    }
    const int code = app.run();
    if (server >= 0)
        close(server);
    return code;
}
