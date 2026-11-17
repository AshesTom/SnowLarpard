// SPDX-License-Identifier: LGPL-2.1-or-later
// Preview.app, on AquaKit (no Qt): PDFs and pictures in 10.6's document window (PVDocument.nib), its
// toolbar the one CommonToolbar.proplist lays out, its pages drawn by poppler.
//   Preview [file.pdf|picture...]
// Without a file Preview opens no window and no panel (-[PVApplication
// applicationShouldOpenUntitledFile:], 0x100002a65, answers NO; applicationDidFinishLaunching:
// only seeds random()): it is in front with its menus alone, File > Open… (-[PVDocumentController
// openDocument:], 0x10002f0ca) runs the panel. Its last window closed, it keeps running (PVApplication
// has no applicationShouldTerminateAfterLastWindowClosed:). One Preview: launched again, it hands
// its files to the running one (or brings it forward, as the Dock's click does).
#include "controls.h"
#include "document.h"
#include "foundation.h"
#include "mainmenu.h"
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

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

extern char **environ;

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

// ---- File > Open…: the desktop's own file chooser stands for the Mac's open panel

// A program's answer on its standard output, without blocking the windows; `ran` false when the
// program isn't there
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
    // The chooser on this session's display, in its own process: no desktop portal (on the host's
    // Plasma, KDE's and GTK's dialogs go through xdg-desktop-portal, whose window opens on the host's
    // desktop, not in the Snow Leopard session), the toolkits' own dialogs on X11
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
    const int err = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (err != 0) {
        close(out[0]);
        return done(false, {});
    }
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    auto text = std::make_shared<std::string>();
    auto timer = std::make_shared<int>(0);
    *timer = App::shared().addTimer(0.1, true, [=] {
        char buf[512];
        ssize_t n;
        while ((n = read(out[0], buf, sizeof buf)) > 0)
            text->append(buf, size_t(n));
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) != pid)
            return;
        while ((n = read(out[0], buf, sizeof buf)) > 0)
            text->append(buf, size_t(n));
        close(out[0]);
        App::shared().removeTimer(*timer);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        // 127: posix_spawnp's child found no such program
        done(!(WIFEXITED(status) && WEXITSTATUS(status) == 127), ok ? *text : std::string());
    });
}

