// SPDX-License-Identifier: LGPL-2.1-or-later
// Photo Booth.app (3.0, 10.6) on AquaKit. Its window is MainMenu.nib's: 640 x 660, PBView (the
// video, 640 x 480) over PBTabViewWithGradientBackground (the controls, 61 high: the tabs LiveVideo,
// Still, CountDown), PBTrayView (the photos, 588 x 96) between its two PBScrollButtons, and the
// photo count in the 23 points under it. The pictures are the application's own (the DVD's).
// What each class does was read in the binary (docs/reverse-status.md, section 14); the numbers
// below are its own.
#include "appkit.h"
#include "camera.h"
#include "controls.h"
#include "effects.h"
#include "foundation.h"
#include "library.h"
#include "mainmenu.h"
#include "panels.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <spawn.h>

extern char **environ;

using namespace AK;
using namespace PB;

namespace {

const char *Bundle = "/Applications/Photo Booth.app";

std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }

// [NSImage imageNamed:]
Image art(const std::string &name)
{
    static std::map<std::string, Image> cache;
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.emplace(name, Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/" + name + ".png"))).first;
    return it->second;
}

void spawn(const std::vector<std::string> &argv)
{
    std::vector<char *> args;
    for (const std::string &a : argv)
        args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid;
    posix_spawnp(&pid, args[0], nullptr, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
}

// "%@ photo" with a number
std::string counted(const std::string &format, long n)
{
    std::string s = tr(format);
    const size_t at = s.find("%@");
    return at == std::string::npos ? s : s.replace(at, 2, std::to_string(n));
}

double ease(double t)   // CAMediaTimingFunction's ease in, ease out (NSAnimationContext's default)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// ---- A button made of pictures: PBShutterButton (-setImages: "<name>_norm", "_click", "_inactive"),
// PBScrollButtonCell (scroll_left, _click, _inactive, _disabled), the nib's stop button

class ImageButton : public Control {
public:
    Image normal, pressed, inactive, disabled;
    unsigned modifiers = 0;   // of the click that fired the action
    void draw(cairo_t *cr) override
    {
        const Image *image = &normal;
        if (!isEnabled())
            image = disabled.isNull() ? &inactive : &disabled;
        else if (!windowIsKey())
            image = &inactive;
        else if (m_pressed)
            image = &pressed;
        if (image->isNull())
            image = &normal;
        if (!image->isNull())
            image->draw(cr, Point{std::floor((frame().w - image->width()) / 2), std::floor((frame().h - image->height()) / 2)});
    }

protected:
    void mouseUp(Event &e) override
    {
        modifiers = e.modifiers;
        Control::mouseUp(e);
    }
};

// ---- PBRecordingRadioButtonCell: three segments of 30 points, a point between them
// (-_rectForSegment:inFrame: x = segment * 31), each a picture by its place and state

class RecordingRadioButton : public View {
public:
    int selected = 0;
    std::function<void(int)> onSelect;
    void draw(cairo_t *cr) override
    {
        static const char *places[3] = {"left", "middle", "right"};
        static const char *glyphs[3] = {"RecordingCamera", "RecordingAnimation", "RecordingMovie"};
        static const double nudge[3] = {1, 0, -1};
        const bool key = windowIsKey();
        for (int i = 0; i < 3; ++i) {
            const bool on = i == selected, press = i == m_pressed && m_inside;
            std::string name = std::string("rb_") + places[i];
            if (!key)
                name += on ? "_on_inactive" : "_normal_inactive";
            else
                name += on ? (press ? "_on_press" : "_on") : (press ? "_press" : "_normal");
            art(name).draw(cr, Point{i * 31.0, 0});
            if (i > 0)
                art(key ? "rb_active_separator" : "rb_inactive_separator").draw(cr, Point{i * 31.0 - 1, 0});
            const Image &glyph = art(glyphs[i]);
            glyph.draw(cr, Point{i * 31.0 + std::floor(15 - glyph.width() / 2.0) + nudge[i], std::floor(25 / 2.0 - glyph.height() / 2.0)},
                       isEnabled() ? 1 : 0.4);
        }
    }
    void mouseDown(Event &e) override
    {
        m_pressed = isEnabled() ? segmentAt(e.location) : -1;
        m_inside = true;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        m_inside = segmentAt(e.location) == m_pressed;
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const int hit = segmentAt(e.location);
        if (m_pressed >= 0 && hit == m_pressed && hit != selected) {
            selected = hit;
            if (onSelect)
                onSelect(hit);
        }
        m_pressed = -1;
        setNeedsDisplay();
    }

private:
    int segmentAt(Point p) const { return bounds().contains(p) ? std::clamp(int(p.x / 31), 0, 2) : -1; }
    int m_pressed = -1;
    bool m_inside = false;
};

// ---- PBEffectsToggleButton: "Effects" (115 x 25 at least), on while the effects' pages show; then
// its two ends are the pages' arrows (eb_left_arrow, eb_right_arrow at 0.3 when there is no page that way)

class EffectsButton : public View {
public:
    bool on = false, leftEnabled = false, rightEnabled = false;
    std::function<void()> onToggle, onLeft, onRight;
    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        const bool key = windowIsKey();
        const bool pressMiddle = m_pressed == 1 && m_inside;
        const char *fill = !key ? (on ? "eb_inactive_on_fill" : "eb_inactive_normal_fill")
                                : on ? (pressMiddle ? "eb_on_and_press_fill" : "eb_on_fill") : (pressMiddle ? "eb_press_fill" : "eb_normal_fill");
        art(fill).draw(cr, Rect{1, 1, b.w - 2, 22});
        if (on) {
            const bool pressLeft = m_pressed == 0 && m_inside && leftEnabled, pressRight = m_pressed == 2 && m_inside && rightEnabled;
            const Image &left = art(pressLeft ? "eb_left_press" : key ? "eb_left_normal" : "eb_inactive_left_normal");
            const Image &right = art(pressRight ? "eb_right_press" : key ? "eb_right_normal" : "eb_inactive_right_normal");
            left.draw(cr, Point{1, 1});
            right.draw(cr, Point{b.w - 1 - right.width(), 1});
            const Image &la = art("eb_left_arrow"), &ra = art("eb_right_arrow");
            la.draw(cr, Point{1 + std::floor((left.width() - la.width()) / 2.0) - 1, 1 + std::floor((22 - la.height()) / 2.0)}, leftEnabled ? 1 : 0.3);
            ra.draw(cr, Point{b.w - 1 - right.width() + std::ceil((right.width() - ra.width()) / 2.0) + 1, 1 + std::floor((22 - ra.height()) / 2.0)},
                    rightEnabled ? 1 : 0.3);
        }
        // the outline: eb_active_mask in three parts, its ends 4 points
        const Image &mask = art(key ? "eb_active_mask" : "eb_inactive_mask");
        const double mw = mask.width(), mh = mask.height();
        mask.draw(cr, Rect{0, 0, 4, mh}, Rect{0, 0, 4, mh});
        mask.draw(cr, Rect{4, 0, mw - 8, mh}, Rect{4, 0, b.w - 8, mh});
        mask.draw(cr, Rect{mw - 4, 0, 4, mh}, Rect{b.w - 4, 0, 4, mh});
        TextStyle style;
        style.font = Font::system(13);   // +[PBEffectsToggleButton font]
        style.align = Align::Center;
        style.color = on && key ? Color::gray(255) : Color::gray(key ? 0 : 110);
        style.engraved = !on;
        drawText(cr, tr("Effects"), Rect{0, 0, b.w, 23}, style);
    }
    void mouseDown(Event &e) override
    {
        m_pressed = partAt(e.location);
        m_inside = true;
        setNeedsDisplay();
    }
    void mouseDragged(Event &e) override
    {
        m_inside = bounds().contains(e.location) && partAt(e.location) == m_pressed;
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const int part = m_pressed;
        const bool fire = bounds().contains(e.location) && partAt(e.location) == part;
        m_pressed = -1;
        setNeedsDisplay();
        if (!fire)
            return;
        if (part == 0 && leftEnabled && onLeft)
            onLeft();
        else if (part == 2 && rightEnabled && onRight)
            onRight();
        else if (part == 1 && onToggle)
            onToggle();
    }

private:
    int partAt(Point p) const { return !on ? 1 : p.x < 34 ? 0 : p.x >= frame().w - 34 ? 2 : 1; }
    int m_pressed = -1;
    bool m_inside = false;
};

// ---- PBEffectPageIndicatorView: a dot a page (EffectPage_Normal, EffectPage_Selected, 16 x 17)

class PageIndicator : public View {
public:
    int count = 0, current = 0;
    std::function<void(int)> onPage;
    void draw(cairo_t *cr) override
    {
        for (int i = 0; i < count; ++i)
            art(i == current ? "EffectPage_Selected" : "EffectPage_Normal").draw(cr, Point{i * 16.0, std::floor((frame().h - 17) / 2)});
    }
    void mouseUp(Event &e) override
    {
        const int page = int(e.location.x / 16);
        if (bounds().contains(e.location) && page < count && onPage)
            onPage(page);
    }
};

// ---- PBCountdownView: 3, 2, 1 and the camera, the lit one by its counter (4: none yet)

class CountdownView : public View {
public:
    int counter = 4;
    void draw(cairo_t *cr) override
    {
        art("countdown-background").draw(cr, bounds());
        static const char *names[4] = {"countdown-no3", "countdown-no2", "countdown-no1", "countdown-cam"};
        const double w = art("countdown-no3").width();
        const double x0 = (frame().w - 4 * w - 36) * 0.5;   // -drawRect:
        for (int i = 0; i < 4; ++i)
            art(std::string(names[i]) + (counter == 3 - i ? "" : "_dim")).draw(cr, Point{std::floor(x0 + i * (w + 12)), 0});
    }
};

// ---- A button with its picture over its title (the Still tab's: bezel-less, the title embossed)

class TitledImageButton : public Control {
public:
    Image image;
    std::string title;
    void draw(cairo_t *cr) override
    {
        const double alpha = !isEnabled() ? 0.35 : m_pressed ? 0.6 : 1;
        if (!image.isNull())
            image.draw(cr, Point{std::floor((frame().w - image.width()) / 2), 2}, alpha);
        TextStyle style;
        style.font = Font::system(10);
        style.align = Align::Center;
        style.engraved = true;
        style.elide = false;
        style.color = Color::gray(0, isEnabled() ? 1 : 0.4);
        drawText(cr, title, Rect{-10, 36, frame().w + 20, 13}, style);
    }
};

// ---- PBTabViewWithGradientBackground: 0.7695 to 0.5859 white (0.9102 to 0.8086 when the
// application isn't active), a separator line on its top and bottom edges

class ControlBar : public View {
public:
    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        const bool key = windowIsKey();
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, b.h);
        const double top = key ? 0.769531 : 0.910156, bottom = key ? 0.585938 : 0.808594;
        cairo_pattern_add_color_stop_rgb(g, 0, top, top, top);
        cairo_pattern_add_color_stop_rgb(g, 1, bottom, bottom, bottom);
        cairo_rectangle(cr, 0, 0, b.w, b.h);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        fill(cr, Rect{0, 0, b.w, 1}, Color::gray(0, 0.5));          // contentSeparatorColor
        fill(cr, Rect{0, 1, b.w, 1}, Color::gray(255, 0.35));       // contentInsetColor
        fill(cr, Rect{0, b.h - 1, b.w, 1}, Color::gray(0, 0.5));
    }
};

