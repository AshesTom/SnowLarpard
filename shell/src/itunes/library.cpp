// SPDX-License-Identifier: LGPL-2.1-or-later
#include "library.h"

#include "foundation.h"
#include "resources.h"

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
#include <random>
#include <functional>
#include <iconv.h>
#include <cerrno>
#include <iterator>
#include <set>
#include <sstream>
#include <tuple>
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
    if (e == "m4a" || e == "aac" || e == "mp4" || e == "m4b")
        return "AAC audio file";
    if (e == "m4v")
        return "MPEG-4 video file";   // STR# 3301 (QuickTime's): .004, .001
    if (e == "mov")
        return "QuickTime movie file";
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

// The media kinds' keys (the plist iTunes 8.2.1 writes, FUN_000ab536's strings): Has Video, Video Width
// and Height, Movie, Music Video, TV Show, Podcast, Unplayed; Series, Season, Episode, Episode Order,
// Description, Long Description, Category, Release Date, Podcast URL, External GUID; Remember Bookmark,
// Bookmark Time
void readKinds(const Node &t, Track &tr)
{
    auto flag = [&](const char *k) { const Node *n = t.get(k); return n && n->text == "true"; };
    tr.hasVideo = flag("Has Video");
    tr.videoWidth = int(t.num("Video Width"));
    tr.videoHeight = int(t.num("Video Height"));
    int k = 0;
    if (flag("Movie"))
        k |= 2;
    if (flag("Music Video"))
        k |= 0x20;
    if (flag("TV Show"))
        k |= 0x40;
    if (flag("Podcast"))
        k |= 4;
    if (const Node *n = t.get("Media Kind"))   // ours: the kinds the plist has no key for (audiobooks…)
        k |= int(std::atoll(n->text.c_str()));
    if (tr.hasVideo && !(k & 0xe62))
        k |= 2;
    tr.mediaKind = k ? k : 1;
    tr.unplayed = flag("Unplayed");
    tr.series = t.str("Series");
    tr.season = int(t.num("Season"));
    tr.episode = t.str("Episode");
    tr.episodeOrder = int(t.num("Episode Order"));
    tr.description = t.str("Description");
    tr.longDescription = t.str("Long Description");
    tr.category = t.str("Category");
    tr.releaseDate = t.str("Release Date");
    tr.podcastURL = t.str("Podcast URL");
    tr.guid = t.str("External GUID");
    tr.rememberBookmark = flag("Remember Bookmark");
    tr.bookmark = int(t.num("Bookmark Time"));
}

void writeKinds(std::ostream &o, const Track &t)
{
    auto str = [&](const char *k, const std::string &v) {
        if (!v.empty())
            o << "\t\t\t<key>" << k << "</key><string>" << xmlEscape(v) << "</string>\n";
    };
    auto num = [&](const char *k, long long v) {
        if (v)
            o << "\t\t\t<key>" << k << "</key><integer>" << v << "</integer>\n";
    };
    auto yes = [&](const char *k, bool v) {
        if (v)
            o << "\t\t\t<key>" << k << "</key><true/>\n";
    };
    if (!t.releaseDate.empty())
        o << "\t\t\t<key>Release Date</key><date>" << t.releaseDate << "</date>\n";
    yes("Has Video", t.hasVideo);
    num("Video Width", t.videoWidth);
    num("Video Height", t.videoHeight);
    yes("Movie", (t.mediaKind & 0x8006) == 2);
    yes("Music Video", t.mediaKind & 0x20);
    yes("TV Show", t.mediaKind & 0x40);
    yes("Podcast", t.mediaKind & 4);
    yes("Unplayed", t.unplayed);
    if (t.mediaKind & ~(1 | 2 | 4 | 0x20 | 0x40))
        num("Media Kind", t.mediaKind);
    str("Series", t.series);
    num("Season", t.season);
    str("Episode", t.episode);
    num("Episode Order", t.episodeOrder);
    str("Description", t.description);
    str("Long Description", t.longDescription);
    str("Category", t.category);
    str("Podcast URL", t.podcastURL);
    str("External GUID", t.guid);
    yes("Remember Bookmark", t.rememberBookmark);
    num("Bookmark Time", t.bookmark);
}

} // namespace

