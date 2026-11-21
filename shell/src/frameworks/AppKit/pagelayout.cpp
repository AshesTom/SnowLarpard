// SPDX-License-Identifier: LGPL-2.1-or-later
#include "pagelayout.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"
#include "panels.h"

#include <langinfo.h>
#include <locale.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace AK {

namespace {

const char *const Plugins = "/System/Library/PrivateFrameworks/PrintingPrivate.framework/Versions/A/Plugins";

std::string cocoaUI() { return std::string(Plugins) + "/PrintCocoaUI.bundle/Contents/Resources"; }
std::string pdes() { return std::string(Plugins) + "/PrintingCocoaPDEs.bundle/Contents/Resources"; }

std::string uiString(const std::string &key) { return Assets::tr(std::string(Plugins) + "/PrintCocoaUI.bundle", "Localizable", key, key); }
std::string pdeString(const std::string &key) { return Assets::tr(std::string(Plugins) + "/PrintingCocoaPDEs.bundle", "Localizable", key, key); }

// Generic.ppd's *PageSize entries, in its order (their *PaperDimension, points); the Small kinds
// named by PrintCore's Localizable.strings
struct Paper {
    const char *name, *title;
    double w, h;
};
const Paper Papers[] = {
    {"Letter", "US Letter", 612, 792}, {"Legal", "US Legal", 612, 1008},        {"A4", "A4", 595, 842},
    {"ISOB5", "B5", 499, 708},         {"JISB5", "JIS B5", 516, 729},          {"LetterSmall", "US Letter Small", 612, 792},
    {"LegalSmall", "US Legal Small", 612, 1008}, {"A4Small", "A4 Small", 595, 842},
};

bool metric()
{
    // the locale's measurement (LC_MEASUREMENT: 1 metric, 2 US)
    bool m = true;
    if (locale_t l = newlocale(LC_MEASUREMENT_MASK, "", locale_t(0))) {
        m = int(uintptr_t(nl_langinfo_l(_NL_MEASUREMENT_MEASUREMENT, l))) != 2;
        freelocale(l);
    }
    return m;
}

// PMPaperCreatePaperSizeString: hundredths, Print.framework's "%d%@%02d by %d%@%02d inches" / "cm"
std::string paperSizeString(double w, double h)
{
    const bool cm = metric();
    const double unit = cm ? 72 / 2.54 : 72;
    const long a = std::lround(w / unit * 100), b = std::lround(h / unit * 100);
    char buf[96];
    std::snprintf(buf, sizeof buf, cm ? "%ld.%02ld by %ld.%02ld cm" : "%ld.%02ld by %ld.%02ld inches", a / 100, a % 100, b / 100, b % 100);
    return buf;
}

// The orientation matrix's cells: shadowless square toggles with the bundle's pictures
class OrientationCell : public Button {
public:
    OrientationCell() : Button({}, SmallSquare) {}
    bool on = false;
    void draw(cairo_t *cr) override
    {
        const bool pressed = m_pressed;
        m_pressed = pressed || on;
        Button::draw(cr);
        m_pressed = pressed;
    }
};

struct PageSetup {
    std::unique_ptr<Nib> window, panel;
    Window *parent = nullptr;
    PrintInfo info;
    std::function<void(bool, const PrintInfo &)> done;
    PopUpButton *paperPopup = nullptr;
    Label *paperSize = nullptr;
    TextField *scale = nullptr;
    OrientationCell *portrait = nullptr, *landscape = nullptr;
    std::vector<int> paperIndex;   // the popup's items: a paper's index, -1 none, -2 the custom one

    void showPaper()
    {
        if (paperSize)
            paperSize->setText(paperSizeString(info.paperWidth, info.paperHeight));
    }
    void showOrientation()
    {
        if (portrait) {
            portrait->on = !info.landscape;
            portrait->setNeedsDisplay();
        }
        if (landscape) {
            landscape->on = info.landscape;
            landscape->setNeedsDisplay();
        }
    }
};

std::unique_ptr<PageSetup> s_setup;

void endSetup(bool ok)
{
    if (!s_setup)
        return;
    PageSetup *gone = s_setup.release();
    Sheet::end(gone->parent);
    auto done = gone->done;
    const PrintInfo info = gone->info;
    App::shared().later([gone] { delete gone; });
    if (done)
        done(ok, info);
}

Label *labelOf(View *v)
{
    if (auto *l = dynamic_cast<Label *>(v))
        return l;
    return nullptr;
}

} // namespace

