// SPDX-License-Identifier: LGPL-2.1-or-later
#include "library.h"

#include "foundation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <turbojpeg.h>
#include <unistd.h>

namespace PB {

using namespace AK;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string extension(const std::string &name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}

bool isPicture(const std::string &name)
{
    const std::string e = extension(name);
    return e == "jpg" || e == "jpeg" || e == "png" || e == "tif" || e == "tiff";
}

std::string xmlEscape(const std::string &s)
{
    std::string out;
    for (char c : s)
        out += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
    return out;
}

std::string xmlUnescape(std::string s)
{
    static const std::pair<const char *, const char *> table[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto &[from, to] : table)
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::strlen(to))
            s.replace(at, std::strlen(from), to);
    return s;
}

} // namespace

Image mirrored(const Image &image)
{
    if (image.isNull())
        return image;
    Image out(image.width(), image.height());
    cairo_t *cr = cairo_create(out.surface());
    cairo_translate(cr, image.width(), 0);
    cairo_scale(cr, -1, 1);
    image.draw(cr, Point{0, 0});
    cairo_destroy(cr);
    return out;
}

Image fourUp(const std::vector<Image> &pictures, int width, int height)
{
    Image out(width, height);
    cairo_t *cr = cairo_create(out.surface());
    fill(cr, Rect{0, 0, double(width), double(height)}, Color::gray(0));
    for (size_t i = 0; i < pictures.size() && i < 4; ++i)
        if (!pictures[i].isNull())
            pictures[i].draw(cr, Rect{double(i % 2) * width / 2, double(i / 2) * height / 2, width / 2.0, height / 2.0});
    cairo_destroy(cr);
    return out;
}

bool saveJpeg(const Image &image, const std::string &path)
{
    if (image.isNull())
        return false;
    cairo_surface_flush(image.surface());
    tjhandle tj = tjInitCompress();
    unsigned char *jpeg = nullptr;
    unsigned long size = 0;
    const int r = tjCompress2(tj, cairo_image_surface_get_data(image.surface()), image.width(), cairo_image_surface_get_stride(image.surface()),
                              image.height(), TJPF_BGRX, &jpeg, &size, TJSAMP_444, 90, 0);
    tjDestroy(tj);
    bool ok = false;
    if (r == 0 && jpeg) {
        // whole or not at all (writeToFile:atomically:)
        const std::string tmp = path + ".part";
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char *>(jpeg), std::streamsize(size));
        out.close();
        ok = out.good() && std::rename(tmp.c_str(), path.c_str()) == 0;
        if (!ok)
            std::remove(tmp.c_str());
    }
    tjFree(jpeg);
    return ok;
}

Library::Library()
{
    // FUN_10001224e: the Pictures folder's "Photo Booth", made when it isn't there
    m_root = picturesDir() + "/Photo Booth";
    makePath(m_root);
    load();
}

std::string Library::path(int index, int file) const
{
    if (index < 0 || index >= int(m_assets.size()) || file < 0 || file >= int(m_assets[index].files.size()))
        return {};
    return m_root + "/" + m_assets[index].files[file];
}

// -[PBAssetCollection _refresh]: Recents.plist's entries whose files are still there
void Library::load()
{
    m_assets.clear();
    std::ifstream in(m_root + "/Recents.plist", std::ios::binary);
    if (in) {
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string xml = buffer.str();
        int depth = 0;
        std::vector<std::string> group;
        auto add = [this](std::vector<std::string> names) {
            names.erase(std::remove_if(names.begin(), names.end(), [this](const std::string &n) { return !isFile(m_root + "/" + n); }), names.end());
            if (names.empty())
                return;
            Asset a;
            a.movie = extension(names.front()) == "mov";
            if (!a.movie && !isPicture(names.front()))
                return;
            if (names.size() > 1 && names.size() < 4)   // a 4-up with a picture missing is left out
                return;
            a.files = std::move(names);
            m_assets.push_back(std::move(a));
        };
        for (size_t at = 0; (at = xml.find('<', at)) != std::string::npos;) {
            const size_t end = xml.find('>', at);
            if (end == std::string::npos)
                break;
            const std::string tag = xml.substr(at + 1, end - at - 1);
            at = end + 1;
            if (tag == "array") {
                ++depth;
            } else if (tag == "/array") {
                if (depth == 2) {
                    add(group);
                    group.clear();
                }
                --depth;
            } else if (tag == "string") {
                const size_t close = xml.find("</string>", at);
                if (close == std::string::npos)
                    break;
                const std::string name = xmlUnescape(xml.substr(at, close - at));
                at = close + 9;
                if (depth == 1)
                    add({name});
                else if (depth == 2)
                    group.push_back(name);
            }
        }
    } else {
        // no Recents.plist (a folder filled by hand): its pictures, oldest first
        std::vector<std::string> names;
        for (const std::string &f : files(m_root))
            if (isPicture(fileName(f)))
                names.push_back(fileName(f));
        std::stable_sort(names.begin(), names.end(), [this](const std::string &a, const std::string &b) {
            return modified(m_root + "/" + a) < modified(m_root + "/" + b);
        });
        for (const std::string &n : names) {
            Asset a;
            a.files = {n};
            m_assets.push_back(std::move(a));
        }
    }
    for (Asset &a : m_assets)
        loadPreview(a);
}

