// SPDX-License-Identifier: LGPL-2.1-or-later
#include "quicklook.h"

#include "controls.h"
#include "foundation.h"
#include "text.h"

#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <fstream>

#ifdef FINDER_POPPLER
#include <poppler.h>
#endif

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string core(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "LocalizableCore", key, fallback); }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string extensionOf(const std::string &name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot + 1));
}

bool isPicture(const std::string &ext)
{
    static const char *const kinds[] = {"png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff", "icns", "webp", "ico", "xpm", "svg"};
    return std::find(std::begin(kinds), std::end(kinds), ext) != std::end(kinds);
}

bool isText(const std::string &path, const std::string &ext)
{
    static const char *const kinds[] = {"txt", "text", "md", "c", "h", "cpp", "hpp", "cc", "py", "sh", "js", "json", "xml", "html", "htm", "css",
                                        "log", "ini", "conf", "cfg", "plist", "strings", "rtf", "csv", "yml", "yaml", "toml", "rs", "go", "java",
                                        "m", "mm", "pl", "rb", "lua", "tex", "desktop"};
    if (std::find(std::begin(kinds), std::end(kinds), ext) != std::end(kinds))
        return true;
    if (!ext.empty())
        return false;
    // no extension: text when its first bytes are (README, Makefile...)
    std::ifstream in(path, std::ios::binary);
    char buf[512];
    in.read(buf, sizeof buf);
    const std::streamsize n = in.gcount();
    if (n <= 0)
        return false;
    for (std::streamsize i = 0; i < n; ++i)
        if (buf[i] == 0)
            return false;
    return true;
}

// ---- the panel's content: the HUD's dark rounded body, its title, its close button

class Panel : public View {
public:
    static constexpr double TitleHeight = 22, Margin = 10, BottomHeight = 34;
    std::vector<Item> items;
    int index = 0;
    std::function<void()> onClose;
    std::function<void(int)> onIndex;

    // what is drawn of the item, made once
    Image picture;
    std::vector<std::string> textLines;
    enum Mode { Icon, Picture, Text } mode = Icon;

