// SPDX-License-Identifier: LGPL-2.1-or-later
#include "coreui.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace CoreUI {

using AK::Point;

namespace {

const char *const CoreUIResources = "/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/Resources/";

struct File {
    std::vector<unsigned char> bytes;
    template <typename T> T read(size_t at) const
    {
        // little endian
        unsigned long long v = 0;
        if (at + sizeof(T) <= bytes.size())
            for (int i = int(sizeof(T)) - 1; i >= 0; --i)
                v = (v << 8) | bytes[at + size_t(i)];
        return T(v);
    }
    static File load(const char *name)
    {
        std::ifstream in(AK::Assets::path(std::string(CoreUIResources) + name), std::ios::binary);
        File f;
        if (in)
            f.bytes.assign(std::istreambuf_iterator<char>(in), {});
        return f;
    }
};

// Pixels as CoreUI stores them: A R G B bytes, premultiplied; cairo's ARGB32 is the same word
Image pixels(const File &f, size_t start, int w, int h)
{
    if (w <= 0 || h <= 0 || start + size_t(w) * h * 4 > f.bytes.size())
        return {};
    Image img(w, h);
    cairo_surface_flush(img.surface());
    unsigned char *dst = cairo_image_surface_get_data(img.surface());
    const int stride = cairo_image_surface_get_stride(img.surface());
    const unsigned char *src = f.bytes.data() + start;
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<uint32_t *>(dst + y * stride);
        for (int x = 0; x < w; ++x, src += 4)
            line[x] = uint32_t(src[0]) << 24 | uint32_t(src[1]) << 16 | uint32_t(src[2]) << 8 | src[3];
    }
    cairo_surface_mark_dirty(img.surface());
    return img;
}

// ArtFile.bin (ArtFileSetupOnce, CUIElement::LoadFromArtFile):
//   u16 elements, u16 key length, u16 tokens; u32 token table, token names, path table, data (from 8)
//   token: u32 name offset, u8 value; path entry: u32 record offset, then the key (token values, the
//   widget's first, the others ascending, 0-padded)
//   record: u16 rows, u16 columns; +0x20 u32 piece offsets (from the record), +0x44 u16 widths,
//   +0x56 u16 heights
struct ArtFile {
    File f;
    std::map<std::string, unsigned char> tokens;
    std::map<std::string, uint32_t> paths;
    uint32_t data = 0;
    int keyLength = 0;

    ArtFile() : f(File::load("ArtFile.bin"))
    {
        if (f.bytes.size() < 24)
            return;
        const int count = f.read<uint16_t>(0), tokenCount = f.read<uint16_t>(4);
        keyLength = f.read<uint16_t>(2);
        const uint32_t tokenTable = f.read<uint32_t>(8), names = f.read<uint32_t>(12), pathTable = f.read<uint32_t>(16);
        data = f.read<uint32_t>(20);
        for (int i = 0; i < tokenCount; ++i) {
            const size_t entry = tokenTable + 8 * size_t(i), name = names + size_t(f.read<uint32_t>(entry));
            if (name < f.bytes.size())
                tokens[reinterpret_cast<const char *>(f.bytes.data() + name)] = f.read<uint8_t>(entry + 4);
        }
        const size_t entrySize = size_t(keyLength) + 4;
        for (int i = 0; i < count; ++i) {
            const size_t entry = pathTable + entrySize * size_t(i);
            if (entry + entrySize > f.bytes.size())
                break;
            paths[std::string(reinterpret_cast<const char *>(f.bytes.data() + entry + 4), size_t(keyLength))] = f.read<uint32_t>(entry);
        }
    }

    Element find(const std::vector<std::string> &path) const
    {
        std::vector<unsigned char> values;
        for (const std::string &name : path) {
            auto t = tokens.find(name);
            if (t == tokens.end())
                return {};
            values.push_back(t->second);
        }
        if (values.empty())
            return {};
        std::sort(values.begin() + 1, values.end());
        std::string key(size_t(keyLength), '\0');
        for (size_t i = 0; i < values.size() && i < key.size(); ++i)
            key[i] = char(values[i]);
        auto p = paths.find(key);
        if (p == paths.end())
            return {};
        const size_t record = size_t(data) + p->second;
        Element e;
        e.rows = f.read<uint16_t>(record);
        e.columns = f.read<uint16_t>(record + 2);
        for (int i = 0; i < e.rows * e.columns && i < 9; ++i)
            e.pieces.push_back(pixels(f, record + f.read<uint32_t>(record + 0x20 + 4 * size_t(i)), f.read<uint16_t>(record + 0x44 + 2 * size_t(i)),
                                      f.read<uint16_t>(record + 0x56 + 2 * size_t(i))));
        return e;
    }
};

const ArtFile &artFile()
{
    static const ArtFile a;
    return a;
}

void piece(cairo_t *cr, const Image &img, const Rect &r)
{
    if (!img.isNull() && !r.empty())
        img.draw(cr, r);
}

} // namespace

