// SPDX-License-Identifier: LGPL-2.1-or-later
// Calculator.app (4.5, 10.6) on AquaKit. Reversed from Calculator's and BasicAndSci.calcview's x86_64
// slices (not protected); its nibs, LCD pictures, strings and Functions.xml are the DVD's, read at run
// time (docs/reverse-status.md, section 15).
//
// The window (Calculator.nib, a textured window): the LCD (mDisplayBox, 56 high once the RPN stack is
// hidden: -[LCDController _hideRegRPNView] takes 76 off the nib's 132) over the keys
// (BasicAdvanced.nib's boxes: Basic's 204 x 197, Scientific's 205 x 197 at its left), the keys'
// box at (7, 7) from the bottom left; the window as wide as the boxes and 14 more, as high as the box
// and the LCD and 8 more (-[CalculatorController setCurrentView:], -[BasicAdvancedController
// normalSize:]). Each key 2 points lower than the nib has it (-[BasicAdvancedController awakeFromNib]).
//   Calculator
#include "appkit.h"
#include "controls.h"
#include "coreui.h"
#include "engine.h"
#include "foundation.h"
#include "mainmenu.h"
#include "panels.h"
#include "programmer.h"
#include "text.h"
#include "textview.h"
#include "toolbar.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

using namespace AK;
using namespace Calculator;

namespace {

const char *const Bundle = "/Applications/Calculator.app";
const char *const PlugIn = "/Applications/Calculator.app/Contents/PlugIns/BasicAndSci.calcview";

std::string res(const std::string &name) { return Assets::path(std::string(Bundle) + "/Contents/Resources/" + name); }
std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }

constexpr double BoxHeight = 197, BasicWidth = 204, ScientificWidth = 205, DisplayHeight = 56;
// Hexadecimal.nib's o_ViewBox: 346 wide, 274 high with the binary view, 218 without it
// (-[HexadecimalController normalSize:] takes the binary box's 56 off)
constexpr double ProgrammerWidth = 346, ProgrammerHeight = 274, BinaryHeight = 56;

enum CalcView { BasicView = 0, ScientificView = 1, ProgrammerView = 2 };

// A key's title as the nib and -[BasicAdvancedController awakeFromNib] give it: x², yˣ (a
// superscript), log₂ (a subscript), ˣ√y (a superscript before)
struct Title {
    Title(std::string base = {}, std::string sup = {}, std::string sub = {}, std::string pre = {})
        : base(std::move(base)), sup(std::move(sup)), sub(std::move(sub)), pre(std::move(pre)) {}
    std::string base, sup, sub, pre;
};

// NSButton, NSTexturedSquareBezelStyle (the nib's NSButtonFlags2 0x20), its title in Lucida Grande 13
class Key : public Control {
public:
    Title title;
    int code = 0;          // Functions.xml's operator; < 0 the controller's own (digits, 2nd...)
    double fontSize = 13;
    bool on = false;       // 2nd: held on
    void flash()
    {
        // -[BasicAdvancedController momentarilyHighlightButton:]: a key typed shows pressed a moment
        m_flash = true;
        setNeedsDisplay();
        if (m_timer)
            App::shared().removeTimer(m_timer);
        m_timer = App::shared().addTimer(0.1, false, [this] {
            m_timer = 0;
            m_flash = false;
            setNeedsDisplay();
        });
    }
    ~Key() override
    {
        if (m_timer)
            App::shared().removeTimer(m_timer);
    }
    void draw(cairo_t *cr) override
    {
        CoreUI::State s = controlState(*this, m_pressed);
        const bool down = m_pressed || m_flash || on;
        CoreUI::segments(cr, CoreUI::SegmentStyle::Textured, {bounds()}, {on}, down && !on ? 0 : -1, s);
        TextStyle t;
        t.font = Font::system(fontSize);
        t.color = isEnabled() ? (on ? Color::gray(255) : Color::gray(0)) : Color::gray(0, 0.35);
        TextStyle small = t;
        small.font = Font::system(9);
        const double wBase = textWidth(title.base, t.font);
        const double wSup = title.sup.empty() ? 0 : textWidth(title.sup, small.font);
        const double wSub = title.sub.empty() ? 0 : textWidth(title.sub, small.font);
        const double wPre = title.pre.empty() ? 0 : textWidth(title.pre, small.font);
        double x = std::round((frame().w - (wPre + wBase + wSup + wSub)) / 2);
        const Rect line{0, 0, frame().w, frame().h - 1};
        if (wPre > 0) {
            drawText(cr, title.pre, Rect{x, line.y - 4, wPre + 2, line.h}, small);
            x += wPre;
        }
        drawText(cr, title.base, Rect{x, line.y, wBase + 2, line.h}, t);
        x += wBase;
        if (wSup > 0)
            drawText(cr, title.sup, Rect{x, line.y - 5, wSup + 2, line.h}, small);
        if (wSub > 0)
            drawText(cr, title.sub, Rect{x, line.y + 4, wSub + 2, line.h}, small);
    }

private:
    bool m_flash = false;
    int m_timer = 0;
};

// The LCD: lcd-*.tif's nine pieces (the corners 10 x 15, the edges, the pale yellow middle), the
// number at the right (the nib's field: Lucida Grande 14), the indicators under it (11): M, ( ),
// Deg or Rad
class LCD : public View {
public:
    std::string text, mode;
    std::string left;   // the Programmer view's: the number as an ASCII or Unicode character
    bool memory = false, parentheses = false;
    // RPN (-[LCDController setRPNState:]): the stack table (RPNTableView, rows 18 high, in the
    // nib's scroll view at (10, 33) from the box's bottom, 82 high) in place of the number: as many
    // rows as the stack and x, at least 4, scrolled to its last (x); "RPN" under it (the nib's
    // field at (144, 8))
    bool rpn = false;
    std::vector<std::string> rows;   // the top of the table first, x last
    LCD()
    {
        for (const char *n : {"topleft", "topmid", "topright", "leftmid", "mid", "rightmid", "bottomleft", "bottommid", "bottomright"})
            m_pieces.push_back(Image::load(res(std::string("lcd-") + n + ".tif")));
    }
    std::function<void(Event &)> onContextMenu;   // the nib's LCD menu: RPN, Large Type
    void rightMouseDown(Event &e) override
    {
        if (onContextMenu)
            onContextMenu(e);
    }
    void draw(cairo_t *cr) override
    {
        // the box's NoVOImageViews: 1 in from the left, 2 from the top, 3 from the bottom
        const Rect r{1, 2, frame().w - 1, frame().h - 5};
        const double cw = 10, ch = 15;
        const Image &tl = m_pieces[0], &tm = m_pieces[1], &tr_ = m_pieces[2], &lm = m_pieces[3], &mid = m_pieces[4],
                    &rm = m_pieces[5], &bl = m_pieces[6], &bm = m_pieces[7], &br = m_pieces[8];
        tl.draw(cr, Rect{r.x, r.y, cw, ch});
        tr_.draw(cr, Rect{r.right() - cw, r.y, cw, ch});
        bl.draw(cr, Rect{r.x, r.bottom() - ch, cw, ch});
        br.draw(cr, Rect{r.right() - cw, r.bottom() - ch, cw, ch});
        tm.draw(cr, Rect{r.x + cw, r.y, r.w - 2 * cw, double(tm.height())});
        bm.draw(cr, Rect{r.x + cw, r.bottom() - bm.height(), r.w - 2 * cw, double(bm.height())});
        lm.draw(cr, Rect{r.x, r.y + ch, double(lm.width()), r.h - 2 * ch});
        rm.draw(cr, Rect{r.right() - rm.width(), r.y + ch, double(rm.width()), r.h - 2 * ch});
        mid.draw(cr, Rect{r.x + cw, r.y + double(tm.height()), r.w - 2 * cw, r.h - tm.height() - bm.height()});
        mid.draw(cr, Rect{r.x + lm.width(), r.y + ch, cw - lm.width(), r.h - 2 * ch});
        mid.draw(cr, Rect{r.right() - cw, r.y + ch, cw - rm.width(), r.h - 2 * ch});

        // the fields, from the nib: the number at (10, 33) 18 high, the indicators at y 8, 15 high
        // (from the box's bottom)
        const double h = frame().h;
        TextStyle big;
        big.font = Font::system(14);
        big.align = Align::Right;
        if (rpn) {
            // -[LCDController tableView:objectValueForTableColumn:row:]: the last row x, the
            // others the stack (y over x), empty past it
            cairo_save(cr);
            cairo_rectangle(cr, 11, h - 34 - 80, frame().w - 22, 80);
            cairo_clip(cr);
            for (size_t k = 0; k < rows.size(); ++k) {
                const double bottom = h - 34 - 18.0 * double(rows.size() - 1 - k);
                drawText(cr, rows[k], Rect{10, bottom - 18, frame().w - 22, 18}, big);
            }
            cairo_restore(cr);
        } else {
            drawText(cr, text, Rect{10, h - 33 - 18, frame().w - 22, 18}, big);
        }
        if (!left.empty()) {
            TextStyle l = big;
            l.align = Align::Left;
            drawText(cr, left, Rect{10, h - 9 - 18, 35, 18}, l);
        }
        TextStyle small;
        small.font = Font::system(11);
        if (memory)
            drawText(cr, tr("M"), Rect{11, h - 8 - 15, 37, 15}, small);
        if (parentheses)
            drawText(cr, "( )", Rect{82, h - 8 - 15, 28, 15}, small);
        if (!mode.empty())
            drawText(cr, mode, Rect{109, h - 8 - 15, 30, 15}, small);
        if (rpn)
            drawText(cr, tr("RPN"), Rect{144, h - 8 - 15, 43, 15}, small);
    }

private:
    std::vector<Image> m_pieces;
};

