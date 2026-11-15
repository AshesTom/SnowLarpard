// SPDX-License-Identifier: LGPL-2.1-or-later
// CoreUI, 10.6's theme engine, ported without Qt: the controls' art from ArtFile.bin and SArtFile.bin
// (the DVD's, read at run time) and the way CUIRenderer puts it together. Reversed from CoreUI's x86_64
// slice (docs/reverse-status.md, section 9):
//
//   - an element is 1 x 1, 1 x 2, 1 x 3, 3 x 1 or 3 x 3 pieces (CUIElement::LoadFromArtFile);
//   - __CUICalcAlignedBounds places a piece at its own size in the bounds: alignment 3 at the minimum
//     edge, 2 at the maximum, otherwise centred (floor((bounds - piece) / 2));
//   - Draw3PieceHorizontalWithImages: the left end aligned left, the right end right, both at the
//     descriptor's vertical alignment; the middle between them, stretched (CGContextDrawImages);
//   - Draw3PieceHorizontalWith2Images (popups): the first piece cut into its left end and its last
//     column, which is stretched; the second piece is the right end;
//   - Prepare9PieceInstructions: corners at their size, edges and centre stretched;
//   - DrawSlider: the track centred across, moved 5 points away from the thumb's point (6 for a thumb
//     pointing up or left); the thumb at round((length - thumb) * value); ticks every
//     (length - thumb) / (n - 1) from floor(thumb / 2), 10 points past the track (kTickOffset*Adjustment);
//   - BeginFocus: CGFocusRing with _CUIColorGet(0x3E) (Graphite 0x3F), alpha 0.8, 2 points out.
#pragma once

#include "graphics.h"

#include <functional>
#include <string>
#include <vector>

namespace CoreUI {

using AK::Color;
using AK::Image;
using AK::Rect;

struct Element {
    int rows = 0, columns = 0;
    std::vector<Image> pieces;   // row by row, top down
    bool isNull() const { return pieces.empty(); }
};

// An element by its tokens, the widget's first ("pushbutton", "clear", "regular"...). A state the
// element doesn't have is dropped ("active", "clear", "inactive", in that order), as the art has the
// usual states without them.
Element element(std::vector<std::string> path);
// SArtFile.bin: an image by its ID (20000 + n)
Image shared(int id);
// A named image of CoreUI ("IconView" for kCUIImageIconView, AppKit's NSIconViewTemplate): the ones
// the shell uses
Image named(const std::string &name);

enum Align { AlignCenter = 0, AlignMax = 2, AlignMin = 3 };
Rect aligned(double w, double h, const Rect &bounds, int horizontal, int vertical);

// CUIRenderer's drawing of an element in bounds (see above)
void draw(cairo_t *cr, const Element &e, const Rect &bounds, int vertical = AlignCenter);

// _CUIColorGet: the entries of kColorTable the controls use
enum ColorIndex {
    FocusBlue = 0x3E, FocusGraphite = 0x3F,
    MenuFill = 0xE1, MenuSeparator = 0xE2, MenuFill2 = 0xE3,
    FocusFill = 0xF1,
};
Color color(int index);

// CGFocusRing around what `shape` draws (its alpha), as BeginFocus / EndFocus set it
void focusRing(cairo_t *cr, const Rect &bounds, const std::function<void(cairo_t *)> &shape);

// ---- the controls, as AppKit's cells ask CoreUI for them

struct State {
    bool enabled = true;
    bool pressed = false;
    bool windowActive = true;   // the window is key: blue; otherwise clear
    bool focused = false;
    bool small = false;         // NSSmallControlSize: the art's "small" pieces
};

void pushButton(cairo_t *cr, const Rect &r, const State &s, bool isDefault = false);
void checkBox(cairo_t *cr, const Rect &r, const State &s, int value);   // 0 off, 1 on, 2 mixed
void radioButton(cairo_t *cr, const Rect &r, const State &s, bool on);
void popUpButton(cairo_t *cr, const Rect &r, const State &s);
void helpButton(cairo_t *cr, const Rect &r, const State &s);
void disclosureTriangle(cairo_t *cr, const Rect &r, bool open, bool pressed);
void textField(cairo_t *cr, const Rect &r, const State &s);
void searchField(cairo_t *cr, const Rect &r, const State &s, bool inToolbar);
void imageWell(cairo_t *cr, const Rect &r, const State &s);
void tabPane(cairo_t *cr, const Rect &r, const State &s);
void listBox(cairo_t *cr, const Rect &r, const State &s);

// A segmented control: tabs ("tabs", the selected one blue) or textured toolbar segments
// (NSSegmentStyleTexturedRounded, 10.6's toolbar controls and textured buttons); one rectangle a
// segment, side by side
enum class SegmentStyle { Tabs, Textured };
void segments(cairo_t *cr, SegmentStyle style, const std::vector<Rect> &rects, const std::vector<bool> &selected,
              int pressed, const State &s);

// The scroller: the track, the arrows together at the bottom (at the right when horizontal: wider
// than high), the knob. Geometry first; up is left, down right when horizontal.
struct ScrollerParts {
    Rect track, knob, up, down;
};
ScrollerParts scrollerParts(const Rect &r, double value, double proportion);   // value 0..1, proportion visible
void scroller(cairo_t *cr, const Rect &r, double value, double proportion, const State &s, int pressedArrow = 0);
constexpr double ScrollerWidth = 15;

// A slider: horizontal (vertical when taller than wide), its thumb pointing at its ticks (below, or
// above when ticksAbove), round without ticks
struct SliderParts {
    Rect track, thumb;
    std::vector<double> ticks;   // x (y when vertical) of each
    bool vertical = false;
};
SliderParts sliderParts(const Rect &r, double value, int ticks, bool ticksAbove = false, bool small = false);
void slider(cairo_t *cr, const Rect &r, double value, int ticks, const State &s, bool ticksAbove = false);

// NSBox: the primary (or secondary) group box
void groupBox(cairo_t *cr, const Rect &r, const State &s, bool secondary = false);
// NSProgressIndicator's bar: the track and the done part (value 0..1), or the moving stripes
void progressBar(cairo_t *cr, const Rect &r, double value, bool indeterminate, double phase, const State &s);
// NSComboBox: the field and its arrow end
void comboBox(cairo_t *cr, const Rect &r, const State &s);
// NSStepper: the little arrows; pressed 1 up, 2 down
void stepper(cairo_t *cr, const Rect &r, const State &s, int pressed);
// NSTableHeaderView's cells
void listHeader(cairo_t *cr, const Rect &r, const State &s);

// Menus: the background (allround for popups and contextual menus) and the selection
void menuBackground(cairo_t *cr, const Rect &r, bool fromMenuBar = false);
void menuSelection(cairo_t *cr, const Rect &r);

} // namespace CoreUI
