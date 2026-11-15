// SPDX-License-Identifier: LGPL-2.1-or-later
// Preview.app, on AquaKit (no Qt): PDFs and pictures in 10.6's document window (PVDocument.nib), its
// toolbar the one CommonToolbar.proplist lays out, its pages drawn by poppler.
//   Preview file.pdf|picture...
#include "controls.h"
#include "document.h"
#include "foundation.h"
#include "pdfview.h"
#include "plist.h"
#include "sidebar.h"
#include "toolbar.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace AK;
using namespace Preview;

namespace {

const char *const Bundle = "/Applications/Preview.app";

std::string res(const std::string &name) { return Assets::path(std::string(Bundle) + "/Contents/Resources/" + name); }
std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }

// A printf format of Preview's strings ("%@ (page %lu of %lu)") with its values
std::string format(std::string f, const std::vector<std::string> &values)
{
    std::string out;
    size_t v = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '%' && i + 1 < f.size()) {
            size_t j = i + 1;
            while (j < f.size() && std::strchr("l0123456789", f[j]))
                ++j;
            if (j < f.size() && std::strchr("@dus", f[j])) {
                out += v < values.size() ? values[v++] : std::string();
                i = j;
                continue;
            }
        }
        out += f[i];
    }
    return out;
}

// ---- the toolbar (AquaKit's Toolbar, icon and label)

// A toolbar segmented control showing the TB_ images (PVSegmentedToolbarItem)
SegmentedControl *imageSegments(const std::vector<Image> &images, const std::vector<double> &widths, bool momentary,
                                std::vector<bool> popup = {})
{
    auto *s = new SegmentedControl(CoreUI::SegmentStyle::Textured, std::vector<std::string>(images.size()), widths);
    s->momentary = momentary;
    s->glyph = [images, popup](cairo_t *cr, int i, const Rect &r, bool enabled) {
        const Image &img = images[size_t(i)];
        if (img.isNull())
            return;
        const double x = std::floor(r.x + (r.w - img.width()) / 2), y = std::floor(r.y + (r.h - img.height()) / 2);
        img.draw(cr, Rect{x, y, double(img.width()), double(img.height())}, enabled ? 1 : 0.35);
        if (size_t(i) < popup.size() && popup[size_t(i)]) {
            // TB_popupArrow: the segment has a menu
            static const Image arrow = Image::load(res("TB_popupArrow.png"));
            arrow.draw(cr, Point{r.right() - 9, r.bottom() - 8}, enabled ? 1 : 0.35);
        }
    };
    return s;
}

// ---- the window

class DocumentWindow {
public:
    explicit DocumentWindow(std::unique_ptr<Document> doc) : m_doc(std::move(doc))
    {
        // [NSScreen visibleFrame]: without the menu bar and the Dock; when nothing says where they are,
        // the menu bar's 22 points and a Dock of 70
        const Rect screen = App::shared().screenFrame();
        m_visible = App::shared().visibleFrame();
        if (m_visible.w == screen.w && m_visible.h == screen.h)
            m_visible = {screen.x, screen.y + 22, screen.w, screen.h - 22 - 70};
        const bool multi = m_doc->pageCount() > 1;
        m_sidebarOpen = m_doc->isPDF() && multi;
        const Size size = bestSize();
        m_window = std::make_unique<Window>(size, title(), Window::Titled);
        View *root = m_window->contentView();
        m_pdf = root->add<PDFView>(m_doc.get());
        m_divider = root->add<Divider>(this);
        m_sidebar = root->add<Sidebar>(m_doc.get());
        m_toolbar = root->add<Toolbar>();
        buildToolbar();
        m_sidebar->setHidden(!m_sidebarOpen);
        m_divider->setHidden(!m_sidebarOpen);
        m_pdf->onPageChanged = [this](int page) {
            m_sidebar->setPage(page);
            m_window->setTitle(title());
            updateToolbar();
        };
        m_pdf->onScaleChanged = [this](double) { updateToolbar(); };
        m_sidebar->onPage = [this](int page) { m_pdf->goToPage(page); };
        m_sidebar->onMatch = [this](int i) { m_pdf->showMatch(i); };
        m_sidebar->onModeChanged = [this](Sidebar::Mode m) {
            // the contents want a wider sidebar (kDefaultSidebarWidthForOutline), the contact sheet all
            // the window
            if (m == Sidebar::Contents && m_sidebarWidth < SidebarOutlineWidth)
                m_sidebarWidth = SidebarOutlineWidth;
            layout();
        };
        m_sidebar->setPage(0);
        m_window->setUnifiedToolbarHeight(int(Toolbar::Gradient));
        m_window->setIcon(Image::load(res("preview.icns"), {128, 128}));
        m_window->setRepresentedIcon(documentIcon());
        m_window->setShowsToolbarButton(true);
        m_window->onToggleToolbar = [this] {
            // View > Hide Toolbar
            m_toolbarShown = !m_toolbarShown;
            m_toolbar->setHidden(!m_toolbarShown);
            m_window->setUnifiedToolbarHeight(m_toolbarShown ? int(Toolbar::Gradient) : 0);
            layout();
        };
        m_window->onResize = [this](Size) { layout(); };
        m_window->onClose = [this] {
            if (onClose)
                onClose(this);
        };
        layout();
        m_pdf->goToPage(0);
        m_window->setResizable({m_sidebarOpen ? 573.0 : 408.0, 224 + Toolbar::Height});
        m_window->makeFirstResponder(m_pdf);
        m_window->show();
    }

