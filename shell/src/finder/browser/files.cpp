// SPDX-License-Identifier: LGPL-2.1-or-later
#include "files.h"

#include "foundation.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

using namespace AK;

namespace Finder {

namespace {

const char *const CoreTypes = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/";

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string extension(const std::string &name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot + 1));
}

Image iconFile(const std::string &macPath, int size)
{
    static std::map<std::string, Image> cache;
    const std::string key = macPath + '@' + std::to_string(size);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    Image img = Image::load(Assets::path(macPath), {double(size), double(size)});
    if (!img.isNull() && (img.width() != size || img.height() != size))
        img = img.scaled(size, size);
    return cache[key] = img;
}

// the home folder's folders the Mac names, by their icon (the xdg ones stand for them)
std::string specialFolderIcon(const std::string &path)
{
    const std::string h = home();
    if (path == h)
        return "HomeFolderIcon";
    const std::map<std::string, std::string> xdg = {
        {userDir("DESKTOP", h + "/Desktop"), "DesktopFolderIcon"},
        {userDir("DOCUMENTS", h + "/Documents"), "DocumentsFolderIcon"},
        {userDir("DOWNLOAD", h + "/Downloads"), "DownloadsFolder"},
        {userDir("MUSIC", h + "/Music"), "MusicFolderIcon"},
        {userDir("PICTURES", h + "/Pictures"), "PicturesFolderIcon"},
        {userDir("VIDEOS", h + "/Movies"), "MovieFolderIcon"},
        {userDir("PUBLICSHARE", h + "/Public"), "PublicFolderIcon"},
        {h + "/Library", "LibraryFolderIcon"},
        {h + "/Sites", "SitesFolderIcon"},
        {h + "/Applications", "ApplicationsFolderIcon"},
    };
    auto it = xdg.find(path);
    if (it != xdg.end())
        return it->second;
    if (path == "/Applications" || path == "/usr/share/applications")
        return "ApplicationsFolderIcon";
    if (path == "/home" || path == "/Users")
        return "UsersFolderIcon";
    if (path == "/System" || path == "/usr")
        return "SystemFolderIcon";
    return "GenericFolderIcon";
}

bool isPicture(const std::string &ext)
{
    static const std::set<std::string> pictures = {"png", "jpg", "jpeg", "tif", "tiff", "icns"};
    return pictures.count(ext) > 0;
}

// a .desktop entry's Name and whether it is shown (NoDisplay, Hidden)
bool desktopEntry(const std::string &path, std::string &name)
{
    std::ifstream in(path);
    std::string line;
    bool inEntry = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            inEntry = line == "[Desktop Entry]";
            continue;
        }
        if (!inEntry)
            continue;
        if (line.rfind("Name=", 0) == 0 && name.empty())
            name = line.substr(5);
        else if (line == "NoDisplay=true" || line == "Hidden=true" || line == "Type=Link")
            return false;
    }
    return !name.empty();
}

} // namespace

std::string userDir(const std::string &xdgName, const std::string &fallback)
{
    // user-dirs.dirs: XDG_<NAME>_DIR="$HOME/..."
    const char *x = std::getenv("XDG_CONFIG_HOME");
    std::ifstream in((x && *x ? std::string(x) : home() + "/.config") + "/user-dirs.dirs");
    const std::string key = "XDG_" + xdgName + "_DIR=";
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) != 0)
            continue;
        std::string v = line.substr(key.size());
        if (v.size() >= 2 && v.front() == '"')
            v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0)
            v = home() + v.substr(5);
        while (v.size() > 1 && v.back() == '/')
            v.pop_back();
        return v;
    }
    return fallback;
}

std::vector<Item> list(const std::string &folder)
{
    std::vector<Item> out;
    DIR *d = opendir(folder.c_str());
    if (!d)
        return out;
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        Item it;
        it.name = name;
        it.path = folder == "/" ? "/" + name : folder + "/" + name;
        it.folder = isDir(it.path);
        if (!it.folder && extension(name) == "desktop") {
            std::string shown;
            if (!desktopEntry(it.path, shown))
                continue;
            it.name = shown;
            it.application = true;
        }
        out.push_back(it);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Item &a, const Item &b) { return finderLess(a.name, b.name); });
    return out;
}

