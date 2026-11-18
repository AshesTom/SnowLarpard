// SPDX-License-Identifier: LGPL-2.1-or-later
#include "foundation.h"


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace AK {

static std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? v : "";
}

std::string home() { return env("HOME"); }

std::string configDir()
{
    std::string x = env("XDG_CONFIG_HOME");
    return (x.empty() ? home() + "/.config" : x) + "/snowleopard";
}

std::string cacheDir()
{
    std::string x = env("XDG_CACHE_HOME");
    return (x.empty() ? home() + "/.cache" : x) + "/snowleopard";
}

std::string picturesDir()
{
    // user-dirs.dirs: XDG_PICTURES_DIR="$HOME/Pictures"
    std::string x = env("XDG_CONFIG_HOME");
    std::ifstream in((x.empty() ? home() + "/.config" : x) + "/user-dirs.dirs");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("XDG_PICTURES_DIR=", 0) != 0)
            continue;
        std::string v = line.substr(17);
        if (v.size() >= 2 && v.front() == '"')
            v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0)
            v = home() + v.substr(5);
        return v;
    }
    return home() + "/Pictures";
}

bool isDir(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isFile(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

long long modified(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
}

void makePath(const std::string &dir)
{
    for (size_t at = 1; at != std::string::npos;) {
        at = dir.find('/', at + 1);
        mkdir(dir.substr(0, at).c_str(), 0755);
    }
}

std::string fileName(const std::string &path)
{
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

std::string baseName(const std::string &path)
{
    std::string n = fileName(path);
    const size_t dot = n.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? n : n.substr(0, dot);
}

std::string dirName(const std::string &path)
{
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? "." : s == 0 ? "/" : path.substr(0, s);
}

bool finderLess(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit((unsigned char)a[ie]))
                ++ie;
            while (je < b.size() && std::isdigit((unsigned char)b[je]))
                ++je;
            const unsigned long long x = std::strtoull(a.substr(i, ie - i).c_str(), nullptr, 10);
            const unsigned long long y = std::strtoull(b.substr(j, je - j).c_str(), nullptr, 10);
            if (x != y)
                return x < y;
            i = ie, j = je;
            continue;
        }
        const int x = std::tolower((unsigned char)a[i]), y = std::tolower((unsigned char)b[j]);
        if (x != y)
            return x < y;
        ++i, ++j;
    }
    return a.size() - i < b.size() - j;
}

std::vector<std::string> files(const std::string &dir)
{
    std::vector<std::string> names;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.' && isFile(dir + '/' + e->d_name))
                names.push_back(e->d_name);
        closedir(d);
    }
    std::sort(names.begin(), names.end(), finderLess);
    for (std::string &n : names)
        n = dir + '/' + n;
    return names;
}

std::string executablePath()
{
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

namespace {
std::string &programNameOverride()
{
    static std::string name;
    return name;
}
} // namespace

std::string programName()
{
    return programNameOverride().empty() ? fileName(executablePath()) : programNameOverride();
}

void setProgramName(const std::string &name) { programNameOverride() = name; }

std::string hashName(const std::string &text)
{
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : text)
        h = (h ^ c) * 1099511628211ULL;
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", h);
    return s;
}

// ---- the DVD's files

