// SPDX-License-Identifier: LGPL-2.1-or-later
#include "library.h"

#include "foundation.h"

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

using namespace AK;

namespace iTunes {

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension(const std::string &path)
{
    const size_t dot = path.rfind('.');
    const size_t slash = path.rfind('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string() : lower(path.substr(dot + 1));
}

// The kinds iTunes names its files with ("Kind" in the plist and the Get Info window)
std::string kindOf(const std::string &path)
{
    const std::string e = extension(path);
    if (e == "mp3")
        return "MPEG audio file";
    if (e == "m4a" || e == "aac" || e == "mp4")
        return "AAC audio file";
    if (e == "m4p")
        return "Protected AAC audio file";
    if (e == "aif" || e == "aiff")
        return "AIFF audio file";
    if (e == "wav")
        return "WAV audio file";
    if (e == "flac")
        return "FLAC audio file";
    if (e == "ogg" || e == "oga" || e == "opus")
        return "Ogg Vorbis audio file";
    return "Audio file";
}

std::string xmlEscape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '&')
            o += "&amp;";
        else if (c == '<')
            o += "&lt;";
        else if (c == '>')
            o += "&gt;";
        else
            o += c;
    }
    return o;
}

std::string xmlUnescape(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos) {
            o += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            o += '&';
        else if (ent == "lt")
            o += '<';
        else if (ent == "gt")
            o += '>';
        else if (ent == "quot")
            o += '"';
        else if (ent == "apos")
            o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const unsigned cp = unsigned(ent.size() > 1 && ent[1] == 'x' ? std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                           : std::strtoul(ent.c_str() + 1, nullptr, 10));
            if (cp < 0x80)
                o += char(cp);
            else if (cp < 0x800) {
                o += char(0xC0 | (cp >> 6));
                o += char(0x80 | (cp & 0x3F));
            } else {
                o += char(0xE0 | (cp >> 12));
                o += char(0x80 | ((cp >> 6) & 0x3F));
                o += char(0x80 | (cp & 0x3F));
            }
        }
        i = semi;
    }
    return o;
}

// file:// URLs as iTunes writes Location: the path percent-encoded, "/" kept
std::string fileURL(const std::string &path)
{
    std::string o = "file://localhost";
    static const char *hex = "0123456789ABCDEF";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            o += char(c);
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string pathFromURL(const std::string &url)
{
    std::string s = url;
    for (const char *prefix : {"file://localhost", "file://"})
        if (s.rfind(prefix, 0) == 0) {
            s = s.substr(std::string(prefix).size());
            break;
        }
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else
            o += s[i];
    }
    return o;
}

std::string isoNow()
{
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// A small reader for the plist this file holds: dictionaries, arrays, keys, strings, integers, dates,
// booleans
struct Node {
    enum Kind { Dict, Array, String, Integer, Bool, Other } kind = Other;
    std::string text;
    std::vector<std::pair<std::string, Node>> dict;
    std::vector<Node> array;
    const Node *get(const std::string &k) const
    {
        for (const auto &[key, v] : dict)
            if (key == k)
                return &v;
        return nullptr;
    }
    std::string str(const std::string &k) const { const Node *n = get(k); return n ? n->text : std::string(); }
    long long num(const std::string &k) const { const Node *n = get(k); return n ? std::atoll(n->text.c_str()) : 0; }
};

class Parser {
public:
    explicit Parser(const std::string &s) : m_s(s) {}
    bool parse(Node &out)
    {
        const size_t p = m_s.find("<plist");
        if (p == std::string::npos)
            return false;
        m_p = m_s.find('>', p) + 1;
        return value(out);
    }

private:
    bool tag(std::string &name, bool &closing, bool &empty)
    {
        m_p = m_s.find('<', m_p);
        if (m_p == std::string::npos)
            return false;
        const size_t e = m_s.find('>', m_p);
        if (e == std::string::npos)
            return false;
        std::string t = m_s.substr(m_p + 1, e - m_p - 1);
        m_p = e + 1;
        closing = !t.empty() && t[0] == '/';
        empty = !t.empty() && t.back() == '/';
        if (closing)
            t = t.substr(1);
        if (empty)
            t.pop_back();
        name = t;
        return true;
    }
    std::string textUntil(const std::string &close)
    {
        const size_t e = m_s.find(close, m_p);
        if (e == std::string::npos)
            return {};
        std::string t = xmlUnescape(m_s.substr(m_p, e - m_p));
        m_p = e + close.size();
        return t;
    }
    bool value(Node &out)
    {
        std::string name;
        bool closing = false, empty = false;
        if (!tag(name, closing, empty) || closing)
            return false;
        if (name == "dict") {
            out.kind = Node::Dict;
            if (empty)
                return true;
            while (true) {
                const size_t save = m_p;
                if (!tag(name, closing, empty))
                    return false;
                if (closing && name == "dict")
                    return true;
                if (name != "key") {
                    m_p = save;
                    return false;
                }
                std::string key = textUntil("</key>");
                Node v;
                if (!value(v))
                    return false;
                out.dict.emplace_back(std::move(key), std::move(v));
            }
        }
        if (name == "array") {
            out.kind = Node::Array;
            if (empty)
                return true;
            while (true) {
                const size_t save = m_p;
                if (!tag(name, closing, empty))
                    return false;
                if (closing && name == "array")
                    return true;
                m_p = save;
                Node v;
                if (!value(v))
                    return false;
                out.array.push_back(std::move(v));
            }
        }
        if (name == "true" || name == "false") {
            out.kind = Node::Bool;
            out.text = name;
            return true;
        }
        out.kind = name == "integer" ? Node::Integer : name == "string" ? Node::String : Node::Other;
        if (!empty)
            out.text = textUntil("</" + name + ">");
        return true;
    }
    const std::string &m_s;
    size_t m_p = 0;
};

} // namespace

