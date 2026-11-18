// SPDX-License-Identifier: LGPL-2.1-or-later
#include "rulerview.h"

#include "controls.h"
#include "coreui.h"
#include "foundation.h"
#include "nib.h"
#include "text.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AK {

namespace {

const char *const AppKitBundle = "/System/Library/Frameworks/AppKit.framework/Versions/C";

constexpr double MarkerThickness = 15;   // -[NSRulerView initWithScrollView:orientation:]: 15.0
constexpr double RuleThickness = 16;     // 16.0

// CoreUI's shared art for AppKit's named images (kCUIImageEntries, by CUIImageConstantToMapID)
constexpr int RulerTabCenter = 20121, RulerTabDecimal = 20122, RulerTabLeft = 20123, RulerTabRight = 20124;
constexpr int RulerIndentLine = 20153, RulerIndentFirstLine = 20217;

// the alignment buttons' glyphs (_rulerAccView, the tables _C.1811 and _C.1812): for each of the four
// segments (left, centred, justified, right), five lines 1 point high at y 1, 3, 5, 7, 9 from the
// bottom of a 20 x 11 image, from x, so long
const int GlyphX[4][5] = {{4, 4, 4, 4, 4}, {3, 7, 4, 6, 3}, {3, 3, 3, 3, 3}, {3, 6, 4, 7, 3}};
const int GlyphW[4][5] = {{14, 10, 13, 11, 14}, {14, 6, 12, 8, 14}, {14, 14, 14, 14, 14}, {14, 11, 13, 10, 14}};
// the segments' order and their paragraph alignments
const Alignment SegmentAlignment[4] = {Alignment::Left, Alignment::Center, Alignment::Justified, Alignment::Right};

void drawPiece(cairo_t *cr, int id, double x, double bottom)
{
    const Image img = CoreUI::shared(id);
    if (img.isNull())
        return;
    img.draw(cr, Point{std::round(x), std::round(bottom - img.height())});
}

} // namespace

TextRulerView::TextRulerView(TextView *text) : m_text(text) { buildAccessory(); }

TextRulerView::~TextRulerView() = default;

double TextRulerView::requiredThickness() { return 24 + MarkerThickness + RuleThickness; }

