// SPDX-License-Identifier: LGPL-2.1-or-later
// What Preview shows: a PDF (poppler, drawing with cairo as PDFKit draws with Quartz) or a picture
// (AquaKit's Image: PNG, JPEG, TIFF, icns), as pages in points, top left origin.
#pragma once

#include "graphics.h"

#include <memory>
#include <string>
#include <vector>

typedef struct _PopplerDocument PopplerDocument;

namespace Preview {

class Document {
public:
    ~Document();
    static std::unique_ptr<Document> open(const std::string &path, std::string *error = nullptr);

    std::string path() const { return m_path; }
    bool isPDF() const { return m_pdf != nullptr; }
    int pageCount() const { return int(m_sizes.size()); }
    AK::Size pageSize(int page) const;   // points, its rotation applied
    std::string pageLabel(int page) const;   // "1", or the document's own ("iv")

    // The page at `scale` (points to pixels) into cr, at its origin
    void draw(cairo_t *cr, int page, double scale) const;
    AK::Image render(int page, double scale) const;

    // The text: its matches (in the page's points), what a rectangle selects, its words around a match
    struct Match {
        int page;
        AK::Rect rect;
        std::string context;
    };
    std::vector<Match> find(const std::string &text) const;
    std::string selectedText(int page, const AK::Rect &r) const;
    // the highlight of the text selected between two points (cairo, at scale)
    void drawSelection(cairo_t *cr, int page, double scale, const AK::Rect &r) const;

    // Links: a page to go to, or a URI to open
    struct Link {
        AK::Rect rect;
        int page = -1;
        double top = 0;   // points from the page's top
        std::string uri;
    };
    std::vector<Link> links(int page) const;

    // The table of contents
    struct OutlineItem {
        std::string title;
        int page = -1;
        int level = 0;
    };
    std::vector<OutlineItem> outline() const;

    // The document's attributes (PDFDocument's documentAttributes, the Inspector's General Info): its
    // version, title, author, subject, producer, creator, dates (0: none), encryption and permissions
    struct Info {
        std::string version, title, author, subject, producer, creator;
        long created = 0, modified = 0;
        bool encrypted = false, canPrint = true, canCopy = true;
        std::vector<std::string> keywords;
    };
    Info info() const;
    // a picture's size in pixels (the page's points are its pixels)
    AK::Size pixelSize() const;
    // Save As: the document written again (a PDF through poppler; a picture copied)
    bool saveTo(const std::string &path, std::string *error = nullptr) const;

private:
    Document() = default;
    std::string m_path;
    PopplerDocument *m_pdf = nullptr;
    AK::Image m_image;
    std::vector<AK::Size> m_sizes;
};

} // namespace Preview