std::string formatTime(int ms)
{
    const int s = (ms + 500) / 1000;
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

std::string Library::musicFolder()
{
    // XDG_MUSIC_DIR from user-dirs.dirs ("$HOME/Music")
    std::ifstream f(home() + "/.config/user-dirs.dirs");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("XDG_MUSIC_DIR=", 0) != 0)
            continue;
        std::string v = line.substr(14);
        if (v.size() >= 2 && v.front() == '"')
            v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0)
            v = home() + v.substr(5);
        return v;
    }
    return home() + "/Music";
}

std::string Library::file() const { return musicFolder() + "/iTunes/iTunes Music Library.xml"; }

bool Library::isAudio(const std::string &path) const
{
    static const std::set<std::string> exts = {"mp3", "m4a", "m4p", "aac", "aif", "aiff", "wav", "flac", "ogg", "oga", "opus", "mp4"};
    return exts.count(extension(path)) > 0;
}

int Library::idForPath(const std::string &path) const
{
    for (const auto &[id, t] : m_tracks)
        if (t.path == path)
            return id;
    return 0;
}

const Track *Library::track(int id) const
{
    auto it = m_tracks.find(id);
    return it == m_tracks.end() ? nullptr : &it->second;
}

Track *Library::track(int id)
{
    auto it = m_tracks.find(id);
    return it == m_tracks.end() ? nullptr : &it->second;
}

void Library::remove(int id)
{
    m_tracks.erase(id);
    for (Playlist &p : m_playlists)
        p.items.erase(std::remove(p.items.begin(), p.items.end(), id), p.items.end());
}

