// SPDX-License-Identifier: LGPL-2.1-or-later
#include "infowindow.h"

#include "dsstore.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"
#include "text.h"

#include <ftw.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }
std::string core(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "LocalizableCore", key, fallback); }

std::string format(std::string f, const std::vector<std::string> &values)
{
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string mark = "^" + std::to_string(i);
        for (size_t at; (at = f.find(mark)) != std::string::npos;)
            f.replace(at, mark.size(), values[i]);
    }
    return f;
}

// "2,494,464"
std::string grouped(long long n)
{
    std::string digits = std::to_string(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

std::string bytes(long long n)
{
    // LocalizableCore U2 "Zero bytes", U4 "^0 bytes"
    return n == 0 ? core("U2", "Zero bytes") : format(core("U4", "^0 bytes"), {grouped(n)});
}

void walk(View *v, const std::function<void(View *)> &f)
{
    f(v);
    for (View *c : v->subviews())
        walk(c, f);
}

// a folder's size, counted on a thread of its own: the bytes, the bytes on disk, the items
struct Sizing {
    std::atomic<bool> done{false};
    long long logical = 0, disk = 0, items = 0;
};

thread_local Sizing *g_sizing = nullptr;

int countEntry(const char *, const struct stat *st, int type, struct FTW *)
{
    if (type == FTW_F || type == FTW_SL) {
        g_sizing->logical += st->st_size;
        g_sizing->disk += (long long)st->st_blocks * 512;
    }
    ++g_sizing->items;
    return 0;
}

std::vector<std::unique_ptr<InfoWindow>> &windows()
{
    static std::vector<std::unique_ptr<InfoWindow>> list;
    return list;
}

// the item's comment (Spotlight Comments): its folder's .DS_Store 'cmmt', as the Finder keeps it
std::string comment(const Item &item)
{
    DSStore::Store store(dirName(item.path).empty() ? "/" : dirName(item.path));
    const DSStore::Record *r = store.get(item.name, "cmmt");
    return r ? r->data : std::string();
}

void setComment(const Item &item, const std::string &text)
{
    DSStore::Store store(dirName(item.path).empty() ? "/" : dirName(item.path));
    if (text.empty()) {
        store.remove(item.name, "cmmt");
    } else {
        DSStore::Record r;
        r.name = item.name;
        r.code = "cmmt";
        r.type = "ustr";
        r.data = text;
        store.set(r);
    }
    store.save();
}

// xdg-mime: the item's type and the application the desktop opens it with (Open with)
std::string run(const std::string &command)
{
    std::string out;
    if (FILE *p = popen(command.c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, p))
            out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string quoted(const std::string &s)
{
    std::string q = "'";
    for (char c : s)
        q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

std::string desktopName(const std::string &desktopId)
{
    for (const std::string &dir : {home() + "/.local/share/applications/", std::string("/usr/share/applications/")}) {
        std::ifstream in(dir + desktopId);
        std::string line;
        bool entry = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] == '[')
                entry = line == "[Desktop Entry]";
            else if (entry && line.rfind("Name=", 0) == 0)
                return line.substr(5);
        }
    }
    return {};
}

} // namespace

// TLabelView: "x" then the seven labels' colours (Red, Orange, Yellow, Green, Blue, Purple, Gray)
class LabelSwatches : public View {
public:
    LabelSwatches() { setFrame({0, 0, 153, 32}); }
    void draw(cairo_t *cr) override
    {
        static const Color colors[] = {Color::rgb(252, 107, 107), Color::rgb(251, 171, 86),  Color::rgb(247, 227, 87),
                                       Color::rgb(182, 221, 92),  Color::rgb(102, 166, 251), Color::rgb(204, 140, 226),
                                       Color::rgb(183, 183, 183)};
        const double size = 12, gap = 4, y = 32 - 14 + 1;   // on the "Label:" title's line, the view's last
        // "none": a small x
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, 3, y + 3);
        cairo_line_to(cr, 9, y + 9);
        cairo_move_to(cr, 9, y + 3);
        cairo_line_to(cr, 3, y + 9);
        cairo_stroke(cr);
        for (int i = 0; i < 7; ++i) {
            const double x = 16 + i * (size + gap);
            cairo_new_sub_path(cr);
            cairo_arc(cr, x + size / 2, y + size / 2, size / 2, 0, 2 * M_PI);
            colors[i].set(cr);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
    }
};