// ---- PBView: the video. Its layers: the camera through the effect (VideoLayer's FXLayer), the
// effects' pages (PageLayer: nine FXLayers, +rectForEffectAtIndex:withSize:), the still picture, the
// four pictures of a 4-up (FourUpLayer), the camera's status (CameraLayer)

class VideoView : public View {
public:
    enum Mode { Live, Mosaic, Still, FourUp };
    Mode mode = Live;
    Camera::Status status = Camera::Off;
    Image display;                 // the camera through the current effect, as shown (mirrored)
    Image previews[9];             // a page's effects
    int previewEffects[9] = {};
    Image still;
    std::vector<Image> fourUp;     // a 4-up's pictures so far
    bool positioned = false;       // the effect takes X and Y: a click moves its centre
    std::function<void(int)> onPickEffect;
    std::function<void(Point)> onPosition;

    // +[PageLayer rectForEffectAtIndex:withSize:], from the top
    static Rect cell(int index)
    {
        const double w = (VideoWidth - 64) / 3.0, h = (VideoHeight - 48) / 3.0;
        return {16 + (index % 3) * (w + 16), 12 + (index / 3) * (h + 12), w, h};
    }

    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        fill(cr, b, Color::gray(0));
        if (mode == Still) {
            if (!still.isNull()) {
                const Size s = scaledToFit(still.size(), Size{b.w, b.h});
                still.draw(cr, Rect{std::floor((b.w - s.w) / 2), std::floor((b.h - s.h) / 2), s.w, s.h});
            }
            return;
        }
        if (status != Camera::On) {
            // CameraLayer: the crossed camera and why
            const Image &off = art("camera-off");
            off.draw(cr, Point{std::floor((b.w - off.width()) / 2), std::floor(b.h / 2 - off.height() - 6)});
            TextStyle style;
            style.font = Font::boldSystem(14);
            style.color = Color::gray(255);
            style.align = Align::Center;
            const char *why = status == Camera::Busy ? "The camera is busy." : status == Camera::Off ? "The camera is off." : "There is no connected camera.";
            drawText(cr, tr(why), Rect{0, b.h / 2 + 4, b.w, 20}, style);
            return;
        }
        if (mode == Live) {
            if (!display.isNull())
                display.draw(cr, b);
        } else if (mode == FourUp) {
            for (int i = 0; i < 4; ++i) {
                const Rect q{(i % 2) * b.w / 2, (i / 2) * b.h / 2, b.w / 2, b.h / 2};
                const Image &picture = i < int(fourUp.size()) ? fourUp[i] : display;
                if (i <= int(fourUp.size()) && !picture.isNull())
                    picture.draw(cr, q);
            }
        } else {
            for (int i = 0; i < 9; ++i) {
                const Rect r = cell(i);
                if (!previews[i].isNull())
                    previews[i].draw(cr, r);
                if (i == m_pressedCell) {
                    cairo_rectangle(cr, r.x + 1, r.y + 1, r.w - 2, r.h - 2);
                    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
                    cairo_set_line_width(cr, 2);
                    cairo_stroke(cr);
                }
                // -[FXLayer textLabel]: bold system 14, centred, 10 points over the bottom, a black
                // shadow of radius 1 around it
                TextStyle style;
                style.font = Font::boldSystem(14);
                style.align = Align::Center;
                const Rect label{r.x + 4, r.y + r.h - 10 - 18, r.w - 8, 18};
                const std::string name = previewEffects[i] == NormalEffect ? tr("Normal") : effectInfo(previewEffects[i]).name;
                style.color = Color::gray(0, 0.55);
                for (const Point &o : {Point{-1, 0}, Point{1, 0}, Point{0, -1}, Point{0, 1}})
                    drawText(cr, name, label.translated(o.x, o.y), style);
                style.color = Color::gray(255);
                drawText(cr, name, label, style);
            }
        }
    }
    void mouseDown(Event &e) override
    {
        m_pressedCell = -1;
        if (mode == Mosaic) {
            for (int i = 0; i < 9; ++i)
                if (cell(i).contains(e.location))
                    m_pressedCell = i;
            setNeedsDisplay();
        } else if (mode == Live && positioned) {
            mouseDragged(e);
        }
    }
    void mouseDragged(Event &e) override
    {
        if (mode == Live && positioned && onPosition)   // -[FXLayer onMouseDragged:]: the X and Y inputs
            onPosition(Point{std::clamp(e.location.x / frame().w, 0.0, 1.0), std::clamp(1 - e.location.y / frame().h, 0.0, 1.0)});
    }
    void mouseUp(Event &e) override
    {
        const int pressed = m_pressedCell;
        m_pressedCell = -1;
        setNeedsDisplay();
        if (mode == Mosaic && pressed >= 0 && cell(pressed).contains(e.location) && onPickEffect)
            onPickEffect(previewEffects[pressed]);
    }

