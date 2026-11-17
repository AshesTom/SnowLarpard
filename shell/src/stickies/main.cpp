// SPDX-License-Identifier: LGPL-2.1-or-later
// Stickies.app (7.0, 10.6) on AquaKit. Reversed from Stickies' x86_64 slice (not protected): its
// database, its notes' windows, their colours and controls, Arrange, Find, Import and Export
// (docs/reverse-status.md, section 16). Its nibs, strings, default settings and welcome notes are the
// DVD's, read at run time.
//
// A note (StickiesDocument.nib's StickiesWindow, made borderless: -[StickiesWindow
// initWithContentRect:styleMask:backing:defer:] passes no style) is drawn by its views: the drag bar
// along the top (12 points, the window collapsed to 13: floor(13 × userSpaceScaleFactor)), the
// enclosure under it, the text kept at (5, 5) from the bottom, the window's width less 10 and its
// height less 22 (-[StickiesWindow _scrollViewResized:]), the resize box 8 × 8 at the bottom right.
//   Stickies [file...]   files given are imported as notes (Import Text…)
#include "appkit.h"
#include "controls.h"
#include "database.h"
#include "foundation.h"
#include "mainmenu.h"
#include "nib.h"
#include "richtext.h"
#include "text.h"
#include "textview.h"

#include <cairo-pdf.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace AK;

namespace Stickies {

namespace {

const char *const Bundle = "/Applications/Stickies.app";

// The Command keys Stickies' menus answer that WindowServer would otherwise take: M (Collapse
// Window, -[StickiesWindow toggleShade:]) and Q (Quit: everything saved, nothing asked)
const std::vector<uint32_t> OwnKeys = {XKB_KEY_m, XKB_KEY_q};

std::string res(const std::string &name) { return Assets::path(std::string(Bundle) + "/Contents/Resources/" + name); }
std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }

// -[StickiesWindow setColorByIndex:]'s colours (0x100010ae0, 0x60 a colour): the note, its frame
// (stickyHiliteColor), the active drag bar (stickyDragColor), the controls (stickyControlColor)
const int Palettes[6][4][3] = {
    {{254, 244, 156}, {188, 169, 2}, {254, 234, 61}, {219, 197, 3}},        // Yellow
    {{173, 244, 255}, {2, 188, 215}, {137, 240, 255}, {36, 208, 233}},      // Blue
    {{178, 255, 161}, {72, 162, 72}, {131, 254, 131}, {81, 187, 81}},       // Green
    {{255, 199, 199}, {226, 117, 117}, {255, 178, 178}, {248, 127, 127}},   // Pink
    {{182, 202, 255}, {117, 145, 220}, {155, 182, 254}, {125, 155, 235}},   // Purple
    {{238, 238, 238}, {158, 158, 158}, {218, 218, 218}, {168, 168, 168}},   // Gray
};

Color paletteColour(int index, int which)
{
    const int *c = Palettes[std::clamp(index, 0, 5)][which];
    return Color::rgb(c[0], c[1], c[2]);
}

constexpr double CollapsedHeight = 13;   // -[StickiesWindow scaledDragBarHeight]
constexpr double BarHeight = 12;         // StickiesDocument.nib's StickiesDragBar
constexpr double MinWidth = 30;          // -[StickiesWindow awakeFromNib]: setMinSize: 30 wide
constexpr double ResizeTime = 0.15;      // the collapse's animation (AppKit's setFrame:display:animate:)

std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : std::string();
}

bool writeFile(const std::string &path, const std::string &bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(bytes.data(), std::streamsize(bytes.size()));
    return bool(f);
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension(const std::string &path)
{
    const std::string name = fileName(path);
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}

bool inPath(const char *program)
{
    const char *path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos)
            end = dirs.size();
        if (access((dirs.substr(start, end - start) + "/" + program).c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

// A chooser's answer on its standard output, without blocking the windows (as Preview asks for its
// files); `ran` false when the program isn't there
void runDialog(std::vector<std::string> argv, std::function<void(bool ran, std::string)> done)
{
    int out[2];
    if (pipe(out) != 0)
        return done(false, {});
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    std::vector<std::string> env;
    for (char **e = environ; *e; ++e) {
        const std::string v = *e;
        const std::string name = v.substr(0, v.find('='));
        if (name == "QT_QPA_PLATFORM" || name == "QT_QPA_PLATFORMTHEME" || name == "GTK_USE_PORTAL" || name == "GDK_BACKEND" ||
            name == "KDE_FULL_SESSION" || name == "PLASMA_INTEGRATION_USE_PORTAL" || name == "GDK_DEBUG")
            continue;
        env.push_back(v);
    }
    for (const char *v : {"QT_QPA_PLATFORM=xcb", "QT_QPA_PLATFORMTHEME=generic", "PLASMA_INTEGRATION_USE_PORTAL=0", "GTK_USE_PORTAL=0",
                          "GDK_BACKEND=x11", "GDK_DEBUG=no-portals"})
        env.push_back(v);
    std::vector<char *> envp;
    for (std::string &v : env)
        envp.push_back(v.data());
    envp.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        return done(false, {});
    }
    auto answer = std::make_shared<std::string>();
    const int fd = out[0];
    App::shared().watch(fd, [fd, pid, answer, done]() {
        char buf[4096];
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            answer->append(buf, size_t(n));
            return;
        }
        App::shared().unwatch(fd);
        close(fd);
        int status = 0;
        waitpid(pid, &status, 0);
        std::string a = *answer;
        while (!a.empty() && (a.back() == '\n' || a.back() == '\r'))
            a.pop_back();
        // a chooser cancelled answers nothing (kdialog and zenity exit with 1)
        done(true, WIFEXITED(status) && WEXITSTATUS(status) == 0 ? a : std::string());
    });
}

void openURL(const std::string &path)
{
    std::vector<std::string> argv = {"xdg-open", path};
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) == 0)
        App::shared().addTimer(10, false, [pid] { waitpid(pid, nullptr, WNOHANG); });
}

// ---- NSRunAlertPanel: the application's icon, the message in bold, the informative text, the
// buttons from the right (the default one first)

class Alert {
public:
    static void run(const std::string &message, const std::string &informative, std::vector<std::string> buttons,
                    std::function<void(int)> done)
    {
        auto *alert = new Alert(message, informative, std::move(buttons), std::move(done));
        alert->m_window->show();
    }

private:
    Alert(const std::string &message, const std::string &informative, std::vector<std::string> buttons, std::function<void(int)> done)
        : m_done(std::move(done))
    {
        const double width = 420;
        const double textWidth = width - 105 - 20;
        const double mh = std::ceil(wrappedHeight(message, textWidth, Font::boldSystem(13)));
        const double ih = informative.empty() ? 0 : std::ceil(wrappedHeight(informative, textWidth, Font::system(11)));
        const double height = std::max(15 + 64.0, 15 + mh + (ih ? 8 + ih : 0)) + 20 + 32 + 10;
        m_window = std::make_unique<Window>(Size{width, height}, "");
        m_window->setKeyEquivalents(OwnKeys);
        View *c = m_window->contentView();
        auto *icon = c->add<ImageView>();
        icon->image = Image::load(res("Stickies.icns"), {64, 64});
        icon->setFrame({24, 15, 64, 64});
        auto *m = c->add<Label>(message, Font::boldSystem(13));
        m->wrap = true;
        m->setFrame({105, 15, textWidth, mh});
        if (!informative.empty()) {
            auto *i = c->add<Label>(informative, Font::system(11));
            i->wrap = true;
            i->setFrame({105, 15 + mh + 8, textWidth, ih});
        }
        double x = width - 20;
        for (size_t b = 0; b < buttons.size(); ++b) {
            auto *button = c->add<Button>(buttons[b]);
            const double w = std::max(82.0, std::ceil(AK::textWidth(buttons[b], Font::system())) + 28);
            // a third button (Don't Save) at the left
            const double bx = b == 2 ? 105 - 6 : x - w;
            button->setFrame({bx, height - 32 - 10, w, 32});
            button->isDefault = b == 0;
            button->action = [this, b] { finish(int(b)); };
            if (b != 2)
                x -= w;
            if (b == 0)
                m_default = button;
        }
        m_window->onClose = [this] { finish(int(m_buttons) > 1 ? 1 : 0); };
        m_buttons = buttons.size();
        m_window->onUnhandledKey = [this](Event &e) {
            if (e.keysym == XKB_KEY_Return || e.keysym == XKB_KEY_KP_Enter)
                finish(0);
            else if (e.keysym == XKB_KEY_Escape && m_buttons > 1)
                finish(m_buttons > 2 ? 1 : int(m_buttons) - 1);
        };
    }
    void finish(int button)
    {
        if (m_finished)
            return;
        m_finished = true;
        m_window->onClose = nullptr;
        m_window->close();
        auto done = std::move(m_done);
        App::shared().later([this] { delete this; });
        if (done)
            done(button);
    }
    std::unique_ptr<Window> m_window;
    std::function<void(int)> m_done;
    Button *m_default = nullptr;
    size_t m_buttons = 0;
    bool m_finished = false;
};

class Controller;
class StickyNote;

// ---- the note's views