struct InfoWindow::Slice {
    std::string nib;
    std::unique_ptr<Nib> loaded;
    View *view = nullptr;
    std::map<View *, Rect> original;   // the rows where the nib put them
    double disclosed = 20, margin = 8, height = 0;
    bool expanded = false;
    std::string prefKey;
    View *toggle = nullptr;

    // a row's title ("Kind:") and the field at its right
    Label *title(const std::string &text) const
    {
        for (View *c : view->subviews())
            if (auto *l = dynamic_cast<Label *>(c); l && l->text() == text)
                return l;
        return nullptr;
    }
    View *valueOf(View *t) const
    {
        if (!t)
            return nullptr;
        const Rect tf = original.at(t);
        for (View *c : view->subviews())
            if (c != t && std::abs(original.at(c).y - tf.y) < 3 && original.at(c).x > tf.x)
                return c;
        return nullptr;
    }
    void row(const std::string &titleText, const std::string &value)
    {
        Label *t = title(titleText);
        View *v = valueOf(t);
        if (!t)
            return;
        t->setHidden(value.empty());
        if (v) {
            v->setHidden(value.empty());
            if (auto *l = dynamic_cast<Label *>(v)) {
                // a value too long for its line wraps onto more (the rows under it move down)
                l->setText(value);
                const Rect o = original.at(v);
                l->wrap = textWidth(value, l->style.font) > o.w;
                const double h = l->wrap ? std::ceil(wrappedHeight(value, o.w, l->style.font)) : o.h;
                l->setFrame({o.x, l->frame().y, o.w, std::max(o.h, h)});
            }
        }
    }
    View *button(const std::string &text) const
    {
        for (View *c : view->subviews()) {
            if (auto *b = dynamic_cast<Button *>(c); b && b->title == text)
                return b;
            if (auto *b = dynamic_cast<CheckBox *>(c); b && b->title == text)
                return b;
        }
        return nullptr;
    }
};

void InfoWindow::show(const Item &item)
{
    for (auto &w : windows())
        if (w->m_item.path == item.path) {
            w->m_window->show();
            return;
        }
    windows().push_back(std::unique_ptr<InfoWindow>(new InfoWindow(item)));
}

void InfoWindow::showInTimeMachine(const Item &item)
{
    show(item);
    for (auto &w : windows())
        if (w->m_item.path == item.path) {
            w->m_window->setFloating(true);   // setLevel: _BUGetTimeMachineWindowLevel()
            w->m_window->show();
        }
}

void InfoWindow::leaveTimeMachine()
{
    for (auto &w : windows())
        if (w->m_window->isFloating())
            w->m_window->setFloating(false);
}

void InfoWindow::closeAll()
{
    for (auto &w : windows())
        w->m_window->close();
}

InfoWindow::InfoWindow(const Item &item) : m_item(item)
{
    // "^0 Info"
    m_window = std::make_unique<Window>(Size{265, 400}, format(tr("IN1", "^0 Info"), {item.name}), Window::Titled);
    m_window->setIcon(icon(item, 128));
    m_window->onClose = [this] {
        if (m_timer)
            App::shared().removeTimer(m_timer);
        m_timer = 0;
        App::shared().later([this] {
            auto &list = windows();
            list.erase(std::remove_if(list.begin(), list.end(), [this](const auto &p) { return p.get() == this; }), list.end());
        });
    };
    build();
    layout();
    m_window->show();
}