Element element(std::vector<std::string> path)
{
    static std::map<std::vector<std::string>, Element> cache;
    auto it = cache.find(path);
    if (it != cache.end())
        return it->second;
    const std::vector<std::string> asked = path;
    Element e = artFile().find(path);
    for (const char *drop : {"active", "clear", "inactive"}) {
        if (!e.isNull())
            break;
        auto at = std::find(path.begin(), path.end(), drop);
        if (at != path.end()) {
            path.erase(at);
            e = artFile().find(path);
        }
    }
    return cache[asked] = e;
}

// SArtFile.bin (CUISharedArtReader): u16 version, u16 count, u32 data; u32 entry offsets from 8;
// an entry: u16 type, u16 representations, then from entry + 0xC the representations, 12 bytes each:
// u16 width, u16 height, u32 length, u32 offset (from data). Type 3 (the template images) has a PDF
// first, then the bitmap; the bitmap is the representation whose length is width x height x 4.
Image shared(int id)
{
    static const File f = File::load("SArtFile.bin");
    static std::map<int, Image> cache;
    auto it = cache.find(id);
    if (it != cache.end())
        return it->second;
    Image img;
    const int index = id - 20000, count = f.read<uint16_t>(2);
    if (index > 0 && index < count) {
        const size_t entry = f.read<uint32_t>(8 + 4 * size_t(index));
        const int reps = entry ? std::clamp(int(f.read<uint16_t>(entry + 2)), 1, 4) : 0;
        for (int r = 0; r < reps && img.isNull(); ++r) {
            const size_t rep = entry + 0xC + 12 * size_t(r);
            const int w = f.read<uint16_t>(rep), h = f.read<uint16_t>(rep + 2);
            if (f.read<uint32_t>(rep + 4) == uint32_t(w) * h * 4)
                img = pixels(f, size_t(f.read<uint32_t>(4)) + f.read<uint32_t>(rep + 8), w, h);
        }
    }
    return cache[id] = img;
}

Image named(const std::string &name)
{
    // CUIConstantToMapID's table (kCUIImageConstantIDs) then kCUIImageEntries: the SArtFile ID of each
    // image name AppKit's named images ask for (NSImageNameIconViewTemplate is "image.IconView")
    static const std::map<std::string, int> ids = {
        {"Action", 20163},     {"Add", 20188},         {"ChildContainer", 20146}, {"ColumnView", 20233}, {"FlowView", 20147},
        {"GoLeft", 20136},     {"GoRight", 20159},     {"IconView", 20235},   {"ListView", 20236},
        {"LockLocked", 20173}, {"LockUnlocked", 20174}, {"StopProgressFreestanding", 20156},
        {"Path", 20180},       {"QuickLook", 20170},   {"Remove", 20176},     {"Share", 20197},
    };
    auto it = ids.find(name);
    return it == ids.end() ? Image() : shared(it->second);
}

Rect aligned(double w, double h, const Rect &b, int horizontal, int vertical)
{
    // __CUICalcAlignedBounds
    Rect r{b.x, b.y, w, h};
    if (horizontal == AlignMax)
        r.x = b.x + b.w - w;
    else if (horizontal != AlignMin)
        r.x = b.x + std::floor((b.w - w) * 0.5);
    if (vertical == AlignMax)
        r.y = b.y + b.h - h;
    else if (vertical != AlignMin)
        r.y = b.y + std::floor((b.h - h) * 0.5);
    return r;
}

void draw(cairo_t *cr, const Element &e, const Rect &b, int vertical)
{
    if (e.isNull())
        return;
    const auto &pc = e.pieces;
    auto w = [&](int i) { return double(pc[size_t(i)].width()); };
    auto h = [&](int i) { return double(pc[size_t(i)].height()); };
    if (e.rows == 1 && e.columns == 1) {
        piece(cr, pc[0], aligned(w(0), h(0), b, AlignCenter, vertical));
    } else if (e.rows == 1 && e.columns == 2) {
        // Draw3PieceHorizontalWith2Images: the first piece's last column is the middle
        const Rect left = aligned(w(0) - 1, h(0), b, AlignMin, vertical);
        const Rect right = aligned(w(1), h(1), b, AlignMax, vertical);
        pc[0].draw(cr, Rect{0, 0, w(0) - 1, h(0)}, left);
        pc[0].draw(cr, Rect{w(0) - 1, 0, 1, h(0)}, Rect{left.right(), left.y, right.x - left.right(), h(0)});
        piece(cr, pc[1], right);
    } else if (e.rows == 1 && e.columns == 3) {
        const Rect left = aligned(w(0), h(0), b, AlignMin, vertical);
        const Rect right = aligned(w(2), h(2), b, AlignMax, vertical);
        Rect middle = aligned(w(1), h(1), b, AlignMin, vertical);
        middle.x = b.x + left.w;
        middle.w = b.w - left.w - right.w;
        piece(cr, pc[0], left);
        piece(cr, pc[1], middle);
        piece(cr, pc[2], right);
    } else if (e.rows == 3 && e.columns == 1) {
        const Rect top = aligned(w(0), h(0), b, AlignCenter, AlignMin);
        const Rect bottom = aligned(w(2), h(2), b, AlignCenter, AlignMax);
        Rect middle = aligned(w(1), h(1), b, AlignCenter, AlignMin);
        middle.y = b.y + top.h;
        middle.h = b.h - top.h - bottom.h;
        piece(cr, pc[0], top);
        piece(cr, pc[1], middle);
        piece(cr, pc[2], bottom);
    } else if (e.rows == 3 && e.columns == 3) {
        // Prepare9PieceInstructions
        const double l = w(0), r = w(2), t = h(0), bt = h(6);
        const double xs[] = {b.x, b.x + l, b.right() - r}, ws[] = {l, std::max(0.0, b.w - l - r), r};
        const double ys[] = {b.y, b.y + t, b.bottom() - bt}, hs[] = {t, std::max(0.0, b.h - t - bt), bt};
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                piece(cr, pc[size_t(row * 3 + col)], Rect{xs[col], ys[row], ws[col], hs[row]});
    }
}

