// SPDX-License-Identifier: LGPL-2.1-or-later
#include "store.h"

#include "foundation.h"
#include "text.h"

#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace AK;

namespace iTunes {

namespace {

std::string selfDir()
{
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    std::string p(buf, size_t(n));
    return p.substr(0, p.rfind('/'));
}

std::string storeProgram() { return selfDir() + "/iTunesStore"; }

// AquaKit's modifiers as the process takes them: 1 Shift, 2 Control, 4 Option (Alt), 8 Command (Meta)
int mods(unsigned m)
{
    return ((m & Shift) ? 1 : 0) | ((m & Control) ? 2 : 0) | ((m & Option) ? 4 : 0) | ((m & Command) ? 8 : 0);
}

std::string hex(const std::string &s)
{
    static const char *d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

// A plist's XML as a tree (addProtocol's): dictionaries, arrays, strings, booleans
struct Node {
    enum Kind { Dict, Array, String, True, False } kind = String;
    std::string text;
    std::vector<std::pair<std::string, Node>> entries;   // Dict
    std::vector<Node> items;                              // Array
    const Node *get(const std::string &k) const
    {
        for (const auto &[key, v] : entries)
            if (key == k)
                return &v;
        return nullptr;
    }
    std::string str(const std::string &k) const
    {
        const Node *n = get(k);
        return n && n->kind == String ? n->text : std::string();
    }
};

std::string unescape(std::string v)
{
    for (auto [ent, ch] : {std::pair<const char *, const char *>{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}}) {
        size_t p = 0;
        while ((p = v.find(ent, p)) != std::string::npos)
            v.replace(p, std::strlen(ent), ch), p += std::strlen(ch);
    }
    return v;
}

// The element at `at` (after whitespace): its node, `at` past it
bool parseNode(const std::string &x, size_t &at, Node &out)
{
    at = x.find('<', at);
    if (at == std::string::npos)
        return false;
    const size_t close = x.find('>', at);
    if (close == std::string::npos)
        return false;
    std::string tag = x.substr(at + 1, close - at - 1);
    const bool empty = !tag.empty() && tag.back() == '/';
    if (empty)
        tag.pop_back();
    at = close + 1;
    if (tag == "true" || tag == "false") {
        out.kind = tag == "true" ? Node::True : Node::False;
        if (!empty)
            at = x.find('>', at) + 1;
        return true;
    }
    if (tag == "dict" || tag == "array") {
        out.kind = tag == "dict" ? Node::Dict : Node::Array;
        if (empty)
            return true;
        const std::string end = "</" + tag + ">";
        while (true) {
            const size_t next = x.find('<', at);
            if (next == std::string::npos)
                return false;
            if (x.compare(next, end.size(), end) == 0) {
                at = next + end.size();
                return true;
            }
            if (out.kind == Node::Dict) {
                const size_t ks = x.find("<key>", next), ke = x.find("</key>", next);
                if (ks != next || ke == std::string::npos)
                    return false;
                std::string key = unescape(x.substr(ks + 5, ke - ks - 5));
                at = ke + 6;
                Node v;
                if (!parseNode(x, at, v))
                    return false;
                out.entries.emplace_back(std::move(key), std::move(v));
            } else {
                at = next;
                Node v;
                if (!parseNode(x, at, v))
                    return false;
                out.items.push_back(std::move(v));
            }
        }
    }
    // a string, an integer, a real, a date: the text up to its end tag
    out.kind = Node::String;
    if (empty)
        return true;
    const size_t e = x.find("</" + tag + ">", at);
    if (e == std::string::npos)
        return false;
    out.text = unescape(x.substr(at, e - at));
    at = e + tag.size() + 3;
    return true;
}

StoreMenu menuOf(const Node &n)
{
    StoreMenu m;
    m.title = n.str("title");
    m.url = n.str("url");
    const Node *d = n.get("divider");
    m.divider = d && d->kind == Node::True;
    if (const Node *items = n.get("items"))
        for (const Node &i : items->items)
            m.items.push_back(menuOf(i));
    return m;
}

} // namespace

StoreView::StoreView() = default;

StoreView::~StoreView()
{
    if (m_task)
        m_task->terminate();
    if (!m_frameFile.empty())
        unlink(m_frameFile.c_str());
}

bool StoreView::available() { return access(storeProgram().c_str(), X_OK) == 0; }

void StoreView::start()
{
    if (m_task)
        return;
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    m_frameFile = std::string(rt && *rt ? rt : "/tmp") + "/itunes-store-" + std::to_string(getpid());
    const int fd = ::open(m_frameFile.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
        ::close(fd);
    m_task = std::make_unique<Task>();
    m_task->keepInput = true;
    m_task->errors = Task::Errors::Discard;
    // the system the requests name: the user's files' (SystemVersion.plist)
    auto sys = Assets::plistStrings("/System/Library/CoreServices/SystemVersion.plist");
    if (!sys["ProductVersion"].empty())
        m_task->environment["SNOWLEOPARD_SYSTEM_VERSION"] = sys["ProductVersion"];
    if (!sys["ProductBuildVersion"].empty())
        m_task->environment["SNOWLEOPARD_SYSTEM_BUILD"] = sys["ProductBuildVersion"];
    m_task->onLine = [this](const std::string &l) { line(l); };
    m_task->onExit = [this](int, const std::string &) {
        m_loading = false;
        if (onNavigation)
            onNavigation();
    };
    if (!m_task->launch({storeProgram(), m_frameFile})) {
        m_task.reset();
        return;
    }
    send("size " + std::to_string(int(frame().w)) + " " + std::to_string(int(frame().h)));
}

void StoreView::send(const std::string &l)
{
    if (m_task && m_task->isRunning())
        m_task->write(l + "\n");
}

void StoreView::load(const std::string &url)
{
    start();
    send("load " + url);
}

void StoreView::command(const std::string &c) { send(c); }

void StoreView::setFrame(const Rect &r)
{
    const bool resized = r.w != frame().w || r.h != frame().h;
    View::setFrame(r);
    if (resized && r.w > 0 && r.h > 0)
        send("size " + std::to_string(int(r.w)) + " " + std::to_string(int(r.h)));
}

void StoreView::line(const std::string &l)
{
    if (l.rfind("frame ", 0) == 0) {
        int w = 0, h = 0;
        if (std::sscanf(l.c_str() + 6, "%d %d", &w, &h) == 2 && w > 0 && h > 0) {
            const size_t bytes = size_t(w) * h * 4;
            const int fd = ::open(m_frameFile.c_str(), O_RDONLY);
            if (fd >= 0) {
                struct stat st {};
                if (fstat(fd, &st) == 0 && size_t(st.st_size) >= bytes) {
                    void *map = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
                    if (map != MAP_FAILED) {
                        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
                        cairo_surface_flush(s);
                        unsigned char *dst = cairo_image_surface_get_data(s);
                        const int stride = cairo_image_surface_get_stride(s);
                        for (int y = 0; y < h; ++y)
                            std::memcpy(dst + size_t(y) * stride, static_cast<const char *>(map) + size_t(y) * w * 4, size_t(w) * 4);
                        cairo_surface_mark_dirty(s);
                        munmap(map, bytes);
                        m_frame = Image(s);
                        setNeedsDisplay();
                    }
                }
                ::close(fd);
            }
        }
        send("ack");
    } else if (l.rfind("nav ", 0) == 0) {
        int b = 0, f = 0;
        std::sscanf(l.c_str() + 4, "%d %d", &b, &f);
        m_back = b;
        m_forward = f;
        if (onNavigation)
            onNavigation();
    } else if (l.rfind("loading ", 0) == 0) {
        m_loading = l[8] == '1';
        if (onNavigation)
            onNavigation();
    } else if (l.rfind("protocol ", 0) == 0) {
        std::string x = l.substr(9), out;
        for (size_t i = 0; i < x.size(); ++i) {
            if (x[i] == '\\' && i + 1 < x.size()) {
                out += x[i + 1] == 'n' ? '\n' : x[i + 1];
                ++i;
            } else {
                out += x[i];
            }
        }
        protocol(out);
    } else if (l.rfind("cursor ", 0) == 0) {
        const std::string c = l.substr(7);
        m_cursor = c == "hand" ? "pointer" : c == "text" ? "text" : "default";
    } else if (l == "library") {
        if (onLibrary)
            onLibrary();
    } else if (l.rfind("browser ", 0) == 0) {
        Task::launchDetached({"xdg-open", l.substr(8)});
    }
}

// addProtocol's plist (StoreiTunesJSObject's addProtocol, BWStoreNavBarController): navbar ▸ menus
// (title, url, …) and navbar-selected-menu
void StoreView::protocol(const std::string &xml)
{
    Node root;
    size_t at = 0;
    if (!parseNode(xml, at, root) || root.kind != Node::Dict)
        return;
    if (const Node *nav = root.get("navbar")) {
        if (const Node *menus = nav->get("menus")) {
            std::vector<StoreMenu> found;
            for (const Node &m : menus->items)
                found.push_back(menuOf(m));
            if (!found.empty())
                m_menus = found;
        }
    }
    const std::string sel = root.str("navbar-selected-menu");
    if (!sel.empty())
        m_selected = sel;
    if (onMenus)
        onMenus(m_menus, m_selected);
}

void StoreView::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(242));
    if (!m_frame.isNull())
        m_frame.draw(cr, Point{0, 0});
}

void StoreView::mouse(const char *kind, const Event &e)
{
    send(std::string("mouse ") + kind + " " + std::to_string(int(e.location.x)) + " " + std::to_string(int(e.location.y)) + " 1 " +
         std::to_string(e.clickCount) + " " + std::to_string(mods(e.modifiers)));
}

void StoreView::mouseDown(Event &e)
{
    if (Window *w = window())
        w->makeFirstResponder(this);
    mouse("down", e);
}
void StoreView::mouseDragged(Event &e) { mouse("move", e); }
void StoreView::mouseUp(Event &e) { mouse("up", e); }
void StoreView::mouseMoved(Event &e) { mouse("move", e); }

void StoreView::scrollWheel(Event &e)
{
    send("wheel " + std::to_string(int(e.location.x)) + " " + std::to_string(int(e.location.y)) + " 0 " + std::to_string(-e.deltaY));
}

void StoreView::keyDown(Event &e)
{
    if (e.modifiers & Command) {   // the menus' keys stay the window's
        View::keyDown(e);
        return;
    }
    const std::string base = std::to_string(e.keysym) + " " + std::to_string(mods(e.modifiers)) + " " + hex(e.text);
    send("key down " + base);
    send("key up " + base);
}

void StoreView::focusChanged() { send(std::string("focus ") + (isFirstResponder() ? "1" : "0")); }

} // namespace iTunes
