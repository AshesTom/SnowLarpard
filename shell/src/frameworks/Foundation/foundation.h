// SPDX-License-Identifier: LGPL-2.1-or-later
// What AquaKit's programs need besides drawing, without Qt: the DVD's files at their Mac paths
// (tools/extract.py puts them in .local/assets), the localized strings, the shell's settings files
// (~/.config/snowleopard/*.conf, in the INI format the Qt programs of the shell write with QSettings,
// so both read the same files), folders and paths.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace AK {

namespace Assets {
std::string root();
std::string path(const std::string &macPath);
bool exists(const std::string &macPath);
// A .strings table of a bundle (extract.py converts them to JSON beside them)
std::string tr(const std::string &bundle, const std::string &table, const std::string &key, const std::string &fallback);
// A plist's top-level strings, numbers and booleans as text (extract.py's JSON beside it:
// Info.plist.json)
std::map<std::string, std::string> plistStrings(const std::string &macPath);
} // namespace Assets

// ~/.config/snowleopard/<name>.conf, its [General] section as QSettings writes it
class Settings {
public:
    explicit Settings(const std::string &name);
    std::string string(const std::string &key, const std::string &fallback = {}) const;
    int integer(const std::string &key, int fallback = 0) const;
    bool boolean(const std::string &key, bool fallback = false) const;
    std::vector<std::string> list(const std::string &key) const;
    void set(const std::string &key, const std::string &value);
    void set(const std::string &key, const char *value) { set(key, std::string(value)); }
    void set(const std::string &key, int value) { set(key, std::to_string(value)); }
    void set(const std::string &key, bool value) { set(key, std::string(value ? "true" : "false")); }
    void set(const std::string &key, const std::vector<std::string> &values);
    std::string file() const { return m_file; }

private:
    void write(const std::string &key, const std::string &raw);
    std::string m_file;
    std::vector<std::pair<std::string, std::string>> m_general;   // key, raw value (QSettings' escaping)
    std::vector<std::string> m_otherLines;                        // other sections, kept as they are
};

std::string configDir();
std::string home();
std::string picturesDir();   // XDG_PICTURES_DIR
std::string cacheDir();
bool isDir(const std::string &path);
bool isFile(const std::string &path);
void makePath(const std::string &dir);
std::string fileName(const std::string &path);
std::string baseName(const std::string &path);   // without the last extension
std::string dirName(const std::string &path);
long long modified(const std::string &path);
// a folder's files (not its subfolders), by name as the Finder sorts them (numbers by value, no case)
std::vector<std::string> files(const std::string &dir);
bool finderLess(const std::string &a, const std::string &b);
std::string executablePath();
// The name the windows give the window manager (WM_CLASS, Wayland's app id): the executable's, unless
// the program says otherwise (the Finder's windows are "Finder" whatever their program's file)
std::string programName();
void setProgramName(const std::string &name);
std::string hashName(const std::string &text);   // a file name for a cache: 64-bit FNV-1a, in hex

} // namespace AK