Image coreTypesIcon(const std::string &name, int size) { return iconFile(CoreTypes + name + ".icns", size); }

Image icon(const Item &item, int size, bool preview)
{
    if (item.folder)
        return coreTypesIcon(specialFolderIcon(item.path), size);
    const std::string ext = extension(item.name);
    if (preview && isPicture(ext)) {
        // the icon preview: the picture itself, fitted in the icon's square (kept small in memory)
        static std::map<std::string, Image> previews;
        const std::string key = item.path + '@' + std::to_string(size) + '@' + std::to_string(modified(item.path));
        auto it = previews.find(key);
        if (it != previews.end())
            return it->second;
        struct stat st;
        Image img;
        if (stat(item.path.c_str(), &st) == 0 && st.st_size < 40LL * 1024 * 1024) {
            Image full = Image::load(item.path, {double(size), double(size)});
            if (!full.isNull()) {
                const Size s = scaledToFit(full.size(), {double(size), double(size)});
                img = full.scaled(std::max(1, int(std::lround(s.w))), std::max(1, int(std::lround(s.h))));
            }
        }
        if (previews.size() > 400)
            previews.clear();
        if (!img.isNull())
            return previews[key] = img;
    }
    // the documents Preview opens: its own icons (its Info.plist's CFBundleTypeIconFile)
    static const std::map<std::string, std::string> previewTypes = {
        {"pdf", "pdf"}, {"png", "png"}, {"jpg", "jpeg"}, {"jpeg", "jpeg"}, {"tif", "tiff"}, {"tiff", "tiff"},
        {"gif", "gif"}, {"bmp", "bmp"}, {"psd", "psd"},  {"icns", "icns"}, {"ico", "ico"},   {"tga", "tga"},
    };
    auto type = previewTypes.find(ext);
    if (type != previewTypes.end()) {
        const Image doc = iconFile("/Applications/Preview.app/Contents/Resources/" + type->second + ".icns", size);
        if (!doc.isNull())
            return doc;
    }
    if (item.application)
        return coreTypesIcon("GenericApplicationIcon", size);
    if (ext.empty() && access(item.path.c_str(), X_OK) == 0)
        return coreTypesIcon("ExecutableBinaryIcon", size);
    return coreTypesIcon("GenericDocumentIcon", size);
}

std::string displayName(const std::string &path)
{
    const char *data = std::getenv("XDG_DATA_HOME");
    if (path == (data && *data ? std::string(data) : home() + "/.local/share") + "/Trash/files")
        return "Trash";
    if (path == "/") {
        const std::vector<Volume> v = volumes();
        return v.empty() ? "/" : v.front().name;
    }
    return fileName(path);
}

std::vector<Volume> volumes()
{
    std::vector<Volume> out;
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    std::map<std::string, std::string> labels;   // device -> its label (udev's by-label links)
    for (const std::string &label : files("/dev/disk/by-label")) {
        char target[4096];
        if (realpath(("/dev/disk/by-label/" + label).c_str(), target)) {
            std::string name = label;
            for (size_t at; (at = name.find("\\x20")) != std::string::npos;)
                name.replace(at, 4, " ");
            labels[target] = name;
        }
    }
    std::set<std::string> seen;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string id, parent, dev, root, point, field;
        ss >> id >> parent >> dev >> root >> point;
        while (ss >> field && field != "-") {
        }
        std::string fstype, source;
        ss >> fstype >> source;
        // a disk's own root (not a bind mount); "/" whatever its subvolume (btrfs mounts "/@" there)
        if (source.rfind("/dev/", 0) != 0 || (root != "/" && point != "/"))
            continue;
        const bool removable = point.rfind("/run/media/", 0) == 0 || point.rfind("/media/", 0) == 0;
        if (point != "/" && !removable && point.rfind("/mnt/", 0) != 0)
            continue;
        char real[4096];
        const std::string device = realpath(source.c_str(), real) ? std::string(real) : source;
        if (!seen.insert(device).second)
            continue;
        Volume v;
        v.path = point;
        auto l = labels.find(device);
        v.name = l != labels.end() ? l->second : (point == "/" ? fileName(device) : fileName(point));
        v.kind = point == "/" ? Volume::Internal : removable ? Volume::Removable : Volume::External;
        if (point == "/")
            out.insert(out.begin(), v);
        else
            out.push_back(v);
    }
    if (out.empty() || out.front().path != "/")
        out.insert(out.begin(), Volume{"/", "/", Volume::Internal});
    return out;
}

