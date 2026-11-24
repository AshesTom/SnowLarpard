// SPDX-License-Identifier: LGPL-2.1-or-later
#include "compositor.h"
#include "frame.h"

#include <QRegion>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/shape.h>

namespace {

using Frame::ShadowReach;

xcb_atom_t internAtom(xcb_connection_t *c, const char *name)
{
    xcb_atom_t a = 0;
    if (auto *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(strlen(name)), name), nullptr)) {
        a = r->atom;
        free(r);
    }
    return a;
}

xcb_rectangle_t toXcb(const QRect &r) { return {int16_t(r.x()), int16_t(r.y()), uint16_t(r.width()), uint16_t(r.height())}; }

} // namespace

bool Compositor::findFormats(xcb_visualid_t rootVisual)
{
    auto *r = xcb_render_query_pict_formats_reply(m_conn, xcb_render_query_pict_formats(m_conn), nullptr);
    if (!r)
        return false;
    for (auto it = xcb_render_query_pict_formats_formats_iterator(r); it.rem; xcb_render_pictforminfo_next(&it)) {
        const xcb_render_pictforminfo_t *f = it.data;
        if (f->type != XCB_RENDER_PICT_TYPE_DIRECT)
            continue;
        m_formatAlpha.insert(f->id, f->direct.alpha_mask != 0);
        if (f->depth == 8 && f->direct.alpha_mask == 0xff && f->direct.red_mask == 0)
            m_a8 = f->id;
        if (f->depth == 32 && f->direct.alpha_shift == 24 && f->direct.red_shift == 16 && f->direct.green_shift == 8 &&
            f->direct.blue_shift == 0 && f->direct.alpha_mask == 0xff)
            m_argb32 = f->id;
    }
    for (auto s = xcb_render_query_pict_formats_screens_iterator(r); s.rem; xcb_render_pictscreen_next(&s))
        for (auto d = xcb_render_pictscreen_depths_iterator(s.data); d.rem; xcb_render_pictdepth_next(&d))
            for (auto v = xcb_render_pictdepth_visuals_iterator(d.data); v.rem; xcb_render_pictvisual_next(&v))
                m_visualFormats.insert(v.data->visual, v.data->format);
    free(r);
    m_rootFormat = m_visualFormats.value(rootVisual);
    return m_a8 && m_rootFormat;
}

bool Compositor::start(xcb_connection_t *conn, xcb_window_t root, ShadowFor shadowFor)
{
    m_conn = conn;
    m_root = root;
    m_shadowFor = std::move(shadowFor);
    const xcb_query_extension_reply_t *composite = xcb_get_extension_data(conn, &xcb_composite_id);
    const xcb_query_extension_reply_t *damage = xcb_get_extension_data(conn, &xcb_damage_id);
    const xcb_query_extension_reply_t *render = xcb_get_extension_data(conn, &xcb_render_id);
    const xcb_query_extension_reply_t *fixes = xcb_get_extension_data(conn, &xcb_xfixes_id);
    const xcb_query_extension_reply_t *shape = xcb_get_extension_data(conn, &xcb_shape_id);
    if (!composite || !composite->present || !damage || !damage->present || !render || !render->present || !fixes ||
        !fixes->present || !shape || !shape->present)
        return false;
    m_damageEvent = damage->first_event + XCB_DAMAGE_NOTIFY;
    m_shapeEvent = shape->first_event + XCB_SHAPE_NOTIFY;
    // each extension wants to be told the version spoken before anything else
    free(xcb_composite_query_version_reply(conn, xcb_composite_query_version(conn, 0, 4), nullptr));
    free(xcb_damage_query_version_reply(conn, xcb_damage_query_version(conn, 1, 1), nullptr));
    free(xcb_xfixes_query_version_reply(conn, xcb_xfixes_query_version(conn, 4, 0), nullptr));
    free(xcb_render_query_version_reply(conn, xcb_render_query_version(conn, 0, 11), nullptr));

    auto *setup = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    m_screen = QRect(0, 0, setup->width_in_pixels, setup->height_in_pixels);
    if (!findFormats(setup->root_visual))
        return false;

    // one compositing manager a screen
    const xcb_atom_t selection = internAtom(conn, "_NET_WM_CM_S0");
    if (auto *owner = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, selection), nullptr)) {
        const bool taken = owner->owner != 0;
        free(owner);
        if (taken)
            return false;
    }
    if (xcb_generic_error_t *err =
            xcb_request_check(conn, xcb_composite_redirect_subwindows_checked(conn, root, XCB_COMPOSITE_REDIRECT_MANUAL))) {
        free(err);
        return false;
    }
    m_owner = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, m_owner, root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0,
                      nullptr);
    xcb_set_selection_owner(conn, m_owner, selection, XCB_CURRENT_TIME);

    // The overlay: over every window, where the screen is drawn; the pointer goes through it
    auto *overlay = xcb_composite_get_overlay_window_reply(conn, xcb_composite_get_overlay_window(conn, root), nullptr);
    if (!overlay)
        return false;
    m_overlay = overlay->overlay_win;
    free(overlay);
    const xcb_xfixes_region_t none = xcb_generate_id(conn);
    xcb_xfixes_create_region(conn, none, 0, nullptr);
    xcb_xfixes_set_window_shape_region(conn, m_overlay, XCB_SHAPE_SK_INPUT, 0, 0, none);
    xcb_xfixes_destroy_region(conn, none);

    const uint32_t inferiors = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
    m_overlayPicture = xcb_generate_id(conn);
    xcb_render_create_picture(conn, m_overlayPicture, m_overlay, m_rootFormat, XCB_RENDER_CP_SUBWINDOW_MODE, &inferiors);
    const xcb_pixmap_t pixmap = xcb_generate_id(conn);
    m_depth = setup->root_depth;
    xcb_create_pixmap(conn, setup->root_depth, pixmap, root, uint16_t(m_screen.width()), uint16_t(m_screen.height()));
    m_buffer = xcb_generate_id(conn);
    xcb_render_create_picture(conn, m_buffer, pixmap, m_rootFormat, 0, nullptr);
    xcb_free_pixmap(conn, pixmap);   // the picture keeps it
    m_mirrorAtom = internAtom(conn, "_SNOWLEOPARD_MAGIC_MIRROR");
    m_black = xcb_generate_id(conn);
    xcb_render_create_solid_fill(conn, m_black, {0, 0, 0, 0xffff});
    m_damage = xcb_generate_id(conn);
    xcb_xfixes_create_region(conn, m_damage, 0, nullptr);
    m_scratch = xcb_generate_id(conn);
    xcb_xfixes_create_region(conn, m_scratch, 0, nullptr);

    m_timer.setSingleShot(true);
    m_timer.setInterval(4);   // what arrives together is drawn once
    connect(&m_timer, &QTimer::timeout, this, [this] { paint(); });
    m_animTimer.setInterval(16);
    connect(&m_animTimer, &QTimer::timeout, this, [this] { stepAnimations(); });
    damageAll();
    return true;
}