void TextRulerView::buildAccessory()
{
    // NSTextRulerAccessory.nib (an objects.nib): a window whose box (at 5, 17, 491 x 24) is the accessory
    Nib nib;
    const std::string dir = std::string(AppKitBundle) + "/Resources/English.lproj/NSTextRulerAccessory.nib";
    if (!nib.load(Assets::path(dir + "/keyedobjects.nib"), Assets::path(dir)))
        return;
    std::vector<Nib::WindowEntry> windows = nib.windows();
    if (windows.empty())
        return;
    Size size;
    View *content = nib.instantiateWindow(windows.front().object, &size);
    if (!content)
        return;
    // the box: the content's first subview; the view clipped to it
    auto *clip = add<View>();
    clip->clipsToBounds = true;
    View *box = content->subviews().empty() ? nullptr : content->subviews().front();
    const Rect boxFrame = box ? box->frame() : Rect{5, 17, 491, 24};
    m_accessoryHeight = boxFrame.h;
    clip->setFrame({0, 0, boxFrame.w, boxFrame.h});
    content->setFrame({-boxFrame.x, -boxFrame.y, size.w, size.h});
    clip->addSubview(content);
    m_accessory = clip;
    // the pull-downs (their first item the title) and the alignment matrix, replaced by a segmented
    // control as wide as its cell; the pull-downs after it moved by the difference
    std::vector<PopUpButton *> popups;
    View *matrix = nullptr;
    std::function<void(View *)> walk = [&](View *v) {
        if (auto *p = dynamic_cast<PopUpButton *>(v))
            popups.push_back(p);
        else if (!matrix && v != content && v->frame().w > 100 && v->frame().w < 130 && v->frame().h > 20 && v->frame().h < 30)
            matrix = v;   // the alignment NSMatrix (88, 2, 116 x 25), its image cells not built here
        // the NSStopTouchingMeBoxes are borderless (the typedstream's border type isn't read)
        if (auto *box = dynamic_cast<Box *>(v))
            box->kind = Box::None;
        for (View *s : v->subviews())
            walk(s);
    };
    walk(content);
    for (PopUpButton *p : popups) {
        p->pullDown = true;
        auto index = [p](const char *title) {
            for (size_t k = 0; k < p->items.size(); ++k)
                if (p->items[k].title.rfind(title, 0) == 0)
                    return int(k);
            return -1;
        };
        const int single = index("Single"), twice = index("Double"), other = index("Other");
        if (single >= 0 && twice >= 0) {
            // _rulerAccViewSpacingAction:: Single, Double (Other…: NSTextRulerOptions' sheet, not here)
            if (other >= 0)
                p->items[size_t(other)].enabled = false;
            p->onSelect = [this, single, twice](int i) {
                if (i != single && i != twice)
                    return;
                const double multiple = i == twice ? 2 : 0;
                m_text->changeParagraphStyle([multiple](Style &s) {
                    s.para.lineHeightMultiple = multiple;
                    s.para.lineSpacing = 0;
                    s.para.paragraphSpacingBefore = 0;
                    s.para.paragraphSpacing = 0;
                });
                setNeedsDisplay();
            };
        }
    }
    if (matrix && matrix->superview()) {
        const Rect mf = matrix->frame();
        matrix->setHidden(true);
        const std::vector<double> widths = {26, 24, 24, 26};
        double total = 0;
        for (double w : widths)
            total += w;
        auto *seg = new SegmentedControl(CoreUI::SegmentStyle::Tabs, {"", "", "", ""}, widths);
        seg->small = true;
        seg->setFrame({mf.x, mf.y + 3, total, 19});
        seg->glyph = [](cairo_t *cr, int i, const Rect &r, bool enabled) {
            // the image centred in the segment, its lines black (drawn from the image's bottom)
            const double ox = std::round(r.x + (r.w - 20) / 2), oy = std::round(r.y + (r.h - 11) / 2);
            cairo_set_source_rgba(cr, 0, 0, 0, enabled ? 1 : 0.4);
            for (int k = 0; k < 5; ++k)
                cairo_rectangle(cr, ox + GlyphX[i][k], oy + 11 - (1 + 2 * k) - 1, GlyphW[i][k], 1);
            cairo_fill(cr);
        };
        seg->onSelect = [this](int i) {
            if (i >= 0 && i < 4) {
                m_text->setAlignment(SegmentAlignment[i]);
                setNeedsDisplay();
            }
        };
        matrix->superview()->addSubview(seg);
        m_alignment = seg;
        const double shift = total - mf.w;
        for (PopUpButton *p : popups)
            if (p->frame().x > mf.x)
                p->setFrame({p->frame().x + shift, p->frame().y, p->frame().w, p->frame().h});
    }
    update();
}

void TextRulerView::setFrame(const Rect &r) { View::setFrame(r); }

void TextRulerView::setRichText(bool rich)
{
    m_rich = rich;
    if (m_alignment)
        m_alignment->setEnabled(rich);
    setNeedsDisplay();
}

void TextRulerView::update()
{
    if (m_alignment) {
        const Alignment a = m_text->styleAtSelection().align;
        for (int i = 0; i < 4; ++i)
            if (SegmentAlignment[i] == a && m_alignment->selected() != i)
                m_alignment->setSelected(i);
    }
}

void TextRulerView::draw(cairo_t *cr)
{
    // drawBackgroundInRect: controlColor; the separator: controlShadowColor along the bottom
    const Rect b = bounds();
    fill(cr, b, Color::gray(232));
    const double markersTop = m_accessoryHeight, ruleTop = markersTop + MarkerThickness;
    const Rect rule{0, ruleTop, b.w, std::max(0.0, b.h - ruleTop - 1)};
    drawHashMarks(cr, rule);
    if (m_rich)
        drawMarkers(cr, ruleTop);
    fill(cr, Rect{0, b.h - 1, b.w, 1}, Color::gray(128));
}