Image volumeIcon(const Volume &v, int size)
{
    // IOStorageFamily.kext's icons (the IOMedia's kind: internal, external, removable)
    const char *name = v.kind == Volume::Internal ? "Internal" : v.kind == Volume::Removable ? "Removable" : "External";
    return iconFile(std::string("/System/Library/Extensions/IOStorageFamily.kext/Contents/Resources/") + name + ".icns", size);
}

long long freeSpace(const std::string &path)
{
    struct statvfs s;
    if (statvfs(path.c_str(), &s) != 0)
        return -1;
    return (long long)s.f_bavail * (long long)s.f_frsize;
}

std::string formatBytes(long long bytes)
{
    // NSLocalizedFileSizeDescription with the Finder's options (TFileSizeFormatter: 0x200000): base
    // 1000, the unit's fraction digits (_numberOfFractionDigits: bytes 0, KB 0, MB 1, GB and up 2), the
    // next unit once the rounded value reaches 1000; zero is "Zero KB"; trailing zeros dropped
    if (bytes <= 0)
        return "Zero KB";
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB", "EB"};
    static const int digits[] = {0, 0, 1, 2, 2, 2, 2};
    double v = double(bytes);
    int u = 0;
    auto rounded = [&](double x, int d) { const double f = std::pow(10.0, d); return std::round(x * f) / f; };
    while (u < 6 && rounded(v, digits[u]) >= 1000) {
        v /= 1000;
        ++u;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits[u], rounded(v, digits[u]));
    std::string n = buf;
    if (n.find('.') != std::string::npos) {
        while (n.back() == '0')
            n.pop_back();
        if (n.back() == '.')
            n.pop_back();
    }
    return n + " " + units[u];
}

long long sizeOnDisk(const Item &item)
{
    struct stat st;
    return stat(item.path.c_str(), &st) == 0 ? (long long)st.st_blocks * 512 : -1;
}