Color color(int index)
{
    // kColorTable (CoreUI 0x3FB60): RGBA doubles, the entries used here; Generic RGB, as all of
    // CoreUI's colours (_CUIColorSpaceGetRGB), matched to the screen
    switch (index) {
    case FocusBlue: return Color{0.294, 0.537, 0.816, 1}.matched();
    case FocusGraphite: return Color{0.369, 0.420, 0.475, 1}.matched();
    case MenuFill: return {1, 1, 1, 0.95};
    case MenuSeparator: return Color{0.839, 0.839, 0.839, 1}.matched();
    case MenuFill2: return {0, 0, 0, 0.02};
    case FocusFill: return {1, 1, 1, 1};
    default: return {0, 0, 0, 1};
    }
}

void focusRing(cairo_t *cr, const Rect &bounds, const std::function<void(cairo_t *)> &shape)
{
    // CGFocusRing: the shape's outline grown by about 3 pixels, soft at its outer edge, in the focus
    // colour at 0.8, drawn around (not over) the shape
    const int pad = 5;
    const int W = int(std::ceil(bounds.w)) + 2 * pad, H = int(std::ceil(bounds.h)) + 2 * pad;
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    cairo_t *m = cairo_create(mask);
    cairo_translate(m, pad - bounds.x, pad - bounds.y);
    shape(m);
    cairo_destroy(m);
    cairo_surface_flush(mask);
    const unsigned char *src = cairo_image_surface_get_data(mask);
    const int stride = cairo_image_surface_get_stride(mask);
    std::vector<float> a(size_t(W) * H), grown(a.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            a[size_t(y) * W + x] = src[y * stride + x] / 255.f;
    // grow by 2.5 pixels (a disc), then soften the edge by one pixel
    const float radius = 2.5f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float best = 0;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= W || sy >= H)
                        continue;
                    const float d = std::sqrt(float(dx * dx + dy * dy));
                    const float k = std::clamp(radius + 0.5f - d, 0.f, 1.f);
                    best = std::max(best, a[size_t(sy) * W + sx] * k);
                }
            grown[size_t(y) * W + x] = best;
        }
    cairo_surface_t *ring = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    cairo_surface_flush(ring);
    unsigned char *dst = cairo_image_surface_get_data(ring);
    const int rs = cairo_image_surface_get_stride(ring);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float v = grown[size_t(y) * W + x] * (1 - a[size_t(y) * W + x]);
            dst[y * rs + x] = (unsigned char)std::lround(std::clamp(v, 0.f, 1.f) * 255 * 0.8f);
        }
    cairo_surface_mark_dirty(ring);
    cairo_save(cr);
    color(FocusBlue).set(cr);
    cairo_mask_surface(cr, ring, bounds.x - pad, bounds.y - pad);
    cairo_restore(cr);
    cairo_surface_destroy(ring);
    cairo_surface_destroy(mask);
}

// ---- the controls

namespace {

const char *sizeToken(const State &s) { return s.small ? "small" : "regular"; }

std::string stateToken(const State &s)
{
    if (!s.enabled)
        return "disabled";
    if (s.pressed)
        return "pressed";
    return s.windowActive ? "active" : "inactive";
}

// the first of the paths the art has
Element first(std::initializer_list<std::vector<std::string>> paths)
{
    for (const auto &p : paths) {
        Element e = element(p);
        if (!e.isNull())
            return e;
    }
    return {};
}

void withFocus(cairo_t *cr, const Rect &r, const State &s, const std::function<void(cairo_t *)> &shape)
{
    if (s.focused && s.windowActive && s.enabled)
        focusRing(cr, r, shape);
    shape(cr);
}

} // namespace