// The files chosen, one per line of the chooser's answer; none when cancelled
void runOpenPanel(std::function<void(std::vector<std::string>)> done)
{
    static bool running = false;   // one panel at a time, as the Mac's modal one
    if (running)
        return;
    running = true;
    auto finish = [done](const std::string &answer) {
        running = false;
        std::vector<std::string> files;
        size_t from = 0;
        while (from < answer.size()) {
            size_t to = answer.find('\n', from);
            if (to == std::string::npos)
                to = answer.size();
            if (to > from)
                files.push_back(answer.substr(from, to - from));
            from = to + 1;
        }
        done(files);
    };
    // what this Preview shows: PDFs and the pictures AquaKit reads
    const std::string types = "*.pdf *.png *.jpg *.jpeg *.tif *.tiff *.gif *.bmp *.icns "
                              "*.PDF *.PNG *.JPG *.JPEG *.TIF *.TIFF *.GIF *.BMP";
    runDialog({"kdialog", "--title", "Open", "--multiple", "--separate-output", "--getopenfilename", home(), types + "|PDFs and pictures"},
              [finish, types](bool ran, std::string answer) {
                  if (ran)
                      return finish(answer);
                  runDialog({"zenity", "--file-selection", "--multiple", "--separator=\n", "--title=Open", "--filename=" + home() + "/",
                             "--file-filter=PDFs and pictures | " + types},
                            [finish](bool, std::string a) { finish(a); });
              });
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
        // MainMenu.nib's key equivalents that no view takes (-[NSMenu performKeyEquivalent:])
        m_window->onUnhandledKey = [this](Event &e) {
            if (onKeyEquivalent)
                onKeyEquivalent(e);
        };
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
    std::function<void(Event &)> onKeyEquivalent;   // the main menu's
    bool isKey() const { return m_window->isKey(); }

    // -[PVWindowController validateMenuItem:] (0x100026a58) and -[PVPDFView validateMenuItem:]
    // (0x100061346), for what this port does: enabled, the item's state and title; false for an
    // action the window doesn't have
    bool validate(const std::string &a, MenuValidation &v) const
    {
        const bool pdf = m_doc->isPDF();
        const bool sheet = m_sidebarOpen && m_sidebar->mode() == Sidebar::ContactSheet;   // isInContactSheetMode
        const int page = m_pdf->currentPage(), pages = m_doc->pageCount();
        if (a == "performClose:" || a == "performMiniaturize:" || a == "arrangeInFront:" || a == "toggleToolbarShown:") {
            if (a == "toggleToolbarShown:")   // NSWindow's
                v.title = m_toolbarShown ? "Hide Toolbar" : "Show Toolbar";
            return true;
        }
        if (a == "doGoToPreviousPage:")
            return !sheet && page > 0;
        if (a == "doGoToNextPage:")
            return !sheet && page + 1 < pages;
        if (a == "toggleSidebar:") {
            v.title = tr(m_sidebarOpen && !sheet ? "Hide Sidebar" : "Show Sidebar");
            return pdf;
        }
        if (a == "sidebarSelectContactSheetView:" || a == "sidebarSelectThumbnailView:" || a == "sidebarSelectTableOfContentsView:") {
            const Sidebar::Mode m = a == "sidebarSelectContactSheetView:" ? Sidebar::ContactSheet
                                    : a == "sidebarSelectThumbnailView:"  ? Sidebar::Thumbnails
                                                                          : Sidebar::Contents;
            v.checked = m_sidebar->mode() == m;
            return pdf && m_sidebarOpen && (m != Sidebar::Contents || m_sidebar->hasOutline());
        }
        if (a == "setSinglePageContinuousMode:" || a == "setDisplayCropBox:") {
            // PDFKit's continuous display on the crop box, the only ones here
            v.checked = true;
            return !sheet && pdf;
        }
        if (a == "doZoomIn:")
            return !sheet && m_pdf->scale() < 80;
        if (a == "doZoomOut:")
            return !sheet && m_pdf->scale() > 0.01;
        if (a == "doZoomToFit:")
            return !sheet && !m_pdf->autoFits();
        if (a == "doZoomToActualSize:")
            return !sheet && !(m_pdf->scale() > 0.995 && m_pdf->scale() < 1.005);
        if (a == "toggleAutoResize:") {
            v.checked = m_pdf->autoFits();
            return !sheet;
        }
        if (a == "toggleAnnotationBanner:") {
            v.title = tr(m_banner && !m_banner->isHidden() ? "Hide Annotations Toolbar" : "Show Annotations Toolbar");
            return !sheet && pdf;
        }
        if (a == "toggleNoteDisplay:") {
            v.title = tr("Show Notes");   // no notes' gutter
            return false;
        }
        if (a == "scrollTool:" || a == "selectTool:" || a == "textTool:") {
            const PDFView::Tool t = a == "scrollTool:" ? PDFView::Move : a == "selectTool:" ? PDFView::Select : PDFView::Text;
            v.checked = m_pdf->tool() == t;
            return !sheet && (pdf || a != "textTool:");
        }
        if (a == "copy:")
            return !m_pdf->selectedText().empty();
        if (a == "doFind:")
            return pdf;
        if (a == "doFindNext:" || a == "doFindPrevious:")
            return pdf && m_matchCount > 0;
        if (a == "doUseSelectionForFind:")
            return !sheet && pdf && !m_pdf->selectedText().empty();
        return false;
    }

    // the menus' actions that the window performs (the rest are the toolbar's, perform())
    void performMenu(const std::string &a)
    {
        if (a == "performClose:") {
            m_window->close();
        } else if (a == "performMiniaturize:") {
            m_window->miniaturize();
        } else if (a == "toggleToolbarShown:") {
            m_window->onToggleToolbar();
        } else if (a == "sidebarSelectContactSheetView:") {
            m_sidebar->setMode(Sidebar::ContactSheet);
        } else if (a == "sidebarSelectThumbnailView:") {
            m_sidebar->setMode(Sidebar::Thumbnails);
        } else if (a == "sidebarSelectTableOfContentsView:") {
            m_sidebar->setMode(Sidebar::Contents);
        } else if (a == "toggleAutoResize:") {
            if (m_pdf->autoFits())
                m_pdf->setScale(m_pdf->scale());   // stays at this zoom, no longer fitted
            else
                m_pdf->zoomToFit();
        } else if (a == "copy:") {
            m_pdf->copy();
        } else if (a == "doFind:") {
            if (m_search) {
                if (!m_toolbarShown)
                    m_window->onToggleToolbar();
                m_window->makeFirstResponder(m_search);
                m_search->selectAll();
            }
        } else if (a == "doFindNext:" || a == "doFindPrevious:") {
            m_match = (m_match + (a == "doFindNext:" ? 1 : m_matchCount - 1)) % m_matchCount;
            m_pdf->showMatch(m_match);
        } else if (a == "doUseSelectionForFind:") {
            if (m_search) {
                m_search->text = m_pdf->selectedText();
                m_search->setNeedsDisplay();
                search(m_search->text);
            }
        } else {
            perform(a);
            return;
        }
        if (m_tools)
            m_tools->setSelected(int(m_pdf->tool()));
        updateToolbar();
    }

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
            m_matchCount = 0;
            m_pdf->setMatches({});
            m_sidebar->clearResults();
            return;
        }
        std::vector<Document::Match> matches = m_doc->find(text);
        m_matchCount = int(matches.size());
        m_match = 0;
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
    int m_matchCount = 0, m_match = 0;   // Find Next and Find Previous
};

} // namespace

// ---- one Preview by user and display: an abstract socket; a second launch sends its files, one
// path a line (none: just come forward), and quits

sockaddr_un instanceAddress(socklen_t &length)
{
    const char *display = std::getenv("DISPLAY");
    const char *wayland = std::getenv("WAYLAND_DISPLAY");
    const std::string name = "snowleopard-preview-" + std::to_string(getuid()) + "-" +
                             (wayland && *wayland ? wayland : display ? display : "");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const size_t n = std::min(name.size(), sizeof addr.sun_path - 1);
    std::memcpy(addr.sun_path + 1, name.data(), n);   // sun_path[0] = 0: abstract
    length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
    return addr;
}