std::vector<int> duplicates(const Library &library, const std::vector<int> &ids, bool exact)
{
    struct Key {
        int id;
        bool video;
        std::string name, artist, album;
        const Track *t;
    };
    std::vector<Key> keys;
    for (int id : ids)
        if (const Track *t = library.track(id)) {
            const std::string kind = lower(t->kind);
            keys.push_back({id, kind.find("video") != std::string::npos || kind.find("movie") != std::string::npos,
                            lower(t->name), lower(t->artist), lower(t->album), t});
        }
    // the list sorted by Name (FUN_0045386e with field 2) while the duplicates are shown, the artist
    // and album as the sort's secondary keys
    std::stable_sort(keys.begin(), keys.end(), [](const Key &a, const Key &b) {
        return std::tie(a.name, a.artist, a.album) < std::tie(b.name, b.artist, b.album);
    });
    std::set<int> marked;
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const Key &a = keys[i];
        for (size_t j = i + 1; j < keys.size(); ++j) {
            const Key &b = keys[j];
            if (a.video != b.video)
                continue;   // a different kind: passed over, the run goes on
            if (exact && (a.t->trackNumber != b.t->trackNumber || a.t->trackCount != b.t->trackCount ||
                          a.t->discNumber != b.t->discNumber || a.t->discCount != b.t->discCount))
                continue;
            if (a.name != b.name || a.artist != b.artist)
                break;   // the first that differs ends the run
            if (exact && a.album != b.album)
                continue;
            marked.insert(a.id);
            marked.insert(b.id);
        }
    }
    std::vector<int> out;
    for (int id : ids)
        if (marked.count(id))
            out.push_back(id);
    return out;
}

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
    static const std::set<std::string> exts = {"mp3", "m4a", "m4p", "aac", "aif", "aiff", "wav", "flac", "ogg", "oga", "opus", "mp4",
                                                // the spoken and the moving ones (section 29)
                                                "m4b", "m4r", "m4v", "mov"};
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
            readKinds(t, tr);
            {
                const std::string loc = t.str("Location");
                tr.path = loc.rfind("file:", 0) == 0 || loc.find("://") == std::string::npos ? pathFromURL(loc) : loc;
            }
            if (!(tr.mediaKind & ~1)) {   // nothing in the plist says: the file's type (FUN_0017b984)
                const std::string e = extension(tr.path);
                tr.mediaKind = e == "m4b" || e == "aa" || e == "aax" ? 8 : e == "m4r" ? 0x4000 : tr.mediaKind;
            }
            if (tr.id > 0) {
                m_tracks[tr.id] = tr;
                m_nextId = std::max(m_nextId, tr.id + 1);
            }
        }
    if (const Node *lists = root.get("Playlists"))
        for (const Node &p : lists->array) {
            // the distinguished ones (Music, Movies, TV Shows, Podcasts, Audiobooks) made again from the
            // media kinds on saving: their IDs kept
            if (const Node *d = p.get("Distinguished Kind")) {
                m_distinguishedIds[std::atoi(d->text.c_str())] = p.str("Playlist Persistent ID");
                continue;
            }
            Playlist pl;
            pl.name = p.str("Name");
            const Node *m = p.get("Master");
            pl.master = m && m->text == "true";
            const Node *f = p.get("Folder");
            pl.folder = f && f->text == "true";
            pl.persistentId = p.str("Playlist Persistent ID");
            pl.parentId = p.str("Parent Persistent ID");
            if (pl.persistentId.empty())
                pl.persistentId = newPersistentId();
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

bool Library::save() const { return saveTo(file(), nullptr, nullptr); }

bool Library::saveTo(const std::string &path, const std::set<int> *songs, const std::vector<size_t> *lists) const
{
    const std::string dir = dirName(path);
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
        if (songs && !songs->count(id))
            continue;
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
        writeKinds(o, t);
        if (isStream(t)) {
            o << "\t\t\t<key>Track Type</key><string>URL</string>\n";
            str("Location", t.path);
        } else {
            o << "\t\t\t<key>Track Type</key><string>File</string>\n";
            str("Location", fileURL(t.path));
        }
        o << "\t\t</dict>\n";
    }
    o << "\t</dict>\n\t<key>Playlists</key>\n\t<array>\n";
    for (size_t pi = 0; pi < m_playlists.size(); ++pi) {
        const Playlist &p = m_playlists[pi];
        if (lists && std::find(lists->begin(), lists->end(), pi) == lists->end())
            continue;
        o << "\t\t<dict>\n\t\t\t<key>Name</key><string>" << xmlEscape(p.name) << "</string>\n";
        if (p.master)
            o << "\t\t\t<key>Master</key><true/>\n\t\t\t<key>Visible</key><false/>\n";
        if (!p.persistentId.empty())
            o << "\t\t\t<key>Playlist Persistent ID</key><string>" << p.persistentId << "</string>\n";
        if (!p.parentId.empty())
            o << "\t\t\t<key>Parent Persistent ID</key><string>" << p.parentId << "</string>\n";
        if (p.folder)
            o << "\t\t\t<key>Folder</key><true/>\n";
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
        const std::vector<int> items = p.folder ? folderItems(p) : p.items;
        for (int id : items)
            o << "\t\t\t\t<dict>\n\t\t\t\t\t<key>Track ID</key><integer>" << id << "</integer>\n\t\t\t\t</dict>\n";
        o << "\t\t\t</array>\n\t\t</dict>\n";
    }
    if (!lists)
        writeDistinguished(o, songs);
    o << "\t</array>\n</dict>\n</plist>\n";
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
            return false;
        f << o.str();
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

void Library::writeDistinguished(std::ostream &o, const std::set<int> *songs) const
{
    // FUN_002e68aa's names (STR# 9000: 36 Music, 34 Movies, 35 TV Shows, 37 Audiobooks; 30 Podcasts) and
    // the plist's own key for each
    static const struct { int kind; const char *name, *key; } kinds[] = {
        {Music, "9000.036", nullptr},     {Movies, "9000.034", "Movies"},       {TVShows, "9000.035", "TV Shows"},
        {Podcasts, "9000.030", "Podcasts"}, {Audiobooks, "9000.037", "Audiobooks"},
    };
    for (const auto &k : kinds) {
        std::string &pid = m_distinguishedIds[k.kind];
        if (pid.empty())
            pid = newPersistentId();
        o << "\t\t<dict>\n\t\t\t<key>Name</key><string>" << xmlEscape(tr(k.name)) << "</string>\n"
          << "\t\t\t<key>Playlist Persistent ID</key><string>" << pid << "</string>\n"
          << "\t\t\t<key>Distinguished Kind</key><integer>" << k.kind << "</integer>\n";
        if (k.key)
            o << "\t\t\t<key>" << k.key << "</key><true/>\n";
        o << "\t\t\t<key>All Items</key><true/>\n\t\t\t<key>Playlist Items</key>\n\t\t\t<array>\n";
        for (int id : items(k.kind))
            if (!songs || songs->count(id))
                o << "\t\t\t\t<dict>\n\t\t\t\t\t<key>Track ID</key><integer>" << id << "</integer>\n\t\t\t\t</dict>\n";
        o << "\t\t\t</array>\n\t\t</dict>\n";
    }
}

namespace {

// UTF-8 to the Mac's encoding (Plain Text) or UTF-16LE (Unicode Text on Intel: FUN_000a9f4e's 0x0009)
std::string encodeAs(const std::string &utf8, const char *to)
{
    iconv_t cd = iconv_open(to, "UTF-8");
    if (cd == iconv_t(-1))
        return utf8;
    std::string out(utf8.size() * 4 + 4, '\0');
    char *in = const_cast<char *>(utf8.data()), *op = out.data();
    size_t inLeft = utf8.size(), outLeft = out.size();
    while (inLeft && iconv(cd, &in, &inLeft, &op, &outLeft) == size_t(-1)) {
        if (errno != EILSEQ && errno != EINVAL)
            break;
        ++in;   // a character the encoding has not: left out
        --inLeft;
    }
    iconv_close(cd);
    out.resize(out.size() - outLeft);
    return out;
}

std::string decodeFrom(const std::string &bytes, const char *from)
{
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == iconv_t(-1))
        return bytes;
    std::string out(bytes.size() * 4 + 4, '\0');
    char *in = const_cast<char *>(bytes.data()), *op = out.data();
    size_t inLeft = bytes.size(), outLeft = out.size();
    while (inLeft && iconv(cd, &in, &inLeft, &op, &outLeft) == size_t(-1)) {
        if (errno != EILSEQ && errno != EINVAL)
            break;
        ++in;
        --inLeft;
    }
    iconv_close(cd);
    out.resize(out.size() - outLeft);
    return out;
}

bool validUtf8(const std::string &s)
{
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + size_t(n) > s.size())
            return false;
        for (int k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + size_t(k)]) >> 6) != 2)
                return false;
        i += size_t(n);
    }
    return true;
}