void Compositor::setBackground(xcb_pixmap_t pixmap)
{
    if (!m_conn || !m_buffer)
        return;
    if (m_background)
        xcb_render_free_picture(m_conn, m_background);
    m_background = 0;
    if (pixmap) {
        m_background = xcb_generate_id(m_conn);
        xcb_render_create_picture(m_conn, m_background, pixmap, m_rootFormat, 0, nullptr);
    }
    damageAll();
}

void Compositor::setOpacity(xcb_window_t frame, double opacity)
{
    if (opacity >= 1)
        m_opacity.remove(frame);
    else
        m_opacity.insert(frame, opacity);
    if (Win *w = m_windows.value(frame))
        damageWindow(w);
    else
        damageAll();
}

Compositor::Win *Compositor::add(xcb_window_t id)
{
    auto attrCookie = xcb_get_window_attributes(m_conn, id);
    auto geoCookie = xcb_get_geometry(m_conn, id);
    auto *attr = xcb_get_window_attributes_reply(m_conn, attrCookie, nullptr);
    auto *geo = xcb_get_geometry_reply(m_conn, geoCookie, nullptr);
    if (!attr || !geo) {
        free(attr);
        free(geo);
        return nullptr;
    }
    auto *w = new Win;
    w->id = id;
    w->geometry = QRect(geo->x + geo->border_width, geo->y + geo->border_width, geo->width, geo->height);
    w->mapped = attr->map_state == XCB_MAP_STATE_VIEWABLE;
    w->inputOnly = attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY;
    if (!w->inputOnly) {
        const xcb_render_pictformat_t format = m_visualFormats.value(attr->visual, m_rootFormat);
        w->alpha = m_formatAlpha.value(format);
        const uint32_t inferiors = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
        w->picture = xcb_generate_id(m_conn);
        xcb_render_create_picture(m_conn, w->picture, id, format, XCB_RENDER_CP_SUBWINDOW_MODE, &inferiors);
        w->damage = xcb_generate_id(m_conn);
        xcb_damage_create(m_conn, w->damage, id, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
        xcb_shape_select_input(m_conn, id, 1);
    }
    free(attr);
    free(geo);
    m_windows.insert(id, w);
    return w;
}

void Compositor::release(Win *w)
{
    // a destroyed window's picture and damage went with it
    if (!w->destroyed && !w->inputOnly) {
        xcb_render_free_picture(m_conn, w->picture);
        xcb_damage_destroy(m_conn, w->damage);
        xcb_shape_select_input(m_conn, w->id, 0);
    }
    if (w->shape)
        xcb_xfixes_destroy_region(m_conn, w->shape);
    dropShadow(w);
    m_windows.remove(w->id);
    delete w;
}

// The root's children, bottom to top: the windows to draw and their order
void Compositor::readTree()
{
    m_treeDirty = false;
    auto *tree = xcb_query_tree_reply(m_conn, xcb_query_tree(m_conn, m_root), nullptr);
    if (!tree)
        return;
    const xcb_window_t *children = xcb_query_tree_children(tree);
    const int count = xcb_query_tree_children_length(tree);
    QVector<Win *> order;
    order.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (children[i] == m_overlay || children[i] == m_owner)
            continue;
        Win *w = m_windows.value(children[i]);
        if (w && w->destroyed) {   // its number given again to a new window
            release(w);
            w = nullptr;
        }
        if (!w)
            w = add(children[i]);
        if (w)
            order.append(w);
    }
    free(tree);
    const QVector<Win *> old = m_order;
    for (Win *w : old)
        if (!order.contains(w))
            release(w);   // destroyed, or taken into a frame
    m_order = order;
}