// The running Preview took the files: true
bool handToRunning(const std::vector<std::string> &files)
{
    socklen_t length = 0;
    const sockaddr_un addr = instanceAddress(length);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    if (connect(fd, reinterpret_cast<const sockaddr *>(&addr), length) != 0) {
        close(fd);
        return false;
    }
    std::string msg = "open\n";
    for (const std::string &f : files) {
        // the path whole: the running Preview has its own working directory
        char *whole = realpath(f.c_str(), nullptr);
        msg += (whole ? std::string(whole) : f) + "\n";
        std::free(whole);
    }
    for (size_t done = 0; done < msg.size();) {
        const ssize_t n = write(fd, msg.data() + done, msg.size() - done);
        if (n <= 0)
            break;
        done += size_t(n);
    }
    close(fd);
    return true;
}

int main(int argc, char **argv)
{
    App &app = App::shared();
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-')
            files.push_back(argv[i]);
    if (handToRunning(files))
        return 0;
    if (!app.open()) {
        std::fprintf(stderr, "Preview: no display (Wayland or X11)\n");
        return 1;
    }
    Assets::registerFonts();
    std::vector<std::unique_ptr<DocumentWindow>> windows;
    std::function<void()> openPanel;
    // MainMenu.nib's menus, for the menu bar and the key equivalents; the actions go to the key
    // window (the front one), the application's to the application (PVApplication, PVDocumentController)
    MainMenu menu;
    menu.load(Assets::path(std::string(Bundle) + "/Contents/Resources/English.lproj/MainMenu.nib"));
    auto front = [&windows]() -> DocumentWindow * {
        for (const auto &w : windows)
            if (w->isKey())
                return w.get();
        return windows.empty() ? nullptr : windows.back().get();
    };
    menu.validate = [&front](const MainMenuItem &item, MenuValidation &v) {
        const std::string &a = item.action;
        // the application's; hide:, hideOtherApplications:, unhideAllApplications: are done by the
        // menu bar (the shell hides an application's windows)
        if (a == "openDocument:" || a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:")
            return true;
        DocumentWindow *w = front();
        return w && w->validate(a, v);
    };
    menu.perform = [&front, &openPanel, &app](const MainMenuItem &item) {
        const std::string &a = item.action;
        if (a == "openDocument:")
            openPanel();
        else if (a == "terminate:")
            app.quit();
        else if (DocumentWindow *w = front())
            w->performMenu(a);
    };
    auto openFiles = [&windows, &app, &menu](const std::vector<std::string> &paths) {
        for (const std::string &f : paths) {
            std::string error;
            std::unique_ptr<Document> doc = Document::open(f, &error);
            if (!doc) {
                std::fprintf(stderr, "Preview: %s\n", error.c_str());
                continue;
            }
            windows.push_back(std::make_unique<DocumentWindow>(std::move(doc)));
            windows.back()->onKeyEquivalent = [&menu](Event &e) { menu.performKeyEquivalent(e); };
            windows.back()->onClose = [&windows, &app](DocumentWindow *w) {
                // the last window closed: Preview stays, in front with its menus
                app.later([&windows, &app, w] {
                    windows.erase(std::remove_if(windows.begin(), windows.end(), [w](const auto &p) { return p.get() == w; }), windows.end());
                    if (windows.empty())
                        app.setFrontWithoutWindows(true);
                });
            };
        }
    };
    openPanel = [openFiles] { runOpenPanel([openFiles](std::vector<std::string> chosen) { openFiles(chosen); }); };
    menu.publish();

    // the other launches' files (the Finder's, the Dock's): open, or come forward
    socklen_t length = 0;
    const sockaddr_un addr = instanceAddress(length);
    const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server >= 0 && bind(server, reinterpret_cast<const sockaddr *>(&addr), length) == 0 && listen(server, 4) == 0) {
        app.watch(server, [server, &windows, &app, openFiles] {
            for (int c; (c = accept4(server, nullptr, nullptr, SOCK_CLOEXEC)) >= 0;) {
                std::string text;
                char buf[4096];
                for (ssize_t n; (n = read(c, buf, sizeof buf)) > 0;)
                    text.append(buf, size_t(n));
                close(c);
                std::vector<std::string> paths;
                size_t from = text.find('\n');
                while (from != std::string::npos && from + 1 < text.size()) {
                    const size_t to = text.find('\n', from + 1);
                    const std::string line = text.substr(from + 1, (to == std::string::npos ? text.size() : to) - from - 1);
                    if (!line.empty())
                        paths.push_back(line);
                    from = to;
                }
                openFiles(paths);
                if (windows.empty())
                    app.setFrontWithoutWindows(true);
            }
        });
    }
    openFiles(files);
    if (windows.empty())
        app.setFrontWithoutWindows(true);
    const int code = app.run();
    app.setFrontWithoutWindows(false);
    return code;
}