    void load()
    {
        picture = Image();
        textLines.clear();
        mode = Icon;
        if (items.empty())
            return;
        const Item &item = items[size_t(index)];
        const std::string ext = extensionOf(item.name);
        if (!item.folder && isPicture(ext)) {
            picture = Image::load(item.path);
            if (!picture.isNull())
                mode = Picture;
        }
#ifdef FINDER_POPPLER
        if (mode == Icon && ext == "pdf") {
            GError *err = nullptr;
            gchar *uri = g_filename_to_uri(item.path.c_str(), nullptr, nullptr);
            if (PopplerDocument *doc = uri ? poppler_document_new_from_file(uri, nullptr, &err) : nullptr) {
                if (PopplerPage *page = poppler_document_get_page(doc, 0)) {
                    double w = 0, h = 0;
                    poppler_page_get_size(page, &w, &h);
                    const double scale = std::min(2.0, 1100.0 / std::max(w, h));
                    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(w * scale), int(h * scale));
                    cairo_t *cr = cairo_create(s);
                    cairo_set_source_rgb(cr, 1, 1, 1);
                    cairo_paint(cr);
                    cairo_scale(cr, scale, scale);
                    poppler_page_render(page, cr);
                    cairo_destroy(cr);
                    picture = Image(s);
                    mode = Picture;
                    g_object_unref(page);
                }
                g_object_unref(doc);
            }
            if (err)
                g_error_free(err);
            g_free(uri);
        }
#endif
        if (mode == Icon && !item.folder && isText(item.path, ext)) {
            std::ifstream in(item.path, std::ios::binary);
            std::string line;
            while (textLines.size() < 400 && std::getline(in, line)) {
                for (char &c : line)
                    if (c == '\t' || c == '\r')
                        c = ' ';
                textLines.push_back(line);
            }
            mode = Text;
        }
    }

    // the content's size: the picture's (fitted in the screen's two thirds), a page for text, the icon's
    static Size contentSize(const Panel &p)
    {
        const Rect screen = App::shared().visibleFrame();
        const Size most{std::floor(screen.w * 0.66), std::floor(screen.h * 0.66) - TitleHeight - BottomHeight};
        if (p.mode == Picture) {
            Size s = p.picture.size();
            if (s.w > most.w || s.h > most.h)
                s = scaledToFit(s, most);
            return {std::max(320.0, std::floor(s.w)), std::max(200.0, std::floor(s.h))};
        }
        if (p.mode == Text)
            return {std::min(most.w, 600.0), std::min(most.h, 640.0)};
        return {520, 220};
    }

    void draw(cairo_t *cr) override
    {
        // the HUD: black at 85%, rounded 6 (opaque here), a lighter rim
        const Rect b = bounds();
        cairo_new_path(cr);
        const double r = 6;
        cairo_arc(cr, b.w - r, r, r, -M_PI / 2, 0);
        cairo_arc(cr, b.w - r, b.h - r, r, 0, M_PI / 2);
        cairo_arc(cr, r, b.h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        // the close button: a grey disc and its cross, at the top left
        cairo_arc(cr, 13, 11, 6, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, m_closeHover ? 0.75 : 0.5);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, 10.5, 8.5);
        cairo_line_to(cr, 15.5, 13.5);
        cairo_move_to(cr, 15.5, 8.5);
        cairo_line_to(cr, 10.5, 13.5);
        cairo_stroke(cr);

        if (items.empty())
            return;
        const Item &item = items[size_t(index)];
        TextStyle title;
        title.font = Font::boldSystem(12);
        title.color = Color::gray(255);
        title.align = Align::Center;
        std::string t = item.name;
        if (items.size() > 1)
            t += "  (" + std::to_string(index + 1) + "/" + std::to_string(items.size()) + ")";
        drawText(cr, t, Rect{30, 3, b.w - 60, 17}, title);

        const Rect content{Margin, TitleHeight, b.w - 2 * Margin, b.h - TitleHeight - BottomHeight};
        if (mode == Picture) {
            const Size s = scaledToFit(picture.size(), {content.w, content.h});
            const Size shown = picture.width() <= content.w && picture.height() <= content.h ? picture.size() : s;
            picture.draw(cr, Rect{std::round(content.x + (content.w - shown.w) / 2), std::round(content.y + (content.h - shown.h) / 2), shown.w, shown.h});
        } else if (mode == Text) {
            fill(cr, content, Color::gray(255));
            TextStyle mono;
            mono.font = Font{11, false, "Monaco"};
            mono.color = Color::gray(0);
            const double lh = std::ceil(lineHeight(mono.font));
            cairo_save(cr);
            cairo_rectangle(cr, content.x, content.y, content.w, content.h);
            cairo_clip(cr);
            double y = content.y + 8;
            for (const std::string &line : textLines) {
                if (y > content.bottom())
                    break;
                drawText(cr, line, Rect{content.x + 10, y, content.w - 20, lh}, mono);
                y += lh;
            }
            cairo_restore(cr);
        } else {
            // the generic preview: the icon at 128, the name, the kind, the size and the date
            const Image img = icon(item, 128);
            img.draw(cr, Rect{content.x + 30, content.y + std::floor((content.h - 128) / 2), 128, 128});
            const double tx = content.x + 30 + 128 + 30;
            TextStyle name;
            name.font = Font::boldSystem(16);
            name.color = Color::gray(255);
            double y = content.y + std::floor((content.h - 128) / 2) + 14;
            drawText(cr, item.name, Rect{tx, y, content.right() - tx - 10, 22}, name);
            y += 30;
            TextStyle info;
            info.font = Font::system(12);
            info.color = Color::gray(200);
            const long long bytes = item.folder ? -1 : sizeOnDisk(item);
            for (const std::string &line : {kind(item), bytes >= 0 ? formatBytes(bytes) : std::string(),
                                            formatDate(modificationTime(item), DateLong, true)}) {
                if (line.empty())
                    continue;
                drawText(cr, line, Rect{tx, y, content.right() - tx - 10, 16}, info);
                y += 18;
            }
        }

        // the bottom: the arrows through several items
        if (items.size() > 1) {
            const double y = b.h - BottomHeight / 2;
            for (int side : {-1, 1}) {
                const double cx = b.w / 2 + side * 18;
                cairo_move_to(cr, cx - side * 4, y - 6);
                cairo_line_to(cr, cx + side * 5, y);
                cairo_line_to(cr, cx - side * 4, y + 6);
                cairo_close_path(cr);
                cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
                cairo_fill(cr);
            }
        }
    }

    void mouseDown(Event &e) override
    {
        if (std::hypot(e.location.x - 13, e.location.y - 11) <= 7) {
            if (onClose)
                onClose();
            return;
        }
        if (items.size() > 1 && e.location.y > frame().h - BottomHeight && std::abs(e.location.x - frame().w / 2) < 30) {
            step(e.location.x < frame().w / 2 ? -1 : 1);
            return;
        }
        m_dragFrom = e.screen;
        if (window())
            window()->dragMove(e.screen, e.screen, true);
    }
    void mouseDragged(Event &e) override
    {
        if (window())
            window()->dragMove(m_dragFrom, e.screen, false);
    }
    void mouseMoved(Event &e) override
    {
        const bool over = std::hypot(e.location.x - 13, e.location.y - 11) <= 7;
        if (over != m_closeHover) {
            m_closeHover = over;
            setNeedsDisplay();
        }
    }
    void keyDown(Event &e) override
    {
        if (e.keysym == XKB_KEY_Escape || e.keysym == XKB_KEY_space || ((e.modifiers & Command) && (e.keysym == XKB_KEY_y || e.keysym == XKB_KEY_w))) {
            if (onClose)
                onClose();
        } else if (e.keysym == XKB_KEY_Left || e.keysym == XKB_KEY_Up) {
            step(-1);
        } else if (e.keysym == XKB_KEY_Right || e.keysym == XKB_KEY_Down) {
            step(1);
        }
    }
    bool acceptsFirstResponder() const override { return true; }

    void step(int by)
    {
        if (items.size() < 2)
            return;
        const int n = int(items.size());
        if (onIndex)
            onIndex((index + by + n) % n);
    }