void pushButton(cairo_t *cr, const Rect &r, const State &s, bool isDefault)
{
    const bool blue = (isDefault && s.enabled && s.windowActive) || s.pressed;
    Element e = first({{"pushbutton", blue ? "blue" : "clear", stateToken(s), sizeToken(s)}, {"pushbutton", "clear", sizeToken(s)}});
    withFocus(cr, r, s, [&](cairo_t *c) { draw(c, e, r); });
}

void checkBox(cairo_t *cr, const Rect &r, const State &s, int value)
{
    const std::string v = value == 1 ? "onchecked" : value == 2 ? "mixed" : "off";
    // checked: blue in a key window; pressed: blue when it will be checked
    const bool blue = s.enabled && s.windowActive && (value != 0) != s.pressed;
    Element e = first({{"checkbox", blue ? "blue" : "clear", v, stateToken(s), sizeToken(s)}, {"checkbox", "clear", v, sizeToken(s)}});
    withFocus(cr, r, s, [&](cairo_t *c) { draw(c, e, r); });
}

void radioButton(cairo_t *cr, const Rect &r, const State &s, bool on)
{
    const bool blue = s.enabled && s.windowActive && on != s.pressed;
    Element e = first({{"radiobutton", blue ? "blue" : "clear", on ? "on" : "off", stateToken(s), sizeToken(s)},
                       {"radiobutton", "clear", on ? "on" : "off", sizeToken(s)}});
    withFocus(cr, r, s, [&](cairo_t *c) { draw(c, e, r); });
}

void popUpButton(cairo_t *cr, const Rect &r, const State &s)
{
    const bool blue = s.enabled && s.windowActive;
    Element e = first({{"popup", blue ? "blue" : "clear", stateToken(s), sizeToken(s), "rightsidebutton"},
                       {"popup", "clear", stateToken(s), sizeToken(s), "rightsidebutton"}, {"popup", sizeToken(s), "rightsidebutton"}});
    withFocus(cr, r, s, [&](cairo_t *c) { draw(c, e, r); });
}

void helpButton(cairo_t *cr, const Rect &r, const State &s)
{
    draw(cr, first({{"roundbutton", "help", stateToken(s), "regular"}, {"roundbutton", "help", "regular"}}), r);
}

void disclosureTriangle(cairo_t *cr, const Rect &r, bool open, bool pressed)
{
    std::vector<std::string> p{"disclosuretriangle", open ? "open" : "close"};
    if (pressed)
        p.push_back("pressed");
    draw(cr, element(p), r);
}

void textField(cairo_t *cr, const Rect &r, const State &s)
{
    Element e = first({{"textfield", s.enabled ? "active" : "disabled", "regular"}, {"textfield", "regular"}});
    withFocus(cr, r, s, [&](cairo_t *c) { draw(c, e, r); });
}

void searchField(cairo_t *cr, const Rect &r, const State &s, bool inToolbar)
{
    Element e = inToolbar ? element({"textfieldround", s.enabled ? "active" : "disabled", "fortoolbars", "regular"})
                          : element({"textfieldround", s.enabled ? "active" : "disabled", "regular"});
    withFocus(cr, r, s, [&](cairo_t *c) { draw(c, e, r); });
}

void imageWell(cairo_t *cr, const Rect &r, const State &s)
{
    draw(cr, element({"imagewell", !s.enabled ? "disabled" : s.windowActive ? "active" : "inactive"}), r);
}

void tabPane(cairo_t *cr, const Rect &r, const State &s)
{
    draw(cr, element({"tabpane", !s.enabled ? "disabled" : s.windowActive ? "active" : "inactive", "opaque"}), r);
}

void listBox(cairo_t *cr, const Rect &r, const State &s)
{
    draw(cr, element({"listbox", !s.enabled ? "disabled" : s.windowActive ? "active" : "inactive", "regular"}), r);
}