// The textured window's background: WindowServer's unified gradient, from the title bar down to the
// bottom
class Background : public View {
public:
    void draw(cairo_t *cr) override
    {
        const bool active = windowIsKey();
        const int total = 22 + int(frame().h);
        for (int y = 0; y < int(frame().h); ++y)
            fill(cr, Rect{0, double(y), frame().w, 1}, Toolbar::rowColor(22 + y, total, active));
    }
};

// A key of the nib: its frame in the box (from the bottom left), its title, its operator
struct KeyDef {
    double x, y, w, h;
    Title title;
    int code;
};

// The controller's own keys
enum Special { Digit0 = -100, Point = -50, SecondFunction = -51, ModeKey = -52, ExponentKey = -53 };

// BasicAdvanced.nib's _BasicFunctionsBox (204 x 197)
const std::vector<KeyDef> &basicKeys()
{
    static const std::vector<KeyDef> keys = {
        {2, 161, 49, 32, {"MC"}, MemClear},   {52, 161, 49, 32, {"M+"}, MemPlus},   {102, 161, 49, 32, {"M-"}, MemMinus},
        {152, 161, 49, 32, {"MR"}, MemRecall}, {2, 129, 49, 32, {"C"}, Clear},      {52, 129, 49, 32, {"±"}, Negate},
        {102, 129, 49, 32, {"÷"}, Divide},    {152, 129, 49, 32, {"×"}, Times},     {2, 97, 49, 32, {"7"}, Digit0 + 7},
        {52, 97, 49, 32, {"8"}, Digit0 + 8},  {102, 97, 49, 32, {"9"}, Digit0 + 9}, {152, 97, 49, 32, {"−"}, Minus},
        {2, 65, 49, 32, {"4"}, Digit0 + 4},   {52, 65, 49, 32, {"5"}, Digit0 + 5},  {102, 65, 49, 32, {"6"}, Digit0 + 6},
        {152, 65, 49, 32, {"+"}, Plus},       {2, 33, 49, 32, {"1"}, Digit0 + 1},   {52, 33, 49, 32, {"2"}, Digit0 + 2},
        {102, 33, 49, 32, {"3"}, Digit0 + 3}, {152, 1, 49, 64, {"="}, Equals},      {2, 1, 99, 32, {"0"}, Digit0},
        {102, 1, 49, 32, {"."}, Point},
    };
    return keys;
}

// BasicAdvanced.nib's _AdvancedFunctionsBox (205 x 197); the second functions' titles after 2nd
struct SciDef {
    KeyDef key;
    Title second;
    int secondCode;
};
const std::vector<SciDef> &scientificKeys()
{
    static const std::vector<SciDef> keys = {
        {{3, 161, 49, 32, {"2", "nd"}, SecondFunction}, {}, 0},
        {{53, 161, 49, 32, {"("}, LeftParen}, {}, 0},
        {{103, 161, 49, 32, {")"}, RightParen}, {}, 0},
        {{153, 161, 49, 32, {"%"}, Percent}, {}, 0},
        {{3, 129, 49, 32, {"1/x"}, OneOverX}, {}, 0},
        {{53, 129, 49, 32, {"x", "2"}, Squared}, {}, 0},
        {{103, 129, 49, 32, {"x", "3"}, Cubed}, {}, 0},
        {{153, 129, 49, 32, {"y", "x"}, Power}, {"√y", "", "", "x"}, XRootY},
        {{3, 97, 49, 32, {"x!"}, Factorial}, {}, 0},
        {{53, 97, 49, 32, {"√"}, Sqrt}, {"∛"}, CubeRoot},
        {{103, 97, 49, 32, {"Rad"}, ModeKey}, {}, 0},
        {{153, 97, 49, 32, {"log", "", "2"}, Log2}, {"2", "x"}, TwoToX},
        {{3, 65, 49, 32, {"sin"}, Sin}, {"sin", "-1"}, ASin},
        {{53, 65, 49, 32, {"cos"}, Cos}, {"cos", "-1"}, ACos},
        {{103, 65, 49, 32, {"tan"}, Tan}, {"tan", "-1"}, ATan},
        {{153, 65, 49, 32, {"log", "", "10"}, Log10}, {"10", "x"}, TenToX},
        {{3, 33, 49, 32, {"sinh"}, Sinh}, {"sinh", "-1"}, ASinh},
        {{53, 33, 49, 32, {"cosh"}, Cosh}, {"cosh", "-1"}, ACosh},
        {{103, 33, 49, 32, {"tanh"}, Tanh}, {"tanh", "-1"}, ATanh},
        {{153, 33, 49, 32, {"ln"}, Ln}, {"e", "x"}, EToX},
        {{3, 1, 49, 32, {"π"}, Pi}, {}, 0},
        {{53, 1, 49, 32, {"e"}, E}, {}, 0},
        {{103, 1, 49, 32, {"Rand"}, Random}, {}, 0},
        {{153, 1, 49, 32, {"EE"}, ExponentKey}, {}, 0},
    };
    return keys;
}

// Hexadecimal.nib's o_ButtonBox (346 x 197, at (-1, -2) in the view box): its keys, their tags
// (handleOperatorPress:, handleBitManipulation:, clear:, allClear:, enter:), the digits by title
struct ProgrammerDef {
    double x, y, w, h;
    const char *title;
    int tag;   // 0: a digit
    double font;
};
const std::vector<ProgrammerDef> &programmerKeys()
{
    static const std::vector<ProgrammerDef> keys = {
        {3, 160, 53, 29, "AND", PAnd, 10},          {58, 160, 53, 29, "OR", POr, 10},
        {3, 129, 53, 29, "NOR", PNor, 10},          {58, 129, 53, 29, "XOR", PXor, 10},
        {3, 98, 53, 29, "<<", ShiftLeft1, 10},      {58, 98, 53, 29, ">>", ShiftRight1, 10},
        {3, 67, 53, 29, "X<<Y", ShiftLeftY, 10},    {58, 67, 53, 29, "X>>Y", ShiftRightY, 10},
        {3, 36, 108, 29, "Byte Flip", ByteFlip, 10}, {3, 5, 108, 29, "Word Flip", WordFlip, 10},
        {118, 160, 41, 29, "D", 0, 13},  {160, 160, 41, 29, "E", 0, 13},  {202, 160, 41, 29, "F", 0, 13},
        {118, 129, 41, 29, "A", 0, 13},  {160, 129, 41, 29, "B", 0, 13},  {202, 129, 41, 29, "C", 0, 13},
        {118, 98, 41, 29, "7", 0, 13},   {160, 98, 41, 29, "8", 0, 13},   {202, 98, 41, 29, "9", 0, 13},
        {118, 67, 41, 29, "4", 0, 13},   {160, 67, 41, 29, "5", 0, 13},   {202, 67, 41, 29, "6", 0, 13},
        {118, 36, 41, 29, "1", 0, 13},   {160, 36, 41, 29, "2", 0, 13},   {202, 36, 41, 29, "3", 0, 13},
        {118, 5, 41, 29, "FF", 0, 10},   {160, 5, 41, 29, "0", 0, 13},    {202, 5, 41, 29, "00", 0, 10},
        {249, 160, 47, 29, "AC", PAllClear, 13},    {297, 160, 47, 29, "C", PClear, 13},
        {249, 129, 47, 29, "÷", PDivide, 13},       {297, 129, 47, 29, "−", PMinus, 13},
        {249, 98, 47, 29, "×", PTimes, 13},         {297, 98, 47, 29, "+", PPlus, 13},
        {249, 67, 47, 29, "RoL", RotateLeft, 11},   {297, 67, 47, 29, "RoR", RotateRight, 11},
        {249, 36, 47, 29, "2’s", TwosComplement, 13}, {297, 36, 47, 29, "1’s", OnesComplement, 13},
        {249, 5, 95, 29, "=", PEquals, 12},
    };
    return keys;
}

// The binary view (o_BinaryBox): the 64 bits in two rows, 63 to 32 over 31 to 0, each a button
// showing 0 or 1 (Lucida Grande 10; tag 1000 + the bit), grouped by four; the bit numbers 63, 47,
// 32 over the first row, 31, 15, 0 under the second (9)
class BitsView : public View {
public:
    uint64_t value = 0;
    std::function<void(int bit, bool on)> onToggle;
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(255, 0.35));
        Color::gray(0, 0.25).set(cr);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, 0.5, 0.5, frame().w - 1, frame().h - 1);
        cairo_stroke(cr);
        TextStyle t;
        t.font = Font::system(10);
        t.align = Align::Center;
        for (int bit = 0; bit < 64; ++bit) {
            const Rect r = bitRect(bit);
            drawText(cr, (value >> bit) & 1 ? "1" : "0", r, t);
        }
        TextStyle n;
        n.font = Font::system(9);
        n.color = Color::gray(0, 0.6);
        n.align = Align::Center;
        // the nib's fields: 63, 47, 32 between the rows, 31, 15, 0 under them
        const std::pair<int, double> top[] = {{63, 12}, {47, 178}, {32, 331}}, bottom[] = {{31, 12}, {15, 178}, {0, 331}};
        for (const auto &[b, x] : top)
            drawText(cr, std::to_string(b), Rect{x - 12, 16, 24, 13}, n);
        for (const auto &[b, x] : bottom)
            drawText(cr, std::to_string(b), Rect{x - 12, 40, 24, 13}, n);
    }
    void mouseDown(Event &e) override
    {
        for (int bit = 0; bit < 64; ++bit)
            if (bitRect(bit).contains(e.location) && onToggle)
                onToggle(bit, !((value >> bit) & 1));
    }