int Compositor::shadowKind(Win *w)
{
    const int own = m_shadowFor ? m_shadowFor(w->id) : -1;
    if (own >= 0)
        return own;
    if (w->menu < 0) {
        w->menu = 0;
        static const xcb_atom_t type = internAtom(m_conn, "_NET_WM_WINDOW_TYPE");
        static const xcb_atom_t menus[] = {internAtom(m_conn, "_NET_WM_WINDOW_TYPE_MENU"),
                                           internAtom(m_conn, "_NET_WM_WINDOW_TYPE_POPUP_MENU"),
                                           internAtom(m_conn, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU"),
                                           internAtom(m_conn, "_NET_WM_WINDOW_TYPE_COMBO")};
        if (auto *r = xcb_get_property_reply(m_conn, xcb_get_property(m_conn, 0, w->id, type, XCB_ATOM_ATOM, 0, 16), nullptr)) {
            const auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(r));
            for (int i = 0; i < xcb_get_property_value_length(r) / 4; ++i)
                for (xcb_atom_t m : menus)
                    if (atoms[i] == m)
                        w->menu = 1;
            free(r);
        }
    }
    return w->menu > 0 ? PanelShadow : NoShadow;
}

void Compositor::dropShadow(Win *w)
{
    if (w->shadow)
        xcb_render_free_picture(m_conn, w->shadow);
    w->shadow = 0;
    w->shadowKind = NoShadow;
}

// The window's shadow (frame.cpp's, as 10.6's window server makes it) from its outline: a
// translucent window's alpha (a menu and its round corners, the Dock's menu and its arrow), the
// others' shape (a frame's corners)
void Compositor::makeShadow(Win *w, int kind)
{
    dropShadow(w);
    const int ww = w->geometry.width(), wh = w->geometry.height();
    std::vector<uint8_t> outline(size_t(ww) * size_t(wh), 0);
    auto fill = [&](int x, int y, int width, int height) {
        const int x0 = qMax(0, x), y0 = qMax(0, y), x1 = qMin(ww, x + width), y1 = qMin(wh, y + height);
        for (int row = y0; row < y1; ++row)
            if (x1 > x0)
                memset(outline.data() + size_t(row) * size_t(ww) + size_t(x0), 255, size_t(x1 - x0));
    };
    bool known = false;
    if (w->alpha && ww * wh <= 1 << 20) {
        auto *image = xcb_get_image_reply(
            m_conn, xcb_get_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, w->id, 0, 0, uint16_t(ww), uint16_t(wh), ~0u), nullptr);
        if (image && image->depth == 32 && xcb_get_image_data_length(image) >= ww * wh * 4) {
            const uint8_t *pixels = xcb_get_image_data(image);
            for (size_t i = 0; i < size_t(ww) * size_t(wh); ++i)
                outline[i] = pixels[i * 4 + 3];
            known = true;
        }
        free(image);
    } else if (auto *shape = xcb_shape_get_rectangles_reply(
                   m_conn, xcb_shape_get_rectangles(m_conn, w->id, XCB_SHAPE_SK_BOUNDING), nullptr)) {
        const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(shape);
        for (int i = 0; i < xcb_shape_get_rectangles_rectangles_length(shape); ++i)
            fill(rects[i].x, rects[i].y, rects[i].width, rects[i].height);
        known = xcb_shape_get_rectangles_rectangles_length(shape) > 0;
        free(shape);
    }
    if (!known)
        fill(0, 0, ww, wh);
    QPoint origin;
    const QImage mask = Frame::shadowMask(outline, ww, wh, kind, &origin);
    if (mask.isNull())
        return;
    const int sw = mask.width(), sh = mask.height(), stride = int(mask.bytesPerLine());

    const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
    xcb_create_pixmap(m_conn, 8, pixmap, m_root, uint16_t(sw), uint16_t(sh));
    const xcb_gcontext_t gc = xcb_generate_id(m_conn);
    xcb_create_gc(m_conn, gc, pixmap, 0, nullptr);
    // sent a few rows at a time (a request has a longest length)
    const int rowsAtOnce = qMax(1, 60000 / stride);
    for (int y = 0; y < sh; y += rowsAtOnce) {
        const int n = qMin(rowsAtOnce, sh - y);
        xcb_put_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, uint16_t(sw), uint16_t(n), 0, int16_t(y), 0, 8,
                      uint32_t(stride * n), mask.constScanLine(y));
    }
    xcb_free_gc(m_conn, gc);
    w->shadow = xcb_generate_id(m_conn);
    xcb_render_create_picture(m_conn, w->shadow, pixmap, m_a8, 0, nullptr);
    xcb_free_pixmap(m_conn, pixmap);   // the picture keeps it
    w->shadowKind = kind;
    w->shadowRect = QRect(origin, mask.size());
}

void Compositor::damageRect(QRect r)
{
    r &= m_screen;
    if (r.isEmpty())
        return;
    const xcb_rectangle_t rect = toXcb(r);
    xcb_xfixes_set_region(m_conn, m_scratch, 1, &rect);
    xcb_xfixes_union_region(m_conn, m_damage, m_scratch, m_damage);
    schedule();
}

// the window and its shadow
void Compositor::damageWindow(const Win *w) { damageRect(w->geometry.adjusted(-ShadowReach, -ShadowReach, ShadowReach, ShadowReach)); }

void Compositor::damageAll()
{
    m_all = true;
    schedule();
}

void Compositor::schedule()
{
    if (m_conn && !m_timer.isActive())
        m_timer.start();
}