private:
    int m_pressedCell = -1;
};

// ---- PBCameraFlash: 10.6 flashes the screen white (CGDisplayVisualBell: 0.04 s up, 0.2 s held,
// 0.24 s down) with the displays at full brightness; here the window's content

class FlashView : public View {
public:
    double alpha = 0;
    View *hitTest(Point) override { return nullptr; }
    void draw(cairo_t *cr) override
    {
        if (alpha > 0)
            fill(cr, bounds(), Color::gray(255, alpha));
    }
};

// ---- PBTrayView: the photos, the newest at the right end. Each is a PBThumbnailView (TrayViewNode.nib:
// 90 x 92, photo_back.png, the picture in 80 x 60 at (5, 4), its number right aligned in the white under
// it, the delete button at its left), two points apart (+[ViewLayout xPadding]); its ViewLayouts move
// them: to the end for a new photo (0.5 s), which then drops in from 10 points over the tray (2 s) and
// bounces a point up (0.2 s) and back (0.1 s); a tray's width at a time for the arrows (0.5 s); closing
// the gap after a delete (0.4 s)

class TrayView : public View {
public:
    static constexpr double NodeW = 90, NodeH = 92, Pitch = NodeW + 2;

    Library *library = nullptr;
    int selected = -1;
    std::function<void(int)> onSelect;    // -1: none
    std::function<void(int)> onDelete;
    std::function<void()> onScrolled;     // the scroll buttons' states may have changed
    std::function<void()> onDeveloped;    // PBTrayViewDidFinishDevelopingNotification

    void reloadData()
    {
        m_nodes.assign(library->assets().size(), Node{});
        m_right = frame().w;
        arrange(0);
    }
    bool isScrollingEndForNewPhoto() const { return m_scrollingForNew; }
    bool canScrollPrevious() const { return !m_nodes.empty() && m_right - m_nodes.size() * Pitch < 2; }
    bool canScrollNext() const { return !m_nodes.empty() && m_right > frame().w; }
    // -showPreviousTray:, -showNextTray: (with Command: to the beginning, to the end)
    void scroll(int direction, bool all)
    {
        const double w = frame().w, most = std::max(w, m_nodes.size() * Pitch + 2);
        if (direction < 0)
            m_right = all ? most : std::min(m_right + w, most);
        else
            m_right = all ? w : std::max(m_right - w, w);
        arrange(0.5);
        if (onScrolled)
            onScrolled();
    }
    // -appendNewAsset:, the asset already last in the library
    void appendNew()
    {
        Node n;
        n.hidden = true;
        m_nodes.push_back(n);
        m_right = frame().w;
        m_scrollingForNew = true;
        arrange(0.5);
        Node &last = m_nodes.back();
        last.x = last.tx;
        last.y = last.ty = -(NodeH + 10) + 2;   // ScrollToEndForNewPhotoViewLayout: its height and 10 over its place
        const int serial = ++m_serial;
        App &app = App::shared();
        app.addTimer(0.5, false, [this, serial] {
            if (serial != m_serial || m_nodes.empty())
                return;
            m_scrollingForNew = false;
            m_nodes.back().hidden = false;   // -showNewAssetView
            moveLast(2, 2.0);                // DropNewPhotoViewLayout
        });
        app.addTimer(2.5, false, [this, serial] { if (serial == m_serial) moveLast(1, 0.2); });   // BounceUpNewPhotoViewLayout
        app.addTimer(2.7, false, [this, serial] { if (serial == m_serial) moveLast(2, 0.1); });   // BounceDownNewPhotoViewLayout
        app.addTimer(2.8, false, [this, serial] {
            if (serial == m_serial && onDeveloped)
                onDeveloped();
        });
        if (onScrolled)
            onScrolled();
    }
    void removeNode(int index)
    {
        if (index < 0 || index >= int(m_nodes.size()))
            return;
        ++m_serial;
        m_scrollingForNew = false;
        m_nodes.erase(m_nodes.begin() + index);
        for (Node &n : m_nodes)
            n.hidden = false;
        const double w = frame().w;
        // the photos after it close the gap; the tray stays full while there are photos to fill it
        m_right = std::clamp(m_right - Pitch, w, std::max(w, m_nodes.size() * Pitch + 2));
        arrange(0.4);   // DeleteViewLayout
        if (onScrolled)
            onScrolled();
    }
    // UndoDeleteViewLayout: a photo deleted put back, the others making room (0.4 s)
    void insertNode(int index)
    {
        ++m_serial;
        m_scrollingForNew = false;
        m_nodes.insert(m_nodes.begin() + std::clamp(index, 0, int(m_nodes.size())), Node{});
        const double w = frame().w;
        m_right = std::clamp(m_right + Pitch, w, std::max(w, m_nodes.size() * Pitch + 2));
        arrange(0.4);
        if (onScrolled)
            onScrolled();
    }
    // RevealSelectedLayout: the selected photo brought into the tray
    void reveal(int index)
    {
        if (index < 0 || index >= int(m_nodes.size()))
            return;
        const double w = frame().w, x = m_right - (m_nodes.size() - index) * Pitch;
        double right = m_right;
        if (x < 2)
            right += 2 - x;
        else if (x + NodeW > w - 2)
            right -= x + NodeW - (w - 2);
        if (right != m_right) {
            m_right = right;
            arrange(0.4);
            if (onScrolled)
                onScrolled();
        }
    }
    // the timer's: the photos on their way
    bool animate()
    {
        if (m_duration <= 0 && !m_moving)
            return false;
        const double t = now();
        bool moving = false;
        for (Node &n : m_nodes) {
            const double p = n.duration > 0 ? ease((t - n.start) / n.duration) : 1;
            n.x = n.fx + (n.tx - n.fx) * p;
            n.y = n.fy + (n.ty - n.fy) * p;
            moving = moving || (n.duration > 0 && t - n.start < n.duration);
        }
        m_moving = moving;
        if (!moving)
            m_duration = 0;
        setNeedsDisplay();
        return true;
    }