bool Library::load()
{
    std::ifstream f(file());
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    Node root;
    if (!Parser(text).parse(root) || root.kind != Node::Dict)
        return false;
    m_tracks.clear();
    m_playlists.clear();
    if (const Node *tracks = root.get("Tracks"))
        for (const auto &[key, t] : tracks->dict) {
            Track tr;
            tr.id = int(t.num("Track ID"));
            tr.name = t.str("Name");
            tr.artist = t.str("Artist");
            tr.album = t.str("Album");
            tr.genre = t.str("Genre");
            tr.kind = t.str("Kind");
            tr.size = t.num("Size");
            tr.totalTime = int(t.num("Total Time"));
            tr.trackNumber = int(t.num("Track Number"));
            tr.year = int(t.num("Year"));
            tr.bitRate = int(t.num("Bit Rate"));
            tr.sampleRate = int(t.num("Sample Rate"));
            tr.playCount = int(t.num("Play Count"));
            tr.composer = t.str("Composer");
            tr.albumArtist = t.str("Album Artist");
            tr.grouping = t.str("Grouping");
            tr.comments = t.str("Comments");
            tr.trackCount = int(t.num("Track Count"));
            tr.discNumber = int(t.num("Disc Number"));
            tr.discCount = int(t.num("Disc Count"));
            tr.bpm = int(t.num("BPM"));
            tr.rating = int(t.num("Rating"));
            tr.dateAdded = t.str("Date Added");
            tr.path = pathFromURL(t.str("Location"));
            if (tr.id > 0) {
                m_tracks[tr.id] = tr;
                m_nextId = std::max(m_nextId, tr.id + 1);
            }
        }
    if (const Node *lists = root.get("Playlists"))
        for (const Node &p : lists->array) {
            Playlist pl;
            pl.name = p.str("Name");
            const Node *m = p.get("Master");
            pl.master = m && m->text == "true";
            if (const Node *items = p.get("Playlist Items"))
                for (const Node &i : items->array)
                    pl.items.push_back(int(i.num("Track ID")));
            if (const Node *s = p.get("Smart Rules")) {
                // ours: iTunes keeps them as the binary "Smart Info" and "Smart Criteria" (SLst)
                auto flag = [&](const char *k) { const Node *n = s->get(k); return n && n->text == "true"; };
                pl.smart = true;
                pl.info.any = flag("Match Any");
                pl.info.useRules = !s->get("Match") || flag("Match");
                pl.info.limit = flag("Limit");
                pl.info.limitValue = int(s->num("Limit Value"));
                pl.info.limitUnit = int(s->num("Limit Unit"));
                pl.info.limitOrder = int(s->num("Limit Order"));
                pl.info.checkedOnly = flag("Checked Only");
                pl.info.live = !s->get("Live Updating") || flag("Live Updating");
                if (const Node *rules = s->get("Rules"))
                    for (const Node &r : rules->array) {
                        SmartRule rule;
                        rule.field = int(r.num("Field"));
                        rule.op = uint32_t(r.num("Operator"));
                        rule.text = r.str("Text");
                        rule.from = r.num("From");
                        rule.to = r.num("To");
                        rule.unit = std::max(1, int(r.num("Unit")));
                        pl.info.rules.push_back(rule);
                    }
            }
            m_playlists.push_back(pl);
        }
    return true;
}