// a cell: FUN_000a9f4e puts a space for each character under 0x20
std::string cell(std::string v)
{
    for (char &c : v)
        if (static_cast<unsigned char>(c) < 0x20)
            c = ' ';
    return v;
}

// a date as the export writes it (FUN_00294440: the short date and time, local)
std::string shortDate(const std::string &iso)
{
    if (iso.empty())
        return {};
    std::tm t{};
    if (!strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%SZ", &t))
        return {};
    const time_t secs = timegm(&t);
    std::tm local{};
    localtime_r(&secs, &local);
    char buf[64];
    const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
    std::snprintf(buf, sizeof buf, "%d/%d/%02d %d:%02d %s", local.tm_mon + 1, local.tm_mday, local.tm_year % 100, hour, local.tm_min,
                  local.tm_hour < 12 ? "AM" : "PM");
    return buf;
}

std::string number(long long v) { return v ? std::to_string(v) : std::string(); }

} // namespace

bool Library::exportText(const std::string &path, const std::vector<int> &ids, bool unicode) const
{
    // the columns (STR# 132) in FUN_000aa206's order, by their fields
    static const int columns[][2] = {{2, 1},   {4, 2},   {18, 3},  {3, 4},   {39, 25}, {8, 5},   {12, 6},
                                     {13, 7},  {24, 8},  {24, 9},  {11, 10}, {11, 11}, {7, 12},  {10, 13},
                                     {16, 14}, {5, 15},  {6, 16},  {74, 17}, {9, 18},  {17, 19}, {14, 20},
                                     {22, 22}, {23, 23}, {68, 27}, {69, 28}, {25, 24}, {15, 21}};
    std::string text;
    bool first = true;
    for (const auto &c : columns) {
        text += (first ? "" : "\t") + cell(tr("132." + std::string(c[1] < 10 ? "00" : "0") + std::to_string(c[1])));
        first = false;
    }
    text += "\r";   // the end of a line: a carriage return, as iTunes' text exports have it
    for (int id : ids) {
        const Track *t = track(id);
        if (!t)
            continue;
        const std::string v[] = {
            t->name, t->artist, t->composer, t->album, t->grouping, t->genre,
            t->size ? std::to_string(t->size) : std::string(),   // %llu
            number(t->totalTime / 1000),                         // seconds
            number(t->discNumber), number(t->discCount), number(t->trackNumber), number(t->trackCount), number(t->year),
            std::string(),                                       // Date Modified: not kept
            shortDate(t->dateAdded), number(t->bitRate), number(t->sampleRate),
            std::string(),                                       // Volume Adjustment
            t->kind, std::string(),                              // Equalizer
            t->comments, number(t->playCount), std::string(),    // Last Played
            std::string(), std::string(),                        // Skip Count, Last Skipped
            number(t->rating), t->path,                          // Location: the file's path (HFS on the Mac)
        };
        for (size_t i = 0; i < std::size(v); ++i)
            text += (i ? "\t" : "") + cell(v[i]);
        text += "\r";
    }
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    if (unicode)
        f << std::string("\xFF\xFE") << encodeAs(text, "UTF-16LE");
    else
        f << encodeAs(text, "MACINTOSH");
    return bool(f);
}