// A MicroButton: the close box (0), the zoom box (1), the collapse box (2), drawn by MicroButtonCell
// in the note's colours, its strokes on whole points (half covering two pixels, as Quartz strokes them)
class MicroButton : public View {
public:
    MicroButton(StickyNote *note, int type) : m_note(note), m_type(type) {}
    std::function<void()> action;
    void draw(cairo_t *cr) override;
    void mouseDown(Event &) override
    {
        m_pressed = m_inside = true;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        const bool in = bounds().contains(e.location);
        if (in != m_inside) {
            m_inside = in;
            setNeedsDisplay();
        }
    }
    void mouseUp(Event &e) override
    {
        m_pressed = false;
        setNeedsDisplay();
        if (bounds().contains(e.location) && action)
            action();
    }

private:
    StickyNote *m_note;
    int m_type;
    bool m_pressed = false, m_inside = false;
};

class DragBar : public View {
public:
    explicit DragBar(StickyNote *note);
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;
    void layoutButtons();
    void update();   // which boxes show: -[StickiesDragBar _windowDidBecomeMainNotification:], shade, unshade
    MicroButton *close, *zoom, *unshade;
    Label *title;

private:
    StickyNote *m_note;
    Point m_from, m_origin;
    bool m_moved = false, m_compositorMove = false;
};

class Enclosure : public View {
public:
    explicit Enclosure(StickyNote *note) : m_note(note) {}
    void draw(cairo_t *cr) override;
    View *hitTest(Point) override { return nullptr; }

private:
    StickyNote *m_note;
};

class ResizeIcon : public View {
public:
    explicit ResizeIcon(StickyNote *note) : m_note(note) {}
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;
    View *hitTest(Point p) override;

private:
    StickyNote *m_note;
    Point m_from;
    Size m_start;
};

// ---- a note: StickiesDocument and its StickiesWindow

class StickyNote {
public:
    StickyNote(Controller &c, const Note &data, bool fromDatabase);
    ~StickyNote();

    Controller &controller() { return m_c; }
    Window *window() const { return m_window.get(); }
    TextView *textView() const { return m_text; }
    bool isMain() const { return m_window->isKey(); }

    // the colours of -[StickiesWindow setColorByIndex:]
    Color noteColour() const { return paletteColour(m_colour, 0); }
    Color hiliteColour() const { return paletteColour(m_colour, 1); }
    Color dragColour() const { return paletteColour(m_colour, 2); }
    Color controlColour() const { return paletteColour(m_colour, 3); }
    int colour() const { return m_colour; }
    void setColour(int index);

    bool isShaded() const { return m_shaded; }
    void shade();
    void unshade();
    void toggleShade() { m_shaded ? unshade() : shade(); }
    void zoom();
    bool isFloating() const { return m_window->isFloating(); }
    void setFloating(bool f);
    bool isTranslucent() const { return m_window->alphaValue() < 1; }
    void setTranslucent(bool t);
    int flags() const { return (m_shaded ? Collapsed : 0) | (isFloating() ? Floating : 0) | (isTranslucent() ? Translucent : 0); }

    std::string title() const;       // -[StickiesWindow title]: the Window menu's
    std::string firstLine() const;   // -[StickiesWindow _firstLineOfText]: the collapsed note's
    bool isEmpty() const { return m_text->text().text.empty(); }
    double created() const { return m_created; }
    double modified() const { return m_modified; }
    Note data() const;               // -[StickiesDocument document]

    // its frame on the screen, Cocoa's (from the main screen's bottom left); the expanded one when collapsed
    Rect cocoaFrame() const;
    Rect screenRect() const;         // top left origin, the window as it is
    void moveTo(Point topLeft);
    void setSize(Size s, bool animate = false);
    Size unshadedSize() const { return m_shaded ? m_unshaded : m_window->size(); }
    void setUnshadedSize(Size s) { m_unshaded = s; }
    void layout();
    void show();
    void performClose();             // the close box, ⌘W: -[StickiesDocument windowShouldClose:]
    void closeNow();
    void exportNote(std::function<void(bool)> done);   // -[StickiesDocument exportSticky:]
    void print();

private:
    void textChanged();
    Controller &m_c;
    std::unique_ptr<Window> m_window;
    DragBar *m_bar = nullptr;
    Enclosure *m_enclosure = nullptr;
    TextView *m_text = nullptr;
    ResizeIcon *m_resize = nullptr;
    int m_colour = 0;
    bool m_shaded = false;
    Size m_unshaded;
    double m_created = 0, m_modified = 0;
    bool m_zoomed = false;
    Rect m_userFrame;
    int m_animation = 0;
    bool m_closing = false;
};

// ---- the Find panel (TextFindController, MainMenu.nib's "Find" panel)

class FindPanel {
public:
    explicit FindPanel(Controller &c) : m_c(c) {}
    void show();
    bool find(bool forward);
    void replace();
    void replaceAndFind()
    {
        replace();
        find(true);
    }
    void replaceAll();
    void useSelection();
    void jumpToSelection();
    std::string findString() const { return m_findField ? m_findField->text : m_findString; }

private:
    void build();
    bool ignoreCase() const { return m_ignoreBox && m_ignoreBox->checked(); }
    bool allNotes() const { return m_scope && m_scope->selected() == 1; }
    void status(const std::string &s)
    {
        if (m_status)
            m_status->setText(s);
    }
    Controller &m_c;
    std::unique_ptr<Window> m_window;
    Nib m_nib;
    TextField *m_findField = nullptr, *m_replaceField = nullptr;
    Label *m_status = nullptr;
    CheckBox *m_ignoreBox = nullptr;
    RadioGroup *m_scope = nullptr;
    std::string m_findString;
};

// ---- the controller: NoteController, the application's delegate and document controller

class Controller {
public:
    Controller();
    void start(const std::vector<std::string> &files);

    std::vector<StickyNote *> notes() const;   // front to back (-[NSApplication orderedDocuments])
    StickyNote *mainNote() const;
    void noteBecameMain(StickyNote *n);
    void noteResignedMain(StickyNote *n);
    void remove(StickyNote *n);
    StickyNote *newNote(const RichText *text = nullptr, bool show = true);
    void setDirty(bool dirty);
    bool isDirty() const { return m_dirty; }
    void saveAll();
    void arrange(int tag);
    void undoArrange();
    bool lastWasArrange() const { return m_arrangeUndo.has_value(); }
    void textEdited() { m_arrangeUndo.reset(); }
    void importText();
    void importFiles(const std::vector<std::string> &paths);
    Point nextCascadePoint();
    void quit();
    void updateWindowsMenu();
    MainMenu &menu() { return m_menu; }
    FindPanel &findPanel() { return m_find; }
    std::string databasePath() const { return home() + "/Library/StickiesDatabase"; }
    Settings &prefs() { return m_prefs; }
    Style defaultStyle();
    Size defaultSize();
    void showAbout();

private:
    bool validate(const MainMenuItem &it, MenuValidation &v);
    void perform(const MainMenuItem &it);
    bool unarchive();
    StickyNote *open(const Note &n);

    std::vector<std::unique_ptr<StickyNote>> m_notes;
    std::vector<StickyNote *> m_order;   // front to back
    MainMenu m_menu;
    FindPanel m_find{*this};
    Settings m_prefs{"stickies"};
    bool m_dirty = false;
    int m_saveTimer = 0;
    Point m_cascade;
    std::optional<std::vector<Note>> m_arrangeUndo;
    std::optional<Style> m_copiedStyle;
    size_t m_staticWindowItems = 0;
    std::unique_ptr<Window> m_about;
};

// ---- the views' drawing, as Stickies' code draws them (Cocoa's coordinates turned top down)

void MicroButton::draw(cairo_t *cr)
{
    // -[MicroButtonCell drawWithFrame:inView:], in the 7 × 7 box's own coordinates from its bottom
    const double h = frame().h, w = frame().w;
    auto at = [h](double x, double y) { return Point{x, h - y}; };
    auto stroke = [&](std::initializer_list<Point> pts, const Color &c) {
        bool first = true;
        for (Point p : pts) {
            const Point q = at(p.x, p.y);
            (first ? cairo_move_to : cairo_line_to)(cr, q.x, q.y);
            first = false;
        }
        c.set(cr);
        cairo_set_line_width(cr, 1);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        cairo_stroke(cr);
    };
    const bool lit = m_pressed && m_inside;
    if (lit) {
        fill(cr, bounds(), m_note->controlColour());
        return;
    }
    fill(cr, bounds(), m_note->noteColour());
    const Color control = m_note->controlColour();
    // the box: its bottom and right sides, then its left and top ones
    stroke({{0, 1}, {w - 1, 1}, {w - 1, h}}, control);
    if (m_type == 0) {
        stroke({{1, 1}, {1, h - 1}, {w - 3, h - 1}}, control);
    } else if (m_type == 1) {
        // the zoom box: a smaller box inside its top left
        stroke({{1, 1}, {1, h - 1}, {w - 3, h - 1}}, control);
        stroke({{1, 4}, {3, 4}, {3, h - 1}}, control);
    } else {
        // the collapse box: two lines across, the window shade
        stroke({{1, 1}, {1, 4}, {w - 3, 4}}, control);
        stroke({{1, h - 1}, {w - 3, h - 1}}, control);
    }
}