namespace Assets {

std::string root()
{
    static const std::string r = [] {
        std::string e = env("SNOWLEOPARD_ASSETS");
        if (!e.empty())
            return e;
        // .local/build/apps/X.app/Contents/MacOS/X -> .local/assets
        std::string dir = dirName(executablePath());
        for (int i = 0; i < 7; ++i) {
            if (isFile(dir + "/assets/.complete"))
                return dir + "/assets";
            dir = dirName(dir);
        }
        std::string x = env("XDG_DATA_HOME");
        return (x.empty() ? home() + "/.local/share" : x) + "/snowleopard/assets";
    }();
    return r;
}

std::string path(const std::string &macPath) { return root() + (macPath.rfind('/', 0) == 0 ? macPath : '/' + macPath); }

bool exists(const std::string &macPath)
{
    struct stat st;
    return stat(path(macPath).c_str(), &st) == 0;
}

// The JSON extract.py writes for a .strings table: one object of strings
static std::map<std::string, std::string> stringsTable(const std::string &file)
{
    std::map<std::string, std::string> out;
    std::ifstream in(file);
    const std::string s((std::istreambuf_iterator<char>(in)), {});
    size_t i = 0;
    auto str = [&](std::string &v) {
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        v.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) {
                c = s[i++];
                switch (c) {
                case 'n': v += '\n'; continue;
                case 't': v += '\t'; continue;
                case 'u': {
                    unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {
                        const unsigned lo = unsigned(std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16));
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                    if (cp < 0x80) v += char(cp);
                    else if (cp < 0x800) v += char(0xC0 | cp >> 6), v += char(0x80 | (cp & 0x3F));
                    else if (cp < 0x10000) v += char(0xE0 | cp >> 12), v += char(0x80 | (cp >> 6 & 0x3F)), v += char(0x80 | (cp & 0x3F));
                    else v += char(0xF0 | cp >> 18), v += char(0x80 | (cp >> 12 & 0x3F)), v += char(0x80 | (cp >> 6 & 0x3F)), v += char(0x80 | (cp & 0x3F));
                    continue;
                }
                default: break;
                }
            }
            v += c;
        }
        ++i;
        return true;
    };
    auto skip = [&] {
        while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == ',' || s[i] == ':' || s[i] == '{' || s[i] == '}'))
            ++i;
    };
    std::string k, v;
    for (skip(); i < s.size(); skip()) {
        if (!str(k))
            break;
        skip();
        if (!str(v))
            break;
        out[k] = v;
    }
    return out;
}

std::map<std::string, std::string> plistStrings(const std::string &macPath)
{
    // a JSON object: its keys with string values; other values skipped, nesting and all
    std::map<std::string, std::string> out;
    std::ifstream in(path(macPath) + ".json");
    const std::string s((std::istreambuf_iterator<char>(in)), {});
    size_t i = 0;
    auto ws = [&] {
        while (i < s.size() && std::isspace((unsigned char)s[i]))
            ++i;
    };
    std::function<std::string()> str = [&]() {
        std::string v;
        ++i;   // the quote
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char e = s[++i];
                v += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                v += s[i];
            }
            ++i;
        }
        ++i;
        return v;
    };
    std::function<void()> skip = [&]() {
        ws();
        if (i >= s.size())
            return;
        if (s[i] == '"') {
            str();
        } else if (s[i] == '{' || s[i] == '[') {
            const char close = s[i] == '{' ? '}' : ']';
            ++i;
            for (ws(); i < s.size() && s[i] != close; ws()) {
                if (s[i] == ',' || s[i] == ':') {
                    ++i;
                    continue;
                }
                skip();
            }
            ++i;
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']')
                ++i;
        }
    };
    ws();
    if (i >= s.size() || s[i] != '{')
        return out;
    ++i;
    for (ws(); i < s.size() && s[i] != '}'; ws()) {
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] != '"')
            break;
        const std::string key = str();
        ws();
        if (i < s.size() && s[i] == ':')
            ++i;
        ws();
        if (i < s.size() && s[i] == '"') {
            out[key] = str();
        } else if (i < s.size() && s[i] != '{' && s[i] != '[') {
            // a number or a boolean, as its text (NSMenuExtraWidth's 140, "true")
            const size_t from = i;
            skip();
            size_t end = i;
            while (end > from && std::isspace((unsigned char)s[end - 1]))
                --end;
            out[key] = s.substr(from, end - from);
        } else {
            skip();
        }
    }
    return out;
}

std::string tr(const std::string &bundle, const std::string &table, const std::string &key, const std::string &fallback)
{
    static std::map<std::string, std::map<std::string, std::string>> cache;
    // an application's Contents/Resources, a framework's version's Resources (AppKit.framework/Versions/C)
    const bool framework = bundle.find(".framework/Versions/") != std::string::npos;
    const std::string file = path(bundle + (framework ? "/Resources/English.lproj/" : "/Contents/Resources/English.lproj/") + table + ".strings.json");
    auto it = cache.find(file);
    if (it == cache.end())
        it = cache.emplace(file, stringsTable(file)).first;
    auto s = it->second.find(key);
    return s == it->second.end() || s->second.empty() ? fallback : s->second;
}

} // namespace Assets

// ---- settings: QSettings' INI format (iniEscapedString / iniUnescapedStringList)