void TextRulerView::drawHashMarks(cairo_t *cr, const Rect &rule)
{
    // the unit's hash marks from the origin: a labelled mark every unit (every 2 when the labels
    // would touch), then the step down cycle while the marks stay apart (to an eighth of an inch,
    // a millimetre); the marks' heights the rule's, a half, a quarter
    const double unit = centimeters ? 28.35 : 72;
    const std::vector<double> stepDown = centimeters ? std::vector<double>{0.5, 0.2} : std::vector<double>{0.5, 0.5, 0.5};
    const Color ink = Color::gray(64);   // labelColor: white 0.25
    Font label = Font::system(9);        // +[NSFont toolTipsFontOfSize:9]
    TextStyle ls;
    ls.font = label;
    ls.color = ink;
    ls.elide = false;
    const double minHash = 4;
    // the levels: their spacing
    std::vector<double> spacing = {unit};
    for (size_t k = 0; spacing.back() * stepDown[k % stepDown.size()] >= minHash && spacing.size() < 6; ++k)
        spacing.push_back(spacing.back() * stepDown[k % stepDown.size()]);
    const double finest = spacing.back();
    const double bottom = rule.y + rule.h;
    int labelEvery = 1;
    if (textWidth("00", label) + 4 > unit)
        labelEvery = 2;
    const long last = long(std::ceil((rule.w - originOffset) / finest));
    for (long n = long(std::floor(-originOffset / finest)); n <= last; ++n) {
        const double at = n * finest;
        const double x = std::floor(originOffset + at) + 0.5;
        size_t level = spacing.size() - 1;
        for (size_t l = 0; l < spacing.size(); ++l)
            if (std::fabs(std::remainder(at, spacing[l])) < 0.01) {
                level = l;
                break;
            }
        const double h = level == 0 ? rule.h : level == 1 ? std::round(rule.h * 0.5) : std::round(rule.h * 0.25);
        cairo_move_to(cr, x, bottom);
        cairo_line_to(cr, x, bottom - h);
        ink.set(cr);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        if (level == 0) {
            const long u = std::lround(at / unit);
            if (u >= 0 && u % labelEvery == 0) {
                char s[24];
                std::snprintf(s, sizeof s, "%ld", u);
                drawText(cr, s, Rect{x + 2, rule.y, 30, 11}, ls);
            }
        }
    }
}

ParagraphStyle TextRulerView::selectedParagraph() const
{
    ParagraphStyle p = m_text->styleAtSelection().para;
    if (!p.hasTabs) {
        // the text's default stops, as the paragraph's own
        p.hasTabs = true;
        for (double t : m_text->tabStops)
            p.tabStops.push_back({t, TabStop::Left});
    }
    return p;
}

std::vector<TextRulerView::MarkerRect> TextRulerView::markerRects() const
{
    // the images where drawMarkers puts them: the indents' origin 4, the tabs' by their kind
    std::vector<MarkerRect> out;
    const double baseline = m_accessoryHeight + MarkerThickness;
    const ParagraphStyle p = m_drag != Drag::None ? m_base : selectedParagraph();
    auto rect = [&](int id, double x, double bottom) {
        const Image img = CoreUI::shared(id);
        const double w = img.isNull() ? 9 : img.width(), h = img.isNull() ? 7 : img.height();
        return Rect{std::round(x), std::round(bottom - h), w, h};
    };
    const double width = m_text->textWidth();
    const double tail = p.tailIndent > 0 ? p.tailIndent : width + p.tailIndent;
    out.push_back({Drag::FirstLine, -1, rect(RulerIndentFirstLine, originOffset + p.firstLineHeadIndent - 4, baseline - 7)});
    out.push_back({Drag::Head, -1, rect(RulerIndentLine, originOffset + p.headIndent - 4, baseline)});
    out.push_back({Drag::Tail, -1, rect(RulerIndentLine, originOffset + tail - 4, baseline)});
    for (size_t i = 0; i < p.tabStops.size(); ++i) {
        const TabStop &t = p.tabStops[i];
        if (t.location > width)
            break;
        const int id = t.kind == TabStop::Center ? RulerTabCenter : t.kind == TabStop::Right ? RulerTabRight
                       : t.kind == TabStop::Decimal ? RulerTabDecimal : RulerTabLeft;
        const double origin = t.kind == TabStop::Left ? 0 : t.kind == TabStop::Right ? 5 : 4;
        out.push_back({Drag::Tab, int(i), rect(id, originOffset + t.location - origin, baseline)});
    }
    return out;
}

void TextRulerView::drawMarkers(cairo_t *cr, double baseline)
{
    // the paragraph's markers on the rule's top edge: the first line and head indents, the tail
    // indent, the tab stops; the one dragged where the mouse has it
    (void)baseline;
    ParagraphStyle p = m_drag != Drag::None ? m_base : selectedParagraph();
    for (const MarkerRect &m : markerRects()) {
        const int id = m.kind == Drag::FirstLine ? RulerIndentFirstLine
                       : m.kind != Drag::Tab      ? RulerIndentLine
                       : p.tabStops[size_t(m.tab)].kind == TabStop::Center ? RulerTabCenter
                       : p.tabStops[size_t(m.tab)].kind == TabStop::Right  ? RulerTabRight
                       : p.tabStops[size_t(m.tab)].kind == TabStop::Decimal ? RulerTabDecimal
                                                                            : RulerTabLeft;
        drawPiece(cr, id, m.rect.x, m.rect.y + m.rect.h);
    }
    if (m_drag == Drag::Tab && !m_tabOff) {
        const int id = m_tab.kind == TabStop::Center ? RulerTabCenter : m_tab.kind == TabStop::Right ? RulerTabRight
                       : m_tab.kind == TabStop::Decimal ? RulerTabDecimal : RulerTabLeft;
        const double origin = m_tab.kind == TabStop::Left ? 0 : m_tab.kind == TabStop::Right ? 5 : 4;
        drawPiece(cr, id, originOffset + m_tab.location - origin, m_accessoryHeight + MarkerThickness);
    }
}