void Compositor::handleEvent(xcb_generic_event_t *ev)
{
    if (!m_overlay)
        return;
    const int type = ev->response_type & ~0x80;
    if (type == m_damageEvent) {
        auto *e = reinterpret_cast<xcb_damage_notify_event_t *>(ev);
        if (Win *w = m_windows.value(e->drawable)) {
            // what changed, taken from the window's damage and moved to the screen's coordinates
            xcb_damage_subtract(m_conn, w->damage, XCB_NONE, m_scratch);
            xcb_xfixes_translate_region(m_conn, m_scratch, int16_t(w->geometry.x()), int16_t(w->geometry.y()));
            xcb_xfixes_union_region(m_conn, m_damage, m_scratch, m_damage);
            // a translucent menu's shadow is its outline's: again once it has drawn itself
            if (w->alpha && w->menu > 0 && w->mapped) {
                dropShadow(w);
                damageWindow(w);
            }
            schedule();
        }
        return;
    }
    if (type == XCB_PROPERTY_NOTIFY) {
        auto *e = reinterpret_cast<xcb_property_notify_event_t *>(ev);
        if (Win *w = m_windows.value(e->window); w && e->atom == m_mirrorAtom) {
            damageMirror(w);   // where it was
            w->mirror = -1;
            readMirror(w);
            damageMirror(w);
            schedule();
        }
        return;
    }
    if (type == m_shapeEvent) {
        auto *e = reinterpret_cast<xcb_shape_notify_event_t *>(ev);
        if (Win *w = m_windows.value(e->affected_window)) {
            if (w->shape)
                xcb_xfixes_destroy_region(m_conn, w->shape);
            w->shape = 0;
            damageWindow(w);
        }
        return;
    }
    switch (type) {
    case XCB_CREATE_NOTIFY:
        if (reinterpret_cast<xcb_create_notify_event_t *>(ev)->parent == m_root)
            m_treeDirty = true;
        break;
    case XCB_DESTROY_NOTIFY: {
        auto *e = reinterpret_cast<xcb_destroy_notify_event_t *>(ev);
        if (Win *w = m_windows.value(e->window)) {
            w->destroyed = true;
            m_treeDirty = true;
            if (w->mapped)
                damageWindow(w);
            schedule();
        }
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        auto *e = reinterpret_cast<xcb_reparent_notify_event_t *>(ev);
        if (e->event == m_root) {
            m_treeDirty = true;
            if (Win *w = m_windows.value(e->window); w && w->mapped)
                damageWindow(w);
            schedule();
        }
        break;
    }
    case XCB_CIRCULATE_NOTIFY:
        if (reinterpret_cast<xcb_circulate_notify_event_t *>(ev)->event == m_root)
            damageAll(), m_treeDirty = true;
        break;
    case XCB_MAP_NOTIFY: {
        auto *e = reinterpret_cast<xcb_map_notify_event_t *>(ev);
        if (e->event != m_root)
            break;
        m_treeDirty = true;   // a window made and shown at once may not be known yet
        if (Win *w = m_windows.value(e->window)) {
            w->mapped = true;
            w->menu = -1;     // its type is set by now
            damageWindow(w);
        }
        schedule();
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        auto *e = reinterpret_cast<xcb_unmap_notify_event_t *>(ev);
        if (e->event != m_root)
            break;
        if (Win *w = m_windows.value(e->window); w && w->mapped) {
            w->mapped = false;
            damageWindow(w);
        }
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        auto *e = reinterpret_cast<xcb_configure_notify_event_t *>(ev);
        if (e->event != m_root)
            break;
        m_treeDirty = true;   // its place among the others may have changed
        if (Win *w = m_windows.value(e->window)) {
            const QRect now(e->x + e->border_width, e->y + e->border_width, e->width, e->height);
            if (w->mapped)
                damageWindow(w);
            if (now.size() != w->geometry.size()) {
                dropShadow(w);
                if (w->shape)
                    xcb_xfixes_destroy_region(m_conn, w->shape);
                w->shape = 0;
            }
            w->geometry = now;
            if (w->mapped)
                damageWindow(w);
        }
        schedule();
        break;
    }
    default:
        break;
    }
}

