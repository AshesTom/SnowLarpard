// SPDX-License-Identifier: LGPL-2.1-or-later
// Preview's sidebar (PVDocument.nib's _utilityViewHolder, on the right in 10.6): the pages'
// thumbnails (PVIKImageBrowserView), the table of contents or the search's results (both in
// _outlineView, under the document's group row),
// over its bottom bar (_utilityBottomView, 23 high: the BottomTB background, the size slider in contact
// sheet mode, the view's segmented control at the right). Its width: 190 for thumbnails, 245 for the
// table of contents (kDefaultSidebarWidthFor...), 165 at least (kMinimumSidebarWidth).
#pragma once

#include "controls.h"
#include "document.h"

#include <functional>
#include <map>

namespace Preview {

constexpr double SidebarThumbnailsWidth = 190, SidebarOutlineWidth = 245, SidebarMinimumWidth = 165, BottomBarHeight = 23;

// The thumbnails, one column in the sidebar or as many as fit in contact sheet mode
class ThumbnailView : public AK::View {
public:
    explicit ThumbnailView(const Document *doc) : m_doc(doc) {}
    double thumbnailSize = 135;   // the box each page fits in
    bool contactSheet = false;
    int selected = -1;
    std::function<void(int)> onSelect;
    double heightFor(double width) const;
    int columns(double width) const;
    void select(int page);   // and scrolled to (by the caller's scroll view)
    void reveal();           // the selected one scrolled to
    AK::Rect cellRect(int page, double width) const;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }

private:
    const AK::Image &thumbnail(int page);
    void drawShadow(cairo_t *cr, const AK::Rect &picture);
    const Document *m_doc;
    std::map<int, AK::Image> m_cache;
    double m_cacheSize = 0;
};

class Sidebar : public AK::View {
public:
    enum Mode { ContactSheet = 0, Thumbnails = 1, Contents = 2, Annotations = 3, SearchResults = 4 };
    explicit Sidebar(const Document *doc);
    Mode mode() const { return m_mode; }
    void setMode(Mode m);
    bool hasOutline() const { return !m_outline.empty(); }
    void setPage(int page);   // the PDF view's current page: selected in the thumbnails
    void setResults(const std::vector<Document::Match> &matches, const std::string &text);
    void clearResults();

    std::function<void(int)> onPage;            // a thumbnail or a contents' entry clicked
    std::function<void(int)> onMatch;           // a search result chosen
    std::function<void(Mode)> onModeChanged;

    void setFrame(const AK::Rect &r) override;
    void draw(cairo_t *cr) override;

private:
    void layout();
    void sizeThumbnails();
    const Document *m_doc;
    Mode m_mode = Thumbnails, m_before = Thumbnails;
    AK::ScrollView *m_thumbScroll;
    ThumbnailView *m_thumbs;
    AK::ScrollView *m_outlineScroll;
    AK::OutlineView *m_outlineView;
    AK::ScrollView *m_resultsScroll;
    class ResultsView *m_results;
    std::vector<int> m_resultMatch;   // each result row's first match
    class BottomBar *m_bar;
    std::vector<Document::OutlineItem> m_outline;
};

} // namespace Preview