    void draw(cairo_t *cr) override
    {
        const Rect b = bounds();
        const bool key = windowIsKey();
        // -_activeBackgroundImage: tray_back_left, _middle (stretched), _right
        const std::string state = key ? "_active" : "_inactive";
        const Image &left = art("tray_back_left" + state), &right = art("tray_back_right" + state), &middle = art("tray_back_middle" + state);
        left.draw(cr, Point{0, 0});
        middle.draw(cr, Rect{double(left.width()), 0, b.w - left.width() - right.width(), b.h});
        right.draw(cr, Point{b.w - right.width(), 0});
        for (int i = 0; i < int(m_nodes.size()); ++i) {
            const Node &n = m_nodes[i];
            if (n.hidden || n.x > b.w || n.x + NodeW < 0)
                continue;
            drawNode(cr, i, n, key);
        }
        art("trayshadow_left").draw(cr, Point{0, 0});
        const Image &shadow = art("trayshadow_right");
        shadow.draw(cr, Point{b.w - shadow.width(), 0});
    }
    void mouseDown(Event &e) override
    {
        m_downNode = nodeAt(e.location);
        m_downDelete = m_downNode >= 0 && deleteRect(m_nodes[m_downNode]).contains(e.location);
        if (m_downNode >= 0 && !m_downDelete && onSelect)
            onSelect(m_downNode);   // -[PBThumbnailImageView mouseDown:]
        setNeedsDisplay();
    }
    void mouseUp(Event &e) override
    {
        const int node = m_downNode;
        const bool wasDelete = m_downDelete;
        m_downNode = -1;
        m_downDelete = false;
        setNeedsDisplay();
        if (wasDelete && node >= 0 && node < int(m_nodes.size()) && deleteRect(m_nodes[node]).contains(e.location) && onDelete)
            onDelete(node);   // -[PBTrayViewNode delete:]
    }
    void mouseMoved(Event &e) override
    {
        const int node = nodeAt(e.location);
        const int over = node >= 0 && deleteRect(m_nodes[node]).contains(e.location) ? node : -1;
        if (over != m_overDelete) {
            m_overDelete = over;
            setNeedsDisplay();
        }
    }
    void mouseExited(Event &) override
    {
        if (m_overDelete >= 0) {
            m_overDelete = -1;
            setNeedsDisplay();
        }
    }

private:
    struct Node {
        double x = 0, y = 2, fx = 0, fy = 2, tx = 0, ty = 2, start = 0, duration = 0;
        bool hidden = false;
    };
    // HorizontalViewLayout: the last photo's right edge at m_right, the others before it
    void arrange(double duration)
    {
        const double t = now();
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            Node &n = m_nodes[i];
            n.fx = n.x;
            n.fy = n.y;
            n.tx = m_right - (m_nodes.size() - i) * Pitch;
            n.ty = std::floor((frame().h - NodeH) / 2);
            n.start = t;
            n.duration = duration;
            if (duration <= 0) {
                n.x = n.tx;
                n.y = n.ty;
            }
        }
        m_duration = duration;
        m_moving = duration > 0;
        setNeedsDisplay();
    }
    void moveLast(double y, double duration)
    {
        if (m_nodes.empty())
            return;
        Node &n = m_nodes.back();
        n.fx = n.x = n.tx;
        n.fy = n.y;
        n.ty = y;
        n.start = now();
        n.duration = duration;
        m_duration = duration;
        m_moving = true;
    }
    int nodeAt(Point p) const
    {
        for (int i = 0; i < int(m_nodes.size()); ++i)
            if (!m_nodes[i].hidden && Rect{m_nodes[i].x, m_nodes[i].y, NodeW, NodeH}.contains(p))
                return i;
        return -1;
    }
    static Rect deleteRect(const Node &n) { return {n.x + 4, n.y + 68, 16, 18}; }
    void drawNode(cairo_t *cr, int index, const Node &n, bool key) const
    {
        const double x = std::round(n.x), y = std::round(n.y);
        const bool isSelected = index == selected;
        art("photo_back").draw(cr, Point{x, y});
        if (isSelected) {
            // -[PBThumbnailView _drawSelection]: the lozenge under the picture, blue (grey when inactive)
            cairo_new_path(cr);
            cairo_move_to(cr, x + 14, y + 84);
            cairo_line_to(cr, x + 76, y + 84);
            cairo_curve_to(cr, x + 88, y + 84, x + 88, y + 69, x + 76, y + 69);
            cairo_line_to(cr, x + 14, y + 69);
            cairo_curve_to(cr, x + 2, y + 69, x + 2, y + 84, x + 14, y + 84);
            cairo_close_path(cr);
            if (key)
                cairo_set_source_rgb(cr, 0.219, 0.457, 0.84);
            else
                cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
            cairo_fill(cr);
        }
        const Asset &asset = library->assets()[index];
        if (!asset.preview.isNull()) {
            const Size s = scaledToFit(asset.preview.size(), Size{80, 60});
            asset.preview.draw(cr, Rect{x + 5 + std::floor((80 - s.w) / 2), y + 4 + std::floor((60 - s.h) / 2), s.w, s.h});
        }
        if (asset.movie)
            art("movie").draw(cr, Point{x + 9, y + 53});
        TextStyle style;   // +[PBThumbnailTextField _attributes]: Lucida Grande 10, right aligned; white when selected
        style.font = Font::system(10);
        style.align = Align::Right;
        style.color = isSelected ? Color::gray(255) : Color::gray(0);
        drawText(cr, std::to_string(index + 1), Rect{x + 21, y + 71, 62, 13}, style);
        // -[PBThumbnailDeleteButton drawRect:]
        const bool over = index == m_overDelete, down = m_downDelete && index == m_downNode;
        const std::string name = std::string(isSelected ? "delete_select" : "delete") + (down ? "_click" : over ? "_roll" : "_norm");
        const Image &button = art(name);
        button.draw(cr, Point{x + 4 + std::floor((16 - button.width()) / 2.0), y + 68 + std::floor((18 - button.height()) / 2.0)});
    }

    std::vector<Node> m_nodes;
    double m_right = 0, m_duration = 0;
    bool m_moving = false, m_scrollingForNew = false;
    int m_serial = 0, m_downNode = -1, m_overDelete = -1;
    bool m_downDelete = false;
};

// ---- PBApplicationControl, with what PBImagePickerController and the recording controllers do

