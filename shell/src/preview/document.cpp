// SPDX-License-Identifier: LGPL-2.1-or-later
#include "document.h"

#include "foundation.h"

#include <poppler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

Document::Info Document::info() const
{
    Info i;
    if (!m_pdf)
        return i;
    auto take = [](gchar *v) {
        std::string s = v ? v : "";
        g_free(v);
        return s;
    };
    i.version = take(poppler_document_get_pdf_version_string(m_pdf));
    if (i.version.rfind("PDF-", 0) == 0)
        i.version = i.version.substr(4);
    i.title = take(poppler_document_get_title(m_pdf));
    i.author = take(poppler_document_get_author(m_pdf));
    i.subject = take(poppler_document_get_subject(m_pdf));
    i.producer = take(poppler_document_get_producer(m_pdf));
    i.creator = take(poppler_document_get_creator(m_pdf));
    i.created = long(poppler_document_get_creation_date(m_pdf));
    i.modified = long(poppler_document_get_modification_date(m_pdf));
    // PDF keywords: separated by commas or semicolons
    const std::string kw = take(poppler_document_get_keywords(m_pdf));
    std::string cur;
    for (char c : kw + ",") {
        if (c == ',' || c == ';') {
            const size_t a = cur.find_first_not_of(' '), b = cur.find_last_not_of(' ');
            if (a != std::string::npos)
                i.keywords.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        } else {
            cur += c;
        }
    }
    const PopplerPermissions perms = poppler_document_get_permissions(m_pdf);
    i.canPrint = perms & POPPLER_PERMISSIONS_OK_TO_PRINT;
    i.canCopy = perms & POPPLER_PERMISSIONS_OK_TO_COPY;
    i.encrypted = perms != POPPLER_PERMISSIONS_FULL;
    return i;
}

Size Document::pixelSize() const
{
    if (m_image.isNull())
        return {};
    return {double(m_image.width()), double(m_image.height())};
}

bool Document::saveTo(const std::string &path, std::string *error) const
{
    if (m_pdf) {
        GError *err = nullptr;
        // with its annotations (poppler_document_save writes the changes), through a file beside
        // it moved into place: the document open is read from its own
        const std::string tmp = path + ".preview-save";
        const bool ok = m_annotations.empty() && !m_edited ? poppler_document_save_a_copy(m_pdf, uriOf(tmp).c_str(), &err)
                                                           : poppler_document_save(m_pdf, uriOf(tmp).c_str(), &err);
        if (ok && std::rename(tmp.c_str(), path.c_str()) == 0)
            return true;
        std::remove(tmp.c_str());
        if (error)
            *error = err ? err->message : "";
        if (err)
            g_error_free(err);
        return false;
    }
    // a picture: its file as it is
    std::string bytes;
    if (FILE *in = std::fopen(m_path.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, in)) > 0)
            bytes.append(buf, n);
        std::fclose(in);
    }
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (error)
            *error = "You don’t have permission to save the file in this folder.";
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

std::vector<Rect> Document::selectionRects(int page, const Rect &r) const
{
    std::vector<Rect> out;
    if (!m_pdf)
        return out;
    Page p(m_pdf, page);
    if (!p)
        return out;
    PopplerRectangle sel{r.x, r.y, r.right(), r.bottom()};
    cairo_region_t *region = poppler_page_get_selected_region(p.p, 1, POPPLER_SELECTION_GLYPH, &sel);
    if (!region)
        return out;
    for (int i = 0, n = cairo_region_num_rectangles(region); i < n; ++i) {
        cairo_rectangle_int_t c;
        cairo_region_get_rectangle(region, i, &c);
        out.push_back({double(c.x), double(c.y), double(c.width), double(c.height)});
    }
    cairo_region_destroy(region);
    // one rectangle a line: the region's pieces on the same line joined
    std::sort(out.begin(), out.end(), [](const Rect &a, const Rect &b) { return a.y < b.y || (a.y == b.y && a.x < b.x); });
    std::vector<Rect> lines;
    for (const Rect &q : out) {
        if (!lines.empty() && std::abs(lines.back().y - q.y) < 1 && std::abs(lines.back().h - q.h) < 1) {
            Rect &l = lines.back();
            const double right = std::max(l.right(), q.right());
            l.x = std::min(l.x, q.x);
            l.w = right - l.x;
        } else {
            lines.push_back(q);
        }
    }
    return lines;
}

