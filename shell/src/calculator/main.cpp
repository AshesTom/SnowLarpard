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
#include "programmer.h"
#include "text.h"
#include "toolbar.h"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

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
    LCD()
    {
        for (const char *n : {"topleft", "topmid", "topright", "leftmid", "mid", "rightmid", "bottomleft", "bottommid", "bottomright"})
            m_pieces.push_back(Image::load(res(std::string("lcd-") + n + ".tif")));
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
        drawText(cr, text, Rect{10, h - 33 - 18, frame().w - 22, 18}, big);
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

        m_window = std::make_unique<Window>(contentSize(), "Calculator", Window::Titled);
        m_window->setIcon(Image::load(res("Calculator.icns"), {128, 128}));
        m_window->setRoundedBottomCorners(true);   // NSTexturedBackgroundWindowMask (the nib's style 0x10f)
        m_window->onUnhandledKey = [this](Event &e) { key(e); };
        m_window->onKeyChanged = [this](bool) { m_window->contentView()->setNeedsDisplay(); };
        m_window->onClose = [] { App::shared().quit(); };   // the last window: Calculator quits
        build();
        loadMenus();
        m_window->show();
    }

private:
    Size contentSize() const
    {
        if (m_view == ProgrammerView)
            return {ProgrammerWidth + 14, programmerHeight() + DisplayHeight + 8};
        const double boxes = BasicWidth + (m_view == ScientificView ? ScientificWidth : 0);
        return {boxes + 14, BoxHeight + DisplayHeight + 8};
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
        m_lcd->setFrame({9, 0, s.w - 18, DisplayHeight});
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
        else if (k == XKB_KEY_equal || k == XKB_KEY_Return || k == XKB_KEY_KP_Enter || k == XKB_KEY_KP_Equal)
            code = Equals;
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
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
        m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
        m_menu.publish();
    }

    bool validate(const MainMenuItem &it, MenuValidation &v)
    {
        const std::string &a = it.action;
        if (a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
            a == "performClose:" || a == "miniaturize:" || a == "copy:" || a == "paste:" || a == "clear:")
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
            prefs.set("Precision", m_decimals);
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
    bool m_second = false, m_separators = false, m_unicode = true, m_binaryShown = false;
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