void InfoWindow::build()
{
    struct Spec {
        const char *nib;
        double disclosed, margin;
        bool expanded;
        const char *pref;
    };
    const bool folder = m_item.folder;
    std::vector<Spec> specs = {
        {"InfoWindowHeaderView", 74, 5, false, "InfoCommentsExpanded"},
        {"InfoWindowGeneralView", 20, 8, true, "InfoGeneralExpanded"},
        {"InfoWindowMoreInfoView", 20, 8, false, "InfoMoreInfoExpanded"},
        {"InfoWindowNameView", 20, 8, false, "InfoNameExpanded"},
        {"InfoWindowOpenWithView", 20, 1, false, "InfoOpenWithExpanded"},
        {"InfoWindowPreviewView", 20, 8, true, "InfoPreviewExpanded"},
        {"InfoWindowPermissionsView", 20, 8, false, "InfoPermissionsExpanded"},
    };
    if (folder)   // Open with: documents and applications only
        specs.erase(specs.begin() + 4);
    Settings prefs("finder");
    const std::string res = std::string(Bundle) + "/Contents/Resources/";
    for (const Spec &spec : specs) {
        auto s = std::make_unique<Slice>();
        s->nib = spec.nib;
        s->loaded = std::make_unique<Nib>();
        if (!s->loaded->load(Assets::path(res + "English.lproj/" + spec.nib + ".nib/keyedobjects.nib"), Assets::path(res)))
            continue;
        s->view = s->loaded->instantiate("view");
        if (!s->view)
            continue;
        s->view->clipsToBounds = true;
        for (View *c : s->view->subviews())
            s->original[c] = c->frame();
        s->disclosed = spec.disclosed;
        s->margin = spec.margin;
        s->prefKey = spec.pref;
        s->expanded = prefs.boolean(spec.pref, spec.expanded);
        const auto toggles = s->loaded->withAction("toggleExpanded:");
        s->toggle = toggles.empty() ? nullptr : toggles.front();
        Slice *raw = s.get();
        if (auto *b = dynamic_cast<Button *>(s->toggle)) {
            b->open = s->expanded;
            b->action = [this, raw, b] {
                raw->expanded = !raw->expanded;
                b->open = raw->expanded;
                Settings("finder").set(raw->prefKey, raw->expanded);
                layout();
            };
        }
        m_window->contentView()->addSubview(s->view);
        m_slices.push_back(std::move(s));
    }
    for (auto &s : m_slices) {
        if (s->nib == "InfoWindowHeaderView")
            fillHeader(*s);
        else if (s->nib == "InfoWindowGeneralView")
            fillGeneral(*s);
        else if (s->nib == "InfoWindowMoreInfoView")
            fillMoreInfo(*s);
        else if (s->nib == "InfoWindowNameView")
            fillName(*s);
        else if (s->nib == "InfoWindowOpenWithView")
            fillOpenWith(*s);
        else if (s->nib == "InfoWindowPreviewView")
            fillPreview(*s);
        else if (s->nib == "InfoWindowPermissionsView")
            fillPermissions(*s);
    }
    if (folder)
        startSizing();
}

void InfoWindow::fillHeader(Slice &s)
{
    struct stat st {};
    lstat(m_item.path.c_str(), &st);
    for (View *c : s.view->subviews()) {
        if (auto *img = dynamic_cast<ImageView *>(c))
            img->image = icon(m_item, 32);
        // the name and the size: by their places in the nib (their texts change)
        if (auto *l = dynamic_cast<Label *>(c)) {
            const Rect r = s.original.at(c);
            if (r.y < 20 && r.x < 100)   // the top row: the name at the left, the size at the right
                l->setText(m_item.name);
            else if (r.y < 20 && r.x > 200 && !(m_item.folder && l->text() != "--"))
                l->setText(m_item.folder ? std::string("--") : formatBytes(sizeOnDisk(m_item)));
        }
    }
    s.row("Modified: ", formatDate(st.st_mtime, DateLong, true));
    // Spotlight Comments: a field where the nib has its text view (not in AquaKit), saved in the
    // folder's .DS_Store as the Finder does
    for (View *c : s.view->subviews()) {
        const Rect r = s.original.at(c);
        if (r.h == 46 && r.w == 257) {   // TFocusRingScrollView
            auto *f = new TextField;
            f->setFrame(r.inset(2, 12));
            f->setText(comment(m_item));
            const Item item = m_item;
            f->onChange = [item](const std::string &t) { setComment(item, t); };
            c->addSubview(f);
            f->setFrame({2, 12, r.w - 4, 22});
        }
    }
}

