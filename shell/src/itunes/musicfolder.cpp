// SPDX-License-Identifier: LGPL-2.1-or-later
#include "musicfolder.h"

#include "foundation.h"
#include "library.h"
#include "resources.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sys/statvfs.h>

using namespace AK;

namespace iTunes {
namespace MusicFolder {

namespace {

// UTF-8 to code points and back: the names are measured in UTF-16 units as iTunes' Str255 of UniChar
std::vector<char32_t> decode(const std::string &s)
{
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + size_t(k)]) & 0x3f);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

std::string encode(const std::vector<char32_t> &v)
{
    std::string o;
    for (char32_t cp : v) {
        if (cp < 0x80)
            o += char(cp);
        else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6));
            o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18));
            o += char(0x80 | ((cp >> 12) & 0x3F));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

size_t units(const std::vector<char32_t> &v)
{
    size_t n = 0;
    for (char32_t c : v)
        n += c >= 0x10000 ? 2 : 1;
    return n;
}

// FUN_00254132: the characters a file name cannot have, and "." or " " first or last, made "_"
void sanitize(std::vector<char32_t> &v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        const char32_t c = v[i];
        const bool bad = c == ':' || c == '/' || c == '\\' || c == '<' || c == '>' || c == '"' || c == '|' || c == '*' || c == '?' ||
                         c < 0x20 || c == 0xB4 || c == 0xD8 || (c >= 0x2018 && c <= 0x201F) ||
                         ((c == '.' || c == ' ') && (i == 0 || i + 1 == v.size()));
        if (bad)
            v[i] = '_';
    }
}

// FUN_0025400e: the names DOS keeps (the table at 0xabff60), the part before the extension compared
// without case; its last character made "_"
void reserved(std::vector<char32_t> &v)
{
    static const char *names[] = {"CON",  "PRN",  "AUX",  "NUL",  "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
                                  "LPT8", "LPT9", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9"};
    size_t end = v.size();
    for (size_t i = v.size(); i-- > 0;)
        if (v[i] == '.') {
            end = i;
            break;
        }
    std::string base;
    for (size_t i = 0; i < end; ++i)
        base += v[i] < 0x80 ? char(std::toupper(int(v[i]))) : '\x7f';
    for (const char *n : names)
        if (base == n) {
            v[end - 1] = '_';
            return;
        }
}

bool isSpace(char32_t c) { return c == ' ' || c == '\t' || c == 0xA0 || (c >= 0x2000 && c <= 0x200A) || c == 0x3000; }

std::string fileExt(const std::string &path)
{
    const std::string f = fileName(path);
    const size_t dot = f.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : f.substr(dot + 1);
}

} // namespace

std::string defaultPath() { return Library::musicFolder() + "/iTunes/iTunes Music"; }

std::string path()
{
    std::string p = Settings("itunes").string("MusicFolder");
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return p.empty() ? defaultPath() : p;
}

void setPath(const std::string &folder) { Settings("itunes").set("MusicFolder", folder == defaultPath() ? std::string() : folder); }

bool keepOrganized() { return Settings("itunes").boolean("KeepOrganized", true); }
bool copyWhenAdding() { return Settings("itunes").boolean("CopyFiles", true); }

bool contains(const std::string &file, const std::string &root)
{
    const std::string r = root.empty() || root.back() == '/' ? root : root + "/";
    return !r.empty() && file.rfind(r, 0) == 0;
}

std::string safeName(const std::string &name, const std::string &extension, int number, size_t limit)
{
    if (name.empty())
        return {};
    const std::vector<char32_t> n = decode(name);
    const std::vector<char32_t> suffix = number ? decode(" " + std::to_string(number)) : std::vector<char32_t>{};
    std::vector<char32_t> ext;
    if (!extension.empty() && extension != ".")
        ext = decode(extension[0] == '.' ? extension : "." + extension);
    size_t length = n.size();
    const size_t whole = units(n) + suffix.size() + units(ext);
    if (whole > limit) {
        const size_t over = whole - limit;
        if (n.size() <= over)
            return {};
        length = n.size() - over;
    }
    const size_t room = limit >= suffix.size() + units(ext) ? limit - suffix.size() - units(ext) : 0;
    for (; length > 0; --length) {
        std::vector<char32_t> part(n.begin(), n.begin() + long(length));
        while (!part.empty() && isSpace(part.back()))   // FUN_0028e86a: the trailing spaces
            part.pop_back();
        if (part.empty())
            part.assign(n.begin(), n.begin() + long(length));
        // the port's own limit beside iTunes' one: Linux's 255 bytes a name
        if (units(part) > room || encode(part).size() + suffix.size() + encode(ext).size() > 255)
            continue;
        part.insert(part.end(), suffix.begin(), suffix.end());
        part.insert(part.end(), ext.begin(), ext.end());
        sanitize(part);
        reserved(part);
        return encode(part);
    }
    return {};
}

