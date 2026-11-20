// SPDX-License-Identifier: LGPL-2.1-or-later
#include "search.h"

#include "foundation.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <ctime>

using namespace AK;

namespace Finder {

namespace {

constexpr int MaxResults = 5000, MaxDepth = 16;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string extensionOf(const std::string &name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(name.substr(dot + 1));
}

bool among(const std::string &ext, std::initializer_list<const char *> list)
{
    for (const char *e : list)
        if (ext == e)
            return true;
    return false;
}

// Spotlight's groups (_kMDItemGroupId) by the file's extension: 13 images, 7 movies
bool isImage(const std::string &ext)
{
    return among(ext, {"png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff", "heic", "webp", "psd", "raw", "cr2", "nef", "dng", "svg", "ico", "icns"});
}
bool isMovie(const std::string &ext) { return among(ext, {"mov", "mp4", "m4v", "avi", "mkv", "webm", "mpg", "mpeg", "wmv", "flv", "3gp", "ogv"}); }

// the midnight that starts the day `days` from today; the week's start (Sunday, as 10.6's
// $time.this_week in an English locale)
long long midnight(int days)
{
    const time_t now = std::time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

long long weekStart(int weeks)
{
    const time_t now = std::time(nullptr);
    struct tm tm {};
    localtime_r(&now, &tm);
    return midnight(-tm.tm_wday + 7 * weeks);
}

} // namespace

std::vector<std::string> Search::thisMacFolders()
{
    std::vector<std::string> out = {home()};
    for (const Volume &v : volumes())
        if (v.path != "/" && v.path.rfind(home() + "/", 0) != 0)
            out.push_back(v.path);
    return out;
}

std::shared_ptr<Search> Search::start(const std::string &text, const std::string &canned, const std::string &scope)
{
    std::shared_ptr<Search> s(new Search);
    s->m_text = lower(text);
    s->m_canned = canned;
    if (canned == "Today") {
        s->m_from = midnight(0);
        s->m_to = midnight(1);
    } else if (canned == "Yesterday") {
        s->m_from = midnight(-1);
        s->m_to = midnight(0);
    } else if (canned == "Past Week") {
        s->m_from = weekStart(0);
        s->m_to = weekStart(1);
    }
    const std::vector<std::string> folders = scope.empty() ? thisMacFolders() : std::vector<std::string>{scope};
    Search *raw = s.get();
    s->m_thread = std::thread([raw, folders] {
        for (const std::string &f : folders)
            if (!raw->m_cancel)
                raw->walk(f, 0);
        raw->m_finished = true;
    });
    return s;
}

Search::~Search()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<Item> Search::take()
{
    std::lock_guard<std::mutex> l(m_lock);
    std::vector<Item> out;
    out.swap(m_found);
    return out;
}

bool Search::matches(const std::string &path, const std::string &name, bool folder) const
{
    if (!m_text.empty() && lower(name).find(m_text) == std::string::npos)
        return false;
    if (m_canned.empty())
        return true;
    const std::string ext = extensionOf(name);
    if (m_canned == "All Images")
        return !folder && isImage(ext);
    if (m_canned == "All Movies")
        return !folder && isMovie(ext);
    if (m_canned == "All Documents")
        return !folder;   // public.content: every document
    if (m_from > 0) {
        if (folder)
            return false;   // _kMDItemGroupId 9 (folders) left out
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return false;
        const long long used = std::max<long long>(st.st_atime, st.st_mtime);
        return used >= m_from && used < m_to;
    }
    return true;
}

void Search::walk(const std::string &folder, int depth)
{
    if (m_cancel || depth > MaxDepth || m_count >= MaxResults)
        return;
    DIR *d = opendir(folder.c_str());
    if (!d)
        return;
    std::vector<std::pair<std::string, bool>> entries;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;   // hidden, as the Finder's
        const std::string path = folder == "/" ? "/" + std::string(e->d_name) : folder + "/" + e->d_name;
        bool isFolder = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            isFolder = lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        entries.push_back({e->d_name, isFolder});
    }
    closedir(d);
    std::vector<Item> found;
    for (const auto &[name, isFolder] : entries) {
        const std::string path = folder == "/" ? "/" + name : folder + "/" + name;
        if (matches(path, name, isFolder) && m_count < MaxResults) {
            Item item;
            item.name = name;
            item.path = path;
            item.folder = isFolder;
            found.push_back(item);
            ++m_count;
        }
    }
    if (!found.empty()) {
        std::lock_guard<std::mutex> l(m_lock);
        m_found.insert(m_found.end(), found.begin(), found.end());
    }
    for (const auto &[name, isFolder] : entries)
        if (isFolder && !m_cancel)
            walk(folder == "/" ? "/" + name : folder + "/" + name, depth + 1);
}

} // namespace Finder