bool Library::save() const
{
    const std::string dir = dirName(file());
    makePath(dir);
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n<dict>\n"
         "\t<key>Major Version</key><integer>1</integer>\n"
         "\t<key>Minor Version</key><integer>1</integer>\n"
         "\t<key>Application Version</key><string>8.2.1</string>\n"
         "\t<key>Features</key><integer>5</integer>\n"
         "\t<key>Show Content Ratings</key><true/>\n"
         "\t<key>Music Folder</key><string>"
      << xmlEscape(fileURL(musicFolder() + "/iTunes/iTunes Music/")) << "</string>\n"
      << "\t<key>Tracks</key>\n\t<dict>\n";
    auto str = [&](const char *k, const std::string &v) {
        if (!v.empty())
            o << "\t\t\t<key>" << k << "</key><string>" << xmlEscape(v) << "</string>\n";
    };
    auto num = [&](const char *k, long long v) {
        if (v)
            o << "\t\t\t<key>" << k << "</key><integer>" << v << "</integer>\n";
    };
    for (const auto &[id, t] : m_tracks) {
        o << "\t\t<key>" << id << "</key>\n\t\t<dict>\n";
        num("Track ID", id);
        str("Name", t.name);
        str("Artist", t.artist);
        str("Album Artist", t.albumArtist);
        str("Composer", t.composer);
        str("Album", t.album);
        str("Grouping", t.grouping);
        str("Genre", t.genre);
        str("Kind", t.kind);
        num("Size", t.size);
        num("Total Time", t.totalTime);
        num("Disc Number", t.discNumber);
        num("Disc Count", t.discCount);
        num("Track Number", t.trackNumber);
        num("Track Count", t.trackCount);
        num("Year", t.year);
        num("BPM", t.bpm);
        if (!t.dateAdded.empty())
            o << "\t\t\t<key>Date Added</key><date>" << t.dateAdded << "</date>\n";
        num("Bit Rate", t.bitRate);
        num("Sample Rate", t.sampleRate);
        num("Play Count", t.playCount);
        num("Rating", t.rating);
        str("Comments", t.comments);
        o << "\t\t\t<key>Track Type</key><string>File</string>\n";
        str("Location", fileURL(t.path));
        o << "\t\t</dict>\n";
    }
    o << "\t</dict>\n\t<key>Playlists</key>\n\t<array>\n";
    for (const Playlist &p : m_playlists) {
        o << "\t\t<dict>\n\t\t\t<key>Name</key><string>" << xmlEscape(p.name) << "</string>\n";
        if (p.master)
            o << "\t\t\t<key>Master</key><true/>\n\t\t\t<key>Visible</key><false/>\n";
        if (p.smart) {
            const SmartInfo &s = p.info;
            auto b = [](bool v) { return v ? "<true/>" : "<false/>"; };
            o << "\t\t\t<key>Smart Rules</key>\n\t\t\t<dict>\n"
              << "\t\t\t\t<key>Match</key>" << b(s.useRules) << "\n"
              << "\t\t\t\t<key>Match Any</key>" << b(s.any) << "\n"
              << "\t\t\t\t<key>Limit</key>" << b(s.limit) << "\n"
              << "\t\t\t\t<key>Limit Value</key><integer>" << s.limitValue << "</integer>\n"
              << "\t\t\t\t<key>Limit Unit</key><integer>" << s.limitUnit << "</integer>\n"
              << "\t\t\t\t<key>Limit Order</key><integer>" << s.limitOrder << "</integer>\n"
              << "\t\t\t\t<key>Checked Only</key>" << b(s.checkedOnly) << "\n"
              << "\t\t\t\t<key>Live Updating</key>" << b(s.live) << "\n"
              << "\t\t\t\t<key>Rules</key>\n\t\t\t\t<array>\n";
            for (const SmartRule &r : s.rules)
                o << "\t\t\t\t\t<dict><key>Field</key><integer>" << r.field << "</integer><key>Operator</key><integer>" << r.op
                  << "</integer><key>Text</key><string>" << xmlEscape(r.text) << "</string><key>From</key><integer>" << r.from
                  << "</integer><key>To</key><integer>" << r.to << "</integer><key>Unit</key><integer>" << r.unit << "</integer></dict>\n";
            o << "\t\t\t\t</array>\n\t\t\t</dict>\n";
        }
        o << "\t\t\t<key>All Items</key><true/>\n\t\t\t<key>Playlist Items</key>\n\t\t\t<array>\n";
        for (int id : p.items)
            o << "\t\t\t\t<dict>\n\t\t\t\t\t<key>Track ID</key><integer>" << id << "</integer>\n\t\t\t\t</dict>\n";
        o << "\t\t\t</array>\n\t\t</dict>\n";
    }
    o << "\t</array>\n</dict>\n</plist>\n";
    const std::string tmp = file() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            return false;
        f << o.str();
    }
    return std::rename(tmp.c_str(), file().c_str()) == 0;
}

void Library::addFile(const std::string &path)
{
    for (const auto &[id, t] : m_tracks)
        if (t.path == path)
            return;
    Track t;
    t.id = m_nextId++;
    t.path = path;
    t.kind = kindOf(path);
    t.dateAdded = isoNow();
    struct stat st;
    if (::stat(path.c_str(), &st) == 0)
        t.size = st.st_size;
    TagLib::FileRef f(path.c_str());
    if (!f.isNull() && f.tag()) {
        TagLib::Tag *tag = f.tag();
        t.name = tag->title().to8Bit(true);
        t.artist = tag->artist().to8Bit(true);
        t.album = tag->album().to8Bit(true);
        t.genre = tag->genre().to8Bit(true);
        t.year = int(tag->year());
        t.trackNumber = int(tag->track());
        t.comments = tag->comment().to8Bit(true);
        // the rest from TagLib's property map (ID3's TCOM, TPE2, TIT1, TRCK n/m, TPOS, TBPM; MP4's
        // and Vorbis' own)
        const TagLib::PropertyMap p = f.file()->properties();
        auto prop = [&](const char *k) { return p.contains(k) && !p[k].isEmpty() ? p[k].front().to8Bit(true) : std::string(); };
        auto second = [](const std::string &v) { const size_t s = v.find('/'); return s == std::string::npos ? 0 : std::atoi(v.c_str() + s + 1); };
        t.composer = prop("COMPOSER");
        t.albumArtist = prop("ALBUMARTIST");
        t.grouping = prop("GROUPING").empty() ? prop("CONTENTGROUP") : prop("GROUPING");
        t.trackCount = second(prop("TRACKNUMBER"));
        if (!prop("TRACKTOTAL").empty())
            t.trackCount = std::atoi(prop("TRACKTOTAL").c_str());
        t.discNumber = std::atoi(prop("DISCNUMBER").c_str());
        t.discCount = second(prop("DISCNUMBER"));
        if (!prop("DISCTOTAL").empty())
            t.discCount = std::atoi(prop("DISCTOTAL").c_str());
        t.bpm = std::atoi(prop("BPM").c_str());
    }
    if (!f.isNull() && f.audioProperties()) {
        t.totalTime = f.audioProperties()->lengthInMilliseconds();
        t.bitRate = f.audioProperties()->bitrate();
        t.sampleRate = f.audioProperties()->sampleRate();
    }
    if (t.name.empty())
        t.name = baseName(path);
    m_tracks[t.id] = t;
    for (Playlist &p : m_playlists)
        if (p.master)
            p.items.push_back(t.id);
}

