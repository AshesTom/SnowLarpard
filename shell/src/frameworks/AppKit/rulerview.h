// SPDX-License-Identifier: LGPL-2.1-or-later
// NSRulerView, horizontal, as NSTextView shows it over its scroll view (TextEdit's Show Ruler),
// reversed from AppKit (AppKit-1038, the same on 10.6 and 10.6.8):
//   - its areas from the top (the view is flipped): the accessory view's (its height,
//     -[NSRulerView setAccessoryView:]), the markers' (reservedThicknessForMarkers, 15), the rule's
//     (ruleThickness, 16: -[NSRulerView initWithScrollView:orientation:]); a controlShadowColor line
//     at the bottom (drawSeparatorInRect:); the background controlColor
//   - the units registered by +[NSRulerView initialize]: Inches (in, 72 points, step up 2, step down
//     0.5), Centimeters (cm, 28.35, 2, 0.5 then 0.2), Points (pt, 1, 10, 0.5), Picas (pc, 12, 10, 0.5);
//     the labels in the tool tips font at 9 points (+_labelCell), white 0.25
//   - the text's markers (+[NSRulerMarker(NSTextExtensions) …]): the tab stops (CoreUI's RulerTabLeft,
//     RulerTabRight, RulerTabCenter, RulerTabDecimal), the first line and head indents (RulerIndentFirstLine,
//     RulerIndentLine), the right indent (RulerIndentLine); their image origins (left tab 0, right tab 5,
//     the others 4) on the rule's top edge
//   - the markers moved (NSRulerMarker's trackMouse:adding:, NSTextView's rulerView:didMoveMarker:):
//     the selected paragraphs' first line, head or tail indent, or a tab stop, its location from the
//     text's start, within the line; a tab stop dragged off the ruler removed
//     (rulerView:didRemoveMarker:), a click elsewhere on the markers or the rule a left tab stop
//     added there (rulerView:handleMouseDown:); the paragraphs' default stops given to them first
//   - the Spacing pull-down (-[NSLayoutManager(NSPrivate) _rulerAccViewSpacingAction:]): Single the
//     paragraphs' line height and spacing reset, Double a line height multiple of 2
//   - the accessory: AppKit's NSTextRulerAccessory.nib (Styles, Spacing and Lists pull-downs), its
//     alignment matrix replaced by a small NSSegmentedControl of four 24 point segments whose images
//     AppKit draws (-[NSLayoutManager(NSPrivate) _rulerAccView]: five 1 point lines in 20 x 11)
#pragma once

#include "appkit.h"
#include "textview.h"

#include <functional>
#include <memory>

namespace AK {

class SegmentedControl;
class PopUpButton;

class TextRulerView : public View {
public:
    explicit TextRulerView(TextView *text);
    ~TextRulerView() override;

    // the accessory's height and the markers' and the rule's thickness
    static double requiredThickness();
    // where the text's 0 is, in the ruler's coordinates (the text view's line fragment padding in)
    double originOffset = TextView::Padding;
    // the unit (Inches unless AppleMeasurementUnits says Centimeters)
    bool centimeters = false;
    // the selection's paragraph changed: the alignment buttons and the markers follow it
    void update();
    // TextEdit: the accessory disabled for plain text, the markers gone
    void setRichText(bool rich);

    void draw(cairo_t *cr) override;
    void setFrame(const Rect &r) override;
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;

private:
    enum class Drag { None, FirstLine, Head, Tail, Tab };
    struct MarkerRect {
        Drag kind;
        int tab;   // the tab stop's index
        Rect rect;
    };
    std::vector<MarkerRect> markerRects() const;
    ParagraphStyle selectedParagraph() const;
    void moveMarker(double x, double y, bool done);
    Drag m_drag = Drag::None;
    ParagraphStyle m_base;      // the paragraph's style when the drag began (its default tabs given)
    TabStop m_tab;              // the tab stop dragged, out of m_base's
    double m_grab = 0;          // where in the marker it was taken
    bool m_tabOff = false;      // dragged off the ruler: removed
    void buildAccessory();
    void drawMarkers(cairo_t *cr, double baseline);
    void drawHashMarks(cairo_t *cr, const Rect &rule);

    TextView *m_text;
    View *m_accessory = nullptr;   // the nib's content, its box at the top left
    SegmentedControl *m_alignment = nullptr;
    double m_accessoryHeight = 24;
    bool m_rich = true;
};

} // namespace AK
