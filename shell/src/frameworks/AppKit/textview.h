// SPDX-License-Identifier: LGPL-2.1-or-later
// AppKit's NSTextView, shared by Stickies' notes and TextEdit's documents: rich text
// (attributedstring.h) laid out by pango a paragraph at a time (each with its own paragraph style:
// alignment, indents, tab stops, line and paragraph spacing), wrapped to the view's width less
// NSTextContainer's line fragment padding (5 points each side), its glyphs drawn as AquaKit draws text (Quartz's dilation and LCD smoothing) run by run,
// in each run's colour, with their underlines and strike throughs, and the pictures (U+FFFC) at their
// size. The caret, the selection (selectedTextBackgroundColor), typing in the style at the caret (or
// the one ⌘B / ⌘I / ⌘U set with nothing selected), the arrows (with Option by words, with Command to
// the line's or the text's ends, with Shift extending), clicks (two: a word, three: a paragraph),
// drags, Undo and Redo, Cut, Copy and Paste (the system's clipboard as plain text; between notes with
// their styles). The view scrolls itself, by the wheel and to keep the caret in sight, when it has no
// scroll view around it (StickiesDocument.nib's, NSsFlags 0, has no scroller).
#pragma once

#include "appkit.h"
#include "attributedstring.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

typedef struct _PangoLayout PangoLayout;

namespace AK {

class TextView : public AK::View {
public:
    TextView();
    ~TextView() override;

    RichText &text() { return m_text; }
    void setText(RichText t);
    std::function<void()> onChange;          // NSTextDidChangeNotification
    AK::Color background = AK::Color::gray(255);
    // -[NSTextView setEditable:] (TextEdit's Prevent Editing): the selection and copying only
    bool editable = true;
    // The paragraphs' tab stops (NSParagraphStyle's tabStops), in points from the line's start, then
    // every defaultTabInterval after the last (0: none). Stickies' RTF: twelve 28 points apart.
    std::vector<double> tabStops = {28, 56, 84, 112, 140, 168, 196, 224, 252, 280, 308, 336};
    double defaultTabInterval = 0;
    void setTabs(std::vector<double> stops, double interval);

    // How far down the text is scrolled (the clip view's bounds origin), and its most
    // ScalingScrollView's scale factor (TextEdit's zoom popup): drawn and hit at that scale, the
    // scroll offsets in the text's own points
    void setZoom(double zoom);
    double zoom() const { return m_zoom; }
    double viewWidth() const { return frame().w / m_zoom; }
    double viewHeight() const { return frame().h / m_zoom; }
    double scrollOffset() const { return m_scroll; }
    double maxScrollOffset() const;
    void scrollTo(double y);
    // and across (pages wider than the view)
    double scrollOffsetX() const { return m_scrollX; }
    double maxScrollOffsetX() const;
    void scrollToX(double x);

    // TextEdit's MultiplePageView (Wrap to Page): the text in pages of the paper's size, one under
    // the other pageSeparatorHeight (5) apart, each its text container (documentSizeInPage: the paper
    // less the margins, the line fragment padding out in them) at the margins (documentRectForPageNumber:);
    // a line that doesn't fit what is left of a page goes to the next one (one NSTextView a page, the
    // layout manager going on in the next container). Drawn on the screen: the clip view's
    // controlColor around, each page in marginColor (white), its text container framed in lineColor
    // (lightGrayColor) a point outside. Its contents sized to the pages (updateFrame).
    struct PageGeometry {
        double paperWidth = 612, paperHeight = 792;   // NSPrintInfo's paperSize
        double left = 72, right = 72, top = 72, bottom = 72;
    };
    static constexpr double PageSeparatorHeight = 5;
    void setPages(std::optional<PageGeometry> pages);
    bool hasPages() const { return m_pages.has_value(); }
    const std::optional<PageGeometry> &pages() const { return m_pages; }
    size_t numberOfPages() const;
    // a page's rectangle in the contents (pageRectForPageNumber:)
    Rect pageRect(size_t page) const;
    // the width the lines wrap at, and where their 0 is in the contents (the padding in)
    double textWidth() const;
    double textOriginX() const;
    double contentWidth() const;

    // the selection, in bytes of the text
    size_t selectionStart() const { return std::min(m_caret, m_anchor); }
    size_t selectionEnd() const { return std::max(m_caret, m_anchor); }
    bool hasSelection() const { return m_caret != m_anchor; }
    void select(size_t from, size_t to);
    void selectAll() { select(0, m_text.size()); }
    void scrollToSelection();
    std::string selectedString() const { return m_text.text.substr(selectionStart(), selectionEnd() - selectionStart()); }

    // NSText's editing, as the menus ask for it
    void insertText(const std::string &s);
    void replaceSelection(const RichText &with);
    void deleteSelection();
    void cut();
    void copy();
    void paste(bool matchStyle = false);
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void undo();
    void redo();
    // A change of the selection's style (Bold, Italic, Underline, Bigger...): with nothing selected,
    // the style typing will use
    void changeStyle(const std::function<void(Style &)> &change);
    Style typingStyle() const { return m_typing; }
    void setTypingStyle(const Style &s) { m_typing = s; }
    // the style at the insertion point, as -[NSTextView typingAttributes] gives it
    Style styleAtSelection() const;
    void setAlignment(Alignment a);   // the selected paragraphs'
    // A change of the selected paragraphs' style (their alignment, NSParagraphStyle); not undoable:
    // after beginParagraphStyleChange (the ruler's drags, undone at once)
    void changeParagraphStyle(const std::function<void(Style &)> &change, bool undoable = true);
    void beginParagraphStyleChange();
    void transform(int kind);         // 0 upper case, 1 lower case, 2 capitalized
    // Lists (NSTextList): -[NSLayoutManager _doUserSetListMarkerFormat:options:startingItemNumber:
    // inRange:level:] (0x417b32): the selected paragraphs in a list of that marker format (empty:
    // taken out of theirs), each with its marker between tabs, indented 36 points, a tab 25 points
    // before that and one at it; -_reformListAtIndex: numbers the items again
    void setListMarkerFormat(const std::string &format, unsigned options = 0, long start = 1);
    std::optional<TextList> selectedList() const;