DragBar::DragBar(StickyNote *note) : m_note(note)
{
    // -[StickiesDragBar initWithFrame:]: the title (message font, 9 points, black) at (25, 1) from the
    // bottom, 50 narrower than the bar, 11 high; the close box 7 × 7 at (5, 1), the zoom box 10 from
    // the right, the collapse box 22 from it
    title = add<Label>(std::string(), Font::system(9));
    title->style.color = Color::gray(0);
    title->setHidden(true);
    close = add<MicroButton>(note, 0);
    zoom = add<MicroButton>(note, 1);
    unshade = add<MicroButton>(note, 2);
    close->action = [note] { note->performClose(); };
    zoom->action = [note] { note->zoom(); };
    unshade->action = [note] { note->unshade(); };
}

void DragBar::layoutButtons()
{
    const double w = frame().w, h = frame().h;
    auto box = [h](double x) { return Rect{x, h - 1 - 7, 7, 7}; };
    close->setFrame(box(5));
    zoom->setFrame(box(w - 10));
    unshade->setFrame(box(w - 22));
    title->setFrame({25, h - 1 - 11, std::max(0.0, w - 50), 11});
}

void DragBar::update()
{
    const bool main = m_note->isMain();
    close->setHidden(!main);
    zoom->setHidden(!main || m_note->isShaded());
    unshade->setHidden(!main || !m_note->isShaded());
    title->setHidden(!m_note->isShaded());
    if (m_note->isShaded())
        title->setText(m_note->firstLine());
    setNeedsDisplay();
}

void DragBar::draw(cairo_t *cr)
{
    // -[StickiesDragBar drawRect:]
    const double w = frame().w, h = frame().h;
    if (m_note->isMain()) {
        fill(cr, bounds(), m_note->dragColour());
        // framed in the hilite colour, its bottom line under the content's
        const Color c = m_note->hiliteColour();
        fill(cr, {0, 0, w, 1}, c);
        fill(cr, {0, 0, 1, h}, c);
        fill(cr, {w - 1, 0, 1, h}, c);
    } else {
        fill(cr, bounds(), m_note->noteColour());
        fill(cr, {0, 0, 1, h}, Color::gray(255));
        fill(cr, {0, 0, w, 1}, Color::gray(255));
        fill(cr, {w - 1, 1, 1, h - 1}, m_note->hiliteColour());
    }
}

void DragBar::mouseDown(Event &e)
{
    // -[StickiesDragBar mouseDown:]: two clicks collapse or expand it; otherwise it moves
    if (e.clickCount == 2) {
        m_note->toggleShade();
        return;
    }
    m_from = e.screen;
    m_origin = m_note->window()->screenOrigin();
    m_moved = false;
    m_compositorMove = !m_note->window()->moveTo(m_origin);
    if (m_compositorMove)
        m_note->window()->dragMove(e.screen, e.screen, true);
}

void DragBar::mouseDragged(Event &e)
{
    if (m_compositorMove)
        return;
    Point p{m_origin.x + e.screen.x - m_from.x, m_origin.y + e.screen.y - m_from.y};
    // its top kept under the menu bar
    const Rect visible = App::shared().visibleFrame();
    const Rect screen = App::shared().screenFrame();
    const double menuBar = visible.h < screen.h ? visible.y : 22;
    p.y = std::max(p.y, menuBar);
    m_note->moveTo(p);
    m_moved = true;
}

void DragBar::mouseUp(Event &)
{
    if (m_moved || m_compositorMove)
        m_note->controller().setDirty(true);
}

void Enclosure::draw(cairo_t *cr)
{
    // -[StickiesEnclosure drawRect:]: the note's colour framed in the hilite colour (inactive, its left
    // line taken by a white one)
    const double w = frame().w, h = frame().h;
    fill(cr, bounds(), m_note->noteColour());
    const Color c = m_note->hiliteColour();
    fill(cr, {0, 0, w, 1}, c);
    fill(cr, {0, h - 1, w, 1}, c);
    fill(cr, {w - 1, 0, 1, h}, c);
    fill(cr, {0, 0, 1, h}, m_note->isMain() ? c : Color::gray(255));
}

void ResizeIcon::draw(cairo_t *cr)
{
    if (!m_note->isMain())
        return;
    // -[StickiesResizeIcon drawRect:]: its two strips (adjustRectanglesTo:), 4 points each, framed in
    // the control colour, filled with the note's
    const Color control = m_note->controlColour(), note = m_note->noteColour();
    auto frameRect = [&](const Rect &r) {
        fill(cr, {r.x, r.y, r.w, 1}, control);
        fill(cr, {r.x, r.bottom() - 1, r.w, 1}, control);
        fill(cr, {r.x, r.y, 1, r.h}, control);
        fill(cr, {r.right() - 1, r.y, 1, r.h}, control);
    };
    const Rect lower{0, 4, 8, 4}, right{4, 0, 4, 8};
    frameRect(lower);
    frameRect(right);
    fill(cr, lower.inset(1, 1), note);
    fill(cr, right.inset(1, 1), note);
}

View *ResizeIcon::hitTest(Point p)
{
    if (!m_note->isMain())
        return nullptr;
    const Point local{p.x - frame().x, p.y - frame().y};
    // -[StickiesResizeIcon mouseInsideResizeIcon:]: in one of its two strips
    if (Rect{0, 4, 8, 4}.contains(local) || Rect{4, 0, 4, 8}.contains(local))
        return this;
    return nullptr;
}

void ResizeIcon::mouseDown(Event &e)
{
    m_from = e.screen;
    m_start = m_note->window()->size();
}

void ResizeIcon::mouseDragged(Event &e)
{
    // -[StickiesResizeIcon trackMouseDown:]: the bottom right corner follows, the top left stays;
    // collapsed, the width only (and the expanded frame's with it)
    Size s{std::max(MinWidth, m_start.w + e.screen.x - m_from.x), m_start.h + e.screen.y - m_from.y};
    if (m_note->isShaded()) {
        s.h = CollapsedHeight;
        Size u = m_note->unshadedSize();
        u.w = s.w;
        m_note->setUnshadedSize(u);
    } else {
        s.h = std::max(30.0, s.h);
    }
    m_note->setSize(s);
}

void ResizeIcon::mouseUp(Event &) { m_note->controller().setDirty(true); }

// ---- the note

StickyNote::StickyNote(Controller &c, const Note &data, bool fromDatabase) : m_c(c)
{
    m_colour = std::clamp(data.colour, 0, 5);
    m_created = data.created;
    m_modified = data.modified;
    Size size{std::max(MinWidth, data.width), data.height};
    // -[StickiesDocument windowControllerDidLoadNib:]: a frame of the drag bar's height or less takes the
    // default size
    if (size.h <= CollapsedHeight)
        size.h = c.defaultSize().h;
    if (size.w < CollapsedHeight)
        size.w = c.defaultSize().w;
    m_window = std::make_unique<Window>(size, "Untitled");
    m_window->setBorderless();
    m_window->setKeyEquivalents(OwnKeys);
    View *content = m_window->contentView();
    m_enclosure = content->add<Enclosure>(this);
    m_text = content->add<TextView>();
    m_bar = content->add<DragBar>(this);
    m_resize = content->add<ResizeIcon>(this);
    m_text->background = noteColour();

    RichText text = fromRTFD(data.rtfd);
    if (data.rtfd.empty()) {
        text = RichText();
        text.styles.push_back(c.defaultStyle());
    }
    m_text->setTypingStyle(text.styles.empty() ? c.defaultStyle() : text.styles[0]);
    m_text->setText(std::move(text));
    m_text->onChange = [this] { textChanged(); };

    m_window->onKeyChanged = [this](bool key) {
        if (key)
            m_c.noteBecameMain(this);
        else
            m_c.noteResignedMain(this);
        m_bar->update();
        m_resize->setNeedsDisplay();
        m_enclosure->setNeedsDisplay();
        m_window->invalidate();
    };
    m_window->onUnhandledKey = [this](Event &e) { m_c.menu().performKeyEquivalent(e); };
    m_window->onClose = [this] { performClose(); };
    m_window->onResize = [this](Size) { layout(); };

    layout();
    const int flags = data.flags;
    if (flags & Floating)
        m_window->setFloating(true);
    if (flags & Translucent)
        m_window->setAlphaValue(0.75);

    // where it was, unless its drag bar is off the screen: then the next cascade point
    const Rect screen = App::shared().screenFrame();
    Rect r{data.x, screen.h - (data.y + data.height), size.w, data.height};
    const Rect visible = App::shared().visibleFrame();
    const Rect bar{r.x, r.y, r.w, BarHeight};
    const bool seen = bar.right() > visible.x && bar.x < visible.right() && bar.bottom() > visible.y && bar.y < visible.bottom();
    if (!fromDatabase || !seen) {
        const Point p = fromDatabase ? c.nextCascadePoint() : Point{r.x, r.y};
        r.x = p.x;
        r.y = p.y;
    }
    r.y = std::max(r.y, visible.y);
    m_window->moveTo({r.x, r.y});
    if (flags & Collapsed) {
        m_unshaded = size;
        m_shaded = true;
        m_window->setContentSize({size.w, CollapsedHeight});
        layout();
    }
    m_window->setTitle(title());
    m_bar->update();
}