// IKSlideshow over the photos (IKSlideshowModeImages): black, the picture fitted; ← →, Escape
class SlideView : public View {
public:
    Library *library = nullptr;
    int index = 0;
    std::function<void()> onEnd;
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(0));
        if (index < 0 || index >= int(library->assets().size()))
            return;
        if (m_shown != index) {
            m_image = library->fullImage(index);
            m_shown = index;
        }
        if (m_image.isNull())
            return;
        const double k = std::min(bounds().w / m_image.width(), bounds().h / m_image.height());
        const double w = m_image.width() * k, h = m_image.height() * k;
        m_image.draw(cr, Rect{std::floor((bounds().w - w) / 2), std::floor((bounds().h - h) / 2), w, h});
    }
    void keyDown(Event &e) override
    {
        const int n = int(library->assets().size());
        if (e.keysym == 0xff1b) {
            if (onEnd)
                onEnd();
        } else if (e.keysym == 0xff53 || e.keysym == 0x20) {
            index = std::min(n - 1, index + 1);
            setNeedsDisplay();
        } else if (e.keysym == 0xff51) {
            index = std::max(0, index - 1);
            setNeedsDisplay();
        }
    }
    void mouseDown(Event &) override
    {
        index = std::min(int(library->assets().size()) - 1, index + 1);
        setNeedsDisplay();
    }
    bool acceptsFirstResponder() const override { return true; }

private:
    Image m_image;
    int m_shown = -1;
};