private:
    // the nib's frames: the groups of four at 6, 47, 89... 299, the bits 9 apart in a group; the
    // upper row 5 down, the lower 27
    Rect bitRect(int bit) const
    {
        static const double groups[] = {6, 47, 89, 131, 173, 215, 257, 299};
        const int inRow = bit % 32;
        const int group = 7 - inRow / 4, within = 3 - inRow % 4;
        return {groups[group] + within * 9, bit >= 32 ? 5.0 : 27.0, 10, 13};
    }
};

void copyText(const std::string &text)
{
    const char *cmd = std::getenv("WAYLAND_DISPLAY") ? "wl-copy 2>/dev/null" : "xclip -selection clipboard 2>/dev/null";
    if (FILE *p = popen(cmd, "w")) {
        fwrite(text.data(), 1, text.size(), p);
        pclose(p);
    }
}

std::string pasteText()
{
    const char *cmd = std::getenv("WAYLAND_DISPLAY") ? "wl-paste -n 2>/dev/null" : "xclip -selection clipboard -o 2>/dev/null";
    std::string out;
    if (FILE *p = popen(cmd, "r")) {
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0 && out.size() < 4096)
            out.append(buf, n);
        pclose(p);
    }
    return out;
}


// ---- Large Type (-[LCDController drawLargeType:], CustomWindow, CustomView)
//
// The number shown (0 when it is 0) in white bold system type, sized so that it is the screen's
// width less 160 at most, 200 points at most (contentRectForString:), in a window as tall as the
// type's size and its width plus 60, centred on the screen; the view a black rounded rectangle at
// 0.64 (corners 20) under the text, with a shadow. CustomWindow: borderless, at the status level,
// gone at a key, a click, or when another window becomes key.
class LargeTypeView : public View {
public:
    std::string value;
    double size = 72;
    void draw(cairo_t *cr) override
    {
        const double w = frame().w, h = frame().h, r = 20;
        // the corners: no alpha channel to leave them clear, black as the rest at the window's 0.64
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        cairo_new_path(cr);
        cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
        cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
        cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, r, r, r, M_PI, 1.5 * M_PI);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_fill(cr);
        TextStyle t;
        t.font = Font::boldSystem(size);
        t.align = Align::Center;
        t.elide = false;
        t.color = Color::gray(0, 0.5);
        drawText(cr, value, Rect{1, -2, w, h}, t);   // the shadow
        t.color = Color::gray(255);
        drawText(cr, value, Rect{0, -3, w, h}, t);
    }
    void mouseDown(Event &) override
    {
        if (onDismiss)
            onDismiss();
    }
    std::function<void()> onDismiss;
};

class LargeType {
public:
    void show(const std::string &value)
    {
        const Rect screen = App::shared().visibleFrame();
        // contentRectForString:: measured at 72, the size the screen's width less 160 allows
        double size = (screen.w - 100 - 60) * 72 / std::max(1.0, textWidth(value, Font::boldSystem(72)));
        if (size > 200)
            size = 200;
        const double width = textWidth(value, Font::boldSystem(size)) + 60;
        m_window = std::make_unique<Window>(Size{width, size}, "Large Type");
        m_window->setBorderless();
        m_window->setFloating(true);
        // AquaKit's windows have no alpha channel: the window at 0.64 as a whole rather than its
        // background alone
        m_window->setAlphaValue(0.64);
        auto *v = m_window->contentView()->add<LargeTypeView>();
        v->setFrame({0, 0, width, size});
        v->value = value;
        v->size = size;
        v->onDismiss = [this] { dismiss(); };
        m_window->onUnhandledKey = [this](Event &) { dismiss(); };
        m_window->onKeyChanged = [this](bool key) {
            if (!key)
                dismiss();
        };
        m_window->moveTo({screen.x + screen.w / 2 - width / 2, screen.y + screen.h / 2 - size / 2});
        m_window->show();
    }
    void dismiss()
    {
        if (m_window)
            App::shared().later([this] { m_window.reset(); });
    }

private:
    std::unique_ptr<Window> m_window;
};

// ---- The paper tape (PaperTapeController, Calculator.nib's window: 228 x 267, its text view in a
// scroll view at (10, 32) 208 x 225, Clear at (161, 8) 58 x 16 in Lucida Grande 9)
class PaperTape {
public:
    std::function<void(bool shown)> onShownChanged;
    bool isShown() const { return m_window && m_window->isVisible(); }
    bool hasText() const { return !m_text.empty(); }
    const std::string &text() const { return m_text; }

    // -setCurrentEquation:: the tape's last line becomes the equation
    void setCurrentEquation(const std::string &eq)
    {
        const size_t nl = m_text.rfind('\n');
        m_text = (nl == std::string::npos ? std::string() : m_text.substr(0, nl + 1)) + eq;
        update();
    }
    // -completeEquationWithResult:: "\n=%@", then -startNewEquation: "\n\n"
    void completeEquation(const std::string &result)
    {
        m_text += "\n=" + result + "\n\n";
        update();
    }
    void clear()
    {
        m_text.clear();
        update();
    }

    void show(Window *parent)
    {
        if (!m_window) {
            // the nib's style 263: textured (one gradient with the title bar), closable, miniaturizable;
            // at the normal level (-awakeFromNib)
            m_window = std::make_unique<Window>(Size{228, 267}, "Paper Tape");
            m_window->setRoundedBottomCorners(true);
            m_window->setUnifiedToolbarHeight(267);
            View *root = m_window->contentView();
            root->add<Background>()->setFrame({0, 0, 228, 267});
            m_scroll = root->add<TextScrollView>();
            m_scroll->setFrame({10, 267 - 32 - 225, 208, 225});
            m_scroll->textView()->editable = false;
            auto *clear = root->add<Button>("Clear", Button::Push);
            clear->small = true;
            clear->setFrame({161, 267 - 8 - 16, 58, 16});
            clear->action = [this] { this->clear(); };
            m_window->onClose = [this] {
                // -windowWillClose:: hidden, the menu's title back to Show Paper Tape
                Settings("calculator").set("PaperTapeVisibleDefaultsKey", false);
                if (onShownChanged)
                    onShownChanged(false);
            };
            update();
        }
        // -adjustTapeWindowPosition:: 20 points right of the calculator
        if (parent) {
            const AK::Point p = parent->screenOrigin();
            m_window->moveTo({p.x + parent->size().w + 20, p.y});
        }
        m_window->show();
        Settings("calculator").set("PaperTapeVisibleDefaultsKey", true);
        if (onShownChanged)
            onShownChanged(true);
    }
    void hide()
    {
        if (!m_window)
            return;
        m_window->close();
    }

    // -saveTape:: the save panel with "tape.txt", the text written as UTF-8
    void save()
    {
        const char *home = std::getenv("HOME");
        SavePanel::run("Save Tape As", std::string(home ? home : "") + "/Documents/" + tr("tapeSavedFileName"), {},
                       [text = m_text](std::string path) {
                           if (path.empty())
                               return;
                           std::ofstream f(path, std::ios::binary);
                           f << text;
                       });
    }
    // -printTape:: NSPrintOperation over the text view, not centred; here the text given to lp
    void print()
    {
        char tmpl[] = "/tmp/calculator-tape-XXXXXX";
        const int fd = mkstemp(tmpl);
        if (fd < 0)
            return;
        if (FILE *f = fdopen(fd, "w")) {
            std::fputs(m_text.c_str(), f);
            std::fclose(f);
        }
        std::vector<std::string> argv = {"lp", "-t", "Calculator", tmpl};
        std::vector<char *> args;
        for (std::string &a : argv)
            args.push_back(a.data());
        args.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, "lp", nullptr, nullptr, args.data(), environ) == 0) {
            const std::string file = tmpl;
            App::shared().addTimer(5, false, [pid, file] {
                waitpid(pid, nullptr, WNOHANG);
                std::remove(file.c_str());
            });
        } else {
            std::remove(tmpl);
        }
    }

private:
    void update()
    {
        if (!m_scroll)
            return;
        Style st;
        st.family = "Lucida Grande";
        st.size = 10;
        TextView *tv = m_scroll->textView();
        tv->setText(fromPlainText(m_text, st));
        tv->scrollTo(tv->maxScrollOffset());   // moveToEndOfDocument:
        tv->setNeedsDisplay();
    }
    std::string m_text;
    std::unique_ptr<Window> m_window;
    TextScrollView *m_scroll = nullptr;
};

// ---- Conversions (ConversionController, ConversionSheet.nib, CalcConversion)
//
// ConversionCategories.plist's categories in the Convert menu, sorted by their names (UnitNames),
// each "…" (prepareConversion:); the sheet (326 x 187): Convert: the category, From: and To: their
// units (UnitNames' names, sorted), the last conversion of the category chosen again; Convert
// (executeConversion:): the number shown times ConversionsFromBase's factor for the unit from, times
// ConversionsToBase's for the unit to (temperature: their operations, in turn), worked by a fresh
// RPNEngine (-[BasicAdvancedController handleConversion:]: enter, the number, the operator); currency
// by FinancialRates.xml's rates from the dollar (the rate to, times one over the rate from), "Last
// Updated:" its timestamp. The conversion goes first in Recent Conversions (20 at most, kept in
// RecentConversionsDefaultsKey_2), "%@ to %@".
struct Conversion {
    std::string type, from, to;
    bool operator==(const Conversion &o) const { return type == o.type && from == o.from && to == o.to; }
};

class Conversions {
public:
    Conversions()
    {
        m_categories = readStringArray(res("ConversionCategories.plist"));
        m_from = readTable(res("ConversionsFromBase.plist"));
        m_to = readTable(res("ConversionsToBase.plist"));
        loadCurrency();
        Settings prefs("calculator");
        std::stringstream ss(prefs.string("RecentConversionsDefaultsKey_2", ""));
        for (std::string line; std::getline(ss, line, ';');) {
            const size_t a = line.find('|'), b = line.find('|', a + 1);
            if (a != std::string::npos && b != std::string::npos)
                m_recent.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
        }
    }