StickyNote::~StickyNote()
{
    if (m_animation)
        App::shared().removeTimer(m_animation);
}

void StickyNote::show()
{
    m_window->show();
    m_window->makeFirstResponder(m_text);
}

void StickyNote::layout()
{
    const Size s = m_window->size();
    m_bar->setFrame({0, 0, s.w, BarHeight});
    m_bar->layoutButtons();
    m_enclosure->setFrame({0, BarHeight, s.w, std::max(0.0, s.h - BarHeight)});
    // -[StickiesWindow _scrollViewResized:]: (5, 5), 10 narrower, 22 lower
    const double th = std::max(0.0, s.h - 22);
    m_text->setFrame({5, s.h - 5 - th, std::max(0.0, s.w - 10), th});
    m_text->setHidden(th <= 0);
    m_resize->setFrame({s.w - 8, s.h - 1 - 8, 8, 8});
    m_window->invalidate();
}

std::string StickyNote::title() const
{
    // the first 27 characters and "…", or "Untitled"
    const std::string &t = m_text->text().text;
    if (t.empty())
        return tr("EMPTY_WINDOW_TITLE");
    std::string out;
    size_t chars = 0, i = 0;
    while (i < t.size() && chars < 0x1b) {
        size_t n = i + 1;
        while (n < t.size() && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80)
            ++n;
        std::string ch = t.substr(i, n - i);
        out += ch == "\n" ? " " : ch == AttachmentCharacter ? "" : ch;
        i = n;
        ++chars;
    }
    if (i < t.size())
        out += tr("ELLIPSES");
    return out.empty() ? tr("EMPTY_WINDOW_TITLE") : out;
}

std::string StickyNote::firstLine() const
{
    const std::string &t = m_text->text().text;
    std::string line = t.substr(0, t.find('\n'));
    for (size_t p; (p = line.find(AttachmentCharacter)) != std::string::npos;)
        line.erase(p, 3);
    return line;
}

void StickyNote::textChanged()
{
    // -[StickiesDocument _textDidChange:]: the Window menu's title, the modification date, dirty
    m_window->setTitle(title());
    m_modified = nowSinceReferenceDate();
    if (m_shaded)
        m_bar->title->setText(firstLine());
    m_c.textEdited();
    m_c.setDirty(true);
    m_c.updateWindowsMenu();
}

Note StickyNote::data() const
{
    Note n;
    n.rtfd = toRTFD(m_text->text());
    n.flags = flags();
    n.colour = m_colour;
    const Rect f = cocoaFrame();
    n.x = f.x;
    n.y = f.y;
    n.width = f.w;
    n.height = f.h;
    n.created = m_created;
    n.modified = m_modified;
    return n;
}

Rect StickyNote::screenRect() const
{
    const Point o = m_window->screenOrigin();
    const Size s = m_window->size();
    return {o.x, o.y, s.w, s.h};
}

Rect StickyNote::cocoaFrame() const
{
    const Rect r = screenRect();
    const Size s = unshadedSize();
    // the expanded frame keeps the top
    const double screenH = App::shared().screenFrame().h;
    return {r.x, screenH - (r.y + s.h), s.w, s.h};
}

void StickyNote::moveTo(Point topLeft) { m_window->moveTo(topLeft); }

void StickyNote::setSize(Size s, bool animate)
{
    if (m_animation) {
        App::shared().removeTimer(m_animation);
        m_animation = 0;
    }
    if (!animate || !m_window->isVisible()) {
        m_window->setContentSize(s);
        layout();
        return;
    }
    // setFrame:display:animate:, the top left kept
    const Size from = m_window->size();
    const double start = now();
    m_animation = App::shared().addTimer(1.0 / 60, true, [this, from, s, start] {
        double t = std::min(1.0, (now() - start) / ResizeTime);
        t = 0.5 - 0.5 * std::cos(t * M_PI);
        m_window->setContentSize({from.w + (s.w - from.w) * t, std::round(from.h + (s.h - from.h) * t)});
        layout();
        if (t >= 1) {
            App::shared().removeTimer(m_animation);
            m_animation = 0;
        }
    });
}

void StickyNote::setColour(int index)
{
    if (index != m_colour)
        m_c.setDirty(true);
    m_colour = std::clamp(index, 0, 5);
    m_text->background = noteColour();
    m_window->invalidate();
}

void StickyNote::shade()
{
    // -[StickiesWindow shade]: the drag bar's height, its top kept, the first line as its title
    if (m_shaded)
        return;
    m_unshaded = m_window->size();
    m_shaded = true;
    m_zoomed = false;
    setSize({m_unshaded.w, CollapsedHeight}, true);
    m_bar->update();
    m_c.setDirty(true);
}

void StickyNote::unshade()
{
    if (!m_shaded)
        return;
    m_shaded = false;
    setSize(m_unshaded, true);
    m_bar->update();
    m_c.setDirty(true);
}

void StickyNote::zoom()
{
    // -[StickiesWindow windowWillUseStandardFrame:defaultFrame:]: collapsed, centred on its screen;
    // otherwise the screen's visible frame, and back
    m_c.setDirty(true);
    const Rect visible = App::shared().visibleFrame();
    const Rect screen = App::shared().screenFrame();
    if (m_shaded) {
        const Size s = m_window->size();
        moveTo({screen.w * 0.5 - s.w * 0.5, screen.h * 0.5});
        return;
    }
    if (!m_zoomed) {
        m_userFrame = screenRect();
        m_zoomed = true;
        moveTo({visible.x, visible.y});
        setSize({visible.w, visible.h});
    } else {
        m_zoomed = false;
        moveTo({m_userFrame.x, m_userFrame.y});
        setSize({m_userFrame.w, m_userFrame.h});
    }
}

void StickyNote::setFloating(bool f)
{
    if (f != isFloating())
        m_c.setDirty(true);
    m_window->setFloating(f);
}

void StickyNote::setTranslucent(bool t)
{
    if (t != isTranslucent())
        m_c.setDirty(true);
    m_window->setAlphaValue(t ? 0.75 : 1);
}

void StickyNote::performClose()
{
    // -[StickiesDocument windowShouldClose:]: an empty note goes; another asks first
    if (m_closing)
        return;
    if (isEmpty()) {
        closeNow();
        return;
    }
    m_closing = true;
    Alert::run(tr("CLOSE"), tr("STICKIE_BEEN_EDITED_SAVE?"), {tr("SAVE"), tr("CANCEL"), tr("DONT_SAVE")}, [this](int button) {
        m_closing = false;
        if (button == 0)
            exportNote([this](bool saved) {
                if (saved)
                    closeNow();
            });
        else if (button == 2)
            closeNow();
    });
}

void StickyNote::closeNow()
{
    m_window->onClose = nullptr;
    m_window->onKeyChanged = nullptr;
    m_window->close();
    Controller &c = m_c;
    App::shared().later([&c, this] { c.remove(this); });
}

void StickyNote::exportNote(std::function<void(bool)> done)
{
    // -[StickiesDocument exportSticky:]: "Untitled" in the home folder; Plain Text (UTF-8), RTF or RTFD
    const std::string start = home() + "/" + tr("UNTITLED") + ".txt";
    auto write = [this, done](std::string path) {
        if (path.empty())
            return done(false);
        std::string ext = extension(path);
        if (ext != "txt" && ext != "rtf" && ext != "rtfd") {
            path += ".txt";
            ext = "txt";
        }
        const RichText &t = m_text->text();
        bool ok = false;
        mode_t keep = 0;
        struct stat st;
        const bool existed = stat(path.c_str(), &st) == 0;
        if (existed)
            keep = st.st_mode & 07777;
        if (ext == "txt") {
            std::string plain;
            for (size_t i = 0; i < t.text.size(); ++i) {
                if (t.text.compare(i, 3, AttachmentCharacter) == 0) {
                    i += 2;
                    continue;
                }
                plain += t.text[i];
            }
            ok = writeFile(path, plain);
        } else if (ext == "rtf") {
            ok = writeFile(path, toRTF(t));
        } else {
            // RTFD: a folder, TXT.rtf and the pictures
            mkdir(path.c_str(), 0755);
            ok = writeFile(path + "/TXT.rtf", toRTF(t));
            for (const Attachment &a : t.attachments)
                if (!a.name.empty())
                    ok = writeFile(path + "/" + a.name, a.data) && ok;
        }
        if (ok && existed && ext != "rtfd")
            chmod(path.c_str(), keep);
        if (!ok) {
            std::string msg = tr("Couldn't save document as %@.");
            if (size_t p = msg.find("%@"); p != std::string::npos)
                msg.replace(p, 2, fileName(path));
            Alert::run(msg, "", {tr("OK")}, nullptr);
        }
        done(ok);
    };
    const std::string title = tr("EXPORT");
    runDialog({"kdialog", "--title", title, "--getsavefilename", start, "*.txt|Plain Text\n*.rtf|RTF\n*.rtfd|RTFD"},
              [write, start, title](bool ran, std::string answer) {
                  if (ran)
                      return write(answer);
                  runDialog({"zenity", "--file-selection", "--save", "--confirm-overwrite", "--title=" + title, "--filename=" + start,
                             "--file-filter=RTF | *.rtf", "--file-filter=Plain Text | *.txt", "--file-filter=RTFD | *.rtfd"},
                            [write](bool ran2, std::string a) {
                                if (!ran2) {
                                    std::fprintf(stderr, "Stickies: Export needs kdialog or zenity\n");
                                    return write(std::string());
                                }
                                write(a);
                            });
              });
}