// -[PBAssetCollection _writeToRecentsPlist]
void Library::save() const
{
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n<array>\n";
    for (const Asset &a : m_assets) {
        if (a.files.size() == 1) {
            xml += "\t<string>" + xmlEscape(a.files[0]) + "</string>\n";
        } else {
            xml += "\t<array>\n";
            for (const std::string &f : a.files)
                xml += "\t\t<string>" + xmlEscape(f) + "</string>\n";
            xml += "\t</array>\n";
        }
    }
    xml += "</array>\n</plist>\n";
    const std::string file = m_root + "/Recents.plist", tmp = file + ".part";
    std::ofstream out(tmp, std::ios::binary);
    out << xml;
    out.close();
    if (out.good())
        std::rename(tmp.c_str(), file.c_str());
}

void Library::loadPreview(Asset &asset) const
{
    if (asset.movie)
        return;
    // -[PBImageAsset loadPreviewImage]: a thumbnail of 160 pixels at most
    auto small = [this](const std::string &name) {
        const Image full = Image::load(m_root + "/" + name, Size{160, 120});
        if (full.isNull())
            return full;
        const Size s = scaledToFit(full.size(), Size{160, 160});
        return full.scaled(std::max(1, int(s.w)), std::max(1, int(s.h)));
    };
    if (asset.files.size() >= 4) {
        std::vector<Image> four;
        for (const std::string &f : asset.files)
            four.push_back(small(f));
        asset.preview = fourUp(four, 160, 120);
    } else {
        asset.preview = small(asset.files[0]);
    }
}

// +[PBAsset generateAssetFilenameForAssetClass:]: the template with the date ("YYYY-MM-dd") and the
// time ("HH.mm"); " #n" when the name is taken
std::string Library::newName(const std::string &templ, const std::string &suffix) const
{
    char date[32], hour[32], name[256];
    const std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
    std::strftime(hour, sizeof hour, "%H.%M", &tm);
    std::snprintf(name, sizeof name, templ.c_str(), date, hour);
    std::string base = name;
    for (int n = 2; isFile(m_root + "/" + base + suffix + ".jpg"); ++n)
        base = std::string(name) + " #" + std::to_string(n);
    return base;
}

bool Library::addPhoto(const Image &photo)
{
    Asset a;
    a.files = {newName("Photo on %s at %s", "") + ".jpg"};
    if (!saveJpeg(photo, m_root + "/" + a.files[0]))
        return false;
    loadPreview(a);
    m_assets.push_back(std::move(a));
    save();
    return true;
}

bool Library::addFourUp(const std::vector<Image> &photos)
{
    if (photos.size() < 4)
        return false;
    const std::string base = newName("4-up on %s at %s", " #1");
    Asset a;
    for (int i = 0; i < 4; ++i) {
        a.files.push_back(base + " #" + std::to_string(i + 1) + ".jpg");
        if (!saveJpeg(photos[i], m_root + "/" + a.files.back()))
            return false;
    }
    loadPreview(a);
    m_assets.push_back(std::move(a));
    save();
    return true;
}

void Library::remove(int index)
{
    if (index < 0 || index >= int(m_assets.size()))
        return;
    // the desktop's trash (the freedesktop Trash specification): files/ and info/<name>.trashinfo
    const char *data = std::getenv("XDG_DATA_HOME");
    const std::string trash = (data && *data ? std::string(data) : home() + "/.local/share") + "/Trash";
    makePath(trash + "/files");
    makePath(trash + "/info");
    for (const std::string &name : m_assets[index].files) {
        const std::string from = m_root + "/" + name;
        std::string to = name;
        for (int n = 2; isFile(trash + "/files/" + to) || isFile(trash + "/info/" + to + ".trashinfo"); ++n)
            to = baseName(name) + " " + std::to_string(n) + "." + extension(name);
        char when[32];
        const std::time_t t = std::time(nullptr);
        std::tm tm {};
        localtime_r(&t, &tm);
        std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
        std::string encoded;
        for (unsigned char c : from) {
            char hex[4];
            if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
                encoded += char(c);
            else {
                std::snprintf(hex, sizeof hex, "%%%02X", c);
                encoded += hex;
            }
        }
        std::ofstream info(trash + "/info/" + to + ".trashinfo");
        info << "[Trash Info]\nPath=" << encoded << "\nDeletionDate=" << when << "\n";
        info.close();
        if (std::rename(from.c_str(), (trash + "/files/" + to).c_str()) != 0)
            std::remove((trash + "/info/" + to + ".trashinfo").c_str());   // another volume: the picture stays
    }
    m_assets.erase(m_assets.begin() + index);
    save();
}

void Library::removeAll()
{
    while (!m_assets.empty())
        remove(int(m_assets.size()) - 1);
}

void Library::flip(int index)
{
    if (index < 0 || index >= int(m_assets.size()) || m_assets[index].movie)
        return;
    for (const std::string &name : m_assets[index].files)
        if (extension(name) == "jpg" || extension(name) == "jpeg")
            saveJpeg(mirrored(Image::load(m_root + "/" + name)), m_root + "/" + name);
    loadPreview(m_assets[index]);
}

Image Library::fullImage(int index) const
{
    if (index < 0 || index >= int(m_assets.size()) || m_assets[index].movie)
        return {};
    const Asset &a = m_assets[index];
    if (a.files.size() < 4)
        return Image::load(m_root + "/" + a.files[0]);
    std::vector<Image> four;
    for (const std::string &f : a.files)
        four.push_back(Image::load(m_root + "/" + f));
    return four[0].isNull() ? Image() : fourUp(four, four[0].width(), four[0].height());
}

} // namespace PB