    static std::string unitName(const std::string &key) { return Assets::tr(Bundle, "UnitNames", key, key); }

    std::vector<std::string> sortedCategories() const
    {
        std::vector<std::string> c = m_categories;
        std::sort(c.begin(), c.end(), [](const std::string &a, const std::string &b) { return lower(unitName(a)) < lower(unitName(b)); });
        return c;
    }
    // the units of a category, as the popups list them: (their key, their title)
    std::vector<std::pair<std::string, std::string>> units(const std::string &type) const
    {
        std::vector<std::pair<std::string, std::string>> out;
        if (type == "currency") {
            for (const auto &[code, rate] : m_rates)
                out.push_back({code, currencyName(code)});
            std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return lower(a.second) < lower(b.second); });
            return out;
        }
        auto it = m_from.find(type);
        if (it == m_from.end())
            return out;
        for (const auto &[unit, ops] : it->second)
            out.push_back({unit, unitName(unit)});
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return lower(a.second) < lower(b.second); });
        return out;
    }

    // executeConversion:'s operations: (operator, number) pairs
    std::vector<std::pair<std::string, std::string>> operations(const Conversion &c) const
    {
        std::vector<std::pair<std::string, std::string>> ops;
        if (c.type == "currency") {
            auto f = m_rates.find(c.from), t = m_rates.find(c.to);
            if (f == m_rates.end() || t == m_rates.end() || std::strtold(f->second.c_str(), nullptr) == 0)
                return ops;
            char one[64];
            std::snprintf(one, sizeof one, "%.16Lg", 1 / std::strtold(f->second.c_str(), nullptr));
            ops.push_back({"*", t->second});
            ops.push_back({"*", one});
            return ops;
        }
        auto pairs = [](const std::vector<std::string> &v) {
            std::vector<std::pair<std::string, std::string>> out;
            if (v.size() == 1)
                out.push_back({"*", v[0]});
            for (size_t i = 0; i + 1 < v.size(); i += 2)
                out.push_back({v[i], v[i + 1]});
            return out;
        };
        auto look = [](const auto &table, const std::string &type, const std::string &unit) -> std::vector<std::string> {
            auto t = table.find(type);
            if (t == table.end())
                return {};
            auto u = t->second.find(unit);
            return u == t->second.end() ? std::vector<std::string>{} : u->second;
        };
        for (const auto &p : pairs(look(m_from, c.type, c.from)))
            ops.push_back(p);
        for (const auto &p : pairs(look(m_to, c.type, c.to)))
            ops.push_back(p);
        return ops;
    }

    // -[CalcConversion localizedTitle]
    std::string title(const Conversion &c) const
    {
        const bool money = c.type == "currency";
        std::string t = tr("%@ to %@");
        const std::string a = money ? currencyName(c.from) : unitName(c.from), b = money ? currencyName(c.to) : unitName(c.to);
        const size_t p = t.find("%@");
        if (p != std::string::npos)
            t.replace(p, 2, a);
        const size_t q = t.find("%@", p == std::string::npos ? 0 : p + a.size());
        if (q != std::string::npos)
            t.replace(q, 2, b);
        return t;
    }

    const std::vector<Conversion> &recent() const { return m_recent; }
    // -addRecentConversion:
    void addRecent(const Conversion &c)
    {
        auto it = std::find(m_recent.begin(), m_recent.end(), c);
        if (it == m_recent.begin() && it != m_recent.end())
            return;
        if (it != m_recent.end())
            m_recent.erase(it);
        m_recent.insert(m_recent.begin(), c);
        if (m_recent.size() > 20)
            m_recent.pop_back();
        save();
    }
    void clearRecent()
    {
        m_recent.clear();
        save();
    }
    const Conversion *lastOfType(const std::string &type) const
    {
        for (const Conversion &c : m_recent)
            if (c.type == type)
                return &c;
        return nullptr;
    }
    std::string ratesDate() const
    {
        // the timestamp of FinancialRates.xml, as the sheet's "Last Updated: 01/20/2009"
        if (!m_timestamp)
            return {};
        char buf[32];
        const time_t t = time_t(m_timestamp);
        std::strftime(buf, sizeof buf, "%m/%d/%Y", std::localtime(&t));
        return tr("last updated") + buf;
    }

    static std::string currencyName(const std::string &code)
    {
        // NSLocale's displayNameForKey:NSLocaleCurrencyCode for the rates' currencies (English)
        static const std::map<std::string, std::string> names = {
            {"AED", "UAE Dirham"}, {"AUD", "Australian Dollar"}, {"BHD", "Bahraini Dinar"}, {"BND", "Brunei Dollar"},
            {"BRL", "Brazilian Real"}, {"CAD", "Canadian Dollar"}, {"CHF", "Swiss Franc"}, {"CLP", "Chilean Peso"},
            {"CNY", "Chinese Yuan Renminbi"}, {"CYP", "Cyprus Pound"}, {"CZK", "Czech Republic Koruna"}, {"DKK", "Danish Krone"},
            {"EUR", "Euro"}, {"GBP", "British Pound Sterling"}, {"HKD", "Hong Kong Dollar"}, {"HUF", "Hungarian Forint"},
            {"IDR", "Indonesian Rupiah"}, {"ILS", "Israeli New Sheqel"}, {"INR", "Indian Rupee"}, {"ISK", "Icelandic Krona"},
            {"JPY", "Japanese Yen"}, {"KRW", "South Korean Won"}, {"KWD", "Kuwaiti Dinar"}, {"KZT", "Kazakhstan Tenge"},
            {"LKR", "Sri Lanka Rupee"}, {"MTL", "Maltese Lira"}, {"MUR", "Mauritius Rupee"}, {"MXN", "Mexican Peso"},
            {"MYR", "Malaysian Ringgit"}, {"NOK", "Norwegian Krone"}, {"NPR", "Nepalese Rupee"}, {"NZD", "New Zealand Dollar"},
            {"OMR", "Rial Omani"}, {"PKR", "Pakistan Rupee"}, {"QAR", "Qatari Rial"}, {"RUB", "Russian Ruble"},
            {"SAR", "Saudi Riyal"}, {"SEK", "Swedish Krona"}, {"SGD", "Singapore Dollar"}, {"SKK", "Slovak Koruna"},
            {"THB", "Thai Baht"}, {"TWD", "New Taiwan Dollar"}, {"USD", "US Dollar"}, {"VEB", "Venezuelan Bolivar"},
            {"ZAR", "South African Rand"},
        };
        auto it = names.find(code);
        return it == names.end() ? code : it->second;
    }