void InfoWindow::fillGeneral(Slice &s)
{
    struct stat st {}, lst {};
    stat(m_item.path.c_str(), &st);
    lstat(m_item.path.c_str(), &lst);
    s.row("Kind:", S_ISLNK(lst.st_mode) ? launchServicesAlias() : kind(m_item));
    if (!m_item.folder)
        s.row("Size:", format(tr("IN_G1", "^0 on disk (^1)"), {formatBytes(sizeOnDisk(m_item)), bytes(st.st_size)}));
    else
        s.row("Size:", tr("IN_G7", "Calculating Size"));
    const std::string where = dirName(m_item.path);
    s.row("Where:", where.empty() ? "/" : where);
    s.row("Created:", formatDate(creationTime(m_item), DateLong, false));
    s.row("Modified:", formatDate(st.st_mtime, DateLong, false));
    // an alias (a symbolic link here): its original
    if (S_ISLNK(lst.st_mode)) {
        char target[4096];
        const ssize_t n = readlink(m_item.path.c_str(), target, sizeof target - 1);
        s.row("Original:", n > 0 ? std::string(target, size_t(n)) : std::string());
    } else {
        s.row("Original:", {});
    }
    // a disk: its format, capacity, free and used space
    bool volume = false;
    for (const Volume &v : volumes())
        if (v.path == m_item.path)
            volume = true;
    if (volume) {
        struct statvfs vs {};
        statvfs(m_item.path.c_str(), &vs);
        const long long cap = (long long)vs.f_blocks * vs.f_frsize, avail = (long long)vs.f_bavail * vs.f_frsize;
        std::string fstype;
        std::ifstream mounts("/proc/self/mounts");
        std::string dev, point, type;
        while (mounts >> dev >> point >> type) {
            if (point == m_item.path)
                fstype = type;
            mounts.ignore(1 << 16, '\n');
        }
        auto amount = [&](long long n) { return formatBytes(n) + " (" + bytes(n) + ")"; };
        s.row("Format:", fstype);
        s.row("Capacity:", amount(cap));
        s.row("Available:", amount(avail));
        s.row("Used:", amount(cap - avail));
    } else {
        for (const char *t : {"Format:", "Capacity:", "Available:", "Used:"})
            s.row(t, {});
    }
    // what the Mac has and Linux doesn't: hidden, as the Finder hides them when they don't apply
    for (const char *t : {"Server:", "Disk image:", "Version:", "Query:", "Processor:", "Memory:", "Startup Disk:", "Full Name:",
                          "Copyright:", "Encoding:"})
        s.row(t, {});
    for (View *c : s.view->subviews()) {
        const Rect r = s.original.at(c);
        // the message line (y 221 in the nib, under the startup disk), the encoding's pop-up
        if ((r.w == 241 && r.h == 14) || dynamic_cast<PopUpButton *>(c))
            c->setHidden(true);
        // TLabelView: the labels' swatches (none chosen: Linux has no Finder labels)
        if (r.w == 153 && r.h == 32)
            c->addSubview(new LabelSwatches);
    }
    for (const char *t : {"Select New Original…", "Open in 32-bit mode", "Open using Rosetta"})
        if (View *b = s.button(t))
            b->setHidden(true);
    if (View *b = s.button("Shared folder")) {
        b->setHidden(!m_item.folder);
        b->setEnabled(false);
    }
    if (View *b = s.button("Stationery pad")) {
        b->setHidden(m_item.folder);
        b->setEnabled(false);
    }
    if (View *b = s.button("Locked"))
        b->setEnabled(false);
}

std::string InfoWindow::launchServicesAlias()
{
    return Assets::tr("/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework", "Localized", "Alias",
                      "Alias");
}

void InfoWindow::fillMoreInfo(Slice &s)
{
    // Spotlight's attributes: the last time it was opened (and a picture's size)
    struct stat st {};
    stat(m_item.path.c_str(), &st);
    for (View *c : s.view->subviews()) {
        if (dynamic_cast<ProgressIndicator *>(c))
            c->setHidden(true);
        if (auto *l = dynamic_cast<Label *>(c); l && l->text() == "Fetching…")
            l->setHidden(true);
    }
    for (View *c : s.view->subviews()) {
        const Rect r = s.original.at(c);
        if (r.w == 241 && r.h == 14 && !dynamic_cast<Label *>(c)) {   // TSpotlightMetaDataGroupView
            std::vector<std::pair<std::string, std::string>> rows;
            const Size px = Image::sizeOf(m_item.path);
            if (!m_item.folder && !px.empty())
                rows.push_back({"Dimensions:", std::to_string(int(px.w)) + " × " + std::to_string(int(px.h))});
            rows.push_back({"Last opened:", formatDate(st.st_atime, DateLong, false)});
            for (size_t i = 0; i < rows.size(); ++i) {
                auto *t = new Label(rows[i].first, Font::system(11));
                t->style.align = Align::Right;
                t->style.color = Color::gray(0);
                t->setFrame({0, double(i * 14), 86, 14});
                auto *v = new Label(rows[i].second, Font::system(11));
                v->setFrame({88, double(i * 14), r.w - 88, 14});
                c->addSubview(t);
                c->addSubview(v);
            }
            c->setFrame({r.x, r.y, r.w, double(rows.size() * 14)});
            s.original[c] = c->frame();
        }
    }
    // _noDataTextField ("--"): only without attributes
    walk(s.view, [](View *v) {
        if (auto *l = dynamic_cast<Label *>(v); l && l->text() == "--")
            l->setHidden(true);
    });
}