class PhotoBooth {
public:
    void quitting() { m_library.emptyIntermediateTrash(); }
    PhotoBooth() : m_settings("photobooth")
    {
        m_window = std::make_unique<Window>(Size{640, 660}, "Photo Booth");
        m_window->setIcon(Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/PhotoBooth.icns"), {128, 128}));
        m_autoFlip = m_settings.boolean("AutoFlipPhoto", false);   // Edit ▸ Auto Flip New Photos (its defaults key)
        View *content = m_window->contentView();

        m_video = content->add<VideoView>();
        m_video->setFrame({0, 0, 640, 480});
        m_video->onPickEffect = [this](int effect) { showEffect(effect); };
        m_video->onPosition = [this](Point p) { m_xy = p; };

        m_bar = content->add<ControlBar>();
        m_bar->setFrame({0, 480, 640, 61});
        buildLiveTab();
        buildStillTab();
        m_countdown = m_bar->add<CountdownView>();
        m_countdown->setFrame({0, 0, 640, 61});

        // the tray between its scroll buttons (26 x 96), the count under it
        m_previous = scrollButton("scroll_left", 0);
        m_next = scrollButton("scroll_right", 614);
        m_previous->action = [this] { m_tray->scroll(-1, m_previous->modifiers & Command); };
        m_next->action = [this] { m_tray->scroll(1, m_next->modifiers & Command); };
        m_tray = content->add<TrayView>();
        m_tray->setFrame({26, 541, 588, 96});
        m_tray->library = &m_library;
        m_tray->onSelect = [this](int i) { select(i); };
        m_tray->onDelete = [this](int i) { deleteAsset(i); };
        m_tray->onScrolled = [this] { refreshScrollButtons(); };
        m_tray->onDeveloped = [this] { m_developing = false; refreshControls(); };
        m_count = content->add<Label>("", Font::system(11));
        m_count->setFrame({18, 641, 606, 14});
        m_count->style.align = Align::Center;
        m_count->style.engraved = true;
        m_flash = content->add<FlashView>();
        m_flash->setFrame({0, 0, 640, 660});

        m_tray->reloadData();
        refreshPhotoCount();
        refreshScrollButtons();
        selectTab(Tab::Live);

        m_window->onUnhandledKey = [this](Event &e) { key(e); };
        loadMenus();
        m_window->onKeyChanged = [this](bool) { m_window->invalidate(); };
        m_window->onClose = [] { App::shared().quit(); };   // applicationShouldTerminateAfterLastWindowClosed:
        m_camera.open();
        m_video->status = m_camera.status();
        refreshControls();
        App::shared().addTimer(1.0 / 60, true, [this] { tick(); });
        m_window->show();
    }

private:
    enum class Tab { Live, Still, CountDown };

    ImageButton *scrollButton(const std::string &name, double x)
    {
        auto *b = m_window->contentView()->add<ImageButton>();
        b->setFrame({x, 541, 26, 96});
        b->normal = art(name);
        b->pressed = art(name + "_click");
        b->inactive = art(name + "_inactive");
        b->disabled = art(name + "_disabled");
        return b;
    }
    static void setImages(ImageButton *b, const std::string &name)   // -[PBShutterButton setImages:]
    {
        b->normal = art(name + "_norm");
        b->pressed = art(name + "_click");
        b->inactive = art(name + "_inactive");
        b->setNeedsDisplay();
    }

    // MainMenu.nib's LiveVideo tab: the recording options at (20, 18), the shutter in the middle, and
    // what PBEffectsButtonController adds: the Effects button 14 points from the right, the pages' dots
    void buildLiveTab()
    {
        m_liveTab = m_bar->add<View>();
        m_liveTab->setFrame({0, 0, 640, 61});
        m_option = m_liveTab->add<RecordingRadioButton>();
        m_option->setFrame({20, 18, 98, 25});
        m_option->onSelect = [this](int) {   // -switchRecordingOption:
            setImages(m_shutter, m_option->selected == 2 ? "movieBtn_red" : "shutterBtn_red");
            refreshControls();
        };
        m_shutter = m_liveTab->add<ImageButton>();
        m_shutter->setFrame({298, 9, 43, 45});
        setImages(m_shutter, "shutterBtn_red");
        m_shutter->action = [this] { takeSnapshot(m_shutter->modifiers); };
        const double width = std::max(115.0, textWidth(tr("Effects"), Font::system(13)) + 66 + 14);
        m_effects = m_liveTab->add<EffectsButton>();
        m_effects->setFrame({640 - 14 - width, 18, width, 25});
        m_effects->onToggle = [this] { m_mosaic ? showEffect(m_effect) : showEffects(); };
        m_effects->onLeft = [this] { showPage(m_page - 1); };
        m_effects->onRight = [this] { showPage(m_page + 1); };
        m_pages = m_liveTab->add<PageIndicator>();
        m_pages->count = PageCount;
        m_pages->setFrame({640 - 14 - width - 15 - PageCount * 16, 18, PageCount * 16.0, 25});
        m_pages->onPage = [this](int page) { showPage(page); };
        m_pages->setHidden(true);
    }
    // The Still tab: the grey shutter back to the video, and the picture's uses
    void buildStillTab()
    {
        m_stillTab = m_bar->add<View>();
        m_stillTab->setFrame({0, 0, 640, 61});
        auto *back = m_stillTab->add<ImageButton>();
        back->setFrame({298, 9, 43, 45});
        setImages(back, "shutterBtn_blank");
        back->action = [this] { switchToVideo(); };
        auto button = [this](const char *title, const char *image, Rect frame) {
            auto *b = m_stillTab->add<TitledImageButton>();
            b->setFrame(frame);
            b->title = title;
            b->image = art(image);
            return b;
        };
        auto *email = button("Email", "iconBtn_Mail", {16, 8, 38, 50});
        email->action = [this] { sendEmail(); };
        // iPhoto, the account's picture and iChat's are the Mac's own programs: not done
        button("Account Picture", "iconBtn_account", {455, 8, 80, 50})->action = [this] { useForLogin(); };
        button("Buddy Picture", "iconBtn_iChat", {553, 8, 75, 50})->setEnabled(false);
    }

    void selectTab(Tab tab)
    {
        m_tab = tab;
        m_liveTab->setHidden(tab != Tab::Live);
        m_stillTab->setHidden(tab != Tab::Still);
        m_countdown->setHidden(tab != Tab::CountDown);
        m_bar->setNeedsDisplay();
    }

    void refreshControls()
    {
        const bool camera = m_camera.status() == Camera::On;
        // movies (PBQTMovieController, the Fez recorders) are not done: nothing to record with
        m_shutter->setEnabled(camera && !m_developing && !m_recording && m_option->selected != 2);
        m_effects->setEnabled(camera);
        m_effects->on = m_mosaic;
        m_effects->leftEnabled = m_mosaic && m_page > 0;                 // -updateButtons:currentPage:isMosaic:
        m_effects->rightEnabled = m_mosaic && m_page < PageCount - 1;
        m_pages->setHidden(!m_mosaic);
        m_pages->current = m_page;
        m_effects->setNeedsDisplay();
        m_pages->setNeedsDisplay();
    }
    void refreshScrollButtons()   // -[PBTrayView refreshScrollButtons]
    {
        m_previous->setEnabled(m_tray->canScrollPrevious());
        m_next->setEnabled(m_tray->canScrollNext());
    }
    void refreshPhotoCount()   // -[PBTrayView refreshPhotoCount]
    {
        long photos = 0, movies = 0;
        for (const Asset &a : m_library.assets())
            (a.movie ? movies : photos)++;
        const std::string p = photos == 1 ? counted("%@ photo", 1) : photos > 1 ? counted("%@ photos", photos) : "";
        const std::string m = movies == 1 ? counted("%@ movie", 1) : movies > 1 ? counted("%@ movies", movies) : "";
        std::string text = tr("No photos");
        if (photos > 0 && movies > 0) {
            text = tr("%@ and %@");
            text.replace(text.find("%@"), 2, p);
            text.replace(text.find("%@"), 2, m);
        } else if (photos > 0 || movies > 0) {
            text = photos > 0 ? p : m;
        }
        m_count->setText(text);
    }

    // ---- the video

    void tick()
    {
        Image frame;
        const Camera::Status before = m_video->status;
        if (!m_paused && m_camera.read(frame))
            newFrame(mirrored(frame));   // Effect.qtz shows the camera mirrored (CIAffineTransform, scale x -1)
        if (m_camera.status() != before) {
            m_video->status = m_camera.status();
            m_video->setNeedsDisplay();
            refreshControls();
        }
        m_tray->animate();
        if (m_flashStart > 0) {
            // PBCameraFlash's defaultFlash: 0.04 s up, 0.2 s held, 0.24 s down
            const double t = now() - m_flashStart;
            const double alpha = t < 0.04 ? t / 0.04 : t < 0.24 ? 1 : t < 0.48 ? 1 - (t - 0.24) / 0.24 : 0;
            if (t >= 0.48)
                m_flashStart = 0;
            m_flash->alpha = alpha;
            m_flash->setNeedsDisplay();
        }
    }
    void newFrame(const Image &frame)
    {
        m_frame = frame;
        if (m_video->mode == VideoView::Mosaic) {
            // a page: its eight effects around Normal, each at its cell's size
            const Rect cell = VideoView::cell(0);
            const Image small = frame.scaled(int(cell.w), int(cell.h));
            for (int i = 0; i < 9; ++i) {
                const int effect = i == 4 ? NormalEffect : m_page * 8 + (i < 4 ? i : i - 1);
                m_video->previewEffects[i] = effect;
                m_video->previews[i] = m_fx.apply(effect, small);
            }
        } else if (m_video->mode == VideoView::Live || m_video->mode == VideoView::FourUp) {
            m_video->display = m_fx.apply(m_effect, frame, m_xy);
        }
        m_video->setNeedsDisplay();
    }

    // ---- View ▸ Show Effects, Show Last Effect, Next / Previous Page of Effects

    static constexpr int PageCount = EffectCount / 8;
    void showEffects()
    {
        if (m_camera.status() != Camera::On || m_recording)
            return;
        leaveStill();
        m_mosaic = true;
        m_page = m_effect == NormalEffect ? m_page : m_effect / 8;
        m_video->mode = VideoView::Mosaic;
        if (!m_frame.isNull())
            newFrame(m_frame);
        refreshControls();
    }
    void showPage(int page)
    {
        if (!m_mosaic || page < 0 || page >= PageCount)
            return;
        m_page = page;
        if (!m_frame.isNull())
            newFrame(m_frame);
        refreshControls();
    }
    void showEffect(int effect)
    {
        if (m_recording)
            return;
        leaveStill();
        if (effect != m_effect)
            m_xy = {0.5, 0.5};
        if (effect != NormalEffect)
            m_lastEffect = effect;
        m_effect = effect;
        m_mosaic = false;
        m_video->mode = VideoView::Live;
        m_video->positioned = effectInfo(effect).positioned;
        if (!m_frame.isNull())
            newFrame(m_frame);
        refreshControls();
    }

    // ---- taking pictures

    // -takeSnapshot: Option takes it at once, Shift without the flash
    void takeSnapshot(unsigned modifiers)
    {
        if (m_tray->isScrollingEndForNewPhoto() || m_recording || m_developing || m_camera.status() != Camera::On || m_option->selected == 2)
            return;
        if (m_mosaic || m_video->mode == VideoView::Still)
            showEffect(m_effect);
        m_recording = true;
        m_skipFlash = modifiers & Shift;
        refreshControls();
        if (modifiers & Option) {
            record();
            return;
        }
        // -[PBCountdownController start]: the counter down every 0.8 s, four times; then the picture
        selectTab(Tab::CountDown);
        m_countdown->counter = 4;
        const int serial = ++m_serial;
        for (int i = 1; i <= 4; ++i)
            App::shared().addTimer(i * 0.8, false, [this, serial, i] {
                if (serial != m_serial)
                    return;
                m_countdown->counter = 4 - i;
                m_countdown->setNeedsDisplay();
                if (i == 4)
                    record();
            });
    }
    void cancelRecording()
    {
        if (!m_recording)
            return;
        ++m_serial;
        m_recording = false;
        m_paused = false;
        m_video->fourUp.clear();
        m_video->mode = VideoView::Live;
        selectTab(Tab::Live);
        refreshControls();
    }
    // -[PBPhotoController record:] / -[PBAnimationController record:] (four pictures, one a second)
    void record()
    {
        const int serial = m_serial;
        if (m_option->selected == 1) {
            m_video->fourUp.clear();
            m_video->mode = VideoView::FourUp;
            for (int i = 0; i < 4; ++i)
                App::shared().addTimer(i * 1.0, false, [this, serial, i] {
                    if (serial != m_serial)
                        return;
                    shoot([this, serial, i](const Image &picture) {
                        if (serial != m_serial)
                            return;
                        m_video->fourUp.push_back(picture);
                        if (i < 3)
                            return;
                        std::vector<Image> four;
                        for (const Image &p : m_video->fourUp)
                            four.push_back(m_autoFlip ? mirrored(p) : p);
                        // the four stay a moment, as the photo does
                        App::shared().addTimer(0.5, false, [this, serial] {
                            if (serial != m_serial)
                                return;
                            m_video->fourUp.clear();
                            m_video->mode = VideoView::Live;
                            m_video->setNeedsDisplay();
                        });
                        developed(m_library.addFourUp(four));
                    });
                });
        } else {
            shoot([this, serial](const Image &picture) {
                if (serial == m_serial)
                    developed(m_library.addPhoto(m_autoFlip ? mirrored(picture) : picture));
            });
        }
    }
    // The flash, then the picture once it is at its brightest (-[PBCameraFlash shutterDelay]: 0.24 s)
    void shoot(std::function<void(const Image &)> taken)
    {
        if (!m_skipFlash)
            m_flashStart = now();
        App::shared().addTimer(m_skipFlash ? 0 : 0.24, false, [this, taken] { taken(m_video->display); });
    }
    void developed(bool saved)
    {
        m_recording = false;
        selectTab(Tab::Live);
        if (saved) {
            m_developing = true;
            if (m_tray->selected >= 0)
                m_tray->selected = -1;
            m_tray->appendNew();   // the photo drops into the tray; PBTrayViewDidFinishDeveloping ends it
            refreshPhotoCount();
        } else {
            std::fprintf(stderr, "Photo Booth: %s\n", tr("Photo Booth couldn\\U2019t save your photos").c_str());
        }
        refreshControls();
    }

    // ---- the tray's photos

    void select(int index)
    {
        if (m_recording)
            return;
        m_tray->selected = index;
        m_tray->reveal(index);
        m_tray->setNeedsDisplay();
        if (index < 0) {
            switchToVideo();
            return;
        }
        // -[PBImagePickerController showAsset:]
        m_mosaic = false;
        m_video->still = m_library.fullImage(index);
        m_video->mode = VideoView::Still;
        m_video->setNeedsDisplay();
        selectTab(Tab::Still);
        refreshControls();
    }
    void leaveStill()
    {
        if (m_video->mode != VideoView::Still)
            return;
        m_video->still = Image();
        m_tray->selected = -1;
        m_tray->setNeedsDisplay();
        selectTab(Tab::Live);
    }
    void switchToVideo()   // -switchToVideo:
    {
        leaveStill();
        m_video->mode = m_mosaic ? VideoView::Mosaic : VideoView::Live;
        m_video->setNeedsDisplay();
        refreshControls();
    }
    void deleteAsset(int index)
    {
        if (index < 0 || index >= int(m_library.assets().size()) || m_developing)
            return;
        const bool wasSelected = index == m_tray->selected;
        const int selected = m_tray->selected;
        // -[PBTrayViewNode delete:]: its undo (PBAsset undoDelete, UndoDeleteViewLayout)
        m_undo.push_back({m_library.remove(index), index});
        m_tray->removeNode(index);
        refreshPhotoCount();
        // -_updateSelectedForNewNodeIndex:: the next photo, or the last
        const int count = int(m_library.assets().size());
        if (wasSelected)
            select(count == 0 ? -1 : std::min(index, count - 1));
        else if (selected > index)
            m_tray->selected = selected - 1;
    }
    // Edit ▸ Undo Delete: the photo back from IntermediateTrashDirectory, in its place
    void undoDelete()
    {
        if (m_undo.empty() || m_developing)
            return;
        const auto [asset, index] = m_undo.back();
        m_undo.pop_back();
        m_library.restore(asset, index);
        m_tray->insertNode(index);
        refreshPhotoCount();
        select(index);
    }
    // Edit ▸ Delete All Photos…: -deleteAll:'s critical alert sheet, OK deletes them all (no undo)
    void deleteAll()
    {
        Alert::beginSheet(m_window.get(), Image::load(Assets::path(std::string(Bundle) + "/Contents/Resources/PhotoBooth.icns"), {64, 64}),
                          tr("Are you sure you want to permanently delete all the photos?"),
                          tr("You cannot recover the photos after they are deleted."), {tr("OK"), tr("Cancel")}, [this](int b) {
                              if (b != 0)
                                  return;
                              m_library.removeAll();
                              m_undo.clear();
                              m_tray->reloadData();
                              select(-1);
                              refreshPhotoCount();
                          });
    }
    // Edit ▸ Copy (one photo): its file and its picture on the pasteboard
    void copySelected()
    {
        const int i = m_tray->selected;
        if (i < 0 || m_library.assets()[i].movie)
            return;
        const std::string tmp = "/tmp/photobooth-copy.png";
        m_library.fullImage(i).savePng(tmp);
        const bool wayland = std::getenv("WAYLAND_DISPLAY");
        spawn(wayland ? std::vector<std::string>{"sh", "-c", "wl-copy -t image/png < '" + tmp + "'"}
                      : std::vector<std::string>{"xclip", "-selection", "clipboard", "-t", "image/png", "-i", tmp});
    }
    // File ▸ Export…: the save panel in PBLastExportPath (~/Pictures), the name with .gif; a GIF of the
    // asset's pictures (a 4-up's four, a second each, looping: NSImageFrameCount, NSImageCurrentFrameDuration
    // 1, NSImageLoopCount 0)
    void exportSelected()
    {
        const int i = m_tray->selected;
        if (i < 0 || m_library.assets()[i].movie)
            return;
        const std::string last = m_settings.string("PBLastExportPath", home() + "/Pictures");
        const std::string name = baseName(m_library.assets()[i].files[0]);
        SavePanel::run("Export", last + "/" + (m_library.assets()[i].files.size() == 4 ? name.substr(0, name.rfind(" #")) : name) + ".gif", {},
                       [this, i](std::string path) {
                           if (path.empty())
                               return;
                           m_settings.set("PBLastExportPath", dirName(path));
                           std::vector<std::string> argv = {"ffmpeg", "-y", "-loglevel", "error"};
                           const auto &files = m_library.assets()[size_t(i)].files;
                           if (files.size() > 1) {
                               argv.insert(argv.end(), {"-framerate", "1", "-i", m_library.root() + "/" + baseName(files[0]).substr(0, baseName(files[0]).rfind(" #")) + " #%d.jpg", "-loop", "0"});
                           } else {
                               argv.insert(argv.end(), {"-i", m_library.path(i)});
                           }
                           argv.push_back(path);
                           spawn(argv);
                       });
    }
    // File ▸ Print…: PBPrinting's WebKit templates and accessory not ported: the picture to lp
    void printSelected()
    {
        const int i = m_tray->selected < 0 ? int(m_library.assets().size()) - 1 : m_tray->selected;
        if (i < 0 || m_library.assets()[i].movie)
            return;
        const std::string tmp = "/tmp/photobooth-print.png";
        m_library.fullImage(i).savePng(tmp);
        spawn({"lp", "-t", "Photo Booth", tmp});
    }
    // View ▸ Start Slideshow: IKSlideshow (images) from the photo selected, full screen
    void startSlideshow()
    {
        if (m_library.assets().empty() || m_slideshow)
            return;
        const Rect screen = App::shared().screenFrame();
        m_slideshow = std::make_unique<Window>(Size{screen.w, screen.h}, "Slideshow", Window::FullScreen);
        auto *v = m_slideshow->contentView()->add<SlideView>();
        v->setFrame({0, 0, screen.w, screen.h});
        v->library = &m_library;
        v->index = std::max(0, m_tray->selected);
        v->onEnd = [this] { App::shared().later([this] { m_slideshow.reset(); }); };
        m_slideshow->makeFirstResponder(v);
        m_slideshow->show();
        m_slideshow->grabInput();
    }
    // Account Picture (-useForLogin:, PBUserPicture): the picture made square, the user's picture
    // (here ~/.face, as the display managers read it), then the Accounts pane
    void useForLogin()
    {
        const int i = m_tray->selected;
        if (i < 0 || m_library.assets()[i].movie)
            return;
        const Image full = m_library.fullImage(i);
        const int side = std::min(full.width(), full.height());
        Image square(side, side);
        cairo_t *cr = cairo_create(square.surface());
        full.draw(cr, Rect{-(full.width() - side) / 2.0, -(full.height() - side) / 2.0, double(full.width()), double(full.height())});
        cairo_destroy(cr);
        square.savePng(home() + "/.face");
        spawn({"xdg-open", "x-snowleopard-prefs:accounts"});
    }

    void flipSelected()   // Edit ▸ Flip Photo
    {
        const int i = m_tray->selected;
        if (i < 0)
            return;
        m_library.flip(i);
        m_video->still = m_library.fullImage(i);
        m_video->setNeedsDisplay();
        m_tray->setNeedsDisplay();
    }
    void revealInFinder()   // File ▸ Reveal in Finder
    {
        spawn({"xdg-open", m_library.root()});
    }
    void sendEmail()
    {
        const int i = m_tray->selected;
        if (i < 0)
            return;
        std::vector<std::string> argv = {"xdg-email"};
        for (size_t f = 0; f < m_library.assets()[i].files.size(); ++f) {
            argv.push_back("--attach");
            argv.push_back(m_library.path(i, int(f)));
        }
        spawn(argv);
    }

    // MainMenu.nib's menus in the menu bar (their key equivalents too), as -[PBController
    // validateMenuItem:] would leave them: what this port does enabled
    void loadMenus()
    {
        m_menu.load(Assets::path(std::string(Bundle) + "/Contents/Resources/English.lproj/MainMenu.nib"));
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) {
            const std::string &a = it.action;
            const bool selected = m_tray->selected >= 0;
            if (a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
                a == "performClose:" || a == "performMiniaturize:" || a == "showEffects:" || a == "showLastEffect:" ||
                a == "resetEffect:")
                return true;
            if (a == "takeSnapshot:")
                return !m_recording && m_tab != Tab::Still;
            if (a == "revealInFinder:" || a == "delete:" || a == "mirrorImageButtonClicked:")
                return selected;
            if (a == "autoFlipPhotoOnOff:") {
                v.checked = m_autoFlip;
                return true;
            }
            if (a == "showPicture:")
                return !m_library.assets().empty();
            if (a == "undo:") {
                v.title = m_undo.empty() ? "Undo" : "Undo Delete";
                return !m_undo.empty();
            }
            const bool image = selected && !m_library.assets()[m_tray->selected].movie;
            if (a == "copy:" || a == "export:")
                return image;
            if (a == "printDocument:")
                return image;
            if (a == "deleteAll:" || a == "startSlideshow:")
                return !m_library.assets().empty();
            if (a == "nextEffects:" || a == "previousEffects:")
                return m_video->mode != VideoView::Still;
            return false;
        };
        m_menu.perform = [this](const MainMenuItem &it) {
            const std::string &a = it.action;
            const int count = int(m_library.assets().size());
            if (a == "terminate:" || a == "performClose:") {
                App::shared().quit();
            } else if (a == "performMiniaturize:") {
                m_window->miniaturize();
            } else if (a == "takeSnapshot:") {
                takeSnapshot(0);
            } else if (a == "revealInFinder:") {
                revealInFinder();
            } else if (a == "delete:") {
                deleteAsset(m_tray->selected);
            } else if (a == "mirrorImageButtonClicked:") {
                flipSelected();
            } else if (a == "autoFlipPhotoOnOff:") {
                m_autoFlip = !m_autoFlip;
                m_settings.set("AutoFlipPhoto", m_autoFlip);
            } else if (a == "undo:") {
                undoDelete();
            } else if (a == "deleteAll:") {
                deleteAll();
            } else if (a == "copy:") {
                copySelected();
            } else if (a == "export:") {
                exportSelected();
            } else if (a == "printDocument:") {
                printSelected();
            } else if (a == "startSlideshow:") {
                startSlideshow();
            } else if (a == "showPicture:") {
                if (m_tray->selected < 0 && count > 0)
                    select(count - 1);
            } else if (a == "showEffects:") {
                showEffects();
            } else if (a == "showLastEffect:") {
                showEffect(m_lastEffect);
            } else if (a == "nextEffects:" || a == "previousEffects:") {
                showPage(m_page + (a == "nextEffects:" ? 1 : -1));
            } else if (a == "resetEffect:") {
                m_xy = {0.5, 0.5};
            }
        };
        m_menu.publish();
    }