namespace {

void roundedRect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void verticalGradient(cairo_t *cr, double y0, double y1, std::initializer_list<std::pair<double, double>> stops)
{
    cairo_pattern_t *g = cairo_pattern_create_linear(0, y0, 0, y1);
    for (const auto &[at, gray] : stops)
        cairo_pattern_add_color_stop_rgb(g, at, gray / 255, gray / 255, gray / 255);
    cairo_set_source(cr, g);
    cairo_pattern_destroy(g);
}

// The textured rounded bezel, as CoreUI draws it in code on 10.6 (ArtFile's "segment fortexture"
// pieces are the older metal look): measured on a 10.6 Finder toolbar. A 1 point border going from 88
// to 104, the face 253 down to 186 (a selected or pressed segment dark: 55 under its top edge, 100 five
// points down, 116 at the bottom), 1 point separators, a white emboss under it all; the corners 4
// round. A window that isn't key: lighter, flatter (estimated).
void texturedSegments(cairo_t *cr, const std::vector<Rect> &rects, const std::vector<bool> &selected, int pressed, const State &s)
{
    if (rects.empty())
        return;
    const double x0 = rects.front().x, x1 = rects.back().right();
    // AppKit's 25 point textured control: the bezel 23 high from its top, the emboss under it, its
    // last row clear (a 10.6 Finder toolbar: the border rows 2 to 24 of the toolbar, the emboss 25)
    const double y = rects.front().y, h = rects.front().h - 2;
    const double radius = 4;
    const bool key = s.windowActive;
    cairo_save(cr);
    if (!s.enabled)
        cairo_push_group(cr);

    roundedRect(cr, x0, y + 1, x1 - x0, h, radius);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
    cairo_fill(cr);
    roundedRect(cr, x0, y, x1 - x0, h, radius);
    if (key)
        verticalGradient(cr, y, y + h, {{0, 88}, {1, 104}});
    else
        verticalGradient(cr, y, y + h, {{0, 146}, {1, 156}});
    cairo_fill(cr);

    roundedRect(cr, x0 + 1, y + 1, x1 - x0 - 2, h - 2, radius - 1);
    cairo_clip(cr);
    for (size_t i = 0; i < rects.size(); ++i) {
        const bool on = (i < selected.size() && selected[i]) || int(i) == pressed;
        const double left = rects[i].x + (i == 0 ? 1 : 0), right = rects[i].right() - (i + 1 == rects.size() ? 1 : 0);
        cairo_rectangle(cr, left, y + 1, right - left, h - 2);
        if (on && key)
            verticalGradient(cr, y + 1, y + h - 1, {{0, 55}, {5 / (h - 2), 100}, {1, 116}});
        else if (on)
            verticalGradient(cr, y + 1, y + h - 1, {{0, 140}, {5 / (h - 2), 165}, {1, 175}});
        else if (key)
            verticalGradient(cr, y + 1, y + h - 1, {{0, 253}, {1, 186}});
        else
            verticalGradient(cr, y + 1, y + h - 1, {{0, 250}, {1, 222}});
        cairo_fill(cr);
        if (i == 0)
            continue;
        // the separator: darker beside a selected segment
        const bool dark = on || (selected.size() >= i && selected[i - 1]) || int(i) - 1 == pressed;
        cairo_rectangle(cr, rects[i].x, y + 1, 1, h - 2);
        const double gray = key ? (dark ? 66 : 104) : (dark ? 140 : 160);
        cairo_set_source_rgb(cr, gray / 255, gray / 255, gray / 255);
        cairo_fill(cr);
    }

    if (!s.enabled) {
        cairo_pop_group_to_source(cr);   // the group's clip goes with it
        cairo_paint_with_alpha(cr, 0.55);
    }
    cairo_restore(cr);
}

} // namespace

void segments(cairo_t *cr, SegmentStyle style, const std::vector<Rect> &rects, const std::vector<bool> &selected, int pressed,
              const State &s, const char *size)
{
    if (style == SegmentStyle::Textured) {
        texturedSegments(cr, rects, selected, pressed, s);
        return;
    }
    // DrawSegment: a segment's left end only if it is the first, its right end only if it is the
    // last, its middle stretched between; a segment after another starts with the 1 point line of
    // its …/separator element (that element's middle piece) instead of an end
    auto art = [&](size_t i, bool separator) {
        const bool sel = i < selected.size() && selected[i];
        const bool down = int(i) == pressed;
        const bool blue = s.enabled && s.windowActive && sel;
        std::vector<std::string> p{"tabs", blue ? "blue" : "clear", "horizontal", size};
        if (!s.enabled)
            p.push_back("disabled");
        else if (down)
            p.push_back("pressed");
        else if (sel && !blue)
            p.push_back("pressed");
        else if (!s.windowActive)
            p.push_back("inactive");
        else if (!blue)
            p.push_back("active");
        if (separator)
            p.push_back("separator");
        Element e = element(p);
        if (e.isNull() && separator) {
            p.erase(std::remove_if(p.begin(), p.end(), [](const std::string &t) { return t == "inactive" || t == "disabled" || t == "active"; }),
                    p.end());
            e = element(p);
        }
        return e;
    };
    for (size_t i = 0; i < rects.size(); ++i) {
        Element e = art(i, false);
        if (e.pieces.size() != 3)
            continue;
        const Rect &b = rects[i];
        double left = 0, right = 0;
        if (i == 0) {
            const Rect r = aligned(e.pieces[0].width(), e.pieces[0].height(), b, AlignMin, AlignCenter);
            piece(cr, e.pieces[0], r);
            left = r.w;
        } else {
            Element sep = art(i, true);
            if (sep.pieces.size() == 3) {
                const Rect r = aligned(sep.pieces[1].width(), sep.pieces[1].height(), b, AlignMin, AlignCenter);
                piece(cr, sep.pieces[1], r);
                left = r.w;
            }
        }
        if (i + 1 == rects.size()) {
            const Rect r = aligned(e.pieces[2].width(), e.pieces[2].height(), b, AlignMax, AlignCenter);
            piece(cr, e.pieces[2], r);
            right = r.w;
        }
        Rect middle = aligned(e.pieces[1].width(), e.pieces[1].height(), b, AlignMin, AlignCenter);
        middle.x = b.x + left;
        middle.w = b.w - left - right;
        piece(cr, e.pieces[1], middle);
    }
}