void Compositor::paint()
{
    if (m_treeDirty) {
        const QVector<Win *> before = m_order;
        readTree();
        if (before != m_order)
            m_all = true;
    }
    if (m_all) {
        const xcb_rectangle_t all = toXcb(m_screen);
        xcb_xfixes_set_region(m_conn, m_damage, 1, &all);
        m_all = false;
    }
    // A mirror shows what is beside it: drawn again when that changes, and that drawn again with it
    // (outside the damage the buffer still has what was over it, the Dock's own icons)
    bool anyMirror = false;
    for (Win *w : std::as_const(m_order))
        if (w->mapped && !w->destroyed && (readMirror(w), w->mirror > 0))
            anyMirror = true;
    if (anyMirror) {
        QRect damaged;
        if (auto *r = xcb_xfixes_fetch_region_reply(m_conn, xcb_xfixes_fetch_region(m_conn, m_damage), nullptr)) {
            damaged = QRect(r->extents.x, r->extents.y, r->extents.width, r->extents.height);
            free(r);
        }
        for (Win *w : std::as_const(m_order)) {
            if (!w->mapped || w->destroyed || w->mirror <= 0)
                continue;
            const QRect target = mirrorTarget(w), source = mirrorSource(w);
            if (damaged.intersects(target) || damaged.intersects(source))
                damageMirror(w);
        }
    }
    const uint16_t sw = uint16_t(m_screen.width()), sh = uint16_t(m_screen.height());
    xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_damage, 0, 0);
    const xcb_rectangle_t all = toXcb(m_screen);
    if (m_background)
        xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_SRC, m_background, XCB_NONE, m_buffer, 0, 0, 0, 0, 0, 0, sw, sh);
    else
        xcb_render_fill_rectangles(m_conn, XCB_RENDER_PICT_OP_SRC, m_buffer, {0, 0, 0, 0xffff}, 1, &all);
    for (Win *w : std::as_const(m_order)) {
        if (!w->mapped || w->inputOnly || w->destroyed)
            continue;
        // a window coming out of the Dock: its picture is drawn instead, warped
        if (std::any_of(m_animations.cbegin(), m_animations.cend(), [w](const Animation &a) { return a.frame == w->id; }))
            continue;
        const QRect g = w->geometry.translated(m_offsets.value(w->id));
        if (!g.intersects(m_screen.adjusted(-ShadowReach, -ShadowReach, ShadowReach, ShadowReach)))
            continue;
        const int kind = shadowKind(w);
        if (kind != w->shadowKind) {
            if (kind == NoShadow)
                dropShadow(w);
            else
                makeShadow(w, kind);
        }
        if (w->shadow) {
            // the part of it on the screen (past the screen's left or top edge the server draws none of it)
            const QRect whole = w->shadowRect.translated(g.topLeft());
            const QRect seen = whole & m_screen;
            if (!seen.isEmpty())
                xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_OVER, m_black, w->shadow, m_buffer, 0, 0,
                                     int16_t(seen.x() - whole.x()), int16_t(seen.y() - whole.y()), int16_t(seen.x()),
                                     int16_t(seen.y()), uint16_t(seen.width()), uint16_t(seen.height()));
        }
        if (w->mirror > 0)
            drawMirror(w);
        // the window inside its outline (a frame's round corners): outside it its pixels mean nothing
        if (!w->shape) {
            w->shape = xcb_generate_id(m_conn);
            xcb_xfixes_create_region_from_window(m_conn, w->shape, w->id, XCB_SHAPE_SK_BOUNDING);
        }
        xcb_xfixes_copy_region(m_conn, w->shape, m_scratch);
        xcb_xfixes_translate_region(m_conn, m_scratch, int16_t(g.x()), int16_t(g.y()));
        xcb_xfixes_intersect_region(m_conn, m_scratch, m_damage, m_scratch);
        xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_scratch, 0, 0);
        // the window's pixels through a solid alpha
        const xcb_render_picture_t mask = opacityMask(m_opacity.value(w->id, 1.0));
        xcb_render_composite(m_conn, w->alpha || mask ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC, w->picture, mask, m_buffer, 0,
                             0, 0, 0, int16_t(g.x()), int16_t(g.y()), uint16_t(g.width()), uint16_t(g.height()));
        xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_damage, 0, 0);
    }
    // the windows going into the Dock or coming out of it, over the others
    for (Animation &a : m_animations)
        if (auto it = m_snapshots.constFind(a.key); it != m_snapshots.constEnd())
            a.drawn = drawAnimation(a, *it);
    // Spaces' bezel over everything
    if (m_overlayImage)
        xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_OVER, m_overlayImage, opacityMask(m_overlayOpacity), m_buffer, 0, 0, 0, 0,
                             int16_t(m_overlayRect.x()), int16_t(m_overlayRect.y()), uint16_t(m_overlayRect.width()),
                             uint16_t(m_overlayRect.height()));
    xcb_xfixes_set_picture_clip_region(m_conn, m_overlayPicture, m_damage, 0, 0);
    xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_SRC, m_buffer, XCB_NONE, m_overlayPicture, 0, 0, 0, 0, 0, 0, sw, sh);
    xcb_xfixes_set_region(m_conn, m_damage, 0, nullptr);
    xcb_flush(m_conn);
}

xcb_render_picture_t Compositor::opacityMask(double opacity)
{
    if (opacity >= 1)
        return XCB_NONE;
    const int level = qBound(0, int(std::lround(opacity * 255)), 255);
    xcb_render_picture_t mask = m_opacityMasks.value(level);
    if (!mask) {
        const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
        xcb_create_pixmap(m_conn, 8, pixmap, m_root, 1, 1);
        mask = xcb_generate_id(m_conn);
        const uint32_t repeat = 1;
        xcb_render_create_picture(m_conn, mask, pixmap, m_a8, XCB_RENDER_CP_REPEAT, &repeat);
        const xcb_render_color_t color = {0, 0, 0, uint16_t(level * 257)};
        const xcb_rectangle_t one = {0, 0, 1, 1};
        xcb_render_fill_rectangles(m_conn, XCB_RENDER_PICT_OP_SRC, mask, color, 1, &one);
        xcb_free_pixmap(m_conn, pixmap);
        m_opacityMasks.insert(level, mask);
    }
    return mask;
}

// ---- Spaces: the windows slid aside, the bezel over them

void Compositor::setOffset(xcb_window_t frame, QPoint offset)
{
    if (offset.isNull())
        m_offsets.remove(frame);
    else
        m_offsets.insert(frame, offset);
    damageAll();
}

void Compositor::clearOffsets()
{
    m_offsets.clear();
    damageAll();
}

void Compositor::setOverlay(const QImage &image, QPoint at, double opacity)
{
    if (!running())
        return;
    damageRect(m_overlayRect);
    if (image.isNull()) {
        if (m_overlayImage)
            xcb_render_free_picture(m_conn, m_overlayImage);
        m_overlayImage = 0;
        m_overlayRect = QRect();
        return;
    }
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!m_overlayImage || m_overlayRect.size() != argb.size() || image.cacheKey() != m_overlayKey) {
        if (m_overlayImage)
            xcb_render_free_picture(m_conn, m_overlayImage);
        const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
        xcb_create_pixmap(m_conn, 32, pixmap, m_root, uint16_t(argb.width()), uint16_t(argb.height()));
        const xcb_gcontext_t gc = xcb_generate_id(m_conn);
        xcb_create_gc(m_conn, gc, pixmap, 0, nullptr);
        xcb_put_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, uint16_t(argb.width()), uint16_t(argb.height()), 0, 0, 0, 32,
                      uint32_t(argb.sizeInBytes()), argb.constBits());
        xcb_free_gc(m_conn, gc);
        m_overlayImage = xcb_generate_id(m_conn);
        xcb_render_create_picture(m_conn, m_overlayImage, pixmap, m_argb32, 0, nullptr);
        xcb_free_pixmap(m_conn, pixmap);
        m_overlayKey = image.cacheKey();
    }
    m_overlayRect = QRect(at, argb.size());
    m_overlayOpacity = opacity;
    damageRect(m_overlayRect);
}

