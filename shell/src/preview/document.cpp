// SPDX-License-Identifier: LGPL-2.1-or-later
#include "document.h"

#include "foundation.h"

#include <poppler.h>

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Preview {

namespace {

std::string uriOf(const std::string &path)
{
    // a URI needs the whole path: one given from the working directory (Preview file.pdf) is completed
    gchar *whole = g_canonicalize_filename(path.c_str(), nullptr);
    gchar *uri = g_filename_to_uri(whole, nullptr, nullptr);
    g_free(whole);
    std::string out = uri ? uri : "";
    g_free(uri);
    return out;
}

struct Page {
    PopplerPage *p;
    Page(PopplerDocument *d, int i) : p(poppler_document_get_page(d, i)) {}
    ~Page()
    {
        if (p)
            g_object_unref(p);
    }
    explicit operator bool() const { return p; }
};

// poppler's rectangles have their origin at the bottom left (PDF's), ours at the top left
Rect fromPdf(const PopplerRectangle &r, double height)
{
    const double x1 = std::min(r.x1, r.x2), x2 = std::max(r.x1, r.x2);
    const double y1 = std::min(r.y1, r.y2), y2 = std::max(r.y1, r.y2);
    return {x1, height - y2, x2 - x1, y2 - y1};
}

std::string collapse(std::string s)
{
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::string out;
    for (char c : s)
        if (!(c == ' ' && !out.empty() && out.back() == ' '))
            out += c;
    const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

} // namespace

Document::~Document()
{
    if (m_pdf)
        g_object_unref(m_pdf);
}

std::unique_ptr<Document> Document::open(const std::string &path, std::string *error)
{
    if (!isFile(path)) {
        if (error)
            *error = "The file “" + fileName(path) + "” couldn’t be opened because there is no such file.";
        return nullptr;
    }
    std::unique_ptr<Document> d(new Document);
    d->m_path = path;
    GError *err = nullptr;
    d->m_pdf = poppler_document_new_from_file(uriOf(path).c_str(), nullptr, &err);
    if (d->m_pdf) {
        for (int i = 0, n = poppler_document_get_n_pages(d->m_pdf); i < n; ++i) {
            Page p(d->m_pdf, i);
            double w = 612, h = 792;
            if (p)
                poppler_page_get_size(p.p, &w, &h);
            d->m_sizes.push_back({w, h});
        }
        return d->m_sizes.empty() ? nullptr : std::move(d);
    }
    if (err)
        g_error_free(err);
    d->m_image = Image::load(path);
    if (d->m_image.isNull()) {
        if (error)
            *error = "The file “" + fileName(path) + "” couldn’t be opened because it isn’t in the correct format.";
        return nullptr;
    }
    d->m_sizes.push_back(d->m_image.size());
    return d;
}

Size Document::pageSize(int page) const
{
    return page >= 0 && page < pageCount() ? m_sizes[size_t(page)] : Size{};
}

std::string Document::pageLabel(int page) const
{
    if (m_pdf) {
        Page p(m_pdf, page);
        if (p) {
            gchar *label = nullptr;
            g_object_get(p.p, "label", &label, nullptr);
            std::string out = label ? label : "";
            g_free(label);
            if (!out.empty())
                return out;
        }
    }
    return std::to_string(page + 1);
}

void Document::draw(cairo_t *cr, int page, double scale) const
{
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    if (m_pdf) {
        Page p(m_pdf, page);
        if (p)
            poppler_page_render(p.p, cr);
    } else if (!m_image.isNull()) {
        cairo_set_source_surface(cr, m_image.surface(), 0, 0);
        // a picture larger than shown is smoothed; enlarged, its pixels stay square (as Preview's)
        cairo_pattern_set_filter(cairo_get_source(cr), scale >= 2 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

Image Document::render(int page, double scale) const
{
    const Size s = pageSize(page);
    const int w = std::max(1, int(std::ceil(s.w * scale))), h = std::max(1, int(std::ceil(s.h * scale)));
    Image img(w, h);
    cairo_t *cr = cairo_create(img.surface());
    if (m_pdf) {
        // a PDF page's paper is white (PDFKit's pageColor)
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
    }
    draw(cr, page, scale);
    cairo_destroy(cr);
    return img;
}

std::vector<Document::Match> Document::find(const std::string &text) const
{
    std::vector<Match> out;
    if (!m_pdf || text.empty())
        return out;
    for (int i = 0; i < pageCount(); ++i) {
        Page p(m_pdf, i);
        if (!p)
            continue;
        const double height = m_sizes[size_t(i)].h;
        GList *list = poppler_page_find_text_with_options(p.p, text.c_str(), POPPLER_FIND_DEFAULT);
        for (GList *l = list; l; l = l->next) {
            auto *r = static_cast<PopplerRectangle *>(l->data);
            Match m{i, fromPdf(*r, height), {}};
            // the line around it, across the page
            m.context = selectedText(i, Rect{0, m.rect.y + 1, m_sizes[size_t(i)].w, std::max(1.0, m.rect.h - 2)});
            out.push_back(m);
            poppler_rectangle_free(r);
        }
        g_list_free(list);
    }
    return out;
}

std::string Document::selectedText(int page, const Rect &r) const
{
    if (!m_pdf)
        return {};
    Page p(m_pdf, page);
    if (!p)
        return {};
    PopplerRectangle sel{r.x, r.y, r.right(), r.bottom()};
    gchar *t = poppler_page_get_selected_text(p.p, POPPLER_SELECTION_GLYPH, &sel);
    std::string out = t ? t : "";
    g_free(t);
    return collapse(out);
}

void Document::drawSelection(cairo_t *cr, int page, double scale, const Rect &r) const
{
    if (!m_pdf)
        return;
    Page p(m_pdf, page);
    if (!p)
        return;
    PopplerRectangle sel{r.x, r.y, r.right(), r.bottom()}, old{0, 0, 0, 0};
    // the selected text's highlight, in 10.6's selectedTextBackgroundColor (181, 213, 255)
    PopplerColor glyph{0, 0, 0}, background{181 * 257, 213 * 257, 255 * 257};
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render_selection(p.p, cr, &sel, &old, POPPLER_SELECTION_GLYPH, &glyph, &background);
    cairo_restore(cr);
}

std::vector<Document::Link> Document::links(int page) const
{
    std::vector<Link> out;
    if (!m_pdf)
        return out;
    Page p(m_pdf, page);
    if (!p)
        return out;
    const double height = m_sizes[size_t(page)].h;
    GList *map = poppler_page_get_link_mapping(p.p);
    for (GList *l = map; l; l = l->next) {
        auto *m = static_cast<PopplerLinkMapping *>(l->data);
        Link link;
        link.rect = fromPdf(m->area, height);
        PopplerAction *a = m->action;
        if (a->type == POPPLER_ACTION_URI && a->uri.uri) {
            link.uri = a->uri.uri;
        } else if (a->type == POPPLER_ACTION_GOTO_DEST && a->goto_dest.dest) {
            PopplerDest *dest = a->goto_dest.dest;
            PopplerDest *named = nullptr;
            if (dest->type == POPPLER_DEST_NAMED && dest->named_dest)
                dest = named = poppler_document_find_dest(m_pdf, dest->named_dest);
            if (dest) {
                link.page = dest->page_num - 1;
                if (dest->change_top && link.page >= 0 && link.page < pageCount())
                    link.top = std::max(0.0, m_sizes[size_t(link.page)].h - dest->top);
            }
            if (named)
                poppler_dest_free(named);
        } else {
            continue;
        }
        out.push_back(link);
    }
    poppler_page_free_link_mapping(map);
    return out;
}

std::vector<Document::OutlineItem> Document::outline() const
{
    std::vector<OutlineItem> out;
    if (!m_pdf)
        return out;
    auto walk = [&](auto &&self, PopplerIndexIter *it, int level) -> void {
        do {
            PopplerAction *a = poppler_index_iter_get_action(it);
            if (a) {
                OutlineItem item;
                item.title = a->any.title ? a->any.title : "";
                item.level = level;
                if (a->type == POPPLER_ACTION_GOTO_DEST && a->goto_dest.dest) {
                    PopplerDest *dest = a->goto_dest.dest, *named = nullptr;
                    if (dest->type == POPPLER_DEST_NAMED && dest->named_dest)
                        dest = named = poppler_document_find_dest(m_pdf, dest->named_dest);
                    if (dest)
                        item.page = dest->page_num - 1;
                    if (named)
                        poppler_dest_free(named);
                }
                out.push_back(item);
                poppler_action_free(a);
            }
            if (PopplerIndexIter *child = poppler_index_iter_get_child(it)) {
                self(self, child, level + 1);
                poppler_index_iter_free(child);
            }
        } while (poppler_index_iter_next(it));
    };
    if (PopplerIndexIter *it = poppler_index_iter_new(m_pdf)) {
        walk(walk, it, 0);
        poppler_index_iter_free(it);
    }
    return out;
}

} // namespace Preview
