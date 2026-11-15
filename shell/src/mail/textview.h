// SPDX-License-Identifier: LGPL-2.1-or-later
// A plain text NSTextView for Mail's compose window: several lines wrapped at words, the caret, typing,
// the arrows, Return and Delete, a click to place the caret. Inside a ScrollView, it grows with its
// text (onHeightChanged).
#pragma once

#include "appkit.h"
#include "text.h"

#include <functional>
#include <string>
#include <vector>

namespace Mail {

class TextView : public AK::View {
public:
    TextView();
    ~TextView() override;
    std::string text;
    AK::Font font = AK::Font::system(12);   // Mail's message font: Helvetica 12 on 10.6, Lucida here
    double inset = 6;
    std::function<void(double)> onHeightChanged;
    void setText(const std::string &t);
    double contentHeight() const;
    void draw(cairo_t *cr) override;
    void mouseDown(AK::Event &e) override;
    void keyDown(AK::Event &e) override;
    bool acceptsFirstResponder() const override { return isEnabled(); }
    void focusChanged() override;
    void setFrame(const AK::Rect &r) override;

private:
    struct Line {
        size_t start, end;   // bytes of text, end excluding the newline
    };
    void layout() const;
    size_t lineOf(size_t pos) const;
    size_t posAt(AK::Point p) const;
    void changed();
    mutable std::vector<Line> m_lines;
    mutable double m_layoutWidth = -1;
    mutable std::string m_layoutText;
    size_t m_caret = 0;
    int m_blink = 0;
    bool m_caretOn = true;
};

} // namespace Mail
