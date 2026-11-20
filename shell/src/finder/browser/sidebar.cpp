// SPDX-License-Identifier: LGPL-2.1-or-later
#include "sidebar.h"

#include "files.h"

#include "foundation.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string core(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "LocalizableCore", key, fallback); }

// a canned search's name: its InfoPlist.strings' CFBundleDisplayName (extract.py's JSON of it)
std::string cannedName(const std::string &macPath, const std::string &fallback)
{
    std::ifstream in(Assets::path(macPath));
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string key = "\"CFBundleDisplayName\": \"";
    const size_t at = json.find(key);
    if (at == std::string::npos)
        return fallback;
    const size_t end = json.find('"', at + key.size());
    return end == std::string::npos ? fallback : json.substr(at + key.size(), end - at - key.size());
}

Image fitted(const Image &img, int size)
{
    return img.isNull() || (img.width() == size && img.height() == size) ? img : img.scaled(size, size);
}

} // namespace

bool Sidebar::shows(const std::string &item)
{
    return Settings("finder").boolean("SidebarShow" + item, true);
}

void Sidebar::add(const std::string &path)
{
    Settings conf("finder");
    std::vector<std::string> places = conf.list("SidebarPlaces");
    if (std::find(places.begin(), places.end(), path) == places.end()) {
        places.push_back(path);
        conf.set("SidebarPlaces", places);
    }
}

void Sidebar::remove(const std::string &path)
{
    Settings conf("finder");
    std::vector<std::string> places = conf.list("SidebarPlaces");
    places.erase(std::remove(places.begin(), places.end(), path), places.end());
    conf.set("SidebarPlaces", places);
}

void Sidebar::reload()
{
    rows.clear();
    rows.push_back({core("SD5", "DEVICES"), {}, {}, true});
    for (const Volume &v : volumes()) {
        const char *pref = v.kind == Volume::Internal ? "HardDisks" : v.kind == Volume::External ? "ExternalDisks" : "Removable";
        if (!shows(pref))
            continue;
        Row r{v.name, v.path, volumeIcon(v, 16), false};
        r.ejectable = v.kind != Volume::Internal;
        rows.push_back(r);
    }

    // PLACES: the folders' toolbar icons (CoreTypes' Toolbar*FolderIcon, their 16-point pictures), as
    // 10.6's sidebar shows them, not their Finder icons
    rows.push_back({core("SD8", "PLACES"), {}, {}, true});
    const std::string h = home();
    auto place = [&](const std::string &path, const std::string &title, const char *icon, const char *pref) {
        if (isDir(path) && shows(pref))
            rows.push_back({title, path, coreTypesIcon(icon, 16), false});
    };
    place(userDir("DESKTOP", h + "/Desktop"), "Desktop", "ToolbarDesktopFolderIcon", "Desktop");
    place(h, fileName(h), "HomeFolderIcon", "Home");
    if (shows("Applications"))
        rows.push_back({"Applications", "/usr/share/applications", coreTypesIcon("ToolbarAppsFolderIcon", 16), false});
    place(userDir("DOCUMENTS", h + "/Documents"), "Documents", "ToolbarDocumentsFolderIcon", "Documents");
    // the user's own (Add to Sidebar)
    for (const std::string &p : Settings("finder").list("SidebarPlaces")) {
        if (!isDir(p) && !isFile(p))
            continue;
        Item item{fileName(p), p, isDir(p)};
        Row r{displayName(p), p, icon(item, 16), false};
        r.added = true;
        rows.push_back(r);
    }

    // SEARCH FOR: the Finder's canned searches, their names from their bundles; the times' own icons
    // (a clock), the kinds' a smart folder (CoreTypes' SmartFolderIcon)
    rows.push_back({core("SD7", "SEARCH FOR"), {}, {}, true});
    const std::string canned = std::string(Bundle) + "/Contents/Resources/CannedSearches/";
    const struct {
        const char *bundle, *icns;
    } searches[] = {{"Today", "Today"},          {"Yesterday", "Yesterday"},   {"Past Week", "ThisWeek"},
                    {"All Images", nullptr},      {"All Movies", nullptr},      {"All Documents", nullptr}};
    for (const auto &s : searches) {
        const std::string res = canned + s.bundle + ".cannedSearch/Resources/";
        const std::string title = cannedName(res + "English.lproj/InfoPlist.strings.json", s.bundle);
        const Image icon = s.icns ? fitted(Image::load(Assets::path(res + s.icns + ".icns"), {16, 16}), 16)
                                  : coreTypesIcon("SmartFolderIcon", 16);
        std::string pref = s.bundle;
        pref.erase(std::remove(pref.begin(), pref.end(), ' '), pref.end());
        if (shows(pref))
            rows.push_back({title, std::string("search:") + s.bundle, icon, false});
    }
    if (m_selected >= int(rows.size()))
        m_selected = -1;
    setNeedsDisplay();
}

void Sidebar::selectPath(const std::string &path)
{
    m_selected = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].header && rows[i].path == path) {
            m_selected = int(i);
            break;
        }
    setNeedsDisplay();
}

double Sidebar::rowTop(size_t i) const
{
    double y = 2;
    for (size_t k = 1; k <= i && k < rows.size(); ++k)
        y += RowHeight + (rows[k].header ? SectionGap : 0);
    return y;
}

