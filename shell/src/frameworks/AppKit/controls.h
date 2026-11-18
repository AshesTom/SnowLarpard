// SPDX-License-Identifier: LGPL-2.1-or-later
// AquaKit's controls: AppKit's, drawn by CoreUI (coreui.h) with 10.6's metrics. Each has an action, a
// std::function called as NSControl sends its action to its target.
#pragma once

#include "appkit.h"
#include "coreui.h"
#include "text.h"

#include <functional>
#include <string>
#include <vector>

namespace AK {

// Text colours of 10.6 (NSColor controlTextColor, disabledControlTextColor)
inline Color textColor(bool enabled) { return enabled ? Color::gray(0) : Color::gray(0, 0.4); }
CoreUI::State controlState(const View &v, bool pressed = false);

class Label : public View {
public:
    explicit Label(const std::string &text = {}, Font font = Font::system()) : m_text(text) { style.font = font; }
    void setText(const std::string &t);
    const std::string &text() const { return m_text; }
    TextStyle style;
    bool wrap = false;             // several lines (a field that isn't NSTextField's single line)
    Color background{0, 0, 0, 0};  // NSDrawsBackground
    void draw(cairo_t *cr) override;
    View *hitTest(Point) override { return nullptr; }

private:
    std::string m_text;
};

class Control : public View {
public:
    Control() { clipsToBounds = false; }   // the focus ring is outside the frame
    std::function<void()> action;
protected:
    bool m_pressed = false, m_inside = false;
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;
    virtual void clicked() { if (action) action(); }
};

// NSButton: a push button (32 high), a textured toolbar button, the round help button, the small
// square one (+ and - under lists: AppKit draws it, not CoreUI; measured)
class Button : public Control {
public:
    enum Kind { Push, Textured, Help, SmallSquare, Round, Disclosure, Plain };
    explicit Button(const std::string &title = {}, Kind kind = Push) : title(title), kind(kind) {}
    std::string title;
    Kind kind;
    bool isDefault = false;
    bool small = false;
    Image image;                   // drawn centred (Plain: alone, as a borderless image button)
    bool open = false;             // Disclosure: its state
    Font font = Font::system();    // Plain: its title's
    Color titleColor{0, 0, 0, 0};  // the title's own colour (an attributed title's), else the control's
    void draw(cairo_t *cr) override;
protected:
    void clicked() override;
};

// NSButton as a check box or a radio button: the art then the title, 2 points apart
class CheckBox : public Control {
public:
    explicit CheckBox(const std::string &title, bool radio = false) : title(title), radio(radio) {}
    std::string title;
    bool radio;
    bool small = false;
    bool imageAbove = false;       // the title under the box (NSImageAbove)
    Font font = Font::system();
    std::function<void(CheckBox *)> onToggle;
    bool checked() const { return m_checked; }
    void setChecked(bool c);
    void draw(cairo_t *cr) override;
protected:
    void clicked() override;
private:
    bool m_checked = false;
};

// NSSegmentedControl: tabs (NSTabView's) or the toolbar's textured segments. A segment shows its
// label, or a glyph drawn by `glyph` when given.
class SegmentedControl : public Control {
public:
    SegmentedControl(CoreUI::SegmentStyle style, std::vector<std::string> labels, std::vector<double> widths);
    int selected() const { return m_selected; }
    bool isHighlighted(int i) const { return (!momentary && i == m_selected) || i == m_pressedSegment; }   // drawn dark
    void setSelected(int i);
    void setSegments(std::vector<std::string> labels, std::vector<double> widths);
    bool momentary = false;   // the toolbar's: no selection kept
    bool small = false;       // NSSmallControlSize
    std::vector<bool> segmentEnabled;
    std::function<void(cairo_t *, int index, const Rect &r, bool enabled)> glyph;
    std::function<void(int)> onSelect;
    void draw(cairo_t *cr) override;
protected:
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;
private:
    std::vector<Rect> rects() const;
    CoreUI::SegmentStyle m_style;
    std::vector<std::string> m_labels;
    std::vector<double> m_widths;
    int m_selected = 0, m_pressedSegment = -1, m_downSegment = -1;
};

// A menu as NSPopUpButton and the contextual menus show it: its own window over everything
struct MenuItem {
    MenuItem(std::string title = {}, bool separator = false, bool enabled = true, bool checked = false)
        : title(std::move(title)), separator(separator), enabled(enabled), checked(checked) {}
    std::string title;
    bool separator = false;
    bool enabled = true;
    bool checked = false;
    static MenuItem line() { return {{}, true, false, false}; }
    std::string keyEquivalent;       // drawn at the right ("⇧⌘N")
    std::vector<MenuItem> submenu;   // a hierarchical item: its submenu opens beside it
    Image image;                     // drawn before the title (Open With's applications)
    // the Finder's label row (TLabelViewForMenu): the title over a row of swatches, the first the
    // "none" one (a cross); a swatch chosen is done(Menu::submenuChoice(item, swatch))
    std::vector<Color> swatches;
    int swatchChecked = -1;
    std::function<void(int swatch)> swatchChosen;   // besides the menu's done
    // NSMenuItem's: its tag; its action when chosen (besides the menu's done); its indentation level
    // (10 points each)
    int tag = 0;
    std::function<void()> action;
    int indentation = 0;
    // a picture at its right (the AirPort menu's lock and bars), a template drawn white while
    // highlighted when trailingTemplate
    Image trailingImage;
    bool trailingTemplate = false;
    // -[NSMenuItem setView:]: a view made each time the menu opens, as wide as the menu, viewHeight high
    std::function<View *(double width)> makeView;
    double viewHeight = 0, viewWidth = 0;
};
class Menu {
public:
    // Its top left at `at` in `parent`'s content, at least minWidth wide; done(-1) when dismissed. An
    // item of a submenu is done(Menu::submenuChoice(item, subitem))
    static void popUp(Window *parent, Point at, const std::vector<MenuItem> &items, double minWidth, Font font, std::function<void(int)> done);
    static constexpr int SubmenuBase = 1 << 16;
    static int submenuChoice(int item, int subitem) { return SubmenuBase * (item + 1) + subitem; }
    // a choice's item and subitem (-1 for an item of the menu itself)
    static void split(int choice, int &item, int &subitem)
    {
        item = choice >= SubmenuBase ? choice / SubmenuBase - 1 : choice;
        subitem = choice >= SubmenuBase ? choice % SubmenuBase : -1;
    }
    static double itemTop(const std::vector<MenuItem> &items, int index);   // within the menu
    static double itemHeight(const MenuItem &item);
    // The menu open (popUp's), changed while it is (NSMenu's items changed under it: the AirPort
    // scan's networks): its items replaced, the highlighted one kept, the window resized; and closed
    // (done(-1)) as -[NSMenu cancelTracking]
    static bool isOpen();
    // The menu bar's tracking (-[NSMenu popUpMenu...] from a title): the pointer moved, or a button went
    // down, outside the open menus, at e.screen; true when the menu bar took it (another title opened, or
    // this one closed), else a click closes the menus. Cleared when they close.
    static std::function<bool(Event &)> trackOutside;
    static void update(const std::vector<MenuItem> &items);
    static void close();
    static constexpr double ItemHeight = 19, SeparatorHeight = 12, Padding = 4, SwatchRowHeight = 40;
};

class PopUpButton : public Control {
public:
    std::vector<MenuItem> items;
    bool pullDown = false;   // its first item is its title; the menu has the others
    bool small = false;
    int selected() const { return m_selected; }
    void setSelected(int i);
    std::function<void(int)> onSelect;
    void draw(cairo_t *cr) override;
protected:
    void mouseDown(Event &e) override;
    void mouseDragged(Event &) override {}
    void mouseUp(Event &) override {}
private:
    int m_selected = 0;
    bool m_open = false;
};

// NSColorWell, its bezel: the colour inside a white-edged button
class ColorWell : public Control {
public:
    Color color = Color::gray(128);
    void draw(cairo_t *cr) override;
};

// NSImageView with the image well's bezel
class ImageWell : public View {
public:
    Image image;
    Rect imageInsets{6, 5, 12, 13};   // the picture's place in the bezel: left, top, then the sum across and down
    void draw(cairo_t *cr) override;
};

// NSTextField that edits: a square bezel (or none), NSSecureTextField's dots; NSSearchField below
class TextField : public View {
public:
    enum Style { Plain, Bezel, Search };
    TextField() { clipsToBounds = false; }
    ~TextField() override;
    Style style = Bezel;
    bool inToolbar = false;       // the search field's toolbar art
    bool secure = false;
    bool editable = true;
    Font font = Font::system();
    Align align = Align::Left;
    std::string text, placeholder;
    std::function<void(const std::string &)> onChange;
    std::function<void()> onEnter;   // Return: the field's action
    std::function<void()> onCancel;  // Escape, when set (the Finder's renaming)
    std::function<bool(Event &)> onKey;   // a key before the field's own handling: true when taken (Tab completing)
    bool acceptsFirstResponder() const override { return editable && isEnabled(); }
    const char *cursor() const override { return editable && isEnabled() ? "text" : nullptr; }   // NSTextView's I-beam
    // the selection (NSText's selectedRange), in bytes; typing replaces it
    void selectRange(size_t from, size_t to);
    void selectAll() { selectRange(0, text.size()); }
    bool hasSelection() const { return m_anchor != m_caret; }
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
    void keyDown(Event &e) override;
    void focusChanged() override;
    void setText(const std::string &t);
protected:
    Rect textRect() const;
    std::string shown() const;
private:
    size_t m_caret = 0, m_anchor = 0;
    bool m_keepSelection = false;   // selectRange before the field had the keyboard
    void deleteSelection();
    int m_blink = 0;
    bool m_caretOn = true;
};

// NSSearchField: rounded, the magnifier at its start, the cancel button when it has text
class SearchField : public TextField {
public:
    SearchField()
    {
        style = Search;
        inToolbar = true;
        font = Font::system(12);
    }
};

// NSSlider: horizontal, ticks under the thumb, stopping on them when asked
class Slider : public Control {
public:
    int ticks = 0;
    bool onlyTicks = false;
    bool ticksAbove = false;
    bool small = false;
    double value() const { return m_value; }   // 0..1
    void setValue(double v);
    double minValue = 0, maxValue = 1;
    double doubleValue() const { return minValue + m_value * (maxValue - minValue); }
    void setDoubleValue(double v) { setValue(maxValue > minValue ? (v - minValue) / (maxValue - minValue) : 0); }
    std::function<void(double)> onChange;
    void draw(cairo_t *cr) override;
protected:
    void mouseDown(Event &e) override;
    void mouseDragged(Event &e) override;
    void mouseUp(Event &e) override;
private:
    void track(Point p);
    double m_value = 0, m_grab = 0;
};

// NSScrollView with its vertical scroller (always shown, as 10.6 does); the document is a view of
// any height inside
class ScrollView : public View {
public:
    ScrollView();
    View *document() const { return m_document; }
    void setDocumentHeight(double h);
    double offset() const { return m_offset; }
    void scrollTo(double y);
    void scrollToVisible(const Rect &inDocument);
    void setFrame(const Rect &r) override;
    void scrollWheel(Event &e) override;
    double lineStep = 19;
private:
    friend class Scroller;
    void layout();
    View *m_document;
    class Scroller *m_scroller;
    double m_offset = 0, m_height = 0;
};

// NSOutlineView with one column, as Desktop & Screen Saver's sources: groups with their disclosure
// triangle, rows with an icon and a name, the selection
struct OutlineRow {
    std::string title;
    Image icon;
    int level = 0;
    bool group = false;        // has children, can open and close
    bool expanded = true;
    bool selectable = true;
    std::string data;          // the program's own
    int tag = 0;               // the program's own
    int parent = -1;           // index of the group
    bool header = false;       // a source list header ("PICTURES"): small caps, grey
    bool groupRow = false;     // an NSTableView group row (outlineView:isGroupItem:), not a source list's
};
class OutlineView : public View {
public:
    std::vector<OutlineRow> rows;
    double rowHeight = 19;
    double indent = 14;
    Color background = Color::gray(255);   // NSTableView's backgroundColor
    double trailing = 0;                   // room kept at the right of the titles (another column)
    int selected() const { return m_selected; }
    void select(int index, bool notify = false);
    std::function<void(int)> onSelect;
    void reload();   // after changing rows: the visible ones and the height
    bool acceptsFirstResponder() const override { return true; }
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
    void keyDown(Event &e) override;
    std::vector<int> visibleRows() const;
private:
    int m_selected = -1;
};

// The pictures as IKImageBrowserView shows them in the pane: a grid of thumbnails
class ImageGrid : public View {
public:
    struct Cell {
        Image image;
        std::string data;
        std::string tooltip;
    };
    std::vector<Cell> cells;
    Size cellSize{82, 55}, thumbSize{73, 49};
    Point origin{8, 6};
    int columns = 5;
    int selected = -1;
    std::function<void(int)> onSelect;
    double contentHeight() const;
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
    Rect cellRect(int i) const;
};

// NSBox: a primary or secondary group box, a separator line, or nothing drawn; its title above
class Box : public View {
public:
    enum Kind { Primary, Secondary, Separator, None };
    Kind kind = Primary;
    std::string title;
    Font titleFont = Font::system(11);
    void draw(cairo_t *cr) override;
    View *hitTest(Point p) override;
};

// NSImageView: an image scaled down into its frame, centred; the frames of its styles
class ImageView : public View {
public:
    enum Frame { NoFrame, Photo, GrayBezel, Groove, Button };
    Image image;
    Frame frameStyle = NoFrame;
    void draw(cairo_t *cr) override;
    View *hitTest(Point) override { return nullptr; }
};

// NSTabView: its tabs on its top edge, the selected item's view in the pane
class TabView : public View {
public:
    TabView();
    void addTab(const std::string &label, View *page);   // takes the page
    View *page(int i) const { return i >= 0 && size_t(i) < m_pages.size() ? m_pages[size_t(i)] : nullptr; }
    int count() const { return int(m_pages.size()); }
    void setTabLabel(int i, const std::string &label);   // empty: no tab for that page
    int selected() const { return m_selected; }
    void select(int i);
    std::function<void(int)> onSelect;
    bool noTabs = false;        // NSNoTabsNoBorder / tabless: the pages alone
    bool noBorder = false;
    bool keepPageFrames = false;   // the pages where the nib put them
    void draw(cairo_t *cr) override;
    void setFrame(const Rect &r) override;
private:
    void layout();
    SegmentedControl *m_tabs = nullptr;
    std::vector<int> m_shown;   // the pages that have a tab
    std::vector<std::string> m_labels;
    std::vector<View *> m_pages;
    int m_selected = 0;
};

// NSTableView in its NSScrollView: a header with the columns' titles, rows of text (or check boxes),
// the selection
class TableView : public View {
public:
    struct Column {
        std::string title;
        double width = 100;
        bool checkBox = false;
    };
    std::vector<Column> columns;
    std::vector<std::vector<std::string>> rows;   // a check box column: "1" checked
    bool header = true;
    double rowHeight = 17;
    bool bordered = true;
    int selected = -1;
    std::function<void(int)> onSelect;
    std::function<void(int row, int column, bool on)> onCheck;
    std::function<void(int row)> onDoubleClick;   // NSTableView's doubleAction
    // a cell drawn by its owner (an NSImageCell's picture): true when it drew it; `highlighted`: the
    // row selected in a key, focused table (white on blue)
    std::function<bool(cairo_t *, int row, int column, const Rect &cell, bool highlighted)> drawCell;
    TableView();
    void reload();
    void setFrame(const Rect &r) override;
    void draw(cairo_t *cr) override;
    ScrollView *scroll() const { return m_scroll; }
private:
    ScrollView *m_scroll;
    View *m_body;
};

// NSProgressIndicator: a bar, or the spinning wheel
class ProgressIndicator : public View {
public:
    bool spinning = false, indeterminate = false, animating = false;
    double value = 0, minValue = 0, maxValue = 100;
    ~ProgressIndicator() override;
    void start();
    void stop();
    void draw(cairo_t *cr) override;
private:
    int m_timer = 0;
    double m_phase = 0;
};

// NSStepper: the little arrows, a value between bounds
class Stepper : public Control {
public:
    double value = 0, minValue = 0, maxValue = 100, increment = 1;
    std::function<void(double)> onChange;
    void draw(cairo_t *cr) override;
protected:
    void mouseDown(Event &e) override;
    void mouseUp(Event &e) override;
    void mouseDragged(Event &) override {}
private:
    int m_pressedPart = 0;
};

// NSComboBox: a field with its list of choices
class ComboBox : public TextField {
public:
    std::vector<std::string> items;
    void draw(cairo_t *cr) override;
    void mouseDown(Event &e) override;
};

// NSLevelIndicator: discrete segments filled up to the value
class LevelIndicator : public View {
public:
    double value = 0, maxValue = 10;
    void draw(cairo_t *cr) override;
};

// NSMatrix of radio buttons: one chosen among its cells
class RadioGroup : public View {
public:
    RadioGroup() { clipsToBounds = false; }
    std::vector<CheckBox *> buttons;
    int selected() const;
    void select(int i);
    std::function<void(int)> onSelect;
    View *hitTest(Point p) override;
};

// NSDatePicker's graphical style: this month's calendar, today marked; or the clock's face
class CalendarView : public View {
public:
    void draw(cairo_t *cr) override;
};
class ClockView : public View {
public:
    ClockView();
    ~ClockView() override;
    void draw(cairo_t *cr) override;
private:
    int m_timer = 0;
};

// A sheet (-[NSApplication beginSheet:modalForWindow:...]): a panel sliding down from under the
// toolbar (`top`), over the window's content, the window answering it alone until it ends; the window
// grows for a taller sheet and gets its size back after. Escape cancels it, Return presses its default
// button.
class Sheet {
public:
    static void begin(Window *w, View *content, Size size, double top, std::function<void()> ended = {});
    static void end(Window *w);
    static bool isOpen(Window *w);
};

// SFAuthorizationView: the padlock and its sentence (nothing here asks for a password: open)
class LockView : public View {
public:
    void draw(cairo_t *cr) override;
    View *hitTest(Point) override { return nullptr; }
};

} // namespace AK