    std::function<void(DocumentWindow *)> onClose;

private:
    // The divider between the pages and the sidebar: 1 point (PVNSSplitView's dividerThickness, its
    // thin style), dragged in a few points around it
    class Divider : public View {
    public:
        explicit Divider(DocumentWindow *w) : m_w(w) {}
        void draw(cairo_t *cr) override { fill(cr, Rect{std::floor(bounds().w / 2), 0, 1, bounds().h}, Color::gray(141)); }
        void mouseDown(Event &e) override { m_from = e.screen.x, m_width = m_w->m_sidebarWidth; }
        void mouseDragged(Event &e) override
        {
            const double maxWidth = std::max(SidebarMinimumWidth, m_w->m_window->size().w - 200);
            m_w->m_sidebarWidth = std::clamp(m_width - (e.screen.x - m_from), SidebarMinimumWidth, maxWidth);
            m_w->layout();
        }

    private:
        DocumentWindow *m_w;
        double m_from = 0, m_width = 0;
    };

    std::string title() const
    {
        const std::string name = fileName(m_doc->path());
        if (!m_doc->isPDF())
            return name;
        if (m_doc->pageCount() == 1)
            return format(tr("%@ (1 page)"), {name});
        return format(tr("%@ (page %lu of %lu)"), {name, std::to_string(m_pdf ? m_pdf->currentPage() + 1 : 1), std::to_string(m_doc->pageCount())});
    }

    // -[PVWindowController bestWindowFrameForCurrentMediaContainer]: the screen's visible frame inset 4
    // (28 less across); a PDF at 100% (PVPDFDefaultDocumentScaling), as wide as its row, as tall as its
    // page (all the screen with more than one), its scroller when it scrolls; a picture at its size,
    // scaled down to fit; at least 408 x 224, the sidebar's width added
    Size bestSize() const
    {
        // the title bar (22) is in the window's frame too
        const double maxW = m_visible.w - 8 - 28, maxH = m_visible.h - 8 - 22 - Toolbar::Height;
        const double sidebar = m_sidebarOpen ? m_sidebarWidth + 1 : 0;
        double w, h;
        if (m_doc->isPDF()) {
            w = PDFView::rowWidth(*m_doc, 1);
            h = m_doc->pageCount() > 1 ? 1e9 : PDFView::rowHeight(*m_doc, 0, 1);
        } else {
            const Size s = m_doc->pageSize(0);
            const double k = std::min({1.0, (maxW - sidebar) / s.w, maxH / s.h});
            w = std::floor(s.w * k);
            h = std::floor(s.h * k);
        }
        w = std::max(408.0, w) + sidebar;
        h = std::max(224.0, h);
        if (m_doc->isPDF() && (m_doc->pageCount() > 1 || h > maxH))
            w += CoreUI::ScrollerWidth;
        return {std::min(w, maxW), std::min(h, maxH) + Toolbar::Height};
    }