bool Library::importPlaylist(const std::string &path, std::vector<std::pair<std::string, std::vector<int>>> &lists)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    const std::string ext = extension(path);
    std::string base = path.substr(path.rfind('/') + 1);
    if (const size_t dot = base.rfind('.'); dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);
    const std::string folder = dirName(path);
    // a line's file or URL, its path from the playlist's folder when relative
    auto resolve = [&](std::string entry, const std::string &name) -> int {
        while (!entry.empty() && (entry.back() == '\r' || entry.back() == ' '))
            entry.pop_back();
        if (entry.empty())
            return 0;
        if (entry.rfind("file://", 0) == 0)
            entry = pathFromURL(entry);
        if (entry.find("://") != std::string::npos) {
            const int id = addURL(entry);
            if (!name.empty() && track(id) && track(id)->name != name)
                track(id)->name = name;   // #EXTINF's or TitleN's name for a stream (FUN_000d99b4's info)
            return id;
        }
        if (entry.find(':') != std::string::npos && entry[0] != '/') {
            // an HFS path (the Mac's text exports): its volume left out
            entry = entry.substr(entry.find(':'));
            std::replace(entry.begin(), entry.end(), ':', '/');
        }
        if (entry[0] != '/')
            entry = folder + "/" + entry;
        if (char *real = ::realpath(entry.c_str(), nullptr)) {   // "../" and links resolved
            entry = real;
            std::free(real);
        }
        int id = idForPath(entry);
        if (!id && isAudio(entry)) {
            add({entry});
            id = idForPath(entry);
        }
        return id;
    };
    std::vector<int> ids;
    auto lines = [](const std::string &text) {
        std::vector<std::string> out;
        std::string line;
        for (char c : text) {
            if (c == '\n' || c == '\r') {
                out.push_back(line);
                line.clear();
            } else
                line += c;
        }
        if (!line.empty())
            out.push_back(line);
        return out;
    };
    if (data.size() >= 2 && ((data[0] == '\xFF' && data[1] == '\xFE') || (data[0] == '\xFE' && data[1] == '\xFF')))
        data = decodeFrom(data.substr(2), data[0] == '\xFF' ? "UTF-16LE" : "UTF-16BE");
    else if (ext != "m3u8" && !validUtf8(data))
        data = decodeFrom(data, ext == "m3u" || ext == "pls" ? "WINDOWS-1252" : "MACINTOSH");
    if (ext == "m3u" || ext == "m3u8") {
        // FUN_000d9fa2: #EXTM3U first, then #EXTINF:length,title naming the next entry
        std::string name;
        for (const std::string &l : lines(data)) {
            if (l.rfind("#EXTINF", 0) == 0) {
                const size_t comma = l.find(',');
                name = comma == std::string::npos ? std::string() : l.substr(comma + 1);
            } else if (!l.empty() && l[0] != '#') {
                if (int id = resolve(l, name))
                    ids.push_back(id);
                name.clear();
            }
        }
    } else if (ext == "pls") {
        // FUN_000d9cf2: FileN= starts an entry (the one before added), TitleN= names it
        std::string file, name;
        auto flush = [&] {
            if (!file.empty())
                if (int id = resolve(file, name))
                    ids.push_back(id);
            file.clear();
            name.clear();
        };
        for (const std::string &l : lines(data)) {
            const size_t eq = l.find('=');
            if (eq == std::string::npos)
                continue;
            if (l.rfind("File", 0) == 0) {
                flush();
                file = l.substr(eq + 1);
            } else if (l.rfind("Title", 0) == 0)
                name = l.substr(eq + 1);
        }
        flush();
    } else if (data.find("<plist") != std::string::npos) {
        // an XML: its playlists (not the master), their songs found by their Location
        Node root;
        if (!Parser(data).parse(root) || root.kind != Node::Dict)
            return false;
        std::map<std::string, int> byKey;
        if (const Node *tracks = root.get("Tracks"))
            for (const auto &[key, t] : tracks->dict)
                if (int id = resolve(t.str("Location"), t.str("Name")))
                    byKey[std::to_string(t.num("Track ID"))] = id;
        if (const Node *pls = root.get("Playlists"))
            for (const Node &p : pls->array) {
                const Node *m = p.get("Master");
                if ((m && m->text == "true") || p.get("Folder"))
                    continue;
                std::vector<int> songs;
                if (const Node *items = p.get("Playlist Items"))
                    for (const Node &i : items->array)
                        if (auto it = byKey.find(std::to_string(i.num("Track ID"))); it != byKey.end())
                            songs.push_back(it->second);
                lists.push_back({p.str("Name"), songs});
            }
        if (lists.empty()) {
            for (const auto &[k, id] : byKey)
                ids.push_back(id);
            lists.push_back({base, ids});
        }
        return true;
    } else {
        // a text export: its Location column
        const std::vector<std::string> rows = lines(data);
        if (rows.empty())
            return false;
        auto split = [](const std::string &l) {
            std::vector<std::string> c;
            size_t at = 0;
            for (size_t tab; (tab = l.find('\t', at)) != std::string::npos; at = tab + 1)
                c.push_back(l.substr(at, tab - at));
            c.push_back(l.substr(at));
            return c;
        };
        const std::vector<std::string> head = split(rows[0]);
        const auto loc = std::find(head.begin(), head.end(), tr("132.021"));
        if (loc == head.end())
            return false;
        const size_t col = size_t(loc - head.begin());
        for (size_t r = 1; r < rows.size(); ++r) {
            const std::vector<std::string> c = split(rows[r]);
            if (col < c.size())
                if (int id = resolve(c[col], {}))
                    ids.push_back(id);
        }
    }
    lists.push_back({base, ids});
    return true;
}