private:
    static std::string lower(std::string s)
    {
        for (char &c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string slurp(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    // the XML property lists: <array> of <string>, and <dict> of <dict> of <string> or <array>
    static std::vector<std::string> readStringArray(const std::string &path)
    {
        std::vector<std::string> out;
        const std::string x = slurp(path);
        for (size_t at = 0; (at = x.find("<string>", at)) != std::string::npos;) {
            const size_t e = x.find("</string>", at);
            out.push_back(x.substr(at + 8, e - at - 8));
            at = e;
        }
        return out;
    }
    using Table = std::map<std::string, std::map<std::string, std::vector<std::string>>>;
    static Table readTable(const std::string &path)
    {
        Table t;
        const std::string x = slurp(path);
        size_t at = x.find("<dict>");
        if (at == std::string::npos)
            return t;
        at += 6;
        auto token = [&](size_t &p) -> std::pair<std::string, std::string> {   // (tag, contents)
            p = x.find('<', p);
            if (p == std::string::npos)
                return {};
            const size_t e = x.find('>', p);
            std::string tag = x.substr(p + 1, e - p - 1);
            p = e + 1;
            if (tag == "key" || tag == "string") {
                const size_t c = x.find("</" + tag + ">", p);
                std::string v = x.substr(p, c - p);
                p = c + tag.size() + 3;
                return {tag, v};
            }
            return {tag, {}};
        };
        std::string category, unit;
        int depth = 1;
        while (depth > 0) {
            auto [tag, v] = token(at);
            if (tag.empty())
                break;
            if (tag == "dict") {
                ++depth;
            } else if (tag == "/dict") {
                --depth;
            } else if (tag == "key") {
                (depth == 1 ? category : unit) = v;
            } else if (tag == "string" && depth == 2) {
                t[category][unit].push_back(v);
            }
        }
        return t;
    }
    void loadCurrency()
    {
        // FinancialRates.xml: /response/result[@timestamp]/list/conversion: from USD, to, convertedamount
        const std::string x = slurp(res("FinancialRates.xml"));
        const size_t ts = x.find("timestamp=\"");
        if (ts != std::string::npos)
            m_timestamp = std::atol(x.c_str() + ts + 11);
        for (size_t at = 0; (at = x.find("<conversion>", at)) != std::string::npos;) {
            const size_t end = x.find("</conversion>", at);
            auto field = [&](const std::string &name) {
                const size_t a = x.find("<" + name + ">", at);
                if (a == std::string::npos || a > end)
                    return std::string();
                const size_t b = x.find("</" + name + ">", a);
                return x.substr(a + name.size() + 2, b - a - name.size() - 2);
            };
            const std::string to = field("to"), amount = field("convertedamount");
            if (!to.empty() && !amount.empty())
                m_rates[to] = amount;
            at = end;
        }
    }
    void save()
    {
        std::string out;
        for (const Conversion &c : m_recent)
            out += (out.empty() ? "" : ";") + c.type + "|" + c.from + "|" + c.to;
        Settings("calculator").set("RecentConversionsDefaultsKey_2", out);
    }
    std::vector<std::string> m_categories;
    Table m_from, m_to;
    std::map<std::string, std::string> m_rates;
    long m_timestamp = 0;
    std::vector<Conversion> m_recent;
};

// ---- Speech (SpeechController): what a key says (its Functions.xml speechName, or Speakable.plist's
// word), the totals; NSSpeechSynthesizer here espeak-ng or spd-say when there is one. Only in English.
class Speech {
public:
    bool buttons = false, total = false;
    static bool available() { return Workspace::inPath("espeak-ng") || Workspace::inPath("spd-say") || Workspace::inPath("espeak"); }
    static void say(std::string text)
    {
        if (text.empty())
            return;
        // the speech commands embedded in the names ("[[emph -]] equals, ")
        for (size_t a; (a = text.find("[[")) != std::string::npos;) {
            const size_t b = text.find("]]", a);
            text.erase(a, b == std::string::npos ? std::string::npos : b + 2 - a);
        }
        const char *prog = Workspace::inPath("espeak-ng") ? "espeak-ng" : Workspace::inPath("spd-say") ? "spd-say" : "espeak";
        std::vector<std::string> argv = {prog, text};
        std::vector<char *> args;
        for (std::string &a : argv)
            args.push_back(a.data());
        args.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawnp(&pid, prog, nullptr, nullptr, args.data(), environ) == 0)
            App::shared().addTimer(10, false, [pid] { waitpid(pid, nullptr, WNOHANG); });
    }
    static std::string speakable(const std::string &key)
    {
        static const std::map<std::string, std::string> words = Assets::plistStrings(std::string(Bundle) + "/Contents/Resources/Speakable.plist");
        auto it = words.find(key);
        return it == words.end() ? key : it->second;
    }
};

class CalculatorWindow {
public:
    CalculatorWindow()
    {
        Settings prefs("calculator");
        const std::string view = prefs.string("ViewDefaultsKey", "Basic");
        m_view = view == "Advanced" ? ScientificView : view == "Programmers" ? ProgrammerView : BasicView;
        m_programmer.base = std::clamp(prefs.integer("InputMode", 10), 8, 16);   // the nib's 10 selected
        if (m_programmer.base != 8 && m_programmer.base != 16)
            m_programmer.base = 10;
        m_binaryShown = prefs.boolean("ShowBinary", false);
        m_decimals = std::clamp(prefs.integer("Precision", 12), 0, 15);   // -[LCDController awakeFromNib]: setPrecision: 12
        m_separators = prefs.boolean("ShowSeparators", false);
        m_engine.load(Assets::path(std::string(PlugIn) + "/Contents/Resources/Functions.xml"));
        m_engine.beep = [] { std::fputc('\a', stderr); };
        m_engine.tapePrecision = m_decimals;
        m_engine.onTapeEquation = [this](const std::string &eq) { m_tape.setCurrentEquation(eq); };
        m_engine.onTapeResult = [this](const std::string &r) { m_tape.completeEquation(r); };
        // SpeechController: SpeaksButtonPressedDefaultsKey, SpeaksTotalDefaultsKey
        m_speech.buttons = prefs.boolean("SpeaksButtonPressedDefaultsKey", false);
        m_speech.total = prefs.boolean("SpeaksTotalDefaultsKey", false);
        m_engine.onSpeakButton = [this](const std::string &name) {
            if (m_speech.buttons && m_view != ProgrammerView)
                Speech::say(Speech::speakable(name));
        };
        m_engine.onSpeakTotal = [this](const std::string &total) {
            if (m_speech.total && m_view != ProgrammerView)
                Speech::say(total);
        };
        // -[CalculatorController toggleRPNMode:]'s RPNDefaultsKey
        m_rpn = prefs.boolean("RPNDefaultsKey", false);
        m_engine.setRPN(m_rpn && m_view != ProgrammerView);
        m_programmer.setRPN(m_rpn && m_view == ProgrammerView);
        m_tape.onShownChanged = [](bool) {};

        m_window = std::make_unique<Window>(contentSize(), "Calculator", Window::Titled);
        m_window->setIcon(Image::load(res("Calculator.icns"), {128, 128}));
        m_window->setRoundedBottomCorners(true);   // NSTexturedBackgroundWindowMask (the nib's style 0x10f)
        m_window->onUnhandledKey = [this](Event &e) { key(e); };
        m_window->onKeyChanged = [this](bool) { m_window->contentView()->setNeedsDisplay(); };
        m_window->onClose = [] { App::shared().quit(); };   // the last window: Calculator quits
        build();
        loadMenus();
        m_window->show();
        if (prefs.boolean("PaperTapeVisibleDefaultsKey", false))
            App::shared().later([this] { m_tape.show(m_window.get()); });
    }

private:
    // RPN (-[LCDController _showRegRPNView], _showWideRPNView): Basic's LCD 76 taller with the
    // stack and its four keys under it (the window 111 taller); Scientific's ("wide") 76 taller and
    // 52 narrower, the keys in a column at its right
    bool rpnShown() const { return m_rpn; }   // the three views support it (supportsRPN)
    bool rpnWide() const { return m_view != BasicView; }
    double rpnExtra() const { return rpnShown() ? (rpnWide() ? 76 : 111) : 0; }
    double displayHeight() const { return DisplayHeight + (rpnShown() ? 76 : 0); }

    Size contentSize() const
    {
        if (m_view == ProgrammerView)
            return {ProgrammerWidth + 14, programmerHeight() + DisplayHeight + 8 + rpnExtra()};
        const double boxes = BasicWidth + (m_view == ScientificView ? ScientificWidth : 0);
        return {boxes + 14, BoxHeight + DisplayHeight + 8 + rpnExtra()};
    }

    // The nib's segmented controls' labels: Lucida Grande 11, white on the selected (dark) segment
    static std::function<void(cairo_t *, int, const Rect &, bool)> smallLabels(SegmentedControl *c, std::vector<std::string> labels)
    {
        return [c, labels](cairo_t *cr, int i, const Rect &r, bool enabled) {
            TextStyle t;
            t.font = Font::system(11);
            t.align = Align::Center;
            t.elide = false;
            t.color = !enabled ? Color::gray(0, 0.35) : c->isHighlighted(i) ? Color::gray(255) : Color::gray(0);
            drawText(cr, labels[size_t(i)], Rect{r.x - 3, r.y, r.w + 6, r.h}, t);
        };
    }

    double programmerHeight() const { return ProgrammerHeight - (m_binaryShown ? 0 : BinaryHeight); }

    // Hexadecimal.nib's view box at (7, 7) from the bottom left: its keys' box at (-1, -2), the
    // ASCII / Unicode control, Show Binary and the 8 / 10 / 16 control 21 under its top, the binary
    // box (when shown) under them
    void buildProgrammer()
    {
        View *root = m_window->contentView();
        const Size s = contentSize();
        const double bh = programmerHeight();
        auto at = [&](double x, double y, double w, double h) {   // the view box's (from its bottom) to the window's
            return Rect{7 + x, s.h - 7 - y - h, w, h};
        };
        for (const ProgrammerDef &d : programmerKeys()) {
            auto *k = root->add<Key>();
            k->setFrame(at(-1 + d.x, -2 + d.y, d.w, d.h));
            k->title = {d.title};
            k->fontSize = d.font;
            k->code = d.tag ? 1000 + d.tag : 2000;
            if (rpnShown() && d.tag == PEquals)
                k->title = {tr("enter")};   // -[HexadecimalController setRPNMode:]
            k->action = [this, k] { pressProgrammer(k); };
            m_keys.push_back(k);
        }
        auto *alt = root->add<SegmentedControl>(CoreUI::SegmentStyle::Textured, std::vector<std::string>{"ASCII", "Unicode"},
                                                std::vector<double>{48, 48});
        alt->setFrame(at(2, bh - 21, 99, 19));
        alt->glyph = smallLabels(alt, {"ASCII", "Unicode"});
        alt->setSelected(m_unicode ? 1 : 0);
        alt->onSelect = [this](int i) {
            m_unicode = i == 1;
            refresh();
        };
        auto *binary = root->add<Key>();
        binary->setFrame(at(128, bh - 21, 103, 20));
        binary->title = {tr(m_binaryShown ? "Hide Binary" : "Show Binary")};
        binary->fontSize = 10;
        binary->action = [this] {
            // -[HexadecimalController hideShowBinary:]: the window grows by the binary box
            m_binaryShown = !m_binaryShown;
            Settings("calculator").set("ShowBinary", m_binaryShown);
            m_window->setContentSize(contentSize());
            App::shared().later([this] { build(); });
        };
        auto *bases = root->add<SegmentedControl>(CoreUI::SegmentStyle::Textured, std::vector<std::string>{"8", "10", "16"},
                                                  std::vector<double>{28, 28, 28});
        bases->setFrame(at(256, bh - 21, 87, 19));
        bases->glyph = smallLabels(bases, {"8", "10", "16"});
        bases->setSelected(m_programmer.base == 8 ? 0 : m_programmer.base == 10 ? 1 : 2);
        bases->onSelect = [this](int i) {
            // -[HexadecimalController selectInputMode:]
            m_programmer.setBase(i == 0 ? 8 : i == 1 ? 10 : 16);
            Settings("calculator").set("InputMode", m_programmer.base);
            refresh();
        };
        m_bits = nullptr;
        if (m_binaryShown) {
            m_bits = root->add<BitsView>();
            m_bits->setFrame(at(-1, 195, 346, BinaryHeight));
            m_bits->onToggle = [this](int bit, bool on) {
                m_programmer.setBit(bit, on);
                refresh();
            };
        }
    }

    void build()
    {
        View *root = m_window->contentView();
        root->removeAllSubviews();
        m_keys.clear();
        const Size s = contentSize();
        m_window->setUnifiedToolbarHeight(int(s.h));   // one gradient with the title bar
        auto *bg = root->add<Background>();
        bg->setFrame({0, 0, s.w, s.h});
        m_lcd = root->add<LCD>();
        const bool wide = rpnShown() && rpnWide();
        m_lcd->setFrame({9, 0, s.w - 18 - (wide ? 52 : 0), displayHeight()});
        m_lcd->onContextMenu = [this](Event &e) { contextMenu(e); };
        if (rpnShown()) {
            // Calculator.nib's RPN keys: x↔y (tag 80), R↓ (81), R↑ (82), drop (83); Basic's in a row
            // under the LCD ((-1, -35) from it, 49 x 30 at 2, 52, 102, 152), the wide view's in a
            // column at the LCD's right (52 x 128, the keys at 96, 64, 32, 0 from its bottom)
            const std::pair<const char *, int> keys[] = {{"x↔y", RPNExchange}, {"R↓", RPNRollDown}, {"R↑", RPNRollUp}, {"drop", RPNDrop}};
            for (int i = 0; i < 4; ++i) {
                auto *k = root->add<Key>();
                if (wide)
                    k->setFrame({9 + m_lcd->frame().w + 1, 2 + 32.0 * i, 49, 30});
                else
                    k->setFrame({8 + 2 + 50.0 * i, displayHeight() + 5, 49, 30});
                k->title = {keys[i].first};
                k->code = keys[i].second;
                k->action = [this, k] { press(k); };
                m_keys.push_back(k);
            }
        }
        // the boxes at (7, 7) from the bottom left; Scientific's first, Basic's after it
        const double top = s.h - 7 - BoxHeight;
        if (m_view == ProgrammerView) {
            buildProgrammer();
            refresh();
            return;
        }
        double x = 7;
        if (m_view == ScientificView) {
            for (const SciDef &d : scientificKeys())
                addKey(d.key, x, top);
            x += ScientificWidth;
        }
        for (const KeyDef &d : basicKeys())
            addKey(d, x, top);
        updateTitles();
        refresh();
    }

    void addKey(const KeyDef &d, double boxX, double boxTop)
    {
        auto *k = m_window->contentView()->add<Key>();
        // the nib's frame, from the box's bottom, 2 points less high at its top
        k->setFrame({boxX + d.x, boxTop + BoxHeight - d.y - d.h, d.w, d.h - 2});
        k->title = d.title;
        k->code = d.code;
        // -[BasicAdvancedController setRPNMode:]: = becomes enter (tag 6), the parentheses disabled
        if (rpnShown() && d.code == Equals) {
            k->title = {tr("ENTER")};
            k->code = Enter;
        }
        if (rpnShown() && (d.code == LeftParen || d.code == RightParen))
            k->setEnabled(false);
        k->action = [this, k] { press(k); };
        m_keys.push_back(k);
    }

    // 2nd and Deg/Rad change titles (-[BasicAdvancedController handleSecondFunctionPress:], handleModePress:)
    void updateTitles()
    {
        if (m_view != ScientificView)
            return;
        for (Key *k : m_keys) {
            for (const SciDef &d : scientificKeys()) {
                if (d.key.code != k->code && d.secondCode != k->code)
                    continue;
                if (d.secondCode) {
                    k->code = m_second ? d.secondCode : d.key.code;
                    k->title = m_second ? d.second : d.key.title;
                }
                if (d.key.code == SecondFunction)
                    k->on = m_second;
                if (d.key.code == ModeKey)
                    k->title = {m_engine.radians ? tr("Deg") : tr("Rad")};
                k->setNeedsDisplay();
            }
        }
    }

    void press(Key *k)
    {
        const int code = k->code;
        if (m_view == ProgrammerView) {
            // the RPN keys over the Programmer view (HexadecimalController's rpnExchange...)
            if (code == RPNExchange)
                m_programmer.rpnExchange();
            else if (code == RPNDrop)
                m_programmer.rpnDrop();
            else if (code == RPNRollUp || code == RPNRollDown)
                m_programmer.rpnRoll(code == RPNRollUp);
            refresh();
            return;
        }
        if (m_speech.buttons && code >= Digit0 && code <= Digit0 + 9)
            Speech::say(Speech::speakable(std::to_string(code - Digit0)));
        if (code >= Digit0 && code <= Digit0 + 9)
            m_engine.digit(code - Digit0);
        else if (code == Point)
            m_engine.decimalPoint();
        else if (code == ExponentKey)
            m_engine.exponent();
        else if (code == SecondFunction) {
            m_second = !m_second;
            updateTitles();
            return;
        } else if (code == ModeKey) {
            m_engine.radians = !m_engine.radians;
            updateTitles();
        } else if (code == Negate)
            m_engine.negate();
        else
            m_engine.op(code);
        refresh();
    }

    void pressProgrammer(Key *k)
    {
        const int tag = k->code - 1000;
        if (k->code == 2000)
            m_programmer.digit(k->title.base);
        else if (tag == PEquals && m_programmer.rpn)
            m_programmer.enter();
        else if (tag == PEquals)
            m_programmer.equals();
        else if (tag == PClear)
            m_programmer.clear();
        else if (tag == PAllClear)
            m_programmer.allClear();
        else if (tag == TwosComplement || tag == OnesComplement || tag == ByteFlip || tag == WordFlip || tag == RotateLeft ||
                 tag == RotateRight)
            m_programmer.bits(tag);
        else
            m_programmer.op(tag);
        refresh();
    }

    void refresh()
    {
        if (m_view == ProgrammerView) {
            const uint64_t v = m_programmer.value();
            m_lcd->text = m_programmer.error() ? tr("DivByZero") : m_programmer.display();
            m_lcd->memory = m_lcd->parentheses = false;
            m_lcd->mode.clear();
            m_lcd->rpn = rpnShown();
            m_lcd->rows.clear();
            if (rpnShown()) {
                const std::vector<uint64_t> &stack = m_programmer.stack();
                const size_t n = std::max<size_t>(4, stack.size() + 1);
                for (size_t r = 0; r + 1 < n; ++r) {
                    const size_t index = n - 2 - r;
                    m_lcd->rows.push_back(index < stack.size() ? m_programmer.rowString(stack[index]) : std::string());
                }
                m_lcd->rows.push_back(m_lcd->text);
            }
            // the left LCD: the number as a character (ASCII: up to 127; Unicode: a code point)
            m_lcd->left.clear();
            if (m_unicode ? (v >= 32 && v < 0x110000 && (v < 0xD800 || v > 0xDFFF) && v != 127) : (v >= 32 && v < 127)) {
                const uint32_t c = uint32_t(v);
                std::string u;
                if (c < 0x80) {
                    u = std::string(1, char(c));
                } else if (c < 0x800) {
                    u += char(0xC0 | (c >> 6));
                    u += char(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    u += char(0xE0 | (c >> 12));
                    u += char(0x80 | ((c >> 6) & 0x3F));
                    u += char(0x80 | (c & 0x3F));
                } else {
                    u += char(0xF0 | (c >> 18));
                    u += char(0x80 | ((c >> 12) & 0x3F));
                    u += char(0x80 | ((c >> 6) & 0x3F));
                    u += char(0x80 | (c & 0x3F));
                }
                m_lcd->left = u;
            }
            // the digits of the base only
            for (Key *k : m_keys) {
                if (k->code != 2000)
                    continue;
                const std::string &d = k->title.base;
                const int digit = std::isdigit(static_cast<unsigned char>(d[0])) ? d[0] - '0' : d[0] - 'A' + 10;
                k->setEnabled(digit < m_programmer.base);
            }
            if (m_bits) {
                m_bits->value = v;
                m_bits->setNeedsDisplay();
            }
            m_lcd->setNeedsDisplay();
            return;
        }
        m_lcd->left.clear();
        m_lcd->text = m_engine.display(m_decimals, m_separators);
        m_lcd->rpn = rpnShown();
        m_lcd->rows.clear();
        if (rpnShown()) {
            const std::vector<long double> stack = m_engine.rpnStack();
            const size_t n = std::max<size_t>(4, stack.size() + 1);
            for (size_t r = 0; r + 1 < n; ++r) {
                const size_t index = n - 2 - r;   // the deepest first
                m_lcd->rows.push_back(index < stack.size() ? formatNumber(stack[index], m_decimals, m_separators) : std::string());
            }
            m_lcd->rows.push_back(m_lcd->text);
        }
        m_lcd->memory = m_engine.memoryShown();
        m_lcd->parentheses = m_engine.parenthesesOpen();
        m_lcd->mode = m_view == ScientificView ? (m_engine.radians ? tr("Rad") : tr("Deg")) : std::string();
        m_lcd->setNeedsDisplay();
    }

    // The Programmer view's keys typed (-[HexadecimalController keyDown:]): the digits of the base,
    // the nib's key equivalents (+ - * / & | ~ < >), Return for =, Escape for AC, Delete
    void programmerKey(Event &e)
    {
        const uint32_t k = e.keysym;
        if (k == XKB_KEY_BackSpace || k == XKB_KEY_Delete) {
            m_programmer.deleteDigit();
            refresh();
            return;
        }
        std::string title;
        if (k >= XKB_KEY_0 && k <= XKB_KEY_9)
            title = std::string(1, char('0' + (k - XKB_KEY_0)));
        else if (k >= XKB_KEY_KP_0 && k <= XKB_KEY_KP_9)
            title = std::string(1, char('0' + (k - XKB_KEY_KP_0)));
        else if ((k >= XKB_KEY_a && k <= XKB_KEY_f) || (k >= XKB_KEY_A && k <= XKB_KEY_F))
            title = std::string(1, char(std::toupper(int(k))));
        int tag = 0;
        if (k == XKB_KEY_plus || k == XKB_KEY_KP_Add)
            tag = PPlus;
        else if (k == XKB_KEY_minus || k == XKB_KEY_KP_Subtract)
            tag = PMinus;
        else if (k == XKB_KEY_asterisk || k == XKB_KEY_KP_Multiply)
            tag = PTimes;
        else if (k == XKB_KEY_slash || k == XKB_KEY_KP_Divide)
            tag = PDivide;
        else if (k == XKB_KEY_ampersand)
            tag = PAnd;
        else if (k == XKB_KEY_bar)
            tag = POr;
        else if (k == XKB_KEY_asciitilde)
            tag = PXor;
        else if (k == XKB_KEY_less)
            tag = ShiftLeft1;
        else if (k == XKB_KEY_greater)
            tag = ShiftRight1;
        else if (k == XKB_KEY_Return || k == XKB_KEY_KP_Enter || k == XKB_KEY_equal)
            tag = PEquals;
        else if (k == XKB_KEY_Escape)
            tag = PAllClear;
        for (Key *key : m_keys) {
            const bool match = title.empty() ? (tag && key->code == 1000 + tag) : (key->code == 2000 && key->title.base == title);
            if (match && key->isEnabled()) {
                key->flash();
                pressProgrammer(key);
                return;
            }
        }
    }

    Key *keyFor(int code)
    {
        for (Key *k : m_keys)
            if (k->code == code)
                return k;
        return nullptr;
    }

    // The keys typed: the nib's key equivalents (digits, + - * / = Return, Escape for C, % ( ) ^ ! p e E),
    // the delete key (userDeletedDigit), the menus' with Command
    void key(Event &e)
    {
        // ⌘↑ and ⌘↓: R↑ and R↓ in RPN (keyDown:'s NSUpArrowFunctionKey, NSDownArrowFunctionKey)
        if ((e.modifiers & (Command | Control)) && rpnShown() && (e.keysym == XKB_KEY_Up || e.keysym == XKB_KEY_Down)) {
            if (Key *key = keyFor(e.keysym == XKB_KEY_Up ? RPNRollUp : RPNRollDown)) {
                key->flash();
                press(key);
            }
            return;
        }
        if (e.modifiers & (Command | Control)) {
            m_menu.performKeyEquivalent(e);
            return;
        }
        if (m_view == ProgrammerView)
            return programmerKey(e);
        int code = 0;
        const uint32_t k = e.keysym;
        if (k >= XKB_KEY_0 && k <= XKB_KEY_9)
            code = Digit0 + int(k - XKB_KEY_0);
        else if (k >= XKB_KEY_KP_0 && k <= XKB_KEY_KP_9)
            code = Digit0 + int(k - XKB_KEY_KP_0);
        else if (k == XKB_KEY_period || k == XKB_KEY_comma || k == XKB_KEY_KP_Decimal || k == XKB_KEY_KP_Separator)
            code = Point;
        else if (k == XKB_KEY_plus || k == XKB_KEY_KP_Add)
            code = Plus;
        else if (k == XKB_KEY_minus || k == XKB_KEY_KP_Subtract)
            code = Minus;
        else if (k == XKB_KEY_asterisk || k == XKB_KEY_KP_Multiply)
            code = Times;
        else if (k == XKB_KEY_slash || k == XKB_KEY_KP_Divide)
            code = Divide;
        else if (k == XKB_KEY_Return || k == XKB_KEY_KP_Enter)
            code = rpnShown() ? Enter : Equals;
        else if (k == XKB_KEY_equal || k == XKB_KEY_KP_Equal)
            code = rpnShown() ? 0 : Equals;   // -[BasicAdvancedController keyDown:]: = for AlgebraicEngine only
        else if (k == XKB_KEY_Escape || k == XKB_KEY_Clear)
            code = Clear;
        else if (k == XKB_KEY_percent)
            code = Percent;
        else if (k == XKB_KEY_BackSpace || k == XKB_KEY_Delete) {
            m_engine.deleteDigit();
            refresh();
            return;
        } else if (m_view == ScientificView) {
            if (k == XKB_KEY_parenleft)
                code = LeftParen;
            else if (k == XKB_KEY_parenright)
                code = RightParen;
            else if (k == XKB_KEY_asciicircum)
                code = Power;
            else if (k == XKB_KEY_exclam)
                code = Factorial;
            else if (k == XKB_KEY_p)
                code = Pi;
            else if (k == XKB_KEY_e)
                code = E;
            else if (k == XKB_KEY_E)
                code = ExponentKey;
        }
        if (!code)
            return;
        if (Key *key = keyFor(code)) {
            key->flash();
            press(key);
        } else if (code == Percent || code == Pi || code == E || code == Factorial || code == Power) {
            m_engine.op(code);
            refresh();
        }
    }

    // Calculator.nib's main menu, with View's calculators (-[CalculatorController populate...]: the
    // plug-ins' views, ⌘1 to ⌘3); its Debug menu taken off
    void loadMenus()
    {
        m_menu.load(res("English.lproj/Calculator.nib"));
        auto &menus = m_menu.menus();
        menus.erase(std::remove_if(menus.begin(), menus.end(), [](const MainMenuItem &m) { return m.title == "Debug"; }), menus.end());
        for (MainMenuItem &m : menus) {
            if (m.title != "View")
                continue;
            std::vector<MainMenuItem> views;
            const char *names[] = {"Basic", "Advanced", "Programmers"};
            for (int i = 0; i < 3; ++i) {
                MainMenuItem it;
                it.title = tr(names[i]);
                it.key = std::to_string(i + 1);
                it.modifiers = Command;
                it.action = "setCurrentView:";
                it.tag = i;
                views.push_back(it);
            }
            m.items.insert(m.items.begin(), views.begin(), views.end());
        }
        populateConvertMenu();
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
        m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
        m_menu.publish();
    }

    // -[CalculatorController populateConvertMenu]: the categories after Recent Conversions, each
    // "…" (prepareConversion:); -updateRecentConversionsMenu: the conversions over Clear Menu
    void populateConvertMenu()
    {
        for (MainMenuItem &m : m_menu.menus()) {
            if (m.title != "Convert")
                continue;
            m.items.erase(std::remove_if(m.items.begin(), m.items.end(), [](const MainMenuItem &i) { return i.action == "prepareConversion:"; }),
                          m.items.end());
            const std::vector<std::string> cats = m_conversions.sortedCategories();
            for (size_t i = 0; i < cats.size(); ++i) {
                MainMenuItem it;
                it.title = Conversions::unitName(cats[i]) + tr("Ellipsis");
                it.action = "prepareConversion:";
                it.tag = 3000 + int(i);
                m.items.push_back(it);
            }
            for (MainMenuItem &r : m.items) {
                if (r.tag != 2000)
                    continue;
                std::vector<MainMenuItem> items;
                for (size_t i = 0; i < m_conversions.recent().size(); ++i) {
                    MainMenuItem it;
                    it.title = m_conversions.title(m_conversions.recent()[i]);
                    it.action = "convert:";
                    it.tag = 4000 + int(i);
                    items.push_back(it);
                }
                if (!items.empty()) {
                    MainMenuItem line;
                    line.separator = true;
                    items.push_back(line);
                }
                for (const MainMenuItem &old : r.items)
                    if (old.action == "clearRecentConversionsMenu:")
                        items.push_back(old);
                r.items = items;
            }
        }
    }

    void recentConversionsChanged()
    {
        populateConvertMenu();
        m_menu.publish();
    }

    // -[BasicAdvancedController handleConversion:]: the number shown through the operations
    // (enter, the number, the operator, on a fresh RPNEngine), given back as a number
    void handleConversion(const Conversion &c)
    {
        long double v = m_engine.value();
        for (const auto &[op, number] : m_conversions.operations(c)) {
            const long double n = std::strtold(number.c_str(), nullptr);
            if (op == "*")
                v *= n;
            else if (op == "/")
                v = n != 0 ? v / n : v;
            else if (op == "+")
                v += n;
            else if (op == "-")
                v -= n;
        }
        m_engine.number(v);
        m_conversions.addRecent(c);
        recentConversionsChanged();
        refresh();
    }

    // -[ConversionController prepareConversion:]: ConversionSheet.nib over the window
    void prepareConversion(const std::string &type)
    {
        if (m_engine.error() || Sheet::isOpen(m_window.get())) {
            std::fputc('\a', stderr);   // readyForConversion
            return;
        }
        constexpr double W = 326, H = 187;
        auto *content = new View;
        content->setFrame({0, 0, W, H});
        auto at = [](double x, double y, double w, double h) { return Rect{x, H - y - h, w, h}; };   // the nib's, from the bottom
        auto label = [&](const std::string &text, Rect r, double size = 13, Align align = Align::Right) {
            auto *l = content->add<Label>(text, Font::system(size));
            l->style.align = align;
            l->setFrame(r);
            return l;
        };
        label("Convert:", at(26, 150, 58, 17));
        label("From:", at(38, 111, 41, 17));
        label("To:", at(54, 85, 25, 17));
        const std::vector<std::string> cats = m_conversions.sortedCategories();
        auto *typePopup = content->add<PopUpButton>();
        typePopup->setFrame(at(86, 144, 223, 26));
        for (const std::string &c : cats)
            typePopup->items.push_back(MenuItem(Conversions::unitName(c)));
        auto *fromPopup = content->add<PopUpButton>();
        fromPopup->setFrame(at(86, 105, 223, 26));
        auto *toPopup = content->add<PopUpButton>();
        toPopup->setFrame(at(86, 79, 223, 26));
        Label *updated = label({}, at(94, 61, 185, 14), 11, Align::Center);
        Label *legal = label("Actual conversion rates may vary.", at(36, 0, 254, 13), 9, Align::Center);
        auto *update = content->add<Button>("Update", Button::Push);
        update->setFrame(at(14, 13, 85, 32));
        auto *cancel = content->add<Button>("Cancel", Button::Push);
        cancel->setFrame(at(138, 13, 84, 32));
        auto *convert = content->add<Button>("Convert", Button::Push);
        convert->setFrame(at(222, 13, 90, 32));
        convert->isDefault = true;

        struct State {
            std::string type;
            std::vector<std::pair<std::string, std::string>> units;
        };
        auto state = std::make_shared<State>();
        // -populateConvertPopupsForKey:, -showOrHideCaveatsForKey:: the units, To the second;
        // currency from the dollar, Update, the date and the caveat shown
        auto populate = [this, state, fromPopup, toPopup, updated, legal, update](const std::string &key) {
            state->type = key;
            state->units = m_conversions.units(key);
            fromPopup->items.clear();
            toPopup->items.clear();
            for (const auto &[unit, title] : state->units) {
                fromPopup->items.push_back(MenuItem(title));
                toPopup->items.push_back(MenuItem(title));
            }
            const bool money = key == "currency";
            int from = 0, to = state->units.size() > 1 ? 1 : 0;
            if (money)
                for (size_t i = 0; i < state->units.size(); ++i)
                    if (state->units[i].first == "USD")
                        from = int(i);
            if (const Conversion *last = m_conversions.lastOfType(key))
                for (size_t i = 0; i < state->units.size(); ++i) {
                    if (state->units[i].first == last->from)
                        from = int(i);
                    if (state->units[i].first == last->to)
                        to = int(i);
                }
            fromPopup->setSelected(from);
            toPopup->setSelected(to);
            update->setHidden(!money);
            updated->setHidden(!money);
            legal->setHidden(!money);
            updated->setText(m_conversions.ratesDate());
            fromPopup->setNeedsDisplay();
            toPopup->setNeedsDisplay();
        };
        int index = 0;
        for (size_t i = 0; i < cats.size(); ++i)
            if (cats[i] == type)
                index = int(i);
        typePopup->setSelected(index);
        populate(type);
        typePopup->onSelect = [cats, populate](int i) {
            if (i >= 0 && size_t(i) < cats.size())
                populate(cats[size_t(i)]);
        };
        cancel->action = [this] { Sheet::end(m_window.get()); };
        update->action = [] {
            // -updateCurrencyRates:: Apple's rates server is gone
            Alert::run({}, tr("Currency Rate update failed"), {}, {"OK"}, [](int) {});
        };
        convert->action = [this, state, fromPopup, toPopup] {
            // -okayConvertSheet:: nothing when From and To are the same
            const int f = fromPopup->selected(), t = toPopup->selected();
            Sheet::end(m_window.get());
            if (f == t || f < 0 || t < 0 || size_t(f) >= state->units.size() || size_t(t) >= state->units.size())
                return;
            handleConversion({state->type, state->units[size_t(f)].first, state->units[size_t(t)].first});
        };
        Sheet::begin(m_window.get(), content, {W, H}, 0);
    }

    // Calculator.nib's LCD menu: RPN, Large Type
    void contextMenu(Event &e)
    {
        std::vector<MenuItem> items;
        items.push_back(MenuItem(tr("RPN"), false, true, m_rpn));
        items.push_back(MenuItem::line());
        items.push_back(MenuItem("Large Type"));
        Menu::popUp(m_window.get(), m_lcd->toWindow(e.location), items, 0, Font::system(), [this](int i) {
            if (i == 0)
                toggleRPN();
            else if (i == 2)
                largeType();
        });
    }

    void largeType()
    {
        // -[LCDController drawLargeType:]: 0, else the LCD's text
        const std::string text = m_view == ProgrammerView ? m_programmer.display() : m_engine.display(m_decimals, m_separators);
        m_largeType.show(m_view != ProgrammerView && m_engine.value() == 0 && !m_engine.error() ? "0" : text);
    }

    // -[CalculatorController toggleRPNMode:]: the menu items' state, RPNDefaultsKey, the LCD and the
    // view's engine
    void toggleRPN()
    {
        m_rpn = !m_rpn;
        Settings("calculator").set("RPNDefaultsKey", m_rpn);
        m_engine.setRPN(rpnShown() && m_view != ProgrammerView);
        m_programmer.setRPN(rpnShown() && m_view == ProgrammerView);
        m_window->setContentSize(contentSize());
        build();
    }

    bool validate(const MainMenuItem &it, MenuValidation &v)
    {
        const std::string &a = it.action;
        if (a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
            a == "performClose:" || a == "miniaturize:" || a == "copy:" || a == "paste:" || a == "clear:")
            return true;
        if (a == "toggleRPNMode:") {
            v.checked = m_rpn;
            return true;
        }
        if (a == "toggleTape:") {
            // -[PaperTapeController toggleTape:]'s titles; canShowTape: (not Programmer)
            v.title = tr(m_tape.isShown() ? "HideTape" : "ShowTape");
            return m_view != ProgrammerView;
        }
        if (a == "saveTape:" || a == "printTape:")
            return m_tape.isShown() && m_tape.hasText();   // -validateTapeMenuItems
        if (a == "prepareConversion:")
            return m_view != ProgrammerView;   // supportsConversions
        if (a == "convert:")
            return m_view != ProgrammerView && size_t(it.tag - 4000) < m_conversions.recent().size();
        if (a == "clearRecentConversionsMenu:")
            return !m_conversions.recent().empty();   // recentConversionsAreClearable
        if (a == "speechMenu:") {
            v.checked = it.title == "Speak Button Pressed" ? m_speech.buttons : m_speech.total;
            return m_view != ProgrammerView && Speech::available();   // supportsSpeech
        }
        if (a == "drawLargeType:")
            return true;
        if (a == "setCurrentView:") {
            v.checked = it.tag == m_view;
            return true;
        }
        if (a == "toggleSeparators:") {
            v.checked = m_separators;
            return true;
        }
        if (a == "setPrecision:") {
            v.checked = it.tag - 1 == m_decimals;
            return true;
        }
        return false;
    }

    void perform(const MainMenuItem &it)
    {
        const std::string &a = it.action;
        Settings prefs("calculator");
        if (a == "terminate:") {
            App::shared().quit();
        } else if (a == "performClose:") {
            m_window->close();
        } else if (a == "miniaturize:") {
            m_window->miniaturize();
        } else if (a == "copy:") {
            copyText(m_view == ProgrammerView ? m_programmer.display() : m_engine.display(m_decimals, false));
        } else if (a == "paste:") {
            std::string t = pasteText();
            t.erase(std::remove(t.begin(), t.end(), ','), t.end());
            char *end = nullptr;
            const long double v = std::strtold(t.c_str(), &end);
            if (end != t.c_str())
                m_engine.number(v);
        } else if (a == "clear:") {
            if (m_view == ProgrammerView)
                m_programmer.allClear();
            else
                m_engine.op(Clear);
        } else if (a == "setCurrentView:") {
            if (it.tag != m_view) {
                m_view = CalcView(it.tag);
                m_second = false;
                m_engine.setRPN(rpnShown() && m_view != ProgrammerView);
                m_programmer.setRPN(rpnShown() && m_view == ProgrammerView);
                if (m_view == ProgrammerView)
                    m_tape.hide();
                const char *names[] = {"Basic", "Advanced", "Programmers"};
                prefs.set("ViewDefaultsKey", names[m_view]);
                m_window->setContentSize(contentSize());
                build();
            }
        } else if (a == "toggleSeparators:") {
            m_separators = !m_separators;
            prefs.set("ShowSeparators", m_separators);
        } else if (a == "setPrecision:") {
            m_decimals = it.tag - 1;
            m_engine.tapePrecision = m_decimals;
            prefs.set("Precision", m_decimals);
        } else if (a == "toggleRPNMode:") {
            toggleRPN();
        } else if (a == "toggleTape:") {
            if (m_tape.isShown())
                m_tape.hide();
            else
                m_tape.show(m_window.get());
        } else if (a == "saveTape:") {
            m_tape.save();
        } else if (a == "printTape:") {
            m_tape.print();
        } else if (a == "prepareConversion:") {
            const std::vector<std::string> cats = m_conversions.sortedCategories();
            if (size_t(it.tag - 3000) < cats.size())
                prepareConversion(cats[size_t(it.tag - 3000)]);
        } else if (a == "convert:") {
            if (size_t(it.tag - 4000) < m_conversions.recent().size())
                handleConversion(Conversion(m_conversions.recent()[size_t(it.tag - 4000)]));
        } else if (a == "clearRecentConversionsMenu:") {
            m_conversions.clearRecent();
            recentConversionsChanged();
        } else if (a == "speechMenu:") {
            // -[SpeechController speechMenu:]: "Speech Enabled" said when the first is turned on
            const bool wasOn = m_speech.buttons || m_speech.total;
            bool &flag = it.title == "Speak Button Pressed" ? m_speech.buttons : m_speech.total;
            flag = !flag;
            prefs.set(it.title == "Speak Button Pressed" ? "SpeaksButtonPressedDefaultsKey" : "SpeaksTotalDefaultsKey", flag);
            if (!wasOn && flag)
                Speech::say("Speech Enabled");
        } else if (a == "drawLargeType:") {
            largeType();
        }
        refresh();
    }

    std::unique_ptr<Window> m_window;
    Engine m_engine;
    MainMenu m_menu;
    LCD *m_lcd = nullptr;
    std::vector<Key *> m_keys;
    Programmer m_programmer;
    BitsView *m_bits = nullptr;
    CalcView m_view = BasicView;
    bool m_second = false, m_separators = false, m_unicode = true, m_binaryShown = false, m_rpn = false;
    PaperTape m_tape;
    LargeType m_largeType;
    Conversions m_conversions;
    Speech m_speech;
    int m_decimals = 12;
};

} // namespace

int main()
{
    App &app = App::shared();
    setProgramName("Calculator");
    if (!app.open()) {
        std::fprintf(stderr, "Calculator: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    if (!Assets::exists(std::string(PlugIn) + "/Contents/Resources/Functions.xml")) {
        std::fprintf(stderr, "Calculator: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    CalculatorWindow calculator;
    return app.run();
}