ScrollerParts scrollerParts(const Rect &r, double value, double proportion)
{
    ScrollerParts p;
    if (r.w > r.h) {
        // horizontal: left 13, right 17, after the track (the art's widths)
        p.down = Rect{r.right() - 17, r.y, 17, r.h};
        p.up = Rect{p.down.x - 13, r.y, 13, r.h};
        p.track = Rect{r.x, r.y, p.up.x - r.x, r.h};
        if (proportion < 1 && p.track.w > 0) {
            const double len = std::max(26.0, std::round(p.track.w * proportion));
            const double x = p.track.x + std::round((p.track.w - len) * std::clamp(value, 0.0, 1.0));
            p.knob = Rect{x, r.y, len, r.h};
        }
        return p;
    }
    // the arrows together at the bottom (10.6's default): up 14, down 16, under the track's end
    p.down = Rect{r.x, r.bottom() - 16, r.w, 16};
    p.up = Rect{r.x, p.down.y - 14, r.w, 14};
    p.track = Rect{r.x, r.y, r.w, p.up.y - r.y};
    if (proportion < 1 && p.track.h > 0) {
        const double len = std::max(26.0, std::round(p.track.h * proportion));
        const double y = p.track.y + std::round((p.track.h - len) * std::clamp(value, 0.0, 1.0));
        p.knob = Rect{r.x, y, r.w, len};
    }
    return p;
}

void scroller(cairo_t *cr, const Rect &r, double value, double proportion, const State &s, int pressedArrow)
{
    const bool horizontal = r.w > r.h;
    const char *dir = horizontal ? "horizontal" : "vertical";
    if (proportion >= 1) {
        // one piece, the track's cross section: stretched along the scroller's length
        const Element e = element({"scrollbars", "nothingtoscroll", "regular", "track", dir});
        if (e.pieces.size() == 1)
            e.pieces[0].draw(cr, r);
        else
            draw(cr, e, r);
        return;
    }
    const ScrollerParts p = scrollerParts(r, value, proportion);
    const std::string active = s.windowActive ? "active" : "inactive";
    const char *end = horizontal ? "right" : "bottom";
    Element arrows = element({"scrollbars", active, "arrows", end, "double", "regular"});
    Element down = pressedArrow ? element({"scrollbars", "arrows", "blue", end, "double", "pressed", "regular"}) : Element{};
    // the track runs under the arrows' rounded start (their first piece, 8 long)
    if (horizontal) {
        draw(cr, element({"scrollbars", active, "regular", "track", dir}), Rect{r.x, r.y, p.track.w + 8, r.h});
        if (arrows.pieces.size() == 3) {
            double x = p.up.x - 8;
            for (int i = 0; i < 3; ++i) {
                const Image &img = (i == pressedArrow && down.pieces.size() == 3) ? down.pieces[size_t(i)] : arrows.pieces[size_t(i)];
                img.draw(cr, Rect{x, r.y, double(img.width()), ScrollerWidth});
                x += img.width();
            }
        }
    } else {
        draw(cr, element({"scrollbars", active, "regular", "track", dir}), Rect{r.x, r.y, r.w, p.track.h + 8});
        if (arrows.pieces.size() == 3) {
            double y = p.up.y - 8;
            for (int i = 0; i < 3; ++i) {
                const Image &img = (i == pressedArrow && down.pieces.size() == 3) ? down.pieces[size_t(i)] : arrows.pieces[size_t(i)];
                img.draw(cr, Rect{r.x, y, ScrollerWidth, double(img.height())});
                y += img.height();
            }
        }
    }
    if (!p.knob.empty())
        draw(cr, element({"scrollbars", s.windowActive ? "blue" : "clear", "regular", "thumb", dir}), p.knob);
}