static std::string escaped(const std::string &s)
{
    bool quote = !s.empty() && (std::isspace((unsigned char)s.front()) || std::isspace((unsigned char)s.back()));
    std::string out;
    if (!s.empty() && s[0] == '@')
        out += '@';   // QSettings keeps '@' for its own types
    for (char c : s) {
        switch (c) {
        case ';': case ',': case '=': quote = true; out += c; break;
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return quote ? '"' + out + '"' : out;
}

static std::vector<std::string> unescaped(const std::string &raw, bool *isList)
{
    std::vector<std::string> out(1);
    bool inQuotes = false, any = false;
    *isList = false;
    size_t i = 0;
    while (i < raw.size() && std::isspace((unsigned char)raw[i]))
        ++i;
    for (; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            any = true;
        } else if (c == '\\' && i + 1 < raw.size()) {
            const char n = raw[++i];
            out.back() += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
        } else if (c == ',' && !inQuotes) {
            *isList = true;
            // the spaces after the comma are not the next value's
            while (i + 1 < raw.size() && raw[i + 1] == ' ')
                ++i;
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    if (!any)
        for (std::string &v : out)
            while (!v.empty() && std::isspace((unsigned char)v.back()))
                v.pop_back();
    return out;
}

Settings::Settings(const std::string &name) : m_file(configDir() + '/' + name + ".conf")
{
    std::ifstream in(m_file);
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line[0] == '[') {
            section = line;
            if (section != "[General]")
                m_otherLines.push_back(line);
            continue;
        }
        if (section.empty() || section == "[General]") {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq), value = line.substr(eq + 1);
                while (!key.empty() && key.back() == ' ')
                    key.pop_back();
                while (!value.empty() && value.front() == ' ')
                    value.erase(0, 1);
                m_general.emplace_back(key, value);
            }
        } else {
            m_otherLines.push_back(line);
        }
    }
}

std::string Settings::string(const std::string &key, const std::string &fallback) const
{
    for (const auto &[k, raw] : m_general)
        if (k == key) {
            bool list;
            std::vector<std::string> v = unescaped(raw, &list);
            if (list) {
                std::string joined;
                for (size_t i = 0; i < v.size(); ++i)
                    joined += (i ? ", " : "") + v[i];
                return joined;
            }
            return v[0].rfind("@@", 0) == 0 ? v[0].substr(1) : v[0];
        }
    return fallback;
}

int Settings::integer(const std::string &key, int fallback) const
{
    const std::string s = string(key);
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return s.empty() || *end ? fallback : int(v);
}

bool Settings::boolean(const std::string &key, bool fallback) const
{
    const std::string s = string(key);
    if (s == "true" || s == "1")
        return true;
    if (s == "false" || s == "0")
        return false;
    return fallback;
}

std::vector<std::string> Settings::list(const std::string &key) const
{
    for (const auto &[k, raw] : m_general)
        if (k == key) {
            if (raw == "@Invalid()" || raw.empty())
                return {};
            bool list;
            return unescaped(raw, &list);
        }
    return {};
}

void Settings::write(const std::string &key, const std::string &raw)
{
    bool found = false;
    for (auto &[k, v] : m_general)
        if (k == key)
            v = raw, found = true;
    if (!found)
        m_general.emplace_back(key, raw);
    std::sort(m_general.begin(), m_general.end());   // QSettings writes its keys sorted
    makePath(dirName(m_file));
    std::ofstream out(m_file + ".tmp");
    out << "[General]\n";
    for (const auto &[k, v] : m_general)
        out << k << '=' << v << '\n';
    for (const std::string &l : m_otherLines)
        out << (l.size() && l[0] == '[' ? "\n" : "") << l << '\n';
    out.close();
    std::rename((m_file + ".tmp").c_str(), m_file.c_str());
}

void Settings::set(const std::string &key, const std::string &value) { write(key, escaped(value)); }

void Settings::set(const std::string &key, const std::vector<std::string> &values)
{
    if (values.empty())
        return write(key, "@Invalid()");
    std::string raw;
    for (size_t i = 0; i < values.size(); ++i)
        raw += (i ? ", " : "") + escaped(values[i]);
    write(key, raw);
}

} // namespace AK
