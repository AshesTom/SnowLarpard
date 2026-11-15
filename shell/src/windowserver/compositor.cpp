// SPDX-License-Identifier: LGPL-2.1-or-later
#include "compositor.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/shape.h>

namespace {

// AppKit's __NSSetWindowShadow (CGSSetWindowShadowAndRimParametersWithStretch): the standard
// deviation, the density, how far under the window. The key window's, another window's, and the
// other style's (panels, menus).
struct ShadowParams {
    double sigma, density, dy;
};
const ShadowParams shadowParams[] = {{0, 0, 0}, {20, 0.49, 15}, {10, 0.44, 10}, {8, 0.5, 6}};

// the room any shadow takes around its window: what moves with it
constexpr int ShadowReach = 70;

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
    xcb_create_pixmap(conn, setup->root_depth, pixmap, root, uint16_t(m_screen.width()), uint16_t(m_screen.height()));
    m_buffer = xcb_generate_id(conn);
    xcb_render_create_picture(conn, m_buffer, pixmap, m_rootFormat, 0, nullptr);
    xcb_free_pixmap(conn, pixmap);   // the picture keeps it
    m_black = xcb_generate_id(conn);
    xcb_render_create_solid_fill(conn, m_black, {0, 0, 0, 0xffff});
    m_damage = xcb_generate_id(conn);
    xcb_xfixes_create_region(conn, m_damage, 0, nullptr);
    m_scratch = xcb_generate_id(conn);
    xcb_xfixes_create_region(conn, m_scratch, 0, nullptr);

    m_timer.setSingleShot(true);
    m_timer.setInterval(4);   // what arrives together is drawn once
    connect(&m_timer, &QTimer::timeout, this, [this] { paint(); });
    damageAll();
    return true;
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

// A blurred rectangle the window's size, a little under it: the product of its blurred edges
// across and down, as an alpha mask
void Compositor::makeShadow(Win *w, int kind)
{
    dropShadow(w);
    const ShadowParams &p = shadowParams[kind];
    const int reach = int(std::ceil(2.5 * p.sigma));
    const int left = reach, top = qMax(0, reach - int(p.dy)), bottom = reach + int(p.dy);
    const int ww = w->geometry.width(), wh = w->geometry.height();
    const int sw = ww + 2 * left, sh = wh + top + bottom;
    auto blurred = [&p](int n, double from, double to) {
        std::vector<double> v(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double x = i + 0.5;
            v[size_t(i)] = 0.5 * (std::erf((x - from) / (p.sigma * M_SQRT2)) - std::erf((x - to) / (p.sigma * M_SQRT2)));
        }
        return v;
    };
    const std::vector<double> across = blurred(sw, left, left + ww);
    const std::vector<double> down = blurred(sh, top + p.dy, top + p.dy + wh);

    const xcb_pixmap_t pixmap = xcb_generate_id(m_conn);
    xcb_create_pixmap(m_conn, 8, pixmap, m_root, uint16_t(sw), uint16_t(sh));
    const xcb_gcontext_t gc = xcb_generate_id(m_conn);
    xcb_create_gc(m_conn, gc, pixmap, 0, nullptr);
    // rows of bytes padded to 32 bits, sent a few at a time (a request has a longest length)
    const int stride = (sw + 3) & ~3;
    const int rowsAtOnce = qMax(1, 60000 / stride);
    std::vector<uint8_t> rows(size_t(stride) * size_t(rowsAtOnce));
    for (int y = 0; y < sh; y += rowsAtOnce) {
        const int n = qMin(rowsAtOnce, sh - y);
        for (int r = 0; r < n; ++r) {
            uint8_t *row = rows.data() + size_t(r) * size_t(stride);
            const double d = 255 * p.density * down[size_t(y + r)];
            for (int x = 0; x < sw; ++x)
                row[x] = uint8_t(std::lround(d * across[size_t(x)]));
        }
        xcb_put_image(m_conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, uint16_t(sw), uint16_t(n), 0, int16_t(y), 0, 8,
                      uint32_t(stride * n), rows.data());
    }
    xcb_free_gc(m_conn, gc);
    w->shadow = xcb_generate_id(m_conn);
    xcb_render_create_picture(m_conn, w->shadow, pixmap, m_a8, 0, nullptr);
    xcb_free_pixmap(m_conn, pixmap);   // the picture keeps it
    w->shadowKind = kind;
    w->shadowRect = QRect(-left, -top, sw, sh);
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
    const uint16_t sw = uint16_t(m_screen.width()), sh = uint16_t(m_screen.height());
    xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_damage, 0, 0);
    const xcb_rectangle_t all = toXcb(m_screen);
    xcb_render_fill_rectangles(m_conn, XCB_RENDER_PICT_OP_SRC, m_buffer, {0, 0, 0, 0xffff}, 1, &all);
    for (Win *w : std::as_const(m_order)) {
        if (!w->mapped || w->inputOnly || w->destroyed)
            continue;
        const QRect &g = w->geometry;
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
        // the window inside its outline (a frame's round corners): outside it its pixels mean nothing
        if (!w->shape) {
            w->shape = xcb_generate_id(m_conn);
            xcb_xfixes_create_region_from_window(m_conn, w->shape, w->id, XCB_SHAPE_SK_BOUNDING);
        }
        xcb_xfixes_copy_region(m_conn, w->shape, m_scratch);
        xcb_xfixes_translate_region(m_conn, m_scratch, int16_t(g.x()), int16_t(g.y()));
        xcb_xfixes_intersect_region(m_conn, m_scratch, m_damage, m_scratch);
        xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_scratch, 0, 0);
        xcb_render_composite(m_conn, w->alpha ? XCB_RENDER_PICT_OP_OVER : XCB_RENDER_PICT_OP_SRC, w->picture, XCB_NONE, m_buffer, 0,
                             0, 0, 0, int16_t(g.x()), int16_t(g.y()), uint16_t(g.width()), uint16_t(g.height()));
        xcb_xfixes_set_picture_clip_region(m_conn, m_buffer, m_damage, 0, 0);
    }
    xcb_xfixes_set_picture_clip_region(m_conn, m_overlayPicture, m_damage, 0, 0);
    xcb_render_composite(m_conn, XCB_RENDER_PICT_OP_SRC, m_buffer, XCB_NONE, m_overlayPicture, 0, 0, 0, 0, 0, 0, sw, sh);
    xcb_xfixes_set_region(m_conn, m_damage, 0, nullptr);
    xcb_flush(m_conn);
}