void StickyNote::print()
{
    // -[StickiesDocument printDocumentWithSettings:...]: the text laid out anew at the page's width,
    // the header and footer on (NSPrintHeaderAndFooter), the job named "Stickies"; here a PDF given
    // to lp. A US Letter page, NSPrintInfo's margins (72 at the sides, 90 at the top and bottom).
    const double pw = 612, ph = 792, ml = 72, mr = 72, mt = 90, mb = 90;
    TextView layoutView;
    RichText copy = m_text->text();
    layoutView.setText(copy);
    layoutView.setFrame({0, 0, pw - ml - mr + 2 * TextView::Padding, 1e6});
    const std::vector<double> lines = layoutView.lineBottoms();
    // the pages: as many whole lines as fit
    std::vector<std::pair<double, double>> pages;
    double top = 0, usable = ph - mt - mb;
    double last = 0;
    for (double b : lines) {
        if (b - top > usable && last > top) {
            pages.push_back({top, last});
            top = last;
        }
        last = b;
    }
    pages.push_back({top, std::max(last, top)});
    char tmpl[] = "/tmp/stickies-print-XXXXXX.pdf";
    const int fd = mkstemps(tmpl, 4);
    if (fd < 0)
        return;
    close(fd);
    cairo_surface_t *pdf = cairo_pdf_surface_create(tmpl, pw, ph);
    cairo_t *cr = cairo_create(pdf);
    char date[64];
    const time_t t = time(nullptr);
    strftime(date, sizeof date, "%x %X", localtime(&t));
    for (size_t i = 0; i < pages.size(); ++i) {
        TextStyle hs;
        hs.font = Font::system(9);
        hs.elide = false;
        drawText(cr, "Stickies", {ml, 36, 200, 12}, hs);
        hs.align = Align::Right;
        drawText(cr, date, {pw - mr - 200, 36, 200, 12}, hs);
        hs.align = Align::Center;
        drawText(cr, "Page " + std::to_string(i + 1) + " of " + std::to_string(pages.size()), {ml, ph - 48, pw - ml - mr, 12}, hs);
        cairo_save(cr);
        cairo_rectangle(cr, ml - TextView::Padding, mt, pw - ml - mr + 2 * TextView::Padding, pages[i].second - pages[i].first);
        cairo_clip(cr);
        cairo_translate(cr, ml - TextView::Padding, mt - pages[i].first);
        layoutView.drawGlyphs(cr, pages[i].first, pages[i].second);
        cairo_restore(cr);
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(pdf);
    std::vector<std::string> argv = {"lp", "-t", "Stickies", tmpl};
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, "lp", nullptr, nullptr, args.data(), environ) == 0) {
        const std::string file = tmpl;
        App::shared().addTimer(5, false, [pid, file] {
            waitpid(pid, nullptr, WNOHANG);
            std::remove(file.c_str());
        });
    } else {
        std::remove(tmpl);
    }
}

// ---- Find

void FindPanel::build()
{
    if (m_window)
        return;
    m_nib.load(res("English.lproj/MainMenu.nib/keyedobjects.nib"), res(""));
    View *content = nullptr;
    Size size{547, 166};
    for (const Nib::WindowEntry &w : m_nib.windows())
        if (w.title == "Find") {
            content = m_nib.instantiateWindow(w.object, &size);
            break;
        }
    m_window = std::make_unique<Window>(size, "Find");
    m_window->setKeyEquivalents(OwnKeys);
    if (content) {
        content->setFrame({0, 0, size.w, size.h});
        m_window->contentView()->addSubview(content);
    }
    // its fields by their places: Find over Replace with; the status under them at the left
    std::vector<TextField *> fields;
    std::function<void(View *)> walk = [&](View *v) {
        if (auto *f = dynamic_cast<TextField *>(v); f && f->editable)
            fields.push_back(f);
        if (auto *l = dynamic_cast<Label *>(v); l && l->text().empty() && l->frame().h > 30)
            m_status = l;
        if (auto *r = dynamic_cast<RadioGroup *>(v))
            m_scope = r;
        if (auto *c = dynamic_cast<CheckBox *>(v); c && c->title == "Ignore Case")
            m_ignoreBox = c;
        for (View *s : v->subviews())
            walk(s);
    };
    walk(m_window->contentView());
    std::sort(fields.begin(), fields.end(), [](TextField *a, TextField *b) { return a->toWindow({0, 0}).y < b->toWindow({0, 0}).y; });
    if (!fields.empty())
        m_findField = fields[0];
    if (fields.size() > 1)
        m_replaceField = fields[1];
    if (m_findField) {
        m_findField->setText(m_findString);
        m_findField->onEnter = [this] { find(true); };
    }
    for (const Nib::Connection &c : m_nib.controls()) {
        auto *control = dynamic_cast<class AK::Control *>(c.view);
        if (!control)
            continue;
        const std::string a = c.action;
        if (a == "findNext:")
            control->action = [this] { find(true); };
        else if (a == "findPrevious:")
            control->action = [this] { find(false); };
        else if (a == "replace:")
            control->action = [this] { replace(); };
        else if (a == "replaceAndFind:")
            control->action = [this] { replaceAndFind(); };
        else if (a == "replaceAll:")
            control->action = [this] { replaceAll(); };
    }
    m_window->onUnhandledKey = [this](Event &e) { m_c.menu().performKeyEquivalent(e); };
    m_window->onClose = [this] { m_window->close(); };
}

void FindPanel::show()
{
    build();
    m_window->show();
    if (m_findField) {
        m_window->makeFirstResponder(m_findField);
        m_findField->selectAll();
    }
}

namespace {

// where `needle` is in `hay` from `from`, forwards or backwards, case ignored or not; npos when not
size_t search(const std::string &hay, const std::string &needle, size_t from, bool forward, bool ignoreCase)
{
    if (needle.empty() || hay.size() < needle.size())
        return std::string::npos;
    auto matchAt = [&](size_t at) {
        if (!ignoreCase)
            return hay.compare(at, needle.size(), needle) == 0;
        size_t i = at, j = 0;
        while (j < needle.size()) {
            if (i >= hay.size())
                return false;
            const gunichar a = g_utf8_get_char(hay.c_str() + i), b = g_utf8_get_char(needle.c_str() + j);
            if (g_unichar_tolower(a) != g_unichar_tolower(b))
                return false;
            i = size_t(g_utf8_next_char(hay.c_str() + i) - hay.c_str());
            j = size_t(g_utf8_next_char(needle.c_str() + j) - needle.c_str());
        }
        return true;
    };
    if (forward) {
        for (size_t at = from; at < hay.size(); ++at)
            if ((static_cast<unsigned char>(hay[at]) & 0xC0) != 0x80 && matchAt(at))
                return at;
    } else {
        for (size_t at = std::min(from, hay.size()); at-- > 0;)
            if ((static_cast<unsigned char>(hay[at]) & 0xC0) != 0x80 && matchAt(at))
                return at;
    }
    return std::string::npos;
}

size_t matchLength(const std::string &hay, size_t at, const std::string &needle)
{
    size_t i = at, chars = size_t(g_utf8_strlen(needle.c_str(), long(needle.size())));
    while (chars-- > 0 && i < hay.size())
        i = size_t(g_utf8_next_char(hay.c_str() + i) - hay.c_str());
    return i - at;
}

} // namespace

bool FindPanel::find(bool forward)
{
    // -[TextFindController find:]: from the selection in the main note, wrapping in it; with All Notes,
    // on through the others (the documents' order) until back at the first
    const std::string needle = findString();
    m_findString = needle;
    std::vector<StickyNote *> notes = m_c.notes();
    StickyNote *start = m_c.mainNote();
    if (!start || needle.empty()) {
        status(tr("NOT_FOUND"));
        return false;
    }
    const bool all = allNotes(), ic = ignoreCase();
    // the order to look in: the main note first, then the others after it (wrapping)
    std::vector<StickyNote *> order;
    auto it = std::find(notes.begin(), notes.end(), start);
    const size_t first = size_t(it - notes.begin());
    for (size_t k = 0; k < notes.size(); ++k)
        order.push_back(notes[forward ? (first + k) % notes.size() : (first + notes.size() - k) % notes.size()]);
    if (!all)
        order.resize(1);
    for (size_t k = 0; k <= order.size(); ++k) {
        StickyNote *n = order[k % order.size()];
        TextView *tv = n->textView();
        const std::string &s = tv->text().text;
        size_t from;
        if (k == 0)
            from = forward ? tv->selectionEnd() : tv->selectionStart();
        else
            from = forward ? 0 : s.size();
        if (k == order.size())
            from = forward ? 0 : s.size();   // back at the first: its part before the selection
        size_t at = search(s, needle, from, forward, ic);
        if (at == std::string::npos && !all && k == 0)
            at = search(s, needle, forward ? 0 : s.size(), forward, ic);   // wrapping in the note
        if (at != std::string::npos) {
            if (!n->isMain())
                n->window()->show();
            if (n->isShaded())
                n->unshade();
            tv->select(at, at + matchLength(s, at, needle));
            tv->scrollToSelection();
            n->window()->makeFirstResponder(tv);
            status("");
            return true;
        }
        if (!all)
            break;
    }
    status(tr("NOT_FOUND"));
    return false;
}