int Document::addAnnotation(Annotation a)
{
    if (!m_pdf)
        return -1;
    Page p(m_pdf, a.page);
    if (!p)
        return -1;
    const double h = m_sizes[size_t(a.page)].h;
    PopplerRectangle r{a.rect.x, h - a.rect.bottom(), a.rect.right(), h - a.rect.y};
    PopplerColor color{guint16(a.color.r * 65535), guint16(a.color.g * 65535), guint16(a.color.b * 65535)};
    PopplerAnnot *annot = nullptr;
    if (a.type == 2 || a.type == 3) {
        annot = a.type == 2 ? poppler_annot_circle_new(m_pdf, &r) : poppler_annot_square_new(m_pdf, &r);
        poppler_annot_set_border_width(annot, a.lineWidth);
    } else if (a.type == 1) {
        PopplerPoint s{a.start.x, h - a.start.y}, e{a.end.x, h - a.end.y};
        annot = poppler_annot_line_new(m_pdf, &r, &s, &e);
        poppler_annot_set_border_width(annot, a.lineWidth);
    } else if (a.type == 4) {
        annot = poppler_annot_text_new(m_pdf, &r);
        poppler_annot_text_set_icon(POPPLER_ANNOT_TEXT(annot), POPPLER_ANNOT_TEXT_ICON_COMMENT);
    } else if (a.type == 5) {
        annot = poppler_annot_free_text_new(m_pdf, &r);
        PopplerFontDescription *font = poppler_font_description_new("Helvetica");
        font->size_pt = 36;
        poppler_annot_free_text_set_font_desc(POPPLER_ANNOT_FREE_TEXT(annot), font);
        poppler_font_description_free(font);
        poppler_annot_free_text_set_font_color(POPPLER_ANNOT_FREE_TEXT(annot), &color);
        poppler_annot_set_border_width(annot, 0);
    } else if (a.type >= 7 && a.type <= 9) {
        const std::vector<Rect> lines = selectionRects(a.page, a.rect);
        if (lines.empty())
            return -1;
        GArray *quads = g_array_new(FALSE, FALSE, sizeof(PopplerQuadrilateral));
        double x1 = 1e9, y1 = 1e9, x2 = -1e9, y2 = -1e9;
        for (const Rect &l : lines) {
            PopplerQuadrilateral q;
            q.p1 = {l.x, h - l.y};
            q.p2 = {l.right(), h - l.y};
            q.p3 = {l.x, h - l.bottom()};
            q.p4 = {l.right(), h - l.bottom()};
            g_array_append_val(quads, q);
            x1 = std::min(x1, l.x), x2 = std::max(x2, l.right());
            y1 = std::min(y1, h - l.bottom()), y2 = std::max(y2, h - l.y);
        }
        PopplerRectangle bounds{x1, y1, x2, y2};
        annot = a.type == 7 ? poppler_annot_text_markup_new_highlight(m_pdf, &bounds, quads)
              : a.type == 8 ? poppler_annot_text_markup_new_strikeout(m_pdf, &bounds, quads)
                            : poppler_annot_text_markup_new_underline(m_pdf, &bounds, quads);
        g_array_free(quads, TRUE);
        a.rect = {x1, h - y2, x2 - x1, y2 - y1};
    }
    if (!annot)
        return -1;
    if (a.type != 5)
        poppler_annot_set_color(annot, &color);
    if (!a.contents.empty())
        poppler_annot_set_contents(annot, a.contents.c_str());
    poppler_page_add_annot(p.p, annot);
    a.annot = annot;
    m_annotations.push_back(a);
    m_edited = true;
    return int(m_annotations.size()) - 1;
}

void Document::removeAnnotation(int index)
{
    if (index < 0 || size_t(index) >= m_annotations.size())
        return;
    Annotation &a = m_annotations[size_t(index)];
    Page p(m_pdf, a.page);
    if (p && a.annot)
        poppler_page_remove_annot(p.p, POPPLER_ANNOT(a.annot));
    if (a.annot)
        g_object_unref(a.annot);
    m_annotations.erase(m_annotations.begin() + index);
    m_edited = true;
}

} // namespace Preview