// ---- Minimizing: 10.6's Dock warps the window into its tile (docs/reverse-status.md, the Dock)

namespace {

using Frame::GenieRow;

xcb_render_fixed_t fixed(double v) { return xcb_render_fixed_t(std::lround(v * 65536)); }

xcb_render_transform_t transform(double sx, double sy, double tx, double ty)
{
    return {fixed(sx), 0, fixed(tx), 0, fixed(sy), fixed(ty), 0, 0, fixed(1)};
}

} // namespace

QImage Compositor::snapshot(xcb_window_t key, xcb_window_t frame)
{
    dropSnapshot(key);
    Win *w = m_windows.value(frame);
    if (!running() || !w || !w->mapped || w->inputOnly || !m_argb32 || w->geometry.isEmpty())
        return {};
    const QSize size = w->geometry.size();
    const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
    xcb_create_pixmap(m_conn, 32, pixmap, m_root, uint16_t(size.width()), uint16_t(size.height()));
    Snapshot s;
    s.size = size;
    s.picture = xcb_generate_id(m_conn);
    xcb_render_create_picture(m_conn, s.picture, pixmap, m_argb32, 0, nullptr);
    const xcb_rectangle_t all = {0, 0, uint16_t(size.width()), uint16_t(size.height())};
    xcb_render_fill_rectangles(m_conn, XCB_RENDER_PICT_OP_SRC, s.picture, {0, 0, 0, 0}, 1, &all);
    // inside its outline (the frame's round corners)
    const xcb_xfixes_region_t outline = xcb_generate_id(m_conn);
    xcb_xfixes_create_region_from_window(m_conn, outline, frame, XCB_SHAPE_SK_BOUNDING);
    xcb_xfixes_set_picture_clip_region(m_conn, s.picture, outline, 0, 0);
    xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_SRC, w->picture, XCB_NONE, s.picture, 0, 0, 0, 0, 0, 0, all.width, all.height);
    xcb_xfixes_set_picture_clip_region(m_conn, s.picture, XCB_NONE, 0, 0);
    xcb_xfixes_destroy_region(m_conn, outline);
    xcb_render_set_picture_filter(m_conn, s.picture, 8, "bilinear", 0, nullptr);
    QImage image;
    if (auto *r = xcb_get_image_reply(m_conn, xcb_get_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0, all.width, all.height, ~0u),
                                      nullptr)) {
        if (xcb_get_image_data_length(r) >= size.width() * size.height() * 4)
            image = QImage(xcb_get_image_data(r), size.width(), size.height(), size.width() * 4, QImage::Format_ARGB32_Premultiplied).copy();
        free(r);
    }
    xcb_free_pixmap(m_conn, pixmap);   // the picture keeps it
    m_snapshots.insert(key, s);
    return image;
}

QImage Compositor::capture(xcb_window_t frame, bool shadow, QPoint *offset)
{
    Win *w = m_windows.value(frame);
    if (!running() || !w || !w->mapped || w->inputOnly || !m_argb32 || w->geometry.isEmpty())
        return {};
    if (shadow) {
        const int kind = shadowKind(w);
        if (kind != w->shadowKind && kind != NoShadow)
            makeShadow(w, kind);
    }
    const QSize size = w->geometry.size();
    QRect bounds(QPoint(0, 0), size);
    if (shadow && w->shadow)
        bounds |= w->shadowRect;
    const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
    xcb_create_pixmap(m_conn, 32, pixmap, m_root, uint16_t(bounds.width()), uint16_t(bounds.height()));
    const xcb_render_picture_t picture = xcb_generate_id(m_conn);
    xcb_render_create_picture(m_conn, picture, pixmap, m_argb32, 0, nullptr);
    const xcb_rectangle_t all = {0, 0, uint16_t(bounds.width()), uint16_t(bounds.height())};
    xcb_render_fill_rectangles(m_conn, XCB_RENDER_PICT_OP_SRC, picture, {0, 0, 0, 0}, 1, &all);
    const int16_t x = int16_t(-bounds.x()), y = int16_t(-bounds.y());
    if (shadow && w->shadow)   // as the screen has it: black through the shadow's mask
        xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_OVER, m_black, w->shadow, picture, 0, 0, 0, 0,
                             int16_t(x + w->shadowRect.x()), int16_t(y + w->shadowRect.y()), uint16_t(w->shadowRect.width()),
                             uint16_t(w->shadowRect.height()));
    // the window inside its outline (the frame's round corners), over its shadow
    const xcb_xfixes_region_t outline = xcb_generate_id(m_conn);
    xcb_xfixes_create_region_from_window(m_conn, outline, frame, XCB_SHAPE_SK_BOUNDING);
    xcb_xfixes_translate_region(m_conn, outline, x, y);
    xcb_xfixes_set_picture_clip_region(m_conn, picture, outline, 0, 0);
    xcb_render_composite(m_conn, w->alpha ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC, w->picture, XCB_NONE, picture, 0, 0, 0, 0,
                         x, y, uint16_t(size.width()), uint16_t(size.height()));
    xcb_xfixes_destroy_region(m_conn, outline);
    QImage image;
    if (auto *r = xcb_get_image_reply(m_conn, xcb_get_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0, all.width, all.height, ~0u),
                                      nullptr)) {
        if (xcb_get_image_data_length(r) >= bounds.width() * bounds.height() * 4)
            image = QImage(xcb_get_image_data(r), bounds.width(), bounds.height(), bounds.width() * 4, QImage::Format_ARGB32_Premultiplied)
                        .copy();
        free(r);
    }
    xcb_render_free_picture(m_conn, picture);
    xcb_free_pixmap(m_conn, pixmap);
    if (offset)
        *offset = QPoint(x, y);
    return image;
}

