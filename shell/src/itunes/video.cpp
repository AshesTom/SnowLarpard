// SPDX-License-Identifier: LGPL-2.1-or-later
#include "video.h"

#include "player.h"

#include <xkbcommon/xkbcommon-keysyms.h>

using namespace AK;

namespace iTunes {

void VideoView::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(0));
    const int w = int(frame().w), h = int(frame().h);
    if (!m_player.hasVideo() || w <= 0 || h <= 0)
        return;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_surface_flush(s);
    // mpv's "bgr0": the bytes B, G, R, 0 — cairo's RGB24 on a little-endian machine
    if (m_player.renderFrame(w, h, cairo_image_surface_get_stride(s), cairo_image_surface_get_data(s))) {
        cairo_surface_mark_dirty(s);
        cairo_save(cr);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_surface_destroy(s);
}

void VideoView::mouseDown(Event &e)
{
    window()->makeFirstResponder(this);
    if (e.clickCount == 2 && onDoubleClick)
        onDoubleClick();
}

void VideoView::keyDown(Event &e)
{
    if (e.keysym == XKB_KEY_Escape && onEscape)
        onEscape();
    else
        View::keyDown(e);
}

} // namespace iTunes