void FindPanel::replace()
{
    StickyNote *n = m_c.mainNote();
    if (!n) {
        status("");
        return;
    }
    RichText with;
    with.insert(0, m_replaceField ? m_replaceField->text : std::string(), n->textView()->styleAtSelection());
    n->textView()->replaceSelection(with);
    status("");
}

void FindPanel::replaceAll()
{
    // -[TextFindController replaceAll:]: every match, in the main note or in all of them, in one change each
    const std::string needle = findString();
    const std::string with = m_replaceField ? m_replaceField->text : std::string();
    std::vector<StickyNote *> notes;
    if (allNotes())
        notes = m_c.notes();
    else if (StickyNote *n = m_c.mainNote())
        notes.push_back(n);
    size_t count = 0;
    for (StickyNote *n : notes) {
        TextView *tv = n->textView();
        RichText t = tv->text();
        size_t replaced = 0;
        for (size_t at = search(t.text, needle, 0, true, ignoreCase()); at != std::string::npos;
             at = search(t.text, needle, at + with.size(), true, ignoreCase())) {
            const size_t len = matchLength(t.text, at, needle);
            const Style s = t.styleAt(at);
            t.erase(at, at + len);
            t.insert(at, with, s);
            ++replaced;
        }
        if (replaced) {
            tv->selectAll();
            tv->replaceSelection(t);
            tv->select(0, 0);
            if (n->isShaded())
                n->unshade();
            count += replaced;
        }
    }
    if (count) {
        std::string msg = tr("D_REPLACED");
        if (size_t p = msg.find("%d"); p != std::string::npos)
            msg.replace(p, 2, std::to_string(count));
        status(msg);
    } else {
        status(tr("NOT_FOUND"));
    }
}

void FindPanel::useSelection()
{
    StickyNote *n = m_c.mainNote();
    if (!n || !n->textView()->hasSelection())
        return;
    m_findString = n->textView()->selectedString();
    if (m_findField)
        m_findField->setText(m_findString);
}

void FindPanel::jumpToSelection()
{
    if (StickyNote *n = m_c.mainNote())
        n->textView()->scrollToSelection();
}

// ---- the controller

Controller::Controller()
{
    // -[NoteController awakeFromNib]: the defaults registered from defaultSettings.plist; the next
    // cascade point 8 in from the visible frame's top left
    const Rect visible = App::shared().visibleFrame();
    m_cascade = {visible.x + 8, visible.y + 8};
    m_menu.load(res("English.lproj/MainMenu.nib"));
    m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
    m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
    for (MainMenuItem &m : m_menu.menus())
        if (m.title == "Window")
            m_staticWindowItems = m.items.size();
}

Style Controller::defaultStyle()
{
    const auto d = Assets::plistStrings(std::string(Bundle) + "/Contents/Resources/defaultSettings.plist");
    auto def = [&d](const char *k, const char *fallback) {
        auto it = d.find(k);
        return it == d.end() ? std::string(fallback) : it->second;
    };
    const std::string name = m_prefs.string("FontName", def("FontName", "Helvetica"));
    const double size = std::atof(m_prefs.string("FontSize", def("FontSize", "12")).c_str());
    Style s = Style::fromPostScriptName(name, size > 0 ? size : 12);
    return s;
}

Size Controller::defaultSize()
{
    const auto d = Assets::plistStrings(std::string(Bundle) + "/Contents/Resources/defaultSettings.plist");
    auto value = [&](const char *k, double fallback) {
        const std::string mine = m_prefs.string(k);
        if (!mine.empty())
            return std::atof(mine.c_str());
        auto it = d.find(k);
        return it == d.end() ? fallback : std::atof(it->second.c_str());
    };
    return {std::max(MinWidth, value("ViewWidth", 300)), std::max(30.0, value("ViewHeight", 200))};
}

Point Controller::nextCascadePoint()
{
    // cascadeTopLeftFromPoint: down and right from where the last one went, back to the top when off the screen
    const Rect visible = App::shared().visibleFrame();
    Point p = m_cascade;
    m_cascade = {m_cascade.x + 22, m_cascade.y + 22};
    if (m_cascade.y > visible.bottom() - 100 || m_cascade.x > visible.right() - 100)
        m_cascade = {visible.x + 8, visible.y + 8};
    return p;
}

StickyNote *Controller::open(const Note &n)
{
    m_notes.push_back(std::make_unique<StickyNote>(*this, n, true));
    StickyNote *note = m_notes.back().get();
    m_order.push_back(note);
    return note;
}

bool Controller::unarchive()
{
    // -[NoteController _unarchiveStickies]: ~/Library/StickiesDatabase (the older .StickiesDatabase
    // renamed to it), else the welcome notes
    const std::string db = databasePath();
    const std::string legacy = home() + "/Library/.StickiesDatabase";
    if (isFile(legacy) && !isFile(db)) {
        std::fprintf(stderr, "Stickies: Renaming legacy sticky database.\n");
        std::rename(legacy.c_str(), db.c_str());
    }
    std::string path = db;
    if (!isFile(path)) {
        std::fprintf(stderr, "Stickies: Stickies database not found at %s. Using default.\n", db.c_str());
        path = res("English.lproj/StickiesDefaultDatabase");
    }
    std::vector<Note> notes;
    if (!readDatabase(path, notes) || notes.empty())
        return false;
    // front to back: the back one shown first, the front one last
    for (const Note &n : notes)
        open(n);
    return true;
}

void Controller::start(const std::vector<std::string> &files)
{
    if (!unarchive())
        newNote(nullptr, false);
    m_menu.publish();
    updateWindowsMenu();
    for (auto it = m_order.rbegin(); it != m_order.rend(); ++it)
        (*it)->show();
    if (!files.empty())
        importFiles(files);
    setDirty(false);
}

std::vector<StickyNote *> Controller::notes() const { return m_order; }

StickyNote *Controller::mainNote() const
{
    // NSApp's mainWindow: the note in front (a panel, the Find panel, never becomes main)
    for (StickyNote *n : m_order)
        if (n->isMain())
            return n;
    for (StickyNote *n : m_order)
        if (n->window()->isVisible())
            return n;
    return nullptr;
}

void Controller::noteBecameMain(StickyNote *n)
{
    auto it = std::find(m_order.begin(), m_order.end(), n);
    if (it != m_order.end()) {
        m_order.erase(it);
        m_order.insert(m_order.begin(), n);
    }
    App::shared().setFrontWithoutWindows(false);
}

void Controller::noteResignedMain(StickyNote *)
{
    // -[NoteController applicationWillResignActive:]: saved when none of its windows is key any more
    App::shared().later([this] {
        const bool anyKey = std::any_of(m_order.begin(), m_order.end(), [](StickyNote *n) { return n->isMain(); });
        if (!anyKey && m_dirty)
            saveAll();
    });
}

void Controller::remove(StickyNote *n)
{
    m_order.erase(std::remove(m_order.begin(), m_order.end(), n), m_order.end());
    auto it = std::find_if(m_notes.begin(), m_notes.end(), [n](const std::unique_ptr<StickyNote> &p) { return p.get() == n; });
    if (it != m_notes.end())
        m_notes.erase(it);
    setDirty(true);
    // -[NoteController closeWindow:]: saved at once
    saveAll();
    updateWindowsMenu();
    if (m_order.empty())
        App::shared().setFrontWithoutWindows(true);
}

StickyNote *Controller::newNote(const RichText *text, bool show)
{
    // -[Document init]: the defaults' colour, flags and size at (100, 100); then below the main note
    // (-[StickiesDocument windowControllerDidLoadNib:]: cascadeTopLeftFromPoint: its frame's origin)
    Note n;
    n.colour = std::clamp(m_prefs.integer("ColourIndex", 0), 0, 5);
    n.flags = m_prefs.integer("WindowFlags", 0) & ~Collapsed;
    const Size s = defaultSize();
    n.width = s.w;
    n.height = s.h;
    const Rect screen = App::shared().screenFrame();
    n.x = 100;
    n.y = 100;
    if (StickyNote *main = mainNote()) {
        const Rect r = main->screenRect();
        n.x = r.x;
        double top = r.bottom();
        const Rect visible = App::shared().visibleFrame();
        if (top + s.h > visible.bottom())
            top = std::max(visible.y, visible.bottom() - s.h);
        n.y = screen.h - (top + s.h);
    }
    n.created = n.modified = nowSinceReferenceDate();
    if (text)
        n.rtfd = toRTFD(*text);
    m_notes.push_back(std::make_unique<StickyNote>(*this, n, false));
    StickyNote *note = m_notes.back().get();
    if (!text)
        note->textView()->setTypingStyle(defaultStyle());
    m_order.insert(m_order.begin(), note);
    if (show)
        note->show();
    setDirty(true);
    updateWindowsMenu();
    return note;
}