    void buildToolbar()
    {
        const PList plist = PList::load(res("CommonToolbar.proplist"));
        std::map<std::string, const PList *> byId;
        for (const PList &item : plist["items"].array)
            byId[item.str("identifier")] = &item;
        const bool pdf = m_doc->isPDF();
        for (const PList &id : plist["defaultItems"].array) {
            const std::string identifier = id.string;
            if (identifier == "NSToolbarFlexibleSpaceItem") {
                m_toolbar->addItem(identifier, {}, nullptr, 0);
                continue;
            }
            auto found = byId.find(identifier);
            if (found == byId.end())
                continue;
            const PList &item = *found->second;
            const std::string cls = item.str("class");
            std::string label = item.str("label");
            if (cls == "PVSegmentedToolbarItem") {
                std::vector<Image> images;
                std::vector<double> widths;
                std::vector<bool> popup;
                std::vector<std::string> actions, labels;
                for (const PList &seg : item["segments"].array) {
                    Image img = Image::load(res(seg.str("image") + ".png"));
                    images.push_back(img);
                    widths.push_back(std::max(28.0, double(img.width()) + 6));
                    popup.push_back(!seg["menu"].isNull());
                    actions.push_back(seg.str("action"));
                    labels.push_back(tr(seg.str("label")));
                }
                const bool momentary = item.integer("trackingMode", 2) == 2;
                SegmentedControl *s = imageSegments(images, widths, momentary, popup);
                if (label.empty()) {
                    // the segments' labels side by side
                    for (const std::string &l : labels)
                        label += (label.empty() ? "" : " ") + l;
                } else {
                    label = tr(label);
                }
                double total = 0;
                for (double w : widths)
                    total += w;
                s->onSelect = [this, actions](int i) { perform(actions[size_t(i)]); };
                if (identifier == "toolbarmode") {
                    s->setSelected(int(m_pdf->tool()));
                    m_tools = s;
                }
                if (identifier == "previous_next")
                    m_prevNext = s;
                if (identifier == "zoom")
                    m_zoom = s;
                m_toolbar->addItem(identifier, label, s, total);
            } else if (cls == "PVSearchToolbarItem") {
                auto *field = new SearchField;
                field->setEnabled(pdf);
                field->onChange = [this](const std::string &text) { searchSoon(text); };
                field->onEnter = [this, field] { search(field->text); };
                m_search = field;
                m_toolbar->addItem(identifier, tr(label), field, 150);
            } else {
                Image img = Image::load(res(item.str("image") + ".png"));
                auto *b = new Button({}, Button::Textured);
                b->image = img;
                const std::string action = item.str("action");
                b->action = [this, action] { perform(action); };
                if (identifier == "sidebar")
                    b->setEnabled(pdf);
                if (identifier == "annotation_banner")
                    b->setEnabled(pdf);
                m_toolbar->addItem(identifier, tr(label), b, std::max(32.0, double(img.width()) + 12));
            }
        }
        updateToolbar();
    }

    void updateToolbar()
    {
        if (m_prevNext) {
            m_prevNext->segmentEnabled = {m_pdf->currentPage() > 0, m_pdf->currentPage() + 1 < m_doc->pageCount()};
            m_prevNext->setNeedsDisplay();
        }
        if (m_zoom) {
            m_zoom->segmentEnabled = {m_pdf->scale() > 0.01, m_pdf->scale() < 80};
            m_zoom->setNeedsDisplay();
        }
    }

