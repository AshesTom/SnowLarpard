// SPDX-License-Identifier: LGPL-2.1-or-later
// A note's NSTextView: rich text (richtext.h) laid out by pango a paragraph at a time (each with its
// own alignment), wrapped to the view's width less NSTextContainer's line fragment padding (5 points
// each side), its glyphs drawn as AquaKit draws text (Quartz's dilation and LCD smoothing) run by run,
// in each run's colour, with their underlines and strike throughs, and the pictures (U+FFFC) at their
// size. The caret, the selection (selectedTextBackgroundColor), typing in the style at the caret (or
// the one ⌘B / ⌘I / ⌘U set with nothing selected), the arrows (with Option by words, with Command to
// the line's or the text's ends, with Shift extending), clicks (two: a word, three: a paragraph),
// drags, Undo and Redo, Cut, Copy and Paste (the system's clipboard as plain text; between notes with
// their styles). StickiesDocument.nib's scroll view has no scroller (NSsFlags 0): the view scrolls
// itself, by the wheel and to keep the caret in sight.
#pragma once

#include "appkit.h"
#include "richtext.h"

#include <functional>
#include <memory>
#include <vector>

typedef struct _PangoLayout PangoLayout;

namespace Stickies {

class TextView : public AK::View {
public:
    TextView();
    ~TextView() override;

    RichText &text() { return m_text; }
    void setText(RichText t);
    std::function<void()> onChange;          // NSTextDidChangeNotification
    AK::Color background = AK::Color::gray(255);

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
    void transform(int kind);         // 0 upper case, 1 lower case, 2 capitalized

    double contentHeight() const;
    // For printing: the text alone (no background, selection nor caret) between two heights of it,
    // and where each line ends
    void drawGlyphs(cairo_t *cr, double top, double bottom);
    std::vector<double> lineBottoms() const;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void mouseDragged(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    void scrollWheel(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return true; }
    void focusChanged() override;
    void setFrame(const AK::Rect &r) override;

    static constexpr double Padding = 5;   // NSTextContainer's lineFragmentPadding

private:
    struct Paragraph {
        size_t start = 0, end = 0;   // bytes of text, end before its '\n'
        PangoLayout *layout = nullptr;
        double y = 0, height = 0;
    };
    void layout() const;
    void invalidateLayout();
    size_t indexAt(AK::Point p) const;   // in the view's coordinates
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
    double m_scroll = 0;
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

} // namespace Stickies