void Controller::setDirty(bool dirty)
{
    // -[NoteController setIsDirty:]: dirty, a repeating timer saves everything every 300 seconds
    App &app = App::shared();
    if (!dirty) {
        if (m_saveTimer)
            app.removeTimer(m_saveTimer);
        m_saveTimer = 0;
    } else if (!m_dirty) {
        m_saveTimer = app.addTimer(300, true, [this] { saveAll(); });
    }
    m_dirty = dirty;
}

void Controller::saveAll()
{
    // -[NoteController saveAllDocumentsMenuAction:]: every note, front to back, archived into
    // ~/Library/StickiesDatabase (the folder made when it isn't there)
    std::vector<Note> notes;
    for (StickyNote *n : m_order)
        notes.push_back(n->data());
    makePath(home() + "/Library");
    if (!saveDatabase(databasePath(), notes)) {
        std::fprintf(stderr, "Stickies: archive failed\n");
        return;
    }
    setDirty(false);
}

void Controller::arrange(int tag)
{
    // -[NoteController arrange:]: the database kept for Undo, the notes sorted (Color 0, Content 1,
    // Location on Screen 2, Date 3), each collapsed and stacked from the visible frame's top left, 13
    // apart, a new column 200 to the right below 100
    std::vector<Note> before;
    for (StickyNote *n : m_order)
        before.push_back(n->data());
    m_arrangeUndo = before;
    std::vector<StickyNote *> sorted = m_order;
    auto byContent = [](StickyNote *a, StickyNote *b) { return g_utf8_collate(a->title().c_str(), b->title().c_str()) < 0; };
    if (tag == 0)
        std::stable_sort(sorted.begin(), sorted.end(), [&](StickyNote *a, StickyNote *b) {
            return a->colour() != b->colour() ? a->colour() < b->colour() : byContent(a, b);
        });
    else if (tag == 1)
        std::stable_sort(sorted.begin(), sorted.end(), byContent);
    else if (tag == 2)
        std::stable_sort(sorted.begin(), sorted.end(), [](StickyNote *a, StickyNote *b) {
            const Rect ra = a->screenRect(), rb = b->screenRect();
            return std::fabs(ra.x) + ra.y < std::fabs(rb.x) + rb.y;
        });
    else if (tag == 3)
        std::stable_sort(sorted.begin(), sorted.end(), [](StickyNote *a, StickyNote *b) { return a->modified() < b->modified(); });
    const Rect visible = App::shared().visibleFrame();
    const Rect screen = App::shared().screenFrame();
    // in Cocoa's coordinates, as the code goes: x from the left, y (the window's bottom) from the screen's bottom
    const double cocoaTop = screen.h - visible.y;
    double x = visible.x + 8, y = cocoaTop - 16;
    for (StickyNote *n : sorted) {
        if (!n->isShaded())
            n->shade();
        n->moveTo({x, screen.h - y - CollapsedHeight});
        n->window()->show();
        y -= 13;
        if (y < 100) {
            x += 200;
            y = cocoaTop - 16;
        }
        if (visible.w < x + 200)
            x = double(long(x) % long(std::max(1.0, visible.w))) + 100;
    }
    setDirty(true);
}

void Controller::undoArrange()
{
    // -[NoteController undoArrange:]: every note closed, the archive's notes opened again
    if (!m_arrangeUndo)
        return;
    std::vector<Note> notes = *m_arrangeUndo;
    m_arrangeUndo.reset();
    for (StickyNote *n : m_order)
        n->window()->close();
    m_order.clear();
    std::vector<std::unique_ptr<StickyNote>> old = std::move(m_notes);
    m_notes.clear();
    App::shared().later([o = std::make_shared<std::vector<std::unique_ptr<StickyNote>>>(std::move(old))]() mutable { o->clear(); });
    for (const Note &n : notes)
        open(n);
    for (auto it = m_order.rbegin(); it != m_order.rend(); ++it)
        (*it)->show();
    setDirty(true);
    updateWindowsMenu();
}

void Controller::importFiles(const std::vector<std::string> &paths)
{
    // -[NoteController importText:]: each file a new note; .htm and .html (and anything HTML) as plain
    // text; one that can't be read: an alert, OK (and Cancel while others remain)
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string &path = paths[i];
        const std::string ext = extension(path);
        RichText text;
        bool ok = true;
        if (isDir(path) && ext == "rtfd") {
            std::map<std::string, std::string> files;
            if (DIR *d = opendir(path.c_str())) {
                while (dirent *e = readdir(d))
                    if (e->d_name[0] != '.')
                        files[e->d_name] = readFile(path + "/" + e->d_name);
                closedir(d);
            }
            ok = files.count("TXT.rtf");
            if (ok)
                text = fromRTF(files["TXT.rtf"], files);
        } else if (isFile(path)) {
            const std::string bytes = readFile(path);
            if (ext != "htm" && ext != "html" && bytes.compare(0, 5, "{\\rtf") == 0)
                text = fromRTF(bytes);
            else
                text = fromPlainText(bytes, defaultStyle());
        } else {
            ok = false;
        }
        if (!ok) {
            std::string msg = tr("COULDNT_OPEN_FILE");
            if (size_t p = msg.find("%@"); p != std::string::npos)
                msg.replace(p, 2, path);
            std::vector<std::string> buttons = {tr("OK")};
            if (i + 1 < paths.size())
                buttons.push_back(tr("CANCEL"));
            std::vector<std::string> rest(paths.begin() + long(i) + 1, paths.end());
            Alert::run(tr("FILE_SYSTEM_ERROR"), msg, buttons, [this, rest](int b) {
                if (b == 0 && !rest.empty())
                    importFiles(rest);
            });
            return;
        }
        newNote(&text);
    }
}

void Controller::importText()
{
    runDialog({"kdialog", "--title", "Import Text", "--multiple", "--separate-output", "--getopenfilename", home()},
              [this](bool ran, std::string answer) {
                  auto finish = [this](const std::string &a) {
                      std::vector<std::string> paths;
                      size_t start = 0;
                      while (start < a.size()) {
                          size_t end = a.find('\n', start);
                          if (end == std::string::npos)
                              end = a.size();
                          if (end > start)
                              paths.push_back(a.substr(start, end - start));
                          start = end + 1;
                      }
                      importFiles(paths);
                  };
                  if (ran)
                      return finish(answer);
                  runDialog({"zenity", "--file-selection", "--multiple", "--separator=\n", "--title=Import Text", "--filename=" + home() + "/"},
                            [finish](bool ran2, std::string a) {
                                if (!ran2)
                                    std::fprintf(stderr, "Stickies: Import Text needs kdialog or zenity\n");
                                finish(a);
                            });
              });
}

void Controller::quit()
{
    // -[NoteController reviewUnsavedDocumentsWithAlertTitle:...]: everything saved, nothing asked
    saveAll();
    App::shared().quit();
}

void Controller::updateWindowsMenu()
{
    // the Window menu: its own items, then the notes (-[NSApplication addWindowsItem:title:filename:])
    for (MainMenuItem &m : m_menu.menus()) {
        if (m.title != "Window")
            continue;
        m.items.resize(std::min(m.items.size(), m_staticWindowItems));
        if (m_order.empty())
            break;
        MainMenuItem sep;
        sep.separator = true;
        m.items.push_back(sep);
        for (size_t i = 0; i < m_notes.size(); ++i) {
            MainMenuItem it;
            it.title = m_notes[i]->title();
            it.action = "makeKeyAndOrderFront:";
            it.tag = int(i);
            m.items.push_back(it);
        }
    }
}

void Controller::showAbout()
{
    // -[NSApplication orderFrontStandardAboutPanel:]: the icon, the name, the version, the copyright
    if (!m_about) {
        const auto info = Assets::plistStrings(std::string(Bundle) + "/Contents/Info.plist");
        auto get = [&info](const char *k) {
            auto it = info.find(k);
            return it == info.end() ? std::string() : it->second;
        };
        m_about = std::make_unique<Window>(Size{285, 196}, "");
        m_about->setKeyEquivalents(OwnKeys);
        View *c = m_about->contentView();
        auto *icon = c->add<ImageView>();
        icon->image = Image::load(res("Stickies.icns"), {64, 64});
        icon->setFrame({110, 16, 64, 64});
        auto *name = c->add<Label>("Stickies", Font::boldSystem(14));
        name->style.align = Align::Center;
        name->setFrame({0, 94, 285, 18});
        auto *version = c->add<Label>("Version " + get("CFBundleShortVersionString") + " (" + get("CFBundleVersion") + ")", Font::system(10));
        version->style.align = Align::Center;
        version->setFrame({0, 118, 285, 14});
        auto *copyright = c->add<Label>(get("NSHumanReadableCopyright"), Font::system(10));
        copyright->style.align = Align::Center;
        copyright->setFrame({0, 160, 285, 14});
        m_about->onClose = [this] { m_about->close(); };
        m_about->onUnhandledKey = [this](Event &e) { m_menu.performKeyEquivalent(e); };
    }
    m_about->show();
}