SliderParts sliderParts(const Rect &r, double value, int ticks, bool ticksAbove, bool small)
{
    SliderParts p;
    p.vertical = r.h > r.w;
    const char *size = small ? "small" : "regular";
    // the thumb points at the ticks; round without them
    const char *dir = ticks <= 0 ? "none" : p.vertical ? (ticksAbove ? "west" : "east") : (ticksAbove ? "north" : "south");
    const Element thumb = element({"sliders", "clear", size, dir, "thumb"});
    const double tw = thumb.isNull() ? 19 : thumb.pieces[0].width(), th = thumb.isNull() ? 22 : thumb.pieces[0].height();
    const double v = std::clamp(value, 0.0, 1.0);
    // DrawSlider: the track centred across, 5 points towards the thumb's back (6 when it points up or left)
    const double shift = ticks <= 0 ? 0 : ticksAbove ? 6 : -5;
    if (!p.vertical) {
        p.track = aligned(r.w, small ? 4 : 5, r, AlignMin, AlignCenter);
        p.track.y += shift;
        p.thumb = aligned(tw, th, r, AlignMin, AlignCenter);
        p.thumb.x = r.x + std::round((r.w - tw) * v);
        const double step = ticks > 1 ? (r.w - tw) / (ticks - 1) : 0;
        for (int i = 0; i < ticks; ++i)
            p.ticks.push_back(std::round(r.x + std::floor(tw / 2) + step * i));
    } else {
        p.track = aligned(small ? 4 : 5, r.h, r, AlignCenter, AlignMin);
        p.track.x += ticks <= 0 ? 0 : ticksAbove ? 6 : -5;
        p.thumb = aligned(tw, th, r, AlignCenter, AlignMin);
        // the minimum at the bottom
        p.thumb.y = r.y + std::round((r.h - th) * (1 - v));
        const double step = ticks > 1 ? (r.h - th) / (ticks - 1) : 0;
        for (int i = 0; i < ticks; ++i)
            p.ticks.push_back(std::round(r.y + std::floor(th / 2) + step * i));
    }
    return p;
}

void slider(cairo_t *cr, const Rect &r, double value, int ticks, const State &s, bool ticksAbove)
{
    const SliderParts p = sliderParts(r, value, ticks, ticksAbove, s.small);
    const char *state = !s.enabled ? "disabled" : s.windowActive ? "active" : "inactive";
    draw(cr, element({"sliders", state, p.vertical ? "vertical" : "horizontal", sizeToken(s), "track"}), p.track);
    Element tick = element({"sliders", p.vertical ? "vertical" : "horizontal", sizeToken(s), "ticks"});
    if (tick.isNull() && !p.vertical)
        tick = element({"sliders", "horizontal", "regular", "ticks"});
    if (!tick.isNull())
        for (double t : p.ticks) {
            // kTickOffsetHAdjustment: 10 points past the track, on the thumb's side
            const Image &img = tick.pieces[0];
            if (!p.vertical)
                img.draw(cr, AK::Point{t, ticksAbove ? p.track.y - 10 - img.height() : p.track.bottom() + 10});
            else
                img.draw(cr, AK::Point{ticksAbove ? p.track.x - 9 - img.width() : p.track.right() + 9, t});
        }
    const char *dir = ticks <= 0 ? "none" : p.vertical ? (ticksAbove ? "west" : "east") : (ticksAbove ? "north" : "south");
    // an inactive window's thumb is the active one's: the art has no other
    Element thumb = first({{"sliders", s.pressed ? "blue" : "clear", stateToken(s), sizeToken(s), dir, "thumb"},
                           {"sliders", "active", "clear", sizeToken(s), dir, "thumb"}, {"sliders", "clear", sizeToken(s), dir, "thumb"}});
    withFocus(cr, p.thumb, s, [&](cairo_t *c) { draw(c, thumb, p.thumb); });
}

void groupBox(cairo_t *cr, const Rect &r, const State &s, bool secondary)
{
    draw(cr, element({"groupbox", s.windowActive ? "active" : "inactive", secondary ? "secondary" : "primary"}), r);
}

void progressBar(cairo_t *cr, const Rect &r, double value, bool indeterminate, double phase, const State &s)
{
    // a determinate bar: the track (its left cap, middle and right cap) across, the blue fill over its
    // done part, the "complete" art once it is full; an indeterminate one: its stripes across, moving
    const char *active = s.windowActive ? "active" : "inactive";
    auto tile = [&](bool complete) {
        std::vector<std::string> p{"progressindicator", "blue", "horizontal", "outset", "regular",
                                   indeterminate ? "indeterminate" : "determinate"};
        if (complete)
            p.push_back("complete");
        p.push_back(active);
        return element(p);
    };
    auto run = [&](const Element &e, const Rect &area, double offset) {
        if (e.isNull())
            return;
        const Image &img = e.pieces[0];
        const double y = r.y + std::floor((r.h - img.height()) / 2);
        cairo_save(cr);
        cairo_rectangle(cr, area.x, area.y, area.w, area.h);
        cairo_clip(cr);
        for (double x = area.x - offset; x < area.right(); x += img.width())
            img.draw(cr, AK::Point{x, y});
        cairo_restore(cr);
    };
    if (indeterminate) {
        run(element({"progressindicator", "horizontal", "indeterminate", "outset", "regular", "track"}), r, 0);
        run(tile(true), r, std::fmod(phase, 34.0));
        return;
    }
    const Element left = element({"progressindicator", "determinate", "horizontal", "leftcap", "outset", "regular", "track"});
    const Element middle = element({"progressindicator", "determinate", "horizontal", "outset", "regular", "track"});
    const Element right = element({"progressindicator", "determinate", "horizontal", "outset", "regular", "rightcap", "track"});
    if (!left.isNull() && !middle.isNull() && !right.isNull()) {
        const Image &l = left.pieces[0], &m = middle.pieces[0], &rc = right.pieces[0];
        const double y = r.y + std::floor((r.h - m.height()) / 2);
        l.draw(cr, AK::Point{r.x, y});
        rc.draw(cr, AK::Point{r.right() - rc.width(), y});
        m.draw(cr, Rect{r.x + l.width(), y, r.w - l.width() - rc.width(), double(m.height())});
    } else {
        run(middle, r, 0);
    }
    const double done = std::clamp(value, 0.0, 1.0);
    run(tile(done >= 1), Rect{r.x, r.y, std::round(r.w * done), r.h}, std::fmod(phase, 34.0));
}