    // the toolbar's actions (CommonToolbar.proplist's selectors)
    void perform(const std::string &action)
    {
        if (action == "doGoToPreviousPage:")
            m_pdf->previousPage();
        else if (action == "doGoToNextPage:")
            m_pdf->nextPage();
        else if (action == "doZoomOut:")
            m_pdf->zoomOut();
        else if (action == "doZoomIn:")
            m_pdf->zoomIn();
        else if (action == "doZoomToFit:")
            m_pdf->zoomToFit();
        else if (action == "doZoomToActualSize:")
            m_pdf->actualSize();
        else if (action == "scrollTool:")
            m_pdf->setTool(PDFView::Move);
        else if (action == "textTool:")
            m_pdf->setTool(m_doc->isPDF() ? PDFView::Text : PDFView::Select);
        else if (action == "selectTool:")
            m_pdf->setTool(PDFView::Select);
        else if (action == "toggleSidebar:")
            toggleSidebar();
        else if (action == "toggleAnnotationBanner:")
            toggleBanner();
        if (m_tools)
            m_tools->setSelected(int(m_pdf->tool()));
        updateToolbar();
    }

    void toggleSidebar()
    {
        m_sidebarOpen = !m_sidebarOpen;
        m_sidebar->setHidden(!m_sidebarOpen);
        m_divider->setHidden(!m_sidebarOpen);
        // the window keeps its size: the pages give the sidebar its room (and take it back)
        layout();
        m_window->setResizable({m_sidebarOpen ? 573.0 : 408.0, 224 + Toolbar::Height});
    }

    void toggleBanner()
    {
        if (!m_banner) {
            m_banner = m_window->contentView()->add<AnnotationBanner>();
        } else {
            m_banner->setHidden(!m_banner->isHidden());
        }
        layout();
    }

    void searchSoon(const std::string &text)
    {
        if (m_searchTimer)
            App::shared().removeTimer(m_searchTimer);
        m_searchTimer = App::shared().addTimer(0.4, false, [this, text] {
            m_searchTimer = 0;
            search(text);
        });
    }

    void search(const std::string &text)
    {
        if (m_searchTimer)
            App::shared().removeTimer(m_searchTimer);
        m_searchTimer = 0;
        if (text == m_searched)
            return;
        m_searched = text;
        if (text.empty()) {
            m_pdf->setMatches({});
            m_sidebar->clearResults();
            return;
        }
        std::vector<Document::Match> matches = m_doc->find(text);
        m_pdf->setMatches(matches);
        if (!m_sidebarOpen)
            toggleSidebar();
        m_sidebar->setResults(matches, text);
        if (!matches.empty())
            m_pdf->showMatch(0);
    }