bool Controller::validate(const MainMenuItem &it, MenuValidation &v)
{
    const std::string &a = it.action;
    StickyNote *n = mainNote();
    TextView *tv = n ? n->textView() : nullptr;
    // the application's; the menu bar hides and shows its windows
    if (a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
        a == "orderFrontStandardAboutPanel:" || a == "newDocument:" || a == "importText:" || a == "arrangeInFront:")
        return true;
    if (a == "showHelp:")
        return Assets::exists(std::string(Bundle) + "/Contents/Resources/Stickies.help/Contents/Resources/English.lproj/StickiesHelp.html");
    if (a == "makeKeyAndOrderFront:") {
        v.checked = it.tag >= 0 && size_t(it.tag) < m_notes.size() && m_notes[size_t(it.tag)]->isMain();
        return true;
    }
    if (a == "orderFrontFindPanel:")
        return true;
    if (a == "arrange:")
        return !m_notes.empty();
    if (a == "saveAllDocumentsMenuAction:")
        return m_dirty;
    if (!n)
        return false;
    // the note's (-[StickiesWindow validateMenuItem:])
    if (a == "closeWindow:" || a == "performClose:" || a == "exportSticky:" || a == "doZoom:" || a == "useAsDefaultMenuAction:")
        return true;
    if (a == "printDocument:")
        return inPath("lp");
    if (a == "setColorMenuAction:") {
        v.checked = it.tag - 3000 == n->colour();
        return true;
    }
    if (a == "toggleFloatingMenuAction:") {
        v.checked = n->isFloating();
        return true;
    }
    if (a == "toggleTranslucent:") {
        v.checked = n->isTranslucent();
        return true;
    }
    if (a == "toggleShade:") {
        v.title = tr(n->isShaded() ? "DEMINIMIZE" : "MINIMIZE");
        return true;
    }
    // the text's (NSTextView's)
    if (a == "undo:") {
        if (m_arrangeUndo) {
            v.title = "Undo " + tr("ARRANGE");
            return true;
        }
        return tv->canUndo();
    }
    if (a == "redo:")
        return tv->canRedo();
    if (a == "cut:" || a == "copy:" || a == "delete:")
        return tv->hasSelection();
    if (a == "paste:" || a == "pasteAsPlainText:" || a == "selectAll:")
        return true;
    if (a == "findNext:" || a == "findPrevious:")
        return !m_find.findString().empty();
    if (a == "takeFindStringFromSelection:")
        return tv->hasSelection();
    if (a == "jumpToSelection:")
        return true;
    if (a == "addFontTrait:" || a == "underline:" || a == "copyFont:" || a == "superscript:" || a == "subscript:" || a == "unscript:" ||
        a == "raiseBaseline:" || a == "lowerBaseline:" || a == "capitalizeWord:" || a == "lowercaseWord:" || a == "uppercaseWord:")
        return true;
    if (a == "pasteFont:")
        return m_copiedStyle.has_value();
    if (a == "alignLeft:" || a == "alignCenter:" || a == "alignRight:" || a == "alignJustified:")
        return true;
    // spelling, grammar, substitutions, speech, the font and colour panels, kerning, ligatures, links,
    // Page Setup: AppKit's panels and services, not here
    return false;
}

void Controller::perform(const MainMenuItem &it)
{
    const std::string &a = it.action;
    if (a == "terminate:")
        return quit();
    if (a == "orderFrontStandardAboutPanel:")
        return showAbout();
    if (a == "newDocument:") {
        newNote();
        return;
    }
    if (a == "importText:")
        return importText();
    if (a == "showHelp:")
        return openURL(Assets::path(std::string(Bundle) + "/Contents/Resources/Stickies.help/Contents/Resources/English.lproj/StickiesHelp.html"));
    if (a == "arrangeInFront:") {
        for (auto r = m_order.rbegin(); r != m_order.rend(); ++r)
            (*r)->window()->show();
        return;
    }
    if (a == "makeKeyAndOrderFront:") {
        if (it.tag >= 0 && size_t(it.tag) < m_notes.size())
            m_notes[size_t(it.tag)]->show();
        return;
    }
    if (a == "arrange:")
        return arrange(it.tag);
    if (a == "saveAllDocumentsMenuAction:")
        return saveAll();
    if (a == "orderFrontFindPanel:")
        return m_find.show();
    StickyNote *n = mainNote();
    if (!n)
        return;
    TextView *tv = n->textView();
    if (a == "closeWindow:" || a == "performClose:")
        n->performClose();
    else if (a == "exportSticky:")
        n->exportNote([](bool) {});
    else if (a == "printDocument:")
        n->print();
    else if (a == "doZoom:")
        n->zoom();
    else if (a == "toggleShade:")
        n->toggleShade();
    else if (a == "setColorMenuAction:")
        n->setColour(it.tag - 3000);
    else if (a == "toggleFloatingMenuAction:")
        n->setFloating(!n->isFloating());
    else if (a == "toggleTranslucent:")
        n->setTranslucent(!n->isTranslucent());
    else if (a == "useAsDefaultMenuAction:") {
        // -[PreferencesController useAsDefaultMenuAction:]: the font at the insertion point, the
        // expanded size, the colour, the flags but collapsed
        const Style s = tv->styleAtSelection();
        m_prefs.set("FontName", s.postScriptName());
        char size[32];
        std::snprintf(size, sizeof size, "%g", s.size);
        m_prefs.set("FontSize", std::string(size));
        const Size u = n->unshadedSize();
        m_prefs.set("ViewWidth", std::to_string(int(std::lround(u.w))));
        m_prefs.set("ViewHeight", std::to_string(int(std::lround(u.h))));
        m_prefs.set("ColourIndex", n->colour());
        m_prefs.set("WindowFlags", n->flags() & ~Collapsed);
    } else if (a == "undo:") {
        if (m_arrangeUndo)
            undoArrange();
        else
            tv->undo();
    } else if (a == "redo:")
        tv->redo();
    else if (a == "cut:")
        tv->cut();
    else if (a == "copy:")
        tv->copy();
    else if (a == "paste:")
        tv->paste(false);
    else if (a == "pasteAsPlainText:")
        tv->paste(true);
    else if (a == "delete:")
        tv->deleteSelection();
    else if (a == "selectAll:")
        tv->selectAll();
    else if (a == "findNext:")
        m_find.find(true);
    else if (a == "findPrevious:")
        m_find.find(false);
    else if (a == "takeFindStringFromSelection:")
        m_find.useSelection();
    else if (a == "jumpToSelection:")
        m_find.jumpToSelection();
    else if (a == "addFontTrait:") {
        // NSFontManager's tags: 2 bold, 1 italic, toggled from the selection's first character
        const Style at = tv->styleAtSelection();
        if (it.tag == 2)
            tv->changeStyle([b = !at.bold](Style &s) { s.bold = b; });
        else
            tv->changeStyle([i = !at.italic](Style &s) { s.italic = i; });
    } else if (a == "underline:") {
        const bool u = !tv->styleAtSelection().underline;
        tv->changeStyle([u](Style &s) { s.underline = u; });
    } else if (a == "superscript:")
        tv->changeStyle([](Style &s) { s.script = std::min(1, s.script + 1); });
    else if (a == "subscript:")
        tv->changeStyle([](Style &s) { s.script = std::max(-1, s.script - 1); });
    else if (a == "unscript:")
        tv->changeStyle([](Style &s) {
            s.script = 0;
            s.baseline = 0;
        });
    else if (a == "raiseBaseline:")
        tv->changeStyle([](Style &s) { s.baseline += 1; });
    else if (a == "lowerBaseline:")
        tv->changeStyle([](Style &s) { s.baseline -= 1; });
    else if (a == "copyFont:")
        m_copiedStyle = tv->styleAtSelection();
    else if (a == "pasteFont:") {
        if (m_copiedStyle)
            tv->changeStyle([s = *m_copiedStyle](Style &x) {
                const Alignment keep = x.align;
                x = s;
                x.align = keep;
            });
    } else if (a == "uppercaseWord:")
        tv->transform(0);
    else if (a == "lowercaseWord:")
        tv->transform(1);
    else if (a == "capitalizeWord:")
        tv->transform(2);
    else if (a == "alignLeft:")
        tv->setAlignment(Alignment::Left);
    else if (a == "alignCenter:")
        tv->setAlignment(Alignment::Center);
    else if (a == "alignRight:")
        tv->setAlignment(Alignment::Right);
    else if (a == "alignJustified:")
        tv->setAlignment(Alignment::Justified);
}

} // namespace

} // namespace Stickies

int main(int argc, char **argv)
{
    using namespace Stickies;
    App &app = App::shared();
    setProgramName("Stickies");
    if (!app.open()) {
        std::fprintf(stderr, "Stickies: no display (Wayland or X11)\n");
        return 1;
    }
    Assets::registerFonts();
    Assets::registerAllFonts();
    if (!Assets::exists(std::string(Bundle) + "/Contents/Resources/English.lproj/MainMenu.nib")) {
        std::fprintf(stderr, "Stickies: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
        files.push_back(argv[i]);
    static Controller controller;
    controller.start(files);
    // a session ending (SIGTERM) saves first, as quitting does: the signal through a pipe to the loop
    static int quitPipe[2] = {-1, -1};
    if (pipe(quitPipe) == 0) {
        app.watch(quitPipe[0], [] {
            char c;
            if (read(quitPipe[0], &c, 1) > 0)
                controller.quit();
        });
        std::signal(SIGTERM, [](int) {
            const char c = 1;
            if (write(quitPipe[1], &c, 1) < 0) {
            }
        });
    }
    return app.run();
}