void TextRulerView::mouseDown(Event &e)
{
    const double markersTop = m_accessoryHeight, ruleBottom = markersTop + MarkerThickness + RuleThickness;
    if (!m_rich || !m_text->editable || e.location.y < markersTop || e.location.y > ruleBottom)
        return;
    m_base = selectedParagraph();
    m_drag = Drag::None;
    m_tabOff = false;
    // the marker under the mouse, the last drawn first (the tabs over the indents)
    const std::vector<MarkerRect> rects = markerRects();
    for (auto it = rects.rbegin(); it != rects.rend(); ++it)
        if (it->rect.contains(e.location)) {
            m_drag = it->kind;
            const double at = it->kind == Drag::FirstLine ? m_base.firstLineHeadIndent
                              : it->kind == Drag::Head    ? m_base.headIndent
                              : it->kind == Drag::Tail ? (m_base.tailIndent > 0 ? m_base.tailIndent : m_text->textWidth() + m_base.tailIndent)
                                                          : m_base.tabStops[size_t(it->tab)].location;
            m_grab = e.location.x - (originOffset + at);
            if (it->kind == Drag::Tab) {
                m_tab = m_base.tabStops[size_t(it->tab)];
                m_base.tabStops.erase(m_base.tabStops.begin() + it->tab);
            }
            break;
        }
    if (m_drag == Drag::None) {
        // rulerView:handleMouseDown: a new left tab stop, dragged from there
        m_drag = Drag::Tab;
        m_tab = {std::max(0.0, e.location.x - originOffset), TabStop::Left};
        m_grab = 0;
    }
    m_text->beginParagraphStyleChange();
    moveMarker(e.location.x, e.location.y, false);
}

void TextRulerView::mouseDragged(Event &e)
{
    if (m_drag != Drag::None)
        moveMarker(e.location.x, e.location.y, false);
}

void TextRulerView::mouseUp(Event &e)
{
    if (m_drag == Drag::None)
        return;
    moveMarker(e.location.x, e.location.y, true);
    m_drag = Drag::None;
    setNeedsDisplay();
}

void TextRulerView::moveMarker(double x, double y, bool done)
{
    // the location from the text's start, within the line (a point at a time)
    const double width = m_text->textWidth();
    const double at = std::clamp(std::round(x - m_grab - originOffset), 0.0, width);
    ParagraphStyle p = m_base;
    std::function<void(Style &)> change;
    switch (m_drag) {
    case Drag::FirstLine: {
        const double v = at;
        change = [v](Style &s) { s.para.firstLineHeadIndent = v; };
        p.firstLineHeadIndent = v;
        break;
    }
    case Drag::Head: {
        const double v = at;
        change = [v](Style &s) { s.para.headIndent = v; };
        p.headIndent = v;
        break;
    }
    case Drag::Tail: {
        // from the end, as NSTextView keeps it
        const double v = at - width;
        change = [v](Style &s) { s.para.tailIndent = v; };
        p.tailIndent = v;
        break;
    }
    case Drag::Tab: {
        // off the ruler (above or below it): removed
        m_tabOff = y < m_accessoryHeight - 8 || y > bounds().h + 8;
        m_tab.location = at;
        std::vector<TabStop> stops = m_base.tabStops;
        if (!m_tabOff) {
            stops.push_back(m_tab);
            std::stable_sort(stops.begin(), stops.end(), [](const TabStop &a, const TabStop &b) { return a.location < b.location; });
        }
        change = [stops](Style &s) {
            s.para.hasTabs = true;
            s.para.tabStops = stops;
        };
        break;
    }
    case Drag::None:
        return;
    }
    (void)p;
    (void)done;
    m_text->changeParagraphStyle(change, false);
    setNeedsDisplay();
}

} // namespace AK