    // The tray's keys and the shutter's; the command keys are the menus'

    void key(Event &e)
    {
        const bool cmd = e.modifiers & (Command | Control);
        const int count = int(m_library.assets().size());
        if (e.keysym == 0xff1b) {   // Escape
            if (m_recording)
                cancelRecording();
            else if (m_video->mode == VideoView::Still)
                switchToVideo();
            else if (m_mosaic)
                showEffect(m_effect);
        } else if (!cmd && (e.keysym == 0xff51 || e.keysym == 0xff53)) {   // the tray: the photo before, after
            if (count > 0) {
                const int s = m_tray->selected;
                select(s < 0 ? count - 1 : std::clamp(s + (e.keysym == 0xff53 ? 1 : -1), 0, count - 1));
            }
        } else if (!cmd && (e.keysym == 0xff08 || e.keysym == 0xffff)) {   // Delete
            deleteAsset(m_tray->selected);
        } else if (e.keysym == 0xff0d || e.keysym == 0xff8d) {   // Return: the shutter
            if (m_tab == Tab::Still)
                switchToVideo();
            else
                takeSnapshot(e.modifiers);
        } else if (cmd) {
            m_menu.performKeyEquivalent(e);   // MainMenu.nib's key equivalents
        }
    }

    Settings m_settings;
    MainMenu m_menu;
    std::unique_ptr<Window> m_window;
    Camera m_camera;
    Effects m_fx;
    Library m_library;
    std::vector<std::pair<Asset, int>> m_undo;   // the photos deleted, for Undo
    std::unique_ptr<Window> m_slideshow;