void Compositor::dropSnapshot(xcb_window_t key)
{
    for (int i = m_animations.size() - 1; i >= 0; --i)
        if (m_animations[i].key == key) {
            damageRect(m_animations[i].drawn);
            m_animations.removeAt(i);
        }
    auto it = m_snapshots.find(key);
    if (it == m_snapshots.end())
        return;
    xcb_render_free_picture(m_conn, it->picture);
    m_snapshots.erase(it);
}

void Compositor::animate(xcb_window_t key, xcb_window_t frame, const QRect &window, const QRect &target, Effect effect, bool minimizing,
                         double seconds, bool waitForTarget)
{
    if (!running() || !m_snapshots.contains(key))
        return;
    for (int i = m_animations.size() - 1; i >= 0; --i)
        if (m_animations[i].key == key) {
            damageRect(m_animations[i].drawn);
            m_animations.removeAt(i);
        }
    Animation a;
    a.key = key;
    a.frame = frame;
    a.window = window;
    a.target = target;
    a.effect = effect;
    a.minimizing = minimizing;
    a.seconds = seconds;
    a.started = !(minimizing && waitForTarget);
    a.clock.start();
    m_animations.append(a);
    if (!m_animTimer.isActive())
        m_animTimer.start();
    stepAnimations();
}

void Compositor::retarget(xcb_window_t key, const QRect &target)
{
    for (Animation &a : m_animations)
        if (a.key == key && !target.isEmpty()) {
            a.target = target;
            if (!a.started) {
                a.started = true;
                a.clock.restart();
            }
        }
}

void Compositor::stepAnimations()
{
    for (int i = m_animations.size() - 1; i >= 0; --i) {
        Animation &a = m_animations[i];
        if (!a.started && a.clock.elapsed() >= 150) {   // the Dock didn't answer: where it said before
            a.started = true;
            a.clock.restart();
        }
        damageRect(a.window.united(a.target).united(a.drawn).adjusted(-2, -2, 2, 2));
        if (a.started && a.clock.elapsed() >= a.seconds * 1000) {
            const xcb_window_t key = a.key;
            const bool restored = !a.minimizing;
            m_animations.removeAt(i);
            if (restored)   // out of the Dock: the window itself from now on
                dropSnapshot(key);
        }
    }
    if (m_animations.isEmpty())
        m_animTimer.stop();
    schedule();
}

QRect Compositor::drawAnimation(const Animation &a, const Snapshot &s)
{
    double t = a.started ? std::clamp(a.clock.elapsed() / (a.seconds * 1000), 0.0, 1.0) : 0;
    const double p = a.minimizing ? t : 1 - t;
    const double w = s.size.width(), h = s.size.height();
    const QRectF win(a.window.topLeft(), QSizeF(w, h));
    const QRectF tile(a.target);
    QRect drawn;
    auto put = [&](double x0, double x1, int y, int rows, double sy, double ty) {
        const double dw = x1 - x0;
        if (dw < 0.5 || rows <= 0)
            return;
        const int X0 = int(std::floor(x0)), W = int(std::ceil(x1)) - X0;
        const xcb_render_transform_t m = transform(w / dw, sy, (X0 - x0) * w / dw, ty);
        xcb_render_set_picture_transform(m_conn, s.picture, m);
        xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_OVER, s.picture, XCB_NONE, m_buffer, 0, 0, 0, 0, int16_t(X0), int16_t(y),
                             uint16_t(W), uint16_t(rows));
        drawn |= QRect(X0, y, W, rows);
    };
    if (a.effect == Scale) {
        // setSystemFrame:from: the frame goes to the tile's
        const QRectF r(win.x() + (tile.x() - win.x()) * p, win.y() + (tile.y() - win.y()) * p, w + (tile.width() - w) * p,
                       h + (tile.height() - h) * p);
        if (r.height() >= 0.5) {
            const int Y0 = int(std::floor(r.y())), H = int(std::ceil(r.bottom())) - Y0;
            put(r.x(), r.right(), Y0, H, h / r.height(), (Y0 - r.y()) * h / r.height());
        }
    } else {
        const QVector<GenieRow> rows = Frame::genieRows(win, tile, p);
        // the mesh's bands, two screen rows at a time, each scaled across only
        for (int i = 0; i + 1 < rows.size(); ++i) {
            const GenieRow &r0 = rows[i], &r1 = rows[i + 1];
            const int y0 = int(std::lround(r0.y)), y1 = int(std::lround(r1.y));
            for (int y = y0; y < y1; y += 2) {
                const int n = std::min(2, y1 - y);
                const double f = ((y + n * 0.5) - r0.y) / std::max(1e-6, r1.y - r0.y);
                const double l = r0.left + (r1.left - r0.left) * f, r = r0.right + (r1.right - r0.right) * f;
                put(win.x() + l, win.x() + w + r, y, n, 1, r0.local + (y - r0.y));
            }
        }
    }
    const xcb_render_transform_t identity = transform(1, 1, 0, 0);
    xcb_render_set_picture_transform(m_conn, s.picture, identity);
    return drawn;
}