bool Library::writeTags(const Track &t)
{
    Track *mine = track(t.id);
    if (!mine)
        return false;
    *mine = t;
    TagLib::FileRef f(t.path.c_str());
    if (f.isNull() || !f.tag())
        return false;
    TagLib::Tag *tag = f.tag();
    auto s = [](const std::string &v) { return TagLib::String(v, TagLib::String::UTF8); };
    tag->setTitle(s(t.name));
    tag->setArtist(s(t.artist));
    tag->setAlbum(s(t.album));
    tag->setGenre(s(t.genre));
    tag->setComment(s(t.comments));
    tag->setYear(unsigned(std::max(0, t.year)));
    TagLib::PropertyMap p = f.file()->properties();
    auto set = [&](const char *k, const std::string &v) {
        if (v.empty())
            p.erase(k);
        else
            p.replace(k, TagLib::StringList(s(v)));
    };
    auto pair = [](int n, int of) { return n ? std::to_string(n) + (of ? "/" + std::to_string(of) : std::string()) : std::string(); };
    set("COMPOSER", t.composer);
    set("ALBUMARTIST", t.albumArtist);
    set("GROUPING", t.grouping);
    set("TRACKNUMBER", pair(t.trackNumber, t.trackCount));
    set("DISCNUMBER", pair(t.discNumber, t.discCount));
    set("BPM", t.bpm ? std::to_string(t.bpm) : std::string());
    f.file()->setProperties(p);
    return f.save();
}

int Library::add(const std::vector<std::string> &paths)
{
    if (std::none_of(m_playlists.begin(), m_playlists.end(), [](const Playlist &p) { return p.master; })) {
        Playlist lib;
        lib.name = "Library";
        lib.master = true;
        m_playlists.insert(m_playlists.begin(), lib);
    }
    const size_t before = m_tracks.size();
    std::vector<std::string> todo = paths;
    while (!todo.empty()) {
        const std::string p = todo.back();
        todo.pop_back();
        if (isDir(p)) {
            if (DIR *d = opendir(p.c_str())) {
                std::vector<std::string> names;
                while (dirent *e = readdir(d))
                    if (e->d_name[0] != '.')
                        names.push_back(e->d_name);
                closedir(d);
                std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return b < a; });
                for (const std::string &n : names)
                    todo.push_back(p + "/" + n);
            }
        } else if (isAudio(p)) {
            addFile(p);
        }
    }
    return int(m_tracks.size() - before);
}

std::vector<unsigned char> artworkData(const std::string &path)
{
    TagLib::FileRef f(path.c_str(), false);
    if (f.isNull())
        return {};
    const auto pictures = f.complexProperties("PICTURE");
    for (const auto &p : pictures) {
        auto it = p.find("data");
        if (it == p.end())
            continue;
        const TagLib::ByteVector v = it->second.toByteVector();
        if (!v.isEmpty())
            return std::vector<unsigned char>(v.data(), v.data() + v.size());
    }
    return {};
}

} // namespace iTunes
