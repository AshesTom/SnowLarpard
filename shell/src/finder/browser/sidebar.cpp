// SPDX-License-Identifier: LGPL-2.1-or-later
#include "sidebar.h"

#include "files.h"

#include "foundation.h"
#include "text.h"

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

void Sidebar::reload()
{
    rows.clear();
    rows.push_back({core("SD5", "DEVICES"), {}, {}, true});
    for (const Volume &v : volumes())
        rows.push_back({v.name, v.path, volumeIcon(v, 16), false});

    // PLACES: the folders' toolbar icons (CoreTypes' Toolbar*FolderIcon, their 16-point pictures), as
    // 10.6's sidebar shows them, not their Finder icons
    rows.push_back({core("SD8", "PLACES"), {}, {}, true});
    const std::string h = home();
    auto place = [&](const std::string &path, const std::string &title, const char *icon) {
        if (isDir(path))
            rows.push_back({title, path, coreTypesIcon(icon, 16), false});
    };
    place(userDir("DESKTOP", h + "/Desktop"), "Desktop", "ToolbarDesktopFolderIcon");
    place(h, fileName(h), "HomeFolderIcon");
    rows.push_back({"Applications", "/usr/share/applications", coreTypesIcon("ToolbarAppsFolderIcon", 16), false});
    place(userDir("DOCUMENTS", h + "/Documents"), "Documents", "ToolbarDocumentsFolderIcon");

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

void Sidebar::draw(cairo_t *cr)
{
    const bool active = windowIsKey();
    // kCUIVariantGradientSideBar: _kPairs 0x1A (active), 0x1C (inactive)
    fill(cr, bounds(), active ? Color::rgb(214, 221, 229) : Color::gray(232));
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row &r = rows[i];
        const double y = 2 + i * RowHeight;
        const bool selected = int(i) == m_selected;
        if (selected) {
            // kCUIVariantGradientSideBarSelection: the key window's _kPairs 0x10 the gradient, 0x11 its top
            // line (the bright blue a 10.6 Finder shows); an inactive window's 0x18 and 0x19
            const Color top = active ? Color::rgb(91, 147, 213) : Color::gray(181);
            const Color bottom = active ? Color::rgb(22, 83, 170) : Color::gray(137);
            const Color line = active ? Color::rgb(69, 128, 200) : Color::gray(151);
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
            // down, 7 wide, in the header's colour); places measured on a 10.6 screenshot
            t.font = Font::boldSystem(11);
            t.color = active ? Color::rgb(112, 126, 140) : Color::gray(130);
            t.engraved = true;
            t.engravedAlpha = 0.6;
            for (int pass = 0; pass < 2; ++pass) {
                const double ty = y + 9 + (pass == 0 ? 1 : 0);
                cairo_move_to(cr, 9, ty);
                cairo_line_to(cr, 16, ty);
                cairo_line_to(cr, 12.5, ty + 6);
                cairo_close_path(cr);
                if (pass == 0)
                    cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
                else
                    t.color.set(cr);
                cairo_fill(cr);
            }
            drawText(cr, r.title, Rect{20, y + 3, frame().w - 24, RowHeight - 3}, t);
            continue;
        }
        if (!r.icon.isNull())
            r.icon.draw(cr, Rect{23, y + 2, 16, 16});
        t.font = selected ? Font::boldSystem(11) : Font::system(11);
        t.color = selected ? Color::gray(255) : Color::gray(0);
        t.engraved = !selected;
        drawText(cr, r.title, Rect{43, y + 1, frame().w - 43 - 6, RowHeight - 1}, t);
    }
}

std::string Sidebar::pathAt(Point p) const
{
    const int i = int(std::floor((p.y - 2) / RowHeight));
    if (i < 0 || i >= int(rows.size()) || rows[size_t(i)].header || rows[size_t(i)].path.rfind("search:", 0) == 0)
        return {};
    return rows[size_t(i)].path;
}

void Sidebar::mouseDown(Event &e)
{
    const int i = int(std::floor((e.location.y - 2) / RowHeight));
    if (i < 0 || i >= int(rows.size()) || rows[size_t(i)].header)
        return;
    m_selected = i;
    setNeedsDisplay();
    if (onSelect)
        onSelect(rows[size_t(i)]);
}

} // namespace Finder