long long modificationTime(const Item &item)
{
    struct stat st;
    return stat(item.path.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
}

long long creationTime(const Item &item)
{
    struct statx sx;
    if (statx(AT_FDCWD, item.path.c_str(), 0, STATX_BTIME, &sx) == 0 && (sx.stx_mask & STATX_BTIME))
        return (long long)sx.stx_btime.tv_sec;
    return modificationTime(item);
}

long long accessTime(const Item &item)
{
    struct stat st;
    return stat(item.path.c_str(), &st) == 0 ? (long long)st.st_atime : 0;
}

// ---- Kind: LaunchServices' rule (LSCopyKindStringForURL): the claiming application's document type
// name, else the type's description (CoreTypes' UTTypeDescription), else "%@ Document"

namespace {

// a small JSON reader for extract.py's plists
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string string;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;
    const Json *get(const std::string &key) const
    {
        for (const auto &kv : object)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
    // a string or an array of strings, as the plists put one or several
    std::vector<std::string> strings() const
    {
        if (type == String)
            return {string};
        std::vector<std::string> out;
        for (const Json &j : array)
            if (j.type == String)
                out.push_back(j.string);
        return out;
    }
};

struct JsonReader {
    const std::string &t;
    size_t i = 0;
    void space()
    {
        while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i])))
            ++i;
    }
    std::string str()
    {
        std::string out;
        ++i;
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < t.size()) {
                const char c = t[++i];
                if (c == 'u' && i + 4 < t.size()) {
                    const unsigned cp = unsigned(std::stoul(t.substr(i + 1, 4), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) {
                        out += char(cp);
                    } else if (cp < 0x800) {
                        out += char(0xC0 | (cp >> 6));
                        out += char(0x80 | (cp & 0x3F));
                    } else {
                        out += char(0xE0 | (cp >> 12));
                        out += char(0x80 | ((cp >> 6) & 0x3F));
                        out += char(0x80 | (cp & 0x3F));
                    }
                } else {
                    out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
                }
            } else {
                out += t[i];
            }
            ++i;
        }
        ++i;
        return out;
    }
    Json value()
    {
        space();
        Json j;
        if (i >= t.size())
            return j;
        const char c = t[i];
        if (c == '"') {
            j.type = Json::String;
            j.string = str();
        } else if (c == '[') {
            j.type = Json::Array;
            ++i;
            for (space(); i < t.size() && t[i] != ']'; space()) {
                j.array.push_back(value());
                space();
                if (i < t.size() && t[i] == ',')
                    ++i;
            }
            ++i;
        } else if (c == '{') {
            j.type = Json::Object;
            ++i;
            for (space(); i < t.size() && t[i] != '}'; space()) {
                std::string key = str();
                space();
                ++i;   // ':'
                j.object.emplace_back(std::move(key), value());
                space();
                if (i < t.size() && t[i] == ',')
                    ++i;
            }
            ++i;
        } else {
            // a number, true, false, null: kept as text
            const size_t start = i;
            while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && !std::isspace(static_cast<unsigned char>(t[i])))
                ++i;
            j.type = Json::Number;
            j.string = t.substr(start, i - start);
        }
        return j;
    }
};

Json loadJson(const std::string &macPath)
{
    std::ifstream in(Assets::path(macPath) + ".json");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    JsonReader r{text};
    return r.value();
}

std::map<std::string, std::string> loadStrings(const std::string &macPath)
{
    std::map<std::string, std::string> out;
    const Json j = loadJson(macPath);
    for (const auto &kv : j.object)
        if (kv.second.type == Json::String)
            out[kv.first] = kv.second.string;
    return out;
}

struct TypeTable {
    std::map<std::string, std::string> byExtension;   // extension -> type
    std::map<std::string, std::string> description;   // type -> its kind
};

const TypeTable &types()
{
    static const TypeTable table = [] {
        TypeTable t;
        const std::string core = "/System/Library/CoreServices/CoreTypes.bundle/Contents/";
        const Json info = loadJson(core + "Info.plist");
        const auto strings = loadStrings(core + "Resources/English.lproj/InfoPlist.strings");
        for (const char *list : {"UTImportedTypeDeclarations", "UTExportedTypeDeclarations"}) {
            const Json *decls = info.get(list);
            if (!decls)
                continue;
            for (const Json &d : decls->array) {
                const Json *id = d.get("UTTypeIdentifier");
                if (!id)
                    continue;
                if (const Json *desc = d.get("UTTypeDescription")) {
                    auto l = strings.find(desc->string);
                    t.description[id->string] = l != strings.end() ? l->second : desc->string;
                }
                if (const Json *tags = d.get("UTTypeTagSpecification"))
                    if (const Json *exts = tags->get("public.filename-extension"))
                        for (const std::string &e : exts->strings())
                            t.byExtension.emplace(lower(e), id->string);
            }
        }
        // the applications 10.6 opens these with by default: their document types' names (in their
        // InfoPlist.strings), the first claim wins; "Alternate" handlers and public.data don't claim
        for (const char *app : {"/Applications/Preview.app", "/Applications/TextEdit.app", "/Applications/Safari.app"}) {
            const Json appInfo = loadJson(std::string(app) + "/Contents/Info.plist");
            const auto appStrings = loadStrings(std::string(app) + "/Contents/Resources/English.lproj/InfoPlist.strings");
            const Json *docs = appInfo.get("CFBundleDocumentTypes");
            if (!docs)
                continue;
            for (const Json &d : docs->array) {
                const Json *name = d.get("CFBundleTypeName");
                const Json *rank = d.get("LSHandlerRank");
                const Json *utis = d.get("LSItemContentTypes");
                if (!name || !utis || (rank && rank->string == "Alternate"))
                    continue;
                auto l = appStrings.find(name->string);
                const std::string shown = l != appStrings.end() ? l->second : name->string;
                for (const std::string &uti : utis->strings())
                    if (uti != "public.data" && uti != "public.folder")
                        t.description["app:" + uti] = shown;
            }
        }
        return t;
    }();
    return table;
}

