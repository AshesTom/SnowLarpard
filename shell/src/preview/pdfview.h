// SPDX-License-Identifier: LGPL-2.1-or-later
// PVPDFView (PDFKit's PDFView as Preview 10.6 sets it up): the pages one under the other on the grey
// matte (PDFKit's continuous display, its page breaks), each on white paper with PDFKit's shadow
// (drawShadowedRect:blur:, 4 pixels at the sides, 8 under), the scrollers when the pages don't fit; zoomed by
// Preview's steps (PVViewZoomFactorNext: 1% to 8000%); its tools (setToolMode: Move drags the pages,
// Text selects their text, Select a rectangle); links followed, search matches highlighted. A
// picture is one page without paper or shadow.
#pragma once

#include "appkit.h"
#include "document.h"

#include <functional>
#include <map>

namespace Preview {

class PDFView : public AK::View {
public:
    enum Tool { Move = 0, Text = 1, Select = 2 };
    explicit PDFView(const Document *doc);

    double scale() const { return m_scale; }   // previewZoomFactor
    void setScale(double s);
    void zoomIn();
    void zoomOut();
    void zoomToFit();     // the page's width in the view (a picture: all of it)
    void actualSize() { setScale(1); }
    bool autoFits() const { return m_autoFit; }
    // the zoom that shows the whole first page's width at this view width (bestWindowFrame's)
    static double rowWidth(const Document &doc, double scale);
    static double rowHeight(const Document &doc, int page, double scale);
    double documentHeight() const;

    int currentPage() const { return m_current; }
    void goToPage(int page, double top = 0);   // top: points from the page's top
    void previousPage() { goToPage(std::max(0, m_current - 1)); }
    void nextPage() { goToPage(std::min(m_doc->pageCount() - 1, m_current + 1)); }

    Tool tool() const { return m_tool; }
    void setTool(Tool t);

    void setMatches(std::vector<Document::Match> matches);
    void showMatch(int index);
    std::string selectedText() const;
    void copy() const;   // the selected text to the clipboard (wl-copy or xclip)

    std::function<void(int)> onPageChanged;
    std::function<void(double)> onScaleChanged;

    void setFrame(const AK::Rect &r) override;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void scrollWheel(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }

private:
    struct Layout {
        AK::Rect content;           // where the pages are seen (the scrollers excluded)
        bool vertical = false, horizontal = false;
        double width = 0, height = 0;   // the document's
    };
    Layout layout() const;
    AK::Rect pageRect(int page) const;   // in the document (unscrolled)
    int pageAt(AK::Point inDocument, bool nearest) const;
    AK::Point toDocument(AK::Point inView) const { return {inView.x + m_x - m_originX, inView.y + m_y - m_originY}; }
    void scrollTo(double x, double y);
    void updateCurrent();
    void drawScrollers(cairo_t *cr, const Layout &l);
    const AK::Image &rendered(int page);
    std::vector<double> m_tops;   // each row's top in the document (the page's own top), then its height
    std::vector<AK::Rect> m_pages; // each page in the document, scaled, before rounding to pixels
    void relayout();

    const Document *m_doc;
    double m_scale = 1;
    bool m_autoFit = false;
    double m_x = 0, m_y = 0;              // the scroll
    double m_originX = 0, m_originY = 0;  // the pages' offset when narrower than the view (centred)
    int m_current = 0;
    Tool m_tool = Text;
    std::map<int, AK::Image> m_cache;
    double m_cacheScale = 0;

    // the mouse
    enum Drag { None, Pan, Selecting, Marquee, VKnob, HKnob, Arrow } m_drag = None;
    AK::Point m_down, m_last;
    double m_dragStart = 0;
    int m_arrowDir = 0, m_repeat = 0;
    bool m_vertArrow = true;
    bool m_moved = false;
    int m_selPage = -1;
    AK::Rect m_selection;                 // in that page's points
    AK::Rect m_marquee;                   // in the document
    std::vector<Document::Match> m_matches;
    int m_match = -1;
};

} // namespace Preview
