// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktop.h"

#include "foundation.h"

#include <algorithm>
#include <cmath>

using namespace AK;

namespace Desktop {

Settings load()
{
    AK::Settings q("desktop");
    Settings s;
    s.picture = q.string("picture");
    s.placement = std::clamp(q.integer("placement", FillScreen), 1, 5);
    const Color c = Color::parse(q.string("color"));
    if (c.a > 0)
        s.color = c;
    s.change = std::clamp(q.integer("change", 0), 0, int(changeIntervals().size()) - 1);
    s.random = q.boolean("random", false);
    s.changeFolder = q.string("changeFolder");
    s.folders = q.list("folders");
    return s;
}

void save(const Settings &s)
{
    AK::Settings q("desktop");
    q.set("picture", s.picture);
    q.set("placement", s.placement);
    q.set("color", s.color.name());
    q.set("change", s.change);
    q.set("random", s.random);
    q.set("changeFolder", s.changeFolder);
    q.set("folders", s.folders);
}

std::string picturePath(const Settings &s)
{
    if (!s.picture.empty() && isFile(s.picture))
        return s.picture;
    // 10.6's default: Aurora (DefaultDesktop.jpg stands for it)
    for (const char *p : {"/System/Library/CoreServices/DefaultDesktop.jpg", "/Library/Desktop Pictures/Nature/Aurora.jpg",
                          "/Library/Desktop Pictures/Aqua Blue.jpg"})
        if (Assets::exists(p))
            return Assets::path(p);
    return {};
}

Image render(const Settings &s, Size size)
{
    Image out(int(size.w), int(size.h));
    cairo_t *cr = cairo_create(out.surface());
    fill(cr, {0, 0, size.w, size.h}, s.color);
    const std::string path = picturePath(s);
    const Size own = Image::sizeOf(path);
    if (!own.empty()) {
        // the screen is 1280 wide here: the picture at that scale
        const double k = size.w / 1280;
        Image img = Image::load(path, s.placement == Center || s.placement == Tile ? Size{own.w * k, own.h * k} : size);
        if (!img.isNull()) {
            Rect dst;
            switch (s.placement) {
            case FitToScreen: {
                const Size t = scaledToFit(own, size);
                dst = {std::floor((size.w - t.w) / 2), std::floor((size.h - t.h) / 2), t.w, t.h};
                break;
            }
            case StretchToFillScreen:
                dst = {0, 0, size.w, size.h};
                break;
            case Center:
                dst = {std::floor((size.w - own.w * k) / 2), std::floor((size.h - own.h * k) / 2), own.w * k, own.h * k};
                break;
            case Tile:
                for (double y = 0; y < size.h; y += own.h * k)
                    for (double x = 0; x < size.w; x += own.w * k)
                        img.draw(cr, Rect{x, y, own.w * k, own.h * k});
                break;
            default: {
                const Size t = scaledToFill(own, size);
                dst = {std::floor((size.w - t.w) / 2), std::floor((size.h - t.h) / 2), t.w, t.h};
            }
            }
            if (s.placement != Tile)
                img.draw(cr, dst);
        }
    }
    cairo_destroy(cr);
    return out;
}

std::vector<Source> appleSources()
{
    static const char *const folders[][2] = {{"", "Desktop Pictures"}, {"/Nature", "Nature"}, {"/Plants", "Plants"},
                                             {"/Art", "Art"}, {"/Black & White", "Black & White"}, {"/Abstract", "Abstract"},
                                             {"/Patterns", "Patterns"}, {"/Solid Colors", "Solid Colors"}};
    std::vector<Source> out;
    for (const auto &f : folders) {
        const std::string path = Assets::path(std::string("/Library/Desktop Pictures") + f[0]);
        if (isDir(path))
            out.push_back({f[1], path});
    }
    return out;
}

std::vector<std::string> pictures(const std::string &folder)
{
    std::vector<std::string> out;
    for (const std::string &f : files(folder)) {
        std::string ext = f.substr(f.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == "jpg" || ext == "jpeg" || ext == "png")
            out.push_back(f);
    }
    return out;
}

const std::vector<Interval> &changeIntervals()
{
    static const std::vector<Interval> list = {{"", 0},
                                               {"When logging in", -1},
                                               {"When waking from sleep", -2},
                                               {"Every 5 seconds", 5},
                                               {"Every minute", 60},
                                               {"Every 5 minutes", 300},
                                               {"Every 15 minutes", 900},
                                               {"Every 30 minutes", 1800},
                                               {"Every hour", 3600},
                                               {"Every day", 86400}};
    return list;
}

Image thumbnail(const std::string &path, Size size)
{
    const std::string dir = cacheDir() + "/thumbnails";
    const std::string file = dir + '/' + hashName(path + '@' + std::to_string(modified(path)) + '@' + std::to_string(int(size.w))) + ".png";
    Image cached = Image::load(file);
    if (!cached.isNull())
        return cached;
    Image img = Image::load(path, size);
    if (img.isNull())
        return {};
    const Size t = scaledToFill(img.size(), size);
    Image small = img.width() > t.w * 1.5 ? img.scaled(int(t.w), int(t.h)) : img;
    makePath(dir);
    small.savePng(file);
    return small;
}

} // namespace Desktop

namespace ScreenSaver {

std::string folder()
{
    const std::string f = AK::Settings("screensaver").string("folder");
    if (!f.empty())
        return f;
    const auto sources = Desktop::appleSources();
    return sources.size() > 1 ? sources[1].path : sources.empty() ? std::string() : sources[0].path;   // Nature
}

int idleSeconds() { return AK::Settings("screensaver").integer("idle", 20 * 60); }
void setFolder(const std::string &f) { AK::Settings("screensaver").set("folder", f); }
void setIdleSeconds(int s) { AK::Settings("screensaver").set("idle", s); }

} // namespace ScreenSaver