void InfoWindow::fillName(Slice &s)
{
    for (View *c : s.view->subviews()) {
        if (auto *f = dynamic_cast<TextField *>(c)) {
            f->setText(m_item.name);
            // Return renames the item (the name field's action)
            f->onEnter = [this, f] {
                const std::string name = f->text;
                if (name.empty() || name == m_item.name || name.find('/') != std::string::npos)
                    return;
                const std::string dir = dirName(m_item.path).empty() ? "/" : dirName(m_item.path);
                const std::string to = dir + (dir == "/" ? "" : "/") + name;
                if (isDir(to) || isFile(to) || std::rename(m_item.path.c_str(), to.c_str()) != 0) {
                    f->setText(m_item.name);
                    return;
                }
                m_item.name = name;
                m_item.path = to;
                m_window->setTitle(format(tr("IN1", "^0 Info"), {name}));
                for (auto &sl : m_slices)
                    if (sl->nib == "InfoWindowHeaderView")
                        fillHeader(*sl);
                if (onRenamed)
                    onRenamed(dir);
            };
        }
    }
    if (View *b = s.button("Hide extension"))
        b->setEnabled(false);
}

void InfoWindow::fillOpenWith(Slice &s)
{
    // the application xdg-mime says opens it (the Mac's: LaunchServices' default for its type)
    const std::string type = run("xdg-mime query filetype " + quoted(m_item.path) + " 2>/dev/null");
    const std::string desktop = type.empty() ? std::string() : run("xdg-mime query default " + quoted(type) + " 2>/dev/null");
    std::string name = desktop.empty() ? std::string() : desktopName(desktop);
    if (name.empty())
        name = tr("IN_A4", "Unknown");
    for (View *c : s.view->subviews())
        if (auto *p = dynamic_cast<PopUpButton *>(c)) {
            p->items = {{name, false, true, false}};
            p->setSelected(0);
        }
    if (View *b = s.button("Change All…"))
        b->setEnabled(false);
}

void InfoWindow::fillPreview(Slice &s)
{
    // QLPreviewView: the item large (its preview when it's a picture)
    for (View *c : s.view->subviews()) {
        const Rect r = s.original.at(c);
        if (r.w == 235 && r.h == 128) {
            auto *img = new ImageView;
            const Image pic = icon(m_item, 128, true);
            const Size fit = scaledToFit(pic.size(), {r.h, r.h});
            img->image = pic;
            img->setFrame({std::floor((r.w - fit.w) / 2), std::floor((r.h - fit.h) / 2), fit.w, fit.h});
            c->addSubview(img);
        }
    }
}

void InfoWindow::fillPermissions(Slice &s)
{
    struct stat st {};
    stat(m_item.path.c_str(), &st);
    const bool canRead = access(m_item.path.c_str(), R_OK) == 0, canWrite = access(m_item.path.c_str(), W_OK) == 0;
    // "You can read and write" (IN_S7, S8, S9, S12)
    for (View *c : s.view->subviews())
        if (auto *l = dynamic_cast<Label *>(c); l && l->text() == "You can only read")
            l->setText(canRead && canWrite ? tr("IN_S7", "You can read and write")
                       : canRead           ? tr("IN_S8", "You can only read")
                       : canWrite          ? tr("IN_S9", "You can only write (Drop Box)")
                                           : tr("IN_S12", "You have no access"));
    auto privilege = [](bool r, bool w) {
        return r && w ? tr("IN_S1", "Read & Write") : r ? tr("IN_S2", "Read Only") : w ? tr("IN_S3", "Write Only (Drop Box)") : tr("IN_S4", "No Access");
    };
    const passwd *pw = getpwuid(st.st_uid);
    const group *gr = getgrgid(st.st_gid);
    std::string owner = pw ? pw->pw_name : std::to_string(st.st_uid);
    if (st.st_uid == getuid())
        owner += tr("IN_S25", " (Me)");
    for (View *c : s.view->subviews()) {
        if (auto *t = dynamic_cast<TableView *>(c)) {
            if (t->columns.size() >= 2) {
                t->columns[0].title = tr("IN_S21", "Name");
                t->columns[1].title = tr("IN_S21.1", "Privilege");
            }
            t->rows = {
                {owner, privilege(st.st_mode & S_IRUSR, st.st_mode & S_IWUSR)},
                {gr ? gr->gr_name : std::to_string(st.st_gid), privilege(st.st_mode & S_IRGRP, st.st_mode & S_IWGRP)},
                {"everyone", privilege(st.st_mode & S_IROTH, st.st_mode & S_IWOTH)},
            };
            t->reload();
        }
        // the lock, + and -, the action menu (their template images): changing them needs an
        // administrator, not yet
        const Rect r = s.original.at(c);
        if (auto *b = dynamic_cast<Button *>(c); b && c != s.toggle && b->title.empty()) {
            b->image = CoreUI::named(r.x < 25 ? "Add" : r.x < 50 ? "Remove" : "LockLocked");
            b->setEnabled(r.x > 200);   // the lock opens nothing yet, but it is the lock
        }
        if (auto *p = dynamic_cast<PopUpButton *>(c)) {
            p->setHidden(true);
            auto *gear = new Button({}, Button::SmallSquare);
            gear->image = CoreUI::named("Action");
            gear->setFrame(r);
            gear->setEnabled(false);
            s.view->addSubview(gear);
            s.original[gear] = r;
        }
    }
    if (View *b = s.button("Ignore ownership on this volume"))
        b->setHidden(true);
}