PrintInfo PageLayout::defaultPrintInfo()
{
    Settings s("printing");
    PrintInfo p;
    const std::string name = s.string("PaperName");
    if (!name.empty()) {
        p.paperName = name;
        p.paperWidth = std::strtod(s.string("PaperWidth", "612").c_str(), nullptr);
        p.paperHeight = std::strtod(s.string("PaperHeight", "792").c_str(), nullptr);
        p.landscape = s.boolean("Landscape");
        p.scale = std::strtod(s.string("Scale", "100").c_str(), nullptr);
        if (p.paperWidth > 0 && p.paperHeight > 0 && p.scale > 0)
            return p;
        p = PrintInfo();
    }
    // the locale's paper (LC_PAPER, millimetres): the printers' A4 595 × 842, else its size
    if (locale_t l = newlocale(LC_PAPER_MASK, "", locale_t(0))) {
        const unsigned w = unsigned(uintptr_t(nl_langinfo_l(_NL_PAPER_WIDTH, l)));
        const unsigned h = unsigned(uintptr_t(nl_langinfo_l(_NL_PAPER_HEIGHT, l)));
        freelocale(l);
        if (w == 210 && h == 297) {
            p.paperName = "A4";
            p.paperWidth = 595;
            p.paperHeight = 842;
        } else if (w > 0 && h > 0 && w < 2000 && h < 2000 && !(w == 216 && h == 279)) {
            p.paperName.clear();
            p.paperWidth = std::round(w * 72 / 25.4);
            p.paperHeight = std::round(h * 72 / 25.4);
        }
    }
    return p;
}