std::string Library::newPersistentId()
{
    static std::mt19937_64 random(std::random_device{}());
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(random()));
    return buf;
}

std::vector<int> Library::folderItems(const Playlist &folder) const
{
    std::vector<int> out;
    std::set<int> seen;
    std::function<void(const std::string &, int)> walk = [&](const std::string &parent, int depth) {
        if (depth > 16)
            return;
        for (const Playlist &p : m_playlists) {
            if (p.parentId != parent || p.master)
                continue;
            if (p.folder) {
                walk(p.persistentId, depth + 1);
                continue;
            }
            const std::vector<int> ids = p.smart && p.info.live ? evaluate(p.info) : p.items;
            for (int id : ids)
                if (seen.insert(id).second)
                    out.push_back(id);
        }
    };
    walk(folder.persistentId, 0);
    return out;
}

int Library::addURL(const std::string &url)
{
    for (const auto &[id, t] : m_tracks)
        if (t.path == url)
            return id;
    // FUN_00261b94's parts: the scheme up to ":/", the host, then the path after its "/"; the file name
    // its last component, before a "?"
    std::string rest = url;
    const size_t scheme = rest.find(":/");
    if (scheme != std::string::npos)
        rest = rest.substr(scheme + 1);
    rest.erase(0, rest.find_first_not_of('/'));
    const size_t slash = rest.find('/');
    const std::string path = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    std::string file = path.substr(0, path.find('?'));
    if (const size_t last = file.rfind('/'); last != std::string::npos)
        file = file.substr(last + 1);
    Track t;
    t.id = m_nextId++;
    t.path = url;
    t.name = (!file.empty() ? file : !path.empty() ? path : url).substr(0, 255);
    t.kind = tr("3101.003");   // "MPEG audio stream" (the MP3 decoder's: its format once read)
    t.dateAdded = isoNow();
    m_tracks[t.id] = t;
    for (Playlist &p : m_playlists)
        if (p.master)
            p.items.push_back(t.id);
    return t.id;
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
    readMedia(t);
    m_addedKinds |= t.mediaKind;
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