    VideoView *m_video = nullptr;
    ControlBar *m_bar = nullptr;
    View *m_liveTab = nullptr, *m_stillTab = nullptr;
    RecordingRadioButton *m_option = nullptr;
    ImageButton *m_shutter = nullptr, *m_previous = nullptr, *m_next = nullptr;
    EffectsButton *m_effects = nullptr;
    PageIndicator *m_pages = nullptr;
    CountdownView *m_countdown = nullptr;
    TrayView *m_tray = nullptr;
    Label *m_count = nullptr;
    FlashView *m_flash = nullptr;

    Tab m_tab = Tab::Live;
    Image m_frame;                 // the camera's last, mirrored
    int m_effect = NormalEffect, m_lastEffect = NormalEffect, m_page = 0;
    Point m_xy{0.5, 0.5};
    bool m_mosaic = false, m_recording = false, m_developing = false, m_paused = false;
    bool m_skipFlash = false, m_autoFlip = false;
    int m_serial = 0;
    double m_flashStart = 0;
};

} // namespace

int main()
{
    App &app = App::shared();
    setProgramName("Photo Booth");
    if (!app.open()) {
        std::fprintf(stderr, "Photo Booth: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    if (!Assets::exists(std::string(Bundle) + "/Contents/Resources/photo_back.png")) {
        std::fprintf(stderr, "Photo Booth: its pictures are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    PhotoBooth booth;
    const int code = app.run();
    // -applicationWillTerminate:: the photos deleted go to the Trash
    booth.quitting();
    return code;
}