    double contentHeight() const;
    // For printing: the text alone (no background, selection nor caret) between two heights of the
    // contents, and where each line ends
    void drawGlyphs(cairo_t *cr, double top, double bottom);
    std::vector<double> lineBottoms() const;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    void scrollWheel(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }
    const char *cursor() const override { return "text"; }   // +[NSCursor IBeamCursor]
    void focusChanged() override;
    void setFrame(const AK::Rect &r) override;

    static constexpr double Padding = 5;   // NSTextContainer's lineFragmentPadding

private:
    // the paragraph from `start`'s marker's end (no marker: start)
    size_t listMarkerEnd(size_t start) const;
    void reformList(size_t at);
    void unnestList(size_t start);   // -_unnestListAtIndex:markerRange:: one level out
    void nestList(size_t start);     // -_nestListAtIndex:: one level in
    struct Paragraph {
        size_t start = 0, end = 0;   // bytes of text, end before its '\n'
        PangoLayout *layout = nullptr;
        double y = 0, height = 0;
        double x = 0;   // where its layout starts, from the padding: the nearer of its indents
        // with pages: each line's move down to its page (the next pages' margins and separators)
        std::vector<double> shifts;
        double shift(int line) const { return line >= 0 && size_t(line) < shifts.size() ? shifts[size_t(line)] : (shifts.empty() ? 0 : shifts.back()); }
        double top() const { return y + shift(0); }
        double bottom() const { return y + height + shift(int(shifts.size()) - 1); }
    };
    void layout() const;
    void invalidateLayout();
    // in the view's coordinates; between pages, the line below (above: the line above)
    size_t indexAt(AK::Point p, bool above = false) const;
    AK::Rect caretRect(size_t index) const;
    size_t paragraphOf(size_t index) const;
    size_t lineStart(size_t index) const;
    size_t lineEnd(size_t index) const;
    size_t wordStart(size_t index) const;
    size_t wordEnd(size_t index) const;
    void moveCaret(size_t to, bool extend);
    void snapshot(bool typing);
    void changed();
    void restartBlink();

    RichText m_text;
    Style m_typing;
    size_t m_caret = 0, m_anchor = 0;
    double m_goalX = -1;
    double m_scroll = 0, m_scrollX = 0;
    double m_zoom = 1;
    std::optional<PageGeometry> m_pages;
    mutable size_t m_pageCount = 1;
    int m_clickCount = 1;
    size_t m_dragOrigin = 0, m_dragOriginEnd = 0;
    mutable std::vector<Paragraph> m_paragraphs;
    mutable double m_layoutWidth = -1;
    mutable bool m_layoutValid = false;
    struct State {
        RichText text;
        size_t caret, anchor;
    };
    std::vector<State> m_undo, m_redo;
    bool m_typingGroup = false;
    int m_blink = 0;
    bool m_caretOn = true;
};

// NSScrollView around an NSTextView, as TextEdit's DocumentWindow.nib has it: the text, and a vertical
// scroller always shown at the right (NSsFlags 0x10, not autohiding), without a knob while everything
// fits (CoreUI's nothingtoscroll track); with pages a horizontal one too, at the bottom
// (-[DocumentWindowController setHasMultiplePages:force:] 0x1000028b7: setHasHorizontalScroller:YES)
class TextRulerView;
class TextScrollView : public View {
public:
    TextScrollView();
    TextView *textView() const { return m_text; }
    // NSScrollView's horizontal ruler (rulerview.h), over the text and the scroller, the whole width
    void setRulerVisible(bool visible);
    bool isRulerVisible() const;
    TextRulerView *ruler() const { return m_ruler; }
    void setFrame(const Rect &r) override;
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;
    void scrollWheel(Event &e) override { m_text->scrollWheel(e); }
    double lineStep = 13;   // an arrow's step

    // the text's pages shown (TextView::setPages) and the horizontal scroller with them
    void setPages(std::optional<TextView::PageGeometry> pages);

    // TextEdit's ScalingScrollView: with the horizontal scroller, a popup of the scales at its right
    // end (shadowless square bezel, arrows at the bottom, the tool tips' font at 10), the scroller
    // that much shorter, a black line before it; choosing one scales the text
    void setScales(std::vector<std::pair<std::string, double>> scales, int selected);
    std::function<void(double)> onScale;

private:
    Rect scrollerRect(bool horizontal = false) const;
    double proportion(bool horizontal = false) const;
    double value(bool horizontal = false) const;
    void step();
    double top() const;
    void layoutSubviews();
    TextView *m_text;
    TextRulerView *m_ruler = nullptr;
    double m_dragFrom = 0, m_dragOffset = 0;
    bool m_dragging = false, m_horizontal = false;   // the scroller pressed: the horizontal one
    int m_arrow = 0, m_repeat = 0;
    std::vector<std::pair<std::string, double>> m_scales;
    int m_scale = -1;
    double scalePopupWidth() const;
    Rect scalePopupRect() const;
};

} // namespace AK