    // the document's kind's icon in Preview's bundle (pdf.icns, png.icns...), for the title's proxy icon
    Image documentIcon() const
    {
        const std::string path = m_doc->path();
        const size_t dot = path.rfind('.');
        std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == "jpg")
            ext = "jpeg";
        else if (ext == "tif")
            ext = "tiff";
        Image icon = ext.empty() ? Image() : Image::load(res(ext + ".icns"), {16, 16});
        return icon.isNull() ? Image::load(res("Generic.icns"), {16, 16}) : icon;
    }

    void layout()
    {
        const Size s = m_window->size();
        m_toolbar->setFrame({0, 0, s.w, Toolbar::Height});
        const double top = m_toolbarShown ? Toolbar::Height : 0;
        const bool banner = m_banner && !m_banner->isHidden();
        const double bottom = s.h - (banner ? BottomBarHeight : 0);
        const bool sheet = m_sidebarOpen && m_sidebar->mode() == Sidebar::ContactSheet;
        double sidebar = m_sidebarOpen ? std::min(m_sidebarWidth, std::max(SidebarMinimumWidth, s.w - 200)) : 0;
        if (sheet)
            sidebar = s.w;
        const double pdfW = std::max(0.0, s.w - sidebar - (m_sidebarOpen && !sheet ? 1 : 0));
        m_pdf->setHidden(sheet);
        m_pdf->setFrame({0, top, pdfW, std::max(0.0, bottom - top)});
        m_divider->setHidden(!m_sidebarOpen || sheet);
        m_divider->setFrame({pdfW - 1, top, 3, std::max(0.0, s.h - top)});
        m_sidebar->setFrame({s.w - sidebar, top, sidebar, std::max(0.0, s.h - top)});
        if (m_banner)
            m_banner->setFrame({0, bottom, pdfW, BottomBarHeight});
    }

    // PVAnnotationBanner: the annotation tools on the BottomTB bar under the pages
    class AnnotationBanner : public View {
    public:
        AnnotationBanner()
        {
            m_background = Image::load(res("BottomTB_toolbarButtonBackground.png"));
            m_selectedBackground = Image::load(res("BottomTB_toolbarButtonBackgroundSelected.png"));
            m_divider = Image::load(res("BottomTB_toolbarButtonDivider.png"));
            for (const char *n : {"rectangle", "ellipse", "line", "freeText", "comment", "link", "highlight", "strikeThrough", "underline"})
                m_tools.push_back(Image::load(res(std::string("BottomTB_") + n + ".png")));
        }
        void draw(cairo_t *cr) override
        {
            m_background.draw(cr, bounds());
            fill(cr, Rect{0, 0, bounds().w, 1}, Color::gray(150));
            for (size_t i = 0; i < m_tools.size(); ++i) {
                const Rect r = cell(int(i));
                if (int(i) == m_selected)
                    m_selectedBackground.draw(cr, r);
                m_divider.draw(cr, Rect{r.right(), 0, 1, BottomBarHeight});
                const Image &img = m_tools[i];
                img.draw(cr, Point{std::floor(r.x + (r.w - img.width()) / 2), std::floor((BottomBarHeight - img.height()) / 2)});
            }
        }
        void mouseDown(Event &e) override
        {
            for (size_t i = 0; i < m_tools.size(); ++i)
                if (cell(int(i)).contains(e.location))
                    m_selected = m_selected == int(i) ? -1 : int(i);
            setNeedsDisplay();
        }

    private:
        Rect cell(int i) const { return {double(8 + i * 33), 0, 32, BottomBarHeight}; }
        Image m_background, m_selectedBackground, m_divider;
        std::vector<Image> m_tools;
        int m_selected = -1;
    };

    std::unique_ptr<Document> m_doc;
    std::unique_ptr<Window> m_window;
    Rect m_visible;
    Toolbar *m_toolbar = nullptr;
    PDFView *m_pdf = nullptr;
    Divider *m_divider = nullptr;
    Sidebar *m_sidebar = nullptr;
    AnnotationBanner *m_banner = nullptr;
    SegmentedControl *m_prevNext = nullptr, *m_zoom = nullptr, *m_tools = nullptr;
    SearchField *m_search = nullptr;
    bool m_sidebarOpen = false;
    bool m_toolbarShown = true;
    double m_sidebarWidth = SidebarThumbnailsWidth;   // kDefaultSidebarWidthForThumbnails
    int m_searchTimer = 0;
    std::string m_searched;
};

} // namespace

int main(int argc, char **argv)
{
    App &app = App::shared();
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-')
            files.push_back(argv[i]);
    if (files.empty()) {
        std::fprintf(stderr, "usage: Preview file.pdf|picture...\n");
        return 2;
    }
    if (!app.open()) {
        std::fprintf(stderr, "Preview: no display (Wayland or X11)\n");
        return 1;
    }
    Assets::registerFonts();
    std::vector<std::unique_ptr<DocumentWindow>> windows;
    for (const std::string &f : files) {
        std::string error;
        std::unique_ptr<Document> doc = Document::open(f, &error);
        if (!doc) {
            std::fprintf(stderr, "Preview: %s\n", error.c_str());
            continue;
        }
        windows.push_back(std::make_unique<DocumentWindow>(std::move(doc)));
        windows.back()->onClose = [&windows, &app](DocumentWindow *w) {
            // the last window closed: Preview quits (as 10.6's does once its windows are gone)
            app.later([&windows, &app, w] {
                windows.erase(std::remove_if(windows.begin(), windows.end(), [w](const auto &p) { return p.get() == w; }), windows.end());
                if (windows.empty())
                    app.quit();
            });
        };
    }
    if (windows.empty())
        return 1;
    return app.run();
}