void comboBox(cairo_t *cr, const Rect &r, const State &s)
{
    const bool blue = s.enabled && s.windowActive;
    Element field = element({"combobox", "active", "regular", "rightsidebutton", "textfield"});
    Element end = first({{"combobox", blue ? "blue" : "clear", stateToken(s), "regular", "rightsidebutton"},
                         {"combobox", "clear", "regular", "rightsidebutton"}});
    const double endWidth = end.pieces.size() == 2 ? end.pieces[1].width() : 18;
    withFocus(cr, r, s, [&](cairo_t *c) {
        draw(c, field, Rect{r.x, r.y, r.w - endWidth + 2, std::min(r.h, 22.0)});
        if (end.pieces.size() == 2)
            draw(c, end, Rect{r.right() - endWidth - 6, r.y, endWidth + 6, std::min(r.h, 23.0)}, AlignMin);
    });
}

void stepper(cairo_t *cr, const Rect &r, const State &s, int pressed)
{
    Element e = pressed ? element({"littlearrows", "clear", pressed == 1 ? "pressedtop" : "pressedbottom", "regular", "vertical"})
                        : first({{"littlearrows", "clear", stateToken(s), "regular", "vertical"}, {"littlearrows", "regular", "vertical"}});
    if (e.rows == 2) {
        const Rect b = aligned(e.pieces[0].width(), e.pieces[0].height() + e.pieces[1].height(), r, AlignCenter, AlignCenter);
        e.pieces[0].draw(cr, AK::Point{b.x, b.y});
        e.pieces[1].draw(cr, AK::Point{b.x, b.y + e.pieces[0].height()});
    } else {
        draw(cr, e, r);
    }
}

void listHeader(cairo_t *cr, const Rect &r, const State &s)
{
    draw(cr, first({{"listheader", s.windowActive ? "active" : "clear", "clear", "regular"}, {"listheader", "clear", "regular"}}), r);
}

void menuBackground(cairo_t *cr, const Rect &r, MenuShape shape, double resultsColumn)
{
    // CUIRenderer::DrawMenu (0x8df4): no art, a path of 5 point corners, each round or square by
    // the variant, clipped and filled with _CUIColorGet(0xE1) (white at 0.95); the shadow is the
    // window server's. Quartz's y goes up: its MaxY corners are the top ones here.
    const double radius = 5;   // 0x3f0f0
    bool topLeft = false, topRight = false;
    switch (shape) {
    case MenuShape::Popup: topLeft = topRight = true; break;
    case MenuShape::SubmenuLeft: topLeft = true; break;
    case MenuShape::SubmenuRight: topRight = true; break;
    case MenuShape::Pulldown:
    case MenuShape::Results: break;
    }
    const double x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    cairo_save(cr);
    cairo_new_path(cr);
    if (topLeft)
        cairo_arc(cr, x0 + radius, y0 + radius, radius, M_PI, 1.5 * M_PI);
    else
        cairo_move_to(cr, x0, y0);
    if (topRight)
        cairo_arc(cr, x1 - radius, y0 + radius, radius, 1.5 * M_PI, 2 * M_PI);
    else
        cairo_line_to(cr, x1, y0);
    // the bottom corners always round
    cairo_arc(cr, x1 - radius, y1 - radius, radius, 0, 0.5 * M_PI);
    cairo_arc(cr, x0 + radius, y1 - radius, radius, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
    cairo_clip(cr);
    AK::fill(cr, r, color(MenuFill));
    if (shape == MenuShape::Results) {
        // the results' side darkened (0xE3) from the column, a line of 0xE2 one point before it
        const double column = resultsColumn != 0 ? resultsColumn : 128;   // 0x3f110
        AK::fill(cr, Rect{r.x + column, r.y, r.w - column, r.h}, color(MenuFill2));
        AK::fill(cr, Rect{r.x + column - 1, r.y, 1, r.h}, color(MenuSeparator));
    }
    cairo_restore(cr);
}

void menuSelection(cairo_t *cr, const Rect &r)
{
    Element e = element({"menu", "selection"});
    if (e.isNull())
        AK::fill(cr, r, AK::Color::rgb(0x2a, 0x5c, 0xe0));
    else
        e.pieces[0].draw(cr, r);
}

} // namespace CoreUI