private:
    Point m_dragFrom;
    bool m_closeHover = false;
};

std::unique_ptr<Window> s_window;
Panel *s_panel = nullptr;

void show(const std::vector<Item> &items, int index)
{
    // a new window for each item: its size is the item's (QLPreviewPanel resizes, keeping its middle)
    Point centre{-1, -1};
    if (s_window)
        centre = s_window->toScreen({s_window->size().w / 2, s_window->size().h / 2});
    auto *panel = new Panel;
    panel->items = items;
    panel->index = std::clamp(index, 0, int(items.size()) - 1);
    panel->load();
    const Size c = Panel::contentSize(*panel);
    const Size size{c.w + 2 * Panel::Margin, c.h + Panel::TitleHeight + Panel::BottomHeight};
    auto w = std::make_unique<Window>(size, core("TL14", "Quick Look"), Window::Titled);
    w->setBorderless();
    w->setFloatingPanel();
    w->contentView()->addSubview(panel);
    panel->setFrame({0, 0, size.w, size.h});
    panel->onClose = [] { QuickLook::close(); };
    panel->onIndex = [](int i) {
        if (!s_panel)
            return;
        const std::vector<Item> all = s_panel->items;
        if (QuickLook::onShowItem)
            QuickLook::onShowItem(all[size_t(i)]);
        show(all, i);
    };
    w->onClose = [] { App::shared().later([] { QuickLook::close(); }); };
    w->makeFirstResponder(panel);
    w->show();
    if (centre.x >= 0)
        w->moveTo({std::round(centre.x - size.w / 2), std::round(centre.y - size.h / 2)});
    if (s_window) {
        s_window->onClose = nullptr;
        s_window->close();
        Window *old = s_window.release();
        App::shared().later([old] { delete old; });
    }
    s_window = std::move(w);
    s_panel = panel;
}

} // namespace

bool QuickLook::isOpen() { return s_window != nullptr; }

void QuickLook::toggle(const std::vector<Item> &items)
{
    if (isOpen()) {
        close();
        return;
    }
    if (!items.empty())
        show(items, 0);
}

void QuickLook::close()
{
    if (!s_window)
        return;
    s_window->onClose = nullptr;
    s_window->close();
    Window *old = s_window.release();
    s_panel = nullptr;
    App::shared().later([old] { delete old; });
}

void QuickLook::follow(const std::vector<Item> &items)
{
    if (!isOpen() || items.empty() || !s_panel)
        return;
    if (s_panel->items.size() == items.size() && s_panel->items[size_t(s_panel->index)].path == items.front().path)
        return;
    // several items: the one shown kept if it is among them
    int index = 0;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].path == s_panel->items[size_t(s_panel->index)].path)
            index = int(i);
    if (items.size() == s_panel->items.size() && index == s_panel->index)
        return;
    show(items, index);
}

} // namespace Finder