std::string launchServices(const std::string &key)
{
    static const auto strings = loadStrings("/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/"
                                            "LaunchServices.framework/Versions/A/Resources/English.lproj/Localized.strings");
    auto it = strings.find(key);
    return it == strings.end() ? key : it->second;
}

} // namespace

std::string kind(const Item &item)
{
    if (item.folder)
        return launchServices("Folder");
    if (item.application)
        return launchServices("Application");
    const std::string ext = extension(item.name);
    if (!ext.empty()) {
        const TypeTable &t = types();
        auto type = t.byExtension.find(ext);
        if (type != t.byExtension.end()) {
            auto claimed = t.description.find("app:" + type->second);
            if (claimed != t.description.end())
                return claimed->second;
            auto d = t.description.find(type->second);
            if (d != t.description.end())
                return d->second;
        }
        std::string upper = ext;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        std::string f = launchServices("%@ Document");
        const size_t at = f.find("%@");
        return at == std::string::npos ? f : f.replace(at, 2, upper);
    }
    if (access(item.path.c_str(), X_OK) == 0)
        return launchServices("Unix Executable File");
    return launchServices("Document");
}

std::string formatDate(long long time, DateStyle style, bool relative)
{
    // TDateFormatter's relative formatters: the day's name ("Today", "Yesterday") for the last two
    // days, else the date in the style; the time short ("4:20 PM")
    const time_t t = time_t(time);
    struct tm when {};
    localtime_r(&t, &when);
    char timeText[32];
    std::strftime(timeText, sizeof timeText, "%l:%M %p", &when);
    std::string clock = timeText;
    while (!clock.empty() && clock.front() == ' ')
        clock.erase(clock.begin());
    if (relative) {
        const time_t now = std::time(nullptr);
        struct tm today {};
        localtime_r(&now, &today);
        today.tm_hour = today.tm_min = today.tm_sec = 0;
        const time_t midnight = mktime(&today);
        const char *const bundle = "/System/Library/CoreServices/Finder.app";
        if (t >= midnight && t < midnight + 86400)
            return Assets::tr(bundle, "Localizable", "BU17", "Today") + ", " + clock;
        if (t >= midnight - 86400 && t < midnight)
            return Assets::tr(bundle, "Localizable", "BU18", "Yesterday") + ", " + clock;
    }
    char date[64];
    switch (style) {
    case DateFull: std::strftime(date, sizeof date, "%A, %B %e, %Y", &when); break;
    case DateLong: std::strftime(date, sizeof date, "%B %e, %Y", &when); break;
    case DateMedium: std::strftime(date, sizeof date, "%b %e, %Y", &when); break;
    case DateShort: std::strftime(date, sizeof date, "%m/%d/%y", &when); break;
    }
    std::string d = date;
    // %e pads the day with a space; the Mac's styles don't
    for (size_t at; (at = d.find("  ")) != std::string::npos;)
        d.erase(at, 1);
    if (style == DateShort) {
        // M/d/yy without leading zeros
        std::string out;
        for (size_t i = 0; i < d.size(); ++i)
            if (!(d[i] == '0' && (i == 0 || d[i - 1] == '/')))
                out += d[i];
        d = out;
    }
    return d + " " + clock;
}

} // namespace Finder
