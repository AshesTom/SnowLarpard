// SPDX-License-Identifier: LGPL-2.1-or-later
#include "textview.h"

#include <cmath>

using namespace AK;

namespace Mail {

namespace {
constexpr uint32_t KeyBackSpace = 0xff08, KeyTab = 0xff09, KeyReturn = 0xff0d, KeyEnter = 0xff8d, KeyHome = 0xff50,
                   KeyLeft = 0xff51, KeyUp = 0xff52, KeyRight = 0xff53, KeyDown = 0xff54, KeyEnd = 0xff57,
                   KeyDelete = 0xffff;

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
} // namespace

TextView::TextView() = default;

TextView::~TextView()
{
    if (m_blink)
        App::shared().removeTimer(m_blink);
}

void TextView::setText(const std::string &t)
{
    text = t;
    m_caret = 0;
    changed();
}

void TextView::setFrame(const Rect &r)
{
    View::setFrame(r);
    m_layoutWidth = -1;
}

void TextView::layout() const
{
    const double width = frame().w - 2 * inset;
    if (width == m_layoutWidth && text == m_layoutText)
        return;
    m_layoutWidth = width;
    m_layoutText = text;
    m_lines.clear();
    size_t p = 0;
    for (;;) {
        size_t nl = text.find('\n', p);
        const size_t paraEnd = nl == std::string::npos ? text.size() : nl;
        // the paragraph cut at words where it doesn't fit
        size_t s = p;
        while (true) {
            if (textWidth(text.substr(s, paraEnd - s), font) <= width || s >= paraEnd) {
                m_lines.push_back({s, paraEnd});
                break;
            }
            size_t cut = s, lastSpace = std::string::npos;
            while (cut < paraEnd) {
                size_t next = cut + 1;
                while (next < paraEnd && continuation(text[next]))
                    ++next;
                if (textWidth(text.substr(s, next - s), font) > width)
                    break;
                if (text[cut] == ' ')
                    lastSpace = cut;
                cut = next;
            }
            if (cut == s)   // one character wider than the view
                cut = s + 1;
            const size_t end = lastSpace != std::string::npos && lastSpace > s ? lastSpace + 1 : cut;
            m_lines.push_back({s, end});
            s = end;
        }
        if (nl == std::string::npos)
            break;
        p = nl + 1;
    }
}

double TextView::contentHeight() const
{
    layout();
    return 2 * inset + double(m_lines.size()) * std::ceil(lineHeight(font));
}

size_t TextView::lineOf(size_t pos) const
{
    layout();
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const bool last = i + 1 == m_lines.size();
        if (pos >= m_lines[i].start && (pos < m_lines[i].end || (pos == m_lines[i].end && (last || m_lines[i + 1].start > pos))))
            return i;
    }
    return m_lines.empty() ? 0 : m_lines.size() - 1;
}

size_t TextView::posAt(Point p) const
{
    layout();
    const double lh = std::ceil(lineHeight(font));
    long li = long(std::floor((p.y - inset) / lh));
    if (li < 0)
        return 0;
    if (size_t(li) >= m_lines.size())
        return text.size();
    const Line &l = m_lines[size_t(li)];
    size_t best = l.start;
    double bestDist = 1e9;
    for (size_t i = l.start; i <= l.end; ++i) {
        if (i < l.end && continuation(text[i]))
            continue;
        const double x = inset + textWidth(text.substr(l.start, i - l.start), font);
        if (std::fabs(x - p.x) < bestDist) {
            bestDist = std::fabs(x - p.x);
            best = i;
        }
    }
    return best;
}

void TextView::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(255));
    layout();
    const double lh = std::ceil(lineHeight(font));
    TextStyle style;
    style.font = font;
    style.elide = false;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const Line &l = m_lines[i];
        drawText(cr, text.substr(l.start, l.end - l.start), Rect{inset, inset + double(i) * lh, frame().w, lh}, style);
    }
    if (isFirstResponder() && m_caretOn && windowIsKey()) {
        const size_t li = lineOf(m_caret);
        const Line &l = m_lines.empty() ? Line{0, 0} : m_lines[li];
        const double x = std::floor(inset + textWidth(text.substr(l.start, std::min(m_caret, l.end) - l.start), font)) + 0.5;
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, x, inset + double(li) * lh + 1);
        cairo_line_to(cr, x, inset + double(li + 1) * lh - 1);
        cairo_stroke(cr);
    }
}

void TextView::focusChanged()
{
    if (isFirstResponder() && !m_blink) {
        // the insertion point's blink (NSTextInsertionPointBlinkPeriod: 0.567 s on, 0.567 off)
        m_blink = App::shared().addTimer(0.567, true, [this] {
            m_caretOn = !m_caretOn;
            setNeedsDisplay();
        });
    } else if (!isFirstResponder() && m_blink) {
        App::shared().removeTimer(m_blink);
        m_blink = 0;
    }
    m_caretOn = true;
    setNeedsDisplay();
}

void TextView::mouseDown(Event &e)
{
    if (Window *w = window())
        w->makeFirstResponder(this);
    m_caret = posAt(e.location);
    m_caretOn = true;
    setNeedsDisplay();
}

void TextView::changed()
{
    m_caretOn = true;
    setNeedsDisplay();
    if (onHeightChanged)
        onHeightChanged(contentHeight());
}

void TextView::keyDown(Event &e)
{
    if (e.modifiers & Command) {
        View::keyDown(e);   // the window's shortcuts
        return;
    }
    auto prev = [this](size_t p) {
        if (p == 0)
            return p;
        --p;
        while (p > 0 && continuation(text[p]))
            --p;
        return p;
    };
    auto next = [this](size_t p) {
        if (p >= text.size())
            return text.size();
        ++p;
        while (p < text.size() && continuation(text[p]))
            ++p;
        return p;
    };
    layout();
    switch (e.keysym) {
    case KeyLeft:
        m_caret = prev(m_caret);
        break;
    case KeyRight:
        m_caret = next(m_caret);
        break;
    case KeyUp:
    case KeyDown: {
        const size_t li = lineOf(m_caret);
        const double lh = std::ceil(lineHeight(font));
        const double x = inset + textWidth(text.substr(m_lines[li].start, m_caret - m_lines[li].start), font);
        const long target = long(li) + (e.keysym == KeyUp ? -1 : 1);
        if (target < 0)
            m_caret = 0;
        else if (size_t(target) >= m_lines.size())
            m_caret = text.size();
        else
            m_caret = posAt(Point{x, inset + double(target) * lh + lh / 2});
        break;
    }
    case KeyHome:
        m_caret = m_lines[lineOf(m_caret)].start;
        break;
    case KeyEnd:
        m_caret = m_lines[lineOf(m_caret)].end;
        break;
    case KeyBackSpace:
        if (m_caret > 0) {
            const size_t p = prev(m_caret);
            text.erase(p, m_caret - p);
            m_caret = p;
        }
        break;
    case KeyDelete:
        if (m_caret < text.size())
            text.erase(m_caret, next(m_caret) - m_caret);
        break;
    case KeyReturn:
    case KeyEnter:
        text.insert(m_caret, "\n");
        ++m_caret;
        break;
    case KeyTab:
        View::keyDown(e);
        return;
    default:
        if (e.text.empty() || static_cast<unsigned char>(e.text[0]) < 0x20) {
            View::keyDown(e);
            return;
        }
        text.insert(m_caret, e.text);
        m_caret += e.text.size();
    }
    changed();
}

} // namespace Mail