void InfoWindow::startSizing()
{
    // TFolderSizingThread: the folder's size counted apart, "Calculating Size" until then
    auto sizing = std::make_shared<Sizing>();
    const std::string path = m_item.path;
    std::thread([sizing, path] {
        g_sizing = sizing.get();
        nftw(path.c_str(), countEntry, 32, FTW_PHYS);
        --sizing->items;   // not the folder itself
        sizing->done = true;
    }).detach();
    m_timer = App::shared().addTimer(0.25, true, [this, sizing] {
        if (!sizing->done)
            return;
        App::shared().removeTimer(m_timer);
        m_timer = 0;
        const std::string size = format(tr("IN_G4", "^0 on disk (^1) for ^2 items"),
                                        {formatBytes(sizing->disk), bytes(sizing->logical), grouped(sizing->items)});
        for (auto &s : m_slices) {
            if (s->nib == "InfoWindowGeneralView")
                s->row("Size:", size);
            if (s->nib == "InfoWindowHeaderView")
                for (View *c : s->view->subviews())
                    if (auto *l = dynamic_cast<Label *>(c); l && s->original.at(c).y < 20 && s->original.at(c).x > 200)
                        l->setText(formatBytes(sizing->disk));
        }
        layout();
    });
}

void InfoWindow::pack(Slice &s)
{
    // the rows top down: a row with nothing shown gives its height to the rows under it
    std::vector<View *> views;
    for (View *c : s.view->subviews())
        if (s.original.count(c))
            views.push_back(c);
    std::sort(views.begin(), views.end(), [&](View *a, View *b) { return s.original.at(a).y < s.original.at(b).y; });
    std::vector<std::vector<View *>> rows;
    for (View *v : views) {
        if (dynamic_cast<Box *>(v) && s.original.at(v).h <= 5)
            continue;   // the separator: at the bottom
        if (rows.empty() || std::abs(s.original.at(rows.back().front()).y - s.original.at(v).y) >= 3)
            rows.push_back({});
        rows.back().push_back(v);
    }
    double shift = 0, bottom = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool shown = std::any_of(rows[i].begin(), rows[i].end(), [](View *v) { return !v->isHidden(); });
        if (!shown) {
            const double next = i + 1 < rows.size() ? s.original.at(rows[i + 1].front()).y : s.original.at(rows[i].front()).y;
            shift += next - s.original.at(rows[i].front()).y;
            continue;
        }
        double grown = 0;
        for (View *v : rows[i]) {
            Rect r = s.original.at(v);
            r.y -= shift;
            v->setFrame({r.x, r.y, v->frame().w, v->frame().h});
            if (!v->isHidden()) {
                bottom = std::max(bottom, r.y + v->frame().h);
                grown = std::max(grown, v->frame().h - s.original.at(v).h);
            }
        }
        shift -= grown;
    }
    s.height = s.expanded ? std::ceil(bottom + s.margin) : s.disclosed;
    for (View *v : views)
        if (dynamic_cast<Box *>(v) && s.original.at(v).h <= 5)
            v->setFrame({0, s.height - 3, s.original.at(v).w, s.original.at(v).h});
}

void InfoWindow::layout()
{
    double y = 0;
    for (auto &s : m_slices) {
        pack(*s);
        s->view->setFrame({0, y, 265, s->height});
        y += s->height;
    }
    m_window->setContentSize({265, y});
    m_window->contentView()->setNeedsDisplay();
}

} // namespace Finder