// ---- the magic mirror

// CGSSetMagicMirror(scale, connection, region, position, level, order, window): the Dock gives its
// floor's quadrangle (DOCKFloorLayer's _pt1 to _pt4, CGSQuadrangleToRegion), position 1 for a Dock
// at the bottom, 3 at the left, 4 at the right, scale 1. The window server (_adjustMagicMirror) then
// makes a window named "MagicMirror" just under the Dock, opacity 1, clipped to the region, that
// shows the windows under it (_CGXUpdateMagicMirror redraws them in a framebuffer of their own)
// through scale(1, -1/scale) about the region's top edge for the bottom (its bottom edge for the
// top, its right one for the left, its left one for the right): what is just over the floor,
// turned over onto it. The Dock's glass then goes over that.
void Compositor::readMirror(Win *w)
{
    if (w->mirror >= 0)
        return;
    w->mirror = 0;
    w->mirrorQuad.clear();
    auto *r = xcb_get_property_reply(m_conn, xcb_get_property(m_conn, 0, w->id, m_mirrorAtom, XCB_ATOM_CARDINAL, 0, 9), nullptr);
    if (!r)
        return;
    if (r->format == 32 && xcb_get_property_value_length(r) >= 9 * 4) {
        const auto *v = static_cast<const int32_t *>(xcb_get_property_value(r));
        if (v[0] >= 1 && v[0] <= 4) {
            w->mirror = v[0];
            for (int i = 0; i < 4; ++i)
                w->mirrorQuad << QPoint(v[1 + 2 * i], v[2 + 2 * i]);
        }
    }
    free(r);
}

QRect Compositor::mirrorTarget(const Win *w) const
{
    return w->mirrorQuad.boundingRect().translated(w->geometry.topLeft()) & m_screen;
}

QRect Compositor::mirrorSource(const Win *w) const
{
    const QRect q = w->mirrorQuad.boundingRect().translated(w->geometry.topLeft());
    switch (w->mirror) {
    case 1: return q.translated(0, -q.height()) & m_screen;
    case 2: return q.translated(0, q.height()) & m_screen;
    case 3: return q.translated(q.width(), 0) & m_screen;
    case 4: return q.translated(-q.width(), 0) & m_screen;
    default: return {};
    }
}

void Compositor::damageMirror(const Win *w)
{
    if (w->mirror <= 0 || !w->mapped)
        return;
    damageRect(mirrorTarget(w));
    damageRect(mirrorSource(w));
}

void Compositor::drawMirror(const Win *w)
{
    const QRect target = mirrorTarget(w), source = mirrorSource(w);
    if (target.isEmpty() || source.isEmpty())
        return;
    const QRect q = w->mirrorQuad.boundingRect().translated(w->geometry.topLeft());
    // what is beside it, copied first (the buffer isn't read and written at once)
    const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
    xcb_create_pixmap(m_conn, m_depth, pixmap, m_root, uint16_t(source.width()), uint16_t(source.height()));
    const xcb_render_picture_t copy = xcb_generate_id(m_conn);
    xcb_render_create_picture(m_conn, copy, pixmap, m_rootFormat, 0, nullptr);
    xcb_free_pixmap(m_conn, pixmap);
    xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_SRC, m_buffer, XCB_NONE, copy, int16_t(source.x()), int16_t(source.y()), 0, 0, 0, 0,
                         uint16_t(source.width()), uint16_t(source.height()));
    // Render reads the source at T(destination): the reflection about the edge, into the copy
    auto fixed = [](double v) { return xcb_render_fixed_t(std::lround(v * 65536)); };
    xcb_render_transform_t t = {fixed(1), 0, fixed(-source.x()), 0, fixed(1), fixed(-source.y()), 0, 0, fixed(1)};
    switch (w->mirror) {
    case 1: t.matrix22 = fixed(-1), t.matrix23 = fixed(2.0 * q.top() - source.y()); break;
    case 2: t.matrix22 = fixed(-1), t.matrix23 = fixed(2.0 * (q.top() + q.height()) - source.y()); break;
    case 3: t.matrix11 = fixed(-1), t.matrix13 = fixed(2.0 * (q.left() + q.width()) - source.x()); break;
    case 4: t.matrix11 = fixed(-1), t.matrix13 = fixed(2.0 * q.left() - source.x()); break;
    }
    xcb_render_set_picture_transform(m_conn, copy, t);
    // clipped to the quadrangle (CGSQuadrangleToRegion), the screen and what is drawn now
    const QRegion quad = QRegion(w->mirrorQuad.translated(w->geometry.topLeft())) & QRegion(m_screen);
    std::vector<xcb_rectangle_t> rects;
    for (const QRect &r : quad)
        rects.push_back(toXcb(r));
    xcb_xfixes_set_region(m_conn, m_scratch, uint32_t(rects.size()), rects.data());
    xcb_xfixes_intersect_region(m_conn, m_scratch, m_damage, m_scratch);
    xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_scratch, 0, 0);
    xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_SRC, copy, XCB_NONE, m_buffer, int16_t(target.x()), int16_t(target.y()), 0, 0,
                         int16_t(target.x()), int16_t(target.y()), uint16_t(target.width()), uint16_t(target.height()));
    xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_damage, 0, 0);
    xcb_render_free_picture(m_conn, copy);
}