std::vector<std::string> folders(const Track &t)
{
    const int k = t.mediaKind;
    std::vector<std::string> out;
    // the first level
    if (k & 4)
        out.push_back(tr("128.005"));   // Podcasts
    else if (k & 2)
        out.push_back("Movies");
    else if (k & 0x40)
        out.push_back("TV Shows");
    else if (k & 0x4000)
        out.push_back("Ringtones");
    else if (k & 0x100000)
        out.push_back("Voice Memos");
    else if (t.compilation)
        out.push_back(tr("128.006"));   // Compilations
    else {
        // FUN_001751b6: the album artist, else the artist
        const std::string &a = !t.albumArtist.empty() ? t.albumArtist : t.artist;
        out.push_back(a.empty() ? tr("128.002") : a);
    }
    // the second
    if (!(k & 0x104000) && (!(k & 2) || (k & 4))) {
        if (!(k & 0x40) || (k & 4))
            out.push_back(!t.album.empty() ? t.album : tr((k & 4) ? "128.004" : "128.001"));
        else if (!t.series.empty())
            out.push_back(t.series);
    }
    for (std::string &f : out)
        f = safeName(f, {}, 0, size_t(-1));
    out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());
    return out;
}

std::string fileBase(const Track &t)
{
    std::string prefix;
    if (t.trackNumber && Settings("itunes").boolean("FileNamesWithTrackNumber", true)) {
        char buf[24];
        if (t.discCount == 1 || t.discNumber == 0)
            std::snprintf(buf, sizeof buf, "%02d ", t.trackNumber);
        else
            std::snprintf(buf, sizeof buf, "%d-%02d ", t.discNumber, t.trackNumber);
        prefix = buf;
    }
    // FUN_001832e2: the song's name, else the file's own
    return prefix + (!t.name.empty() ? t.name : baseName(t.path));
}

std::string destination(const Track &t, const std::string &extension, const std::string &current, const std::string &root)
{
    std::string dir = root;
    for (const std::string &f : folders(t))
        dir += "/" + f;
    const std::string ext = !extension.empty() ? extension : fileExt(t.path);
    for (int number = 0; number < 10000; ++number) {   // FUN_0025647c: 0 (none), then 1, 2…
        const std::string name = safeName(fileBase(t), ext, number);
        if (name.empty())
            return {};
        const std::string p = dir + "/" + name;
        if (p == current || !std::filesystem::exists(std::filesystem::symlink_status(p)))
            return p;
    }
    return {};
}

bool copyFile(const std::string &from, const std::string &to)
{
    std::error_code ec;
    std::filesystem::create_directories(dirName(to), ec);
    return std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, ec) && !ec;
}

bool moveFile(const std::string &from, const std::string &to, const std::string &root)
{
    if (from == to)
        return true;
    std::error_code ec;
    std::filesystem::create_directories(dirName(to), ec);
    std::filesystem::rename(from, to, ec);
    if (ec) {
        if (!copyFile(from, to))
            return false;
        std::filesystem::remove(from, ec);
    }
    // the folders left empty, up to the music folder (FUN_00255ce2 fails on one that is not)
    for (std::string d = dirName(from); contains(d, root) && d != root; d = dirName(d))
        if (!std::filesystem::is_empty(d, ec) || ec || !std::filesystem::remove(d, ec))
            break;
    return true;
}

long long freeSpace(const std::string &dir)
{
    std::string d = dir;
    while (!d.empty() && !isDir(d))
        d = dirName(d);
    struct statvfs s;
    if (d.empty() || ::statvfs(d.c_str(), &s) != 0)
        return -1;
    return static_cast<long long>(s.f_bavail) * static_cast<long long>(s.f_frsize);
}

} // namespace MusicFolder
} // namespace iTunes