void PageLayout::beginSheet(Window *window, const PrintInfo &info, std::function<void(bool, const PrintInfo &)> done)
{
    if (!window || Sheet::isOpen(window))
        return;
    auto st = std::make_unique<PageSetup>();
    st->parent = window;
    st->info = info;
    st->done = std::move(done);
    st->window = std::make_unique<Nib>();
    st->panel = std::make_unique<Nib>();
    const std::string wdir = cocoaUI() + "/English.lproj/CocoaPageSetupWindow.nib";
    const std::string pdir = pdes() + "/English.lproj/PageAttributes.nib";
    if (!st->window->load(Assets::path(wdir + "/keyedobjects.nib"), Assets::path(cocoaUI())) ||
        !st->panel->load(Assets::path(pdir + "/keyedobjects.nib"), Assets::path(pdes())))
        return;
    Size size;
    View *content = st->window->instantiate("window");
    size = st->window->size();
    const int attrs = st->panel->outletObject("pageAttributesView");
    Size panelSize;
    View *panel = attrs >= 0 ? st->panel->instantiateWindow(attrs, &panelSize) : nullptr;
    if (!content || !panel)
        return;
    Nib &wn = *st->window, &pn = *st->panel;
    PageSetup *s = st.get();

    // PMPrintWindowController: the PDE's place grown to the panel, what is under it moved down
    if (View *place = wn.outlet("_pdeView")) {
        const Rect pf = place->frame();
        const double grow = panelSize.h - pf.h;
        for (View *v : content->subviews())
            if (v != place && v->frame().y >= pf.bottom() - 1)
                {
                Rect f = v->frame();
                f.y += grow;
                v->setFrame(f);
            }
        place->setFrame({pf.x, pf.y, pf.w, panelSize.h});
        panel->setFrame({std::floor((pf.w - panelSize.w) / 2), 0, panelSize.w, panelSize.h});
        place->addSubview(panel);
        size.h += grow;
        content->setFrame({0, 0, size.w, size.h});
    }

    // Settings: Page Attributes; Save as Default
    if (auto *settings = wn.outlet<PopUpButton>("_pdePopup")) {
        settings->items = {MenuItem(pdeString("Page Attributes")), MenuItem({}, true), MenuItem(uiString("Save as Default"))};
        settings->setSelected(0);
        settings->onSelect = [s, settings](int i) {
            if (s_setup.get() != s)
                return;
            if (i == 2) {
                // the page format as the panel has it, kept for the next documents
                Settings conf("printing");
                conf.set("PaperName", s->info.paperName);
                conf.set("PaperWidth", std::to_string(s->info.paperWidth));
                conf.set("PaperHeight", std::to_string(s->info.paperHeight));
                conf.set("Landscape", s->info.landscape);
                conf.set("Scale", std::to_string(s->info.scale));
            }
            settings->setSelected(0);
        };
    }

    // Format For: Any Printer, no model
    if (auto *printers = pn.outlet<PopUpButton>("printerPopupButton")) {
        printers->items = {MenuItem(pdeString("Any Printer"))};
        printers->setSelected(0);
    }
    if (Label *model = labelOf(pn.outlet("printerModelTextField")))
        model->setText("");

    // Paper Size: the generic printer's papers, the format's chosen (a size not among them: its own
    // item first), Manage Custom Sizes… after a separator
    s->paperPopup = pn.outlet<PopUpButton>("paperSizePopupButton");
    s->paperSize = labelOf(pn.outlet("paperSizeTextField"));
    if (PopUpButton *p = s->paperPopup) {
        p->items.clear();
        int chosen = -1;
        for (size_t i = 0; i < std::size(Papers); ++i)
            if (Papers[i].name == info.paperName ||
                (chosen < 0 && info.paperName.empty() && Papers[i].w == info.paperWidth && Papers[i].h == info.paperHeight))
                chosen = int(i);
        if (chosen < 0) {
            p->items.push_back(MenuItem(paperSizeString(info.paperWidth, info.paperHeight)));
            s->paperIndex.push_back(-2);
        }
        for (size_t i = 0; i < std::size(Papers); ++i) {
            p->items.push_back(MenuItem(Papers[i].title));
            s->paperIndex.push_back(int(i));
        }
        p->items.push_back(MenuItem({}, true));
        s->paperIndex.push_back(-1);
        p->items.push_back(MenuItem(pdeString("Manage Custom Sizes..."), false, false));
        s->paperIndex.push_back(-1);
        for (size_t k = 0; k < s->paperIndex.size(); ++k)
            if (s->paperIndex[k] == (chosen < 0 ? -2 : chosen))
                p->setSelected(int(k));
        p->onSelect = [s, p](int i) {
            if (s_setup.get() != s || i < 0 || size_t(i) >= s->paperIndex.size())
                return;
            const int k = s->paperIndex[size_t(i)];
            if (k >= 0) {
                s->info.paperName = Papers[k].name;
                s->info.paperWidth = Papers[k].w;
                s->info.paperHeight = Papers[k].h;
                s->showPaper();
            }
        };
    }
    s->showPaper();

    // Orientation: the matrix's two cells (Portrait.tiff, Landscape.tiff)
    if (View *matrix = pn.outlet("orientationMatrix")) {
        const Rect mf = matrix->frame();
        matrix->setHidden(true);
        auto cell = [&](double x, const char *picture, bool land) {
            auto *c = new OrientationCell;
            c->image = Image::load(Assets::path(pdes() + "/" + picture));
            c->setFrame({mf.x + x, mf.y, 38, 38});
            c->action = [s, land] {
                if (s_setup.get() != s)
                    return;
                s->info.landscape = land;
                s->showOrientation();
            };
            matrix->superview()->addSubview(c);
            return c;
        };
        s->portrait = cell(0, "Portrait.tiff", false);
        s->landscape = cell(50, "Landscape.tiff", true);   // 38 points and the 12 between
        s->showOrientation();
    }

    // Scale: a whole percentage
    s->scale = pn.outlet<TextField>("scalingTextField");
    if (s->scale)
        s->scale->setText(std::to_string(std::lround(info.scale)));

    for (View *v : wn.withAction("doPageSetup:"))
        if (auto *b = dynamic_cast<Button *>(v)) {
            b->isDefault = true;
            b->action = [s] {
                if (s_setup.get() != s)
                    return;
                const double scale = s->scale ? std::strtod(s->scale->text.c_str(), nullptr) : 100;
                if (!(scale >= 3 && scale <= 10000)) {
                    Alert::run(Image(), pdeString("The value in the \"Scale\" field is out of range."),
                               [] {
                                   std::string f = pdeString("Please enter a number between 1 and %d.");
                                   const size_t at = f.find("%d");
                                   if (at != std::string::npos)
                                       f.replace(at, 2, "10000");
                                   return f;
                               }(),
                               {pdeString("OK")}, nullptr);
                    return;
                }
                s->info.scale = std::round(scale);
                endSetup(true);
            };
        }
    for (View *v : wn.withAction("doCancel:"))
        if (auto *b = dynamic_cast<Button *>(v))
            b->action = [s] {
                if (s_setup.get() == s)
                    endSetup(false);
            };

    s_setup = std::move(st);
    Sheet::begin(window, content, size, 0, [] {
        // Escape: cancelled
        if (s_setup) {
            PageSetup *gone = s_setup.release();
            auto done = gone->done;
            const PrintInfo info = gone->info;
            App::shared().later([gone] { delete gone; });
            if (done)
                done(false, info);
        }
    });
}

} // namespace AK