int Sidebar::rowAt(double y) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (y >= rowTop(i) && y < rowTop(i) + RowHeight)
            return int(i);
    return -1;
}

void Sidebar::draw(cairo_t *cr)
{
    const bool active = windowIsKey();
    // kCUIVariantGradientSideBar: _kPairs 0x1A (active), 0x1C (inactive); CoreUI's gradients are Generic
    // RGB (ShadeRectColorFunction: _CUIColorSpaceGetRGB), matched to the screen
    fill(cr, bounds(), active ? Color::generic(214, 221, 229) : Color::genericGray(232));
    // its last column darkened by the divider beside it (a 10.6.8 Finder: 222/228/234 → 206/210/214)
    fill(cr, Rect{frame().w - 1, 0, 1, frame().h}, Color::gray(0, 0.072));
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        const double y = rowTop(i);
        const bool selected = int(i) == m_selected;
        if (selected) {
            // kCUIVariantGradientSideBarSelection: the key window's _kPairs 0x10 the gradient, 0x11 its top
            // line (the bright blue a 10.6 Finder shows); an inactive window's 0x18 and 0x19
            const Color top = active ? Color::generic(91, 147, 213) : Color::genericGray(181);
            const Color bottom = active ? Color::generic(22, 83, 170) : Color::genericGray(137);
            const Color line = active ? Color::generic(69, 128, 200) : Color::genericGray(151);
            cairo_pattern_t *g = cairo_pattern_create_linear(0, y, 0, y + RowHeight);
            cairo_pattern_add_color_stop_rgb(g, 0, top.r, top.g, top.b);
            cairo_pattern_add_color_stop_rgb(g, 1, bottom.r, bottom.g, bottom.b);
            cairo_rectangle(cr, 0, y, frame().w, RowHeight);
            cairo_set_source(cr, g);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
            fill(cr, Rect{0, y, frame().w, 1}, line);
        }
        TextStyle t;
        if (r.header) {
            // the headers: bold 11, grey-blue, engraved, after their disclosure triangle (open: pointing
            // down, 7 wide, lighter than the text); places and colours measured on a 10.6.8 Finder (the
            // text 119/134/150, the triangle 157/168/179, as the screen shows them)
            t.font = Font::boldSystem(11);
            t.color = active ? Color::rgb(119, 134, 150) : Color::gray(130);
            const Color triangle = active ? Color::rgb(157, 168, 179) : Color::gray(170);
            t.engraved = true;
            t.engravedAlpha = 0.6;
            for (int pass = 0; pass < 2; ++pass) {
                const double ty = y + 8 + (pass == 0 ? 1 : 0);
                cairo_move_to(cr, 9, ty);
                cairo_line_to(cr, 16, ty);
                cairo_line_to(cr, 12.5, ty + 6);
                cairo_close_path(cr);
                if (pass == 0)
                    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
                else
                    triangle.set(cr);
                cairo_fill(cr);
            }
            drawText(cr, r.title, Rect{20, y + 2, frame().w - 24, RowHeight - 3}, t);
            continue;
        }
        if (!r.icon.isNull())
            r.icon.draw(cr, Rect{23, y + 1, 16, 16});   // a 10.6.8 Finder: its art's top 3 under the row's
        t.font = selected ? Font::boldSystem(11) : Font::system(11);
        t.color = selected ? Color::gray(255) : Color::gray(0);
        t.engraved = !selected;
        const double right = r.ejectable ? frame().w - 24 : frame().w - 6;
        drawText(cr, r.title, Rect{43, y, right - 43, RowHeight - 1}, t);
        if (r.ejectable) {
            // the eject button: a triangle over a bar, in the text's colour
            const double ex = frame().w - 18, ey = y + 5;
            const Color c = selected ? Color::gray(255) : Color::gray(80);
            c.set(cr);
            cairo_move_to(cr, ex, ey + 6);
            cairo_line_to(cr, ex + 5, ey);
            cairo_line_to(cr, ex + 10, ey + 6);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_rectangle(cr, ex, ey + 8, 10, 2);
            cairo_fill(cr);
        }
    }
}

std::string Sidebar::pathAt(Point p) const
{
    const int i = rowAt(p.y);
    if (i < 0 || i >= int(rows.size()) || rows[size_t(i)].header || rows[size_t(i)].path.rfind("search:", 0) == 0)
        return {};
    return rows[size_t(i)].path;
}

void Sidebar::mouseDown(Event &e)
{
    const int i = rowAt(e.location.y);
    if (i < 0 || i >= int(rows.size()) || rows[size_t(i)].header)
        return;
    if (rows[size_t(i)].ejectable && e.location.x >= frame().w - 22) {
        if (onEject)
            onEject(rows[size_t(i)]);
        return;
    }
    m_selected = i;
    setNeedsDisplay();
    if (onSelect)
        onSelect(rows[size_t(i)]);
}

void Sidebar::rightMouseDown(Event &e)
{
    const int i = rowAt(e.location.y);
    if (i < 0 || i >= int(rows.size()) || rows[size_t(i)].header)
        return;
    if (onContextMenu)
        onContextMenu(rows[size_t(i)], toWindow(e.location));
}

} // namespace Finder
