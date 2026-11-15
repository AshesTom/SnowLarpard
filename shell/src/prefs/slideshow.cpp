// SPDX-License-Identifier: LGPL-2.1-or-later
#include "slideshow.h"

#include "desktop.h"

#include <cmath>
#include <cstdlib>
#include <random>

using namespace AK;

Slideshow::~Slideshow() { stop(); }

void Slideshow::stop()
{
    if (m_hold)
        App::shared().removeTimer(m_hold);
    if (m_step)
        App::shared().removeTimer(m_step);
    m_hold = m_step = 0;
}

void Slideshow::setFolder(const std::string &folder)
{
    stop();
    m_pictures = Desktop::pictures(folder);
    m_index = -1;
    m_current = m_previous = Image();
    next();
}

void Slideshow::next()
{
    stop();
    if (m_pictures.empty()) {
        setNeedsDisplay();
        return;
    }
    static std::mt19937 random{std::random_device{}()};
    m_index = m_fullScreen ? int(random() % m_pictures.size()) : (m_index + 1) % int(m_pictures.size());
    m_previous = m_current;
    m_current = Image::load(m_pictures[size_t(m_index)], {frame().w, frame().h});
    if (!m_current.isNull() && m_current.width() > frame().w * 1.5) {
        const Size s = scaledToFill(m_current.size(), {frame().w, frame().h});
        m_current = m_current.scaled(int(s.w), int(s.h));
    }
    m_fade = m_previous.isNull() ? 1 : 0;
    if (m_fade < 1)
        m_step = App::shared().addTimer(0.03, true, [this] {
            m_fade = std::min(1.0, m_fade + 0.04);
            setNeedsDisplay();
            if (m_fade >= 1) {
                App::shared().removeTimer(m_step);
                m_step = 0;
                m_hold = App::shared().addTimer(5, false, [this] { m_hold = 0, next(); });
            }
        });
    else
        m_hold = App::shared().addTimer(5, false, [this] { m_hold = 0, next(); });
    setNeedsDisplay();
}

void Slideshow::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(0));
    auto paint = [&](const Image &img, double alpha) {
        if (img.isNull())
            return;
        const Size s = scaledToFill(img.size(), {frame().w, frame().h});
        img.draw(cr, Rect{std::floor((frame().w - s.w) / 2), std::floor((frame().h - s.h) / 2), s.w, s.h}, alpha);
    };
    paint(m_previous, 1);
    paint(m_current, m_fade);
}

// full screen: a key, a click or the pointer moving ends it
void Slideshow::mouseDown(Event &)
{
    if (m_fullScreen)
        App::shared().quit();
}

void Slideshow::keyDown(Event &e)
{
    if (m_fullScreen)
        App::shared().quit();
    else
        View::keyDown(e);
}

void Slideshow::mouseMoved(Event &e)
{
    if (!m_fullScreen)
        return;
    if (m_firstMouse.x < 0)
        m_firstMouse = e.screen;
    else if (std::abs(e.screen.x - m_firstMouse.x) + std::abs(e.screen.y - m_firstMouse.y) > 6)
        App::shared().quit();
}
