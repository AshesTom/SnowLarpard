// SPDX-License-Identifier: LGPL-2.1-or-later
// The podcasts, as iTunes 8.2.1 handles them (docs/itunes-reverse.md section 29):
//   Advanced ▸ Subscribe to Podcast… (`pcsb`, FUN_000b390c): enabled unless the podcasts are hidden
//   (+0x1099D, FUN_000b5314); FUN_000b20dc asks for the URL (STR# 130.034 "Subscribe to Podcast", 130.003
//   "URL:"; the last one kept at +0x1AA4C) and FUN_003cd11c subscribes. The feed parser's strings: RSS's
//   channel, item, title, link, description, pubDate, enclosure (url, length, type), guid, category; the
//   iTunes namespace's (http://www.itunes.com/dtds/podcast-1.0.dtd, its other spellings) author, summary,
//   subtitle, duration, keywords, explicit, image, new-feed-url, block; Atom's entry, published, link
//   rel="alternate"/"enclosure"; the entities amp, copy, hellip, mdash, nbsp, ndash, reg, trade.
//   The episodes' fields (the plist): Podcast, Podcast URL (the feed), External GUID (the item's guid),
//   Release Date, Description, Album the podcast's title, Artist its author, Genre "Podcast".
//   The Podcasts pane (DITL 254): Check for new episodes (MENU 1374), When new episodes are available
//   (1375), Keep (1376).
//   Where the files go: iTunes Music/Podcasts/<the podcast>/ (by iTunes' own folders; the code not read).
#include "podcasts.h"

#include "appkit.h"
#include "foundation.h"
#include "musicfolder.h"
#include "resources.h"
#include "runloop.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

using namespace AK;

namespace iTunes {

namespace {

// ---- a small XML reader: elements, their attributes and text (CDATA kept, entities decoded)

struct Element {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<Element> children;
    const Element *child(const char *n) const
    {
        for (const Element &c : children)
            if (c.name == n)
                return &c;
        return nullptr;
    }
    std::string childText(const char *n) const { const Element *c = child(n); return c ? c->text : std::string(); }
};

void appendUtf8(std::string &o, unsigned cp)
{
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

// the entities XML has, and the HTML ones the feed parser knows (its table's strings)
std::string decode(const std::string &s)
{
    static const std::map<std::string, unsigned> named = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"copy", 0xA9}, {"hellip", 0x2026},
        {"mdash", 0x2014}, {"nbsp", 0xA0}, {"ndash", 0x2013}, {"reg", 0xAE}, {"trade", 0x2122},
    };
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            o += s[i];
            continue;
        }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (!e.empty() && e[0] == '#')
            appendUtf8(o, unsigned(e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtoul(e.c_str() + 2, nullptr, 16)
                                                                                : std::strtoul(e.c_str() + 1, nullptr, 10)));
        else if (auto it = named.find(e); it != named.end())
            appendUtf8(o, it->second);
        else {
            o += s[i];
            continue;
        }
        i = semi;
    }
    return o;
}

class XmlReader {
public:
    explicit XmlReader(const std::string &s) : m_s(s) {}
    bool parse(Element &root)
    {
        Element doc;
        if (!content(doc, ""))
            return false;
        for (Element &c : doc.children)
            if (!c.name.empty()) {
                root = std::move(c);
                return true;
            }
        return false;
    }

private:
    bool content(Element &parent, const std::string &closing)
    {
        while (m_p < m_s.size()) {
            const size_t lt = m_s.find('<', m_p);
            const std::string text = m_s.substr(m_p, (lt == std::string::npos ? m_s.size() : lt) - m_p);
            parent.text += decode(text);
            if (lt == std::string::npos)
                return closing.empty();
            m_p = lt;
            if (m_s.compare(m_p, 9, "<![CDATA[") == 0) {
                const size_t end = m_s.find("]]>", m_p);
                parent.text += m_s.substr(m_p + 9, (end == std::string::npos ? m_s.size() : end) - m_p - 9);
                m_p = end == std::string::npos ? m_s.size() : end + 3;
            } else if (m_s.compare(m_p, 4, "<!--") == 0) {
                const size_t end = m_s.find("-->", m_p);
                m_p = end == std::string::npos ? m_s.size() : end + 3;
            } else if (m_s.compare(m_p, 2, "<?") == 0 || m_s.compare(m_p, 2, "<!") == 0) {
                const size_t end = m_s.find('>', m_p);
                m_p = end == std::string::npos ? m_s.size() : end + 1;
            } else if (m_s.compare(m_p, 2, "</") == 0) {
                const size_t end = m_s.find('>', m_p);
                m_p = end == std::string::npos ? m_s.size() : end + 1;
                return true;   // lenient: any closing tag ends the element
            } else {
                Element e;
                bool empty = false;
                if (!tag(e, empty))
                    return false;
                if (!empty && !content(e, e.name))
                    return false;
                parent.children.push_back(std::move(e));
            }
        }
        return true;
    }
    bool tag(Element &e, bool &empty)
    {
        size_t p = m_p + 1;
        const size_t n = m_s.size();
        while (p < n && !std::isspace(static_cast<unsigned char>(m_s[p])) && m_s[p] != '>' && m_s[p] != '/')
            e.name += m_s[p++];
        while (p < n) {
            while (p < n && std::isspace(static_cast<unsigned char>(m_s[p])))
                ++p;
            if (p < n && m_s[p] == '/') {
                empty = true;
                ++p;
                continue;
            }
            if (p < n && m_s[p] == '>') {
                m_p = p + 1;
                return true;
            }
            std::string key;
            while (p < n && m_s[p] != '=' && m_s[p] != '>' && !std::isspace(static_cast<unsigned char>(m_s[p])))
                key += m_s[p++];
            while (p < n && std::isspace(static_cast<unsigned char>(m_s[p])))
                ++p;
            if (p < n && m_s[p] == '=') {
                ++p;
                while (p < n && std::isspace(static_cast<unsigned char>(m_s[p])))
                    ++p;
                const char q = p < n ? m_s[p] : '"';
                if (q == '"' || q == '\'') {
                    const size_t end = m_s.find(q, p + 1);
                    if (end == std::string::npos)
                        return false;
                    e.attrs[key] = decode(m_s.substr(p + 1, end - p - 1));
                    p = end + 1;
                }
            } else if (key.empty())
                ++p;
        }
        return false;
    }
    const std::string &m_s;
    size_t m_p = 0;
};

std::string trim(std::string s)
{
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The description as the list shows it: the HTML's tags taken out, the white space made single spaces
std::string plain(const std::string &html)
{
    std::string o;
    bool inTag = false, space = false;
    for (char c : decode(html)) {
        if (c == '<')
            inTag = true;
        else if (c == '>')
            inTag = false;
        else if (!inTag) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                space = !o.empty();
                continue;
            }
            if (space)
                o += ' ';
            space = false;
            o += c;
        }
    }
    return o;
}

// RFC 822's dates (RSS's pubDate), ISO 8601's (Atom's): as the plist's
std::string isoDate(const std::string &d)
{
    std::tm t{};
    const std::string s = trim(d);
    const char *end = nullptr;
    for (const char *f : {"%a, %d %b %Y %H:%M:%S", "%d %b %Y %H:%M:%S", "%a, %d %b %Y %H:%M", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d"}) {
        t = {};
        if ((end = strptime(s.c_str(), f, &t)))
            break;
    }
    if (!end)
        return {};
    time_t secs = timegm(&t);
    // the zone: +hhmm / -hhmm, or a name (GMT, UT, Z: none; EST… by their offsets)
    std::string zone = trim(end);
    if (!zone.empty() && (zone[0] == '+' || zone[0] == '-')) {
        std::string digits;
        for (char c : zone.substr(1))
            if (std::isdigit(static_cast<unsigned char>(c)))
                digits += c;
        if (digits.size() >= 4) {
            const int off = std::stoi(digits.substr(0, 2)) * 3600 + std::stoi(digits.substr(2, 2)) * 60;
            secs += zone[0] == '+' ? -off : off;
        }
    } else {
        static const std::map<std::string, int> zones = {{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
                                                         {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
        if (auto it = zones.find(zone); it != zones.end())
            secs -= it->second * 3600;
    }
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&secs));
    return buf;
}

// itunes:duration: seconds, or m:s, or h:m:s
int durationMs(const std::string &d)
{
    int parts[3] = {0, 0, 0}, n = 0;
    for (const char *p = d.c_str(); *p && n < 3;) {
        parts[n++] = std::atoi(p);
        p = std::strchr(p, ':');
        if (!p)
            break;
        ++p;
    }
    const int secs = n == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : n == 2 ? parts[0] * 60 + parts[1] : parts[0];
    return secs * 1000;
}

std::string itunesTag(const Element &e, const char *name)
{
    for (const char *prefix : {"itunes:", "itms:"}) {
        const std::string text = e.childText((std::string(prefix) + name).c_str());
        if (!text.empty())
            return text;
    }
    return {};
}

std::string kindForType(const std::string &type, const std::string &url)
{
    const std::string t = type, u = url.substr(0, url.find('?'));
    auto ends = [&](const char *e) { return u.size() > std::strlen(e) && u.compare(u.size() - std::strlen(e), std::strlen(e), e) == 0; };
    if (t.rfind("video/", 0) == 0 || ends(".m4v") || ends(".mp4") || ends(".mov"))
        return ends(".mov") || t == "video/quicktime" ? "QuickTime movie file" : "MPEG-4 video file";
    if (t == "audio/mpeg" || ends(".mp3"))
        return "MPEG audio file";
    if (t == "audio/x-m4a" || t == "audio/mp4" || t == "audio/aac" || ends(".m4a"))
        return "AAC audio file";
    return "MPEG audio file";
}

std::string sanitize(std::string s)
{
    for (char &c : s)
        if (c == '/' || c == ':' || static_cast<unsigned char>(c) < 0x20)
            c = '_';
    while (!s.empty() && (s[0] == '.' || s[0] == ' '))
        s.erase(0, 1);
    return s.empty() ? std::string("Podcast") : s.substr(0, 128);
}

// STR# 8015.004 "There was a problem downloading “^FILENAME”."
std::string problem(const std::string &what)
{
    std::string m = tr("8015.004");
    const size_t at = m.find("^FILENAME");
    return at == std::string::npos ? m : m.replace(at, 9, what);
}

bool onPath(const char *program)
{
    const char *path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/bin:/bin";
    size_t at = 0;
    while (at <= dirs.size()) {
        const size_t colon = dirs.find(':', at);
        const std::string d = dirs.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        struct stat st;
        if (!d.empty() && ::stat((d + "/" + program).c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
            return true;
        if (colon == std::string::npos)
            break;
        at = colon + 1;
    }
    return false;
}

} // namespace

bool parseFeed(const std::string &xml, const std::string &url, Feed &out)
{
    Element root;
    if (!XmlReader(xml).parse(root))
        return false;
    out.url = url;
    if (root.name == "rss" || root.name == "rdf:RDF") {
        const Element *ch = root.child("channel");
        if (!ch)
            return false;
        out.title = trim(decode(ch->childText("title")));
        out.author = trim(itunesTag(*ch, "author"));
        const std::string summary = itunesTag(*ch, "summary");
        out.description = plain(!summary.empty() ? summary : ch->childText("description"));
        const std::string category = ch->child("category") ? trim(ch->childText("category")) : std::string();
        const Element *items = root.name == "rss" ? ch : &root;
        for (const Element &it : items->children) {
            if (it.name != "item")
                continue;
            const Element *enc = it.child("enclosure");
            if (!enc || !enc->attrs.count("url"))
                continue;   // an item without a file: not an episode
            Track t;
            t.name = trim(it.childText("title"));
            t.path = trim(enc->attrs.at("url"));
            t.size = enc->attrs.count("length") ? std::atoll(enc->attrs.at("length").c_str()) : 0;
            t.kind = kindForType(enc->attrs.count("type") ? enc->attrs.at("type") : std::string(), t.path);
            t.hasVideo = t.kind.find("video") != std::string::npos || t.kind.find("movie") != std::string::npos;
            t.guid = trim(it.childText("guid"));
            if (t.guid.empty())
                t.guid = t.path;
            t.releaseDate = isoDate(it.childText("pubDate"));
            t.totalTime = durationMs(trim(itunesTag(it, "duration")));
            const std::string sub = itunesTag(it, "subtitle"), sum = itunesTag(it, "summary");
            t.description = plain(!sub.empty() ? sub : !sum.empty() ? sum : it.childText("description"));
            t.longDescription = plain(!sum.empty() ? sum : it.childText("description"));
            t.artist = trim(itunesTag(it, "author"));
            if (t.artist.empty())
                t.artist = out.author;
            t.album = out.title;
            t.genre = "Podcast";
            t.category = category;
            t.podcastURL = url;
            if (t.name.empty())
                t.name = t.path.substr(t.path.rfind('/') + 1);
            out.episodes.push_back(std::move(t));
        }
        return true;
    }
    if (root.name == "feed") {   // Atom
        out.title = trim(root.childText("title"));
        if (const Element *a = root.child("author"))
            out.author = trim(a->childText("name"));
        out.description = plain(root.childText("subtitle"));
        for (const Element &e : root.children) {
            if (e.name != "entry")
                continue;
            Track t;
            for (const Element &l : e.children)
                if (l.name == "link" && l.attrs.count("rel") && l.attrs.at("rel") == "enclosure" && l.attrs.count("href")) {
                    t.path = l.attrs.at("href");
                    t.size = l.attrs.count("length") ? std::atoll(l.attrs.at("length").c_str()) : 0;
                    t.kind = kindForType(l.attrs.count("type") ? l.attrs.at("type") : std::string(), t.path);
                }
            if (t.path.empty())
                continue;
            t.hasVideo = t.kind.find("video") != std::string::npos || t.kind.find("movie") != std::string::npos;
            t.name = trim(e.childText("title"));
            t.guid = trim(e.childText("id"));
            if (t.guid.empty())
                t.guid = t.path;
            t.releaseDate = isoDate(!e.childText("published").empty() ? e.childText("published") : e.childText("updated"));
            t.description = plain(e.childText("summary"));
            t.artist = out.author;
            t.album = out.title;
            t.genre = "Podcast";
            t.podcastURL = url;
            out.episodes.push_back(std::move(t));
        }
        return true;
    }
    return false;
}

// ---- the subscriptions

Podcasts::Podcasts(Library &library) : m_library(library)
{
    Settings s("itunes");
    const std::vector<std::string> urls = s.list("PodcastFeeds"), titles = s.list("PodcastTitles"),
                                   descriptions = s.list("PodcastDescriptions"), off = s.list("PodcastUnsubscribed");
    for (size_t i = 0; i < urls.size(); ++i) {
        Info f;
        f.url = urls[i];
        f.title = i < titles.size() ? titles[i] : std::string();
        f.description = i < descriptions.size() ? descriptions[i] : std::string();
        f.subscribed = std::find(off.begin(), off.end(), f.url) == off.end();
        m_feeds.push_back(f);
    }
}

Podcasts::~Podcasts() = default;

void Podcasts::save() const
{
    std::vector<std::string> urls, titles, descriptions, off;
    for (const Info &f : m_feeds) {
        urls.push_back(f.url);
        titles.push_back(f.title);
        descriptions.push_back(f.description);
        if (!f.subscribed)
            off.push_back(f.url);
    }
    Settings s("itunes");
    s.set("PodcastFeeds", urls);
    s.set("PodcastTitles", titles);
    s.set("PodcastDescriptions", descriptions);
    s.set("PodcastUnsubscribed", off);
}

const Podcasts::Info *Podcasts::feed(const std::string &url) const
{
    for (const Info &f : m_feeds)
        if (f.url == url)
            return &f;
    return nullptr;
}

std::string Podcasts::folderFor(const std::string &url) const
{
    const Info *f = feed(url);
    return MusicFolder::path() + "/Podcasts/" + sanitize(f ? f->title : std::string());
}

void Podcasts::subscribe(const std::string &given)
{
    std::string url = trim(given);
    // pcast:// and itpc:// are feeds over HTTP (the Store's links)
    for (const char *scheme : {"pcast://", "itpc://", "feed://"})
        if (url.rfind(scheme, 0) == 0)
            url = "http://" + url.substr(std::strlen(scheme));
    if (url.find("://") == std::string::npos)
        url = "http://" + url;
    if (Info *f = const_cast<Info *>(feed(url))) {
        f->subscribed = true;
        save();
        fetch(url, false);
        return;
    }
    fetch(url, true);
}

void Podcasts::refresh(const std::string &url)
{
    if (!url.empty()) {
        fetch(url, false);
        return;
    }
    for (const Info &f : m_feeds)
        if (f.subscribed)
            fetch(f.url, false);
    Settings("itunes").set("PodcastLastCheck", std::to_string(std::time(nullptr)));
}

void Podcasts::unsubscribe(const std::string &url)
{
    for (Info &f : m_feeds)
        if (f.url == url)
            f.subscribed = false;
    save();
    if (onChanged)
        onChanged();
}

void Podcasts::fetch(const std::string &url, bool first)
{
    if (m_fetches.count(url))
        return;
    if (!onPath("curl")) {
        if (onError)
            onError(problem(url));
        return;
    }
    auto task = std::make_unique<Task>();
    task->onExit = [this, url, first](int status, const std::string &out) {
        App::shared().later([this, url] { m_fetches.erase(url); });
        fetched(url, first, status, out);
    };
    // as iTunes asks (its User-Agent "iTunes/8.2.1")
    if (task->launch({"curl", "-sSL", "--max-time", "60", "-A", "iTunes/8.2.1 (Macintosh; U; Intel Mac OS X 10.6)", url}))
        m_fetches[url] = std::move(task);
}

void Podcasts::fetched(const std::string &url, bool first, int status, const std::string &xml)
{
    Feed f;
    if (status != 0 || !parseFeed(xml, url, f)) {
        if (first && onError)   // the feed could not be read (the message: by its text, the caller not read)
            onError(problem(url));
        return;
    }
    Info *info = const_cast<Info *>(feed(url));
    if (!info) {
        m_feeds.push_back({url, f.title, f.description, true});
        info = &m_feeds.back();
    } else {
        info->title = f.title.empty() ? info->title : f.title;
        info->description = f.description;
    }
    save();
    // what was there: the newest episode's date; an item past it is new
    std::string newest;
    for (const auto &[id, t] : m_library.tracks())
        if ((t.mediaKind & 4) && t.podcastURL == url)
            newest = std::max(newest, t.releaseDate);
    const bool had = !newest.empty();
    std::vector<int> fresh;
    for (Track &e : f.episodes) {
        const int id = m_library.addEpisode(e);
        if (!id)
            continue;
        Track *t = m_library.track(id);
        if (had && t->releaseDate <= newest)
            t->unplayed = false;   // an old item come back (one deleted): not new
        else
            fresh.push_back(id);
    }
    // When new episodes are available (MENU 1375): 0 download all, 1 the most recent one, 2 nothing
    const int when = Settings("itunes").integer("PodcastNew", 1);
    std::sort(fresh.begin(), fresh.end(), [&](int a, int b) { return m_library.track(a)->releaseDate > m_library.track(b)->releaseDate; });
    if (when == 0)
        for (int id : fresh)
            download(id);
    else if (when == 1 && !fresh.empty())
        download(fresh.front());
    m_library.save();
    if (first && onSubscribed)
        onSubscribed(url);
    if (onChanged)
        onChanged();
}

void Podcasts::download(int id)
{
    const Track *t = m_library.track(id);
    if (!t || !isStream(*t) || m_downloads.count(id))
        return;
    if (!onPath("curl")) {
        if (onError)
            onError(problem(t->name));
        return;
    }
    const std::string folder = folderFor(t->podcastURL);
    makePath(folder);
    std::string file = t->path.substr(0, t->path.find('?'));
    file = sanitize(file.substr(file.rfind('/') + 1));
    if (file.find('.') == std::string::npos)
        file += t->hasVideo ? ".m4v" : ".mp3";
    std::string dest = folder + "/" + file;
    struct stat st;
    for (int n = 1; ::stat(dest.c_str(), &st) == 0 && n < 100; ++n) {   // a name not taken: " 1", " 2"…
        const size_t dot = file.rfind('.');
        dest = folder + "/" + file.substr(0, dot) + " " + std::to_string(n) + file.substr(dot);
    }
    const std::string part = dest + ".download";
    auto task = std::make_unique<Task>();
    task->onExit = [this, id, dest, part](int status, const std::string &) {
        App::shared().later([this, id] { m_downloads.erase(id); });
        if (status == 0 && std::rename(part.c_str(), dest.c_str()) == 0) {
            const std::string url = m_library.track(id) ? m_library.track(id)->podcastURL : std::string();
            m_library.setFile(id, dest);
            applyKeep(url);
            m_library.save();
        } else
            std::remove(part.c_str());
        if (onChanged)
            onChanged();
    };
    if (task->launch({"curl", "-sSL", "--max-time", "3600", "-A", "iTunes/8.2.1", "-o", part, t->path})) {
        m_downloads[id] = std::move(task);
        if (onChanged)
            onChanged();
    }
}

// Keep (MENU 1376): 0 all episodes, 1 all unplayed, 2 the most recent, 3–6 the last 2 to 5, 7 the last 10;
// the others' files deleted and the episodes taken out of the library
void Podcasts::applyKeep(const std::string &url)
{
    const int keep = Settings("itunes").integer("PodcastKeep", 0);
    if (keep == 0 || url.empty())
        return;
    std::vector<const Track *> files;
    for (const auto &[id, t] : m_library.tracks())
        if ((t.mediaKind & 4) && t.podcastURL == url && !isStream(t))
            files.push_back(&t);
    std::sort(files.begin(), files.end(), [](const Track *a, const Track *b) { return a->releaseDate > b->releaseDate; });
    static const int counts[] = {0, 0, 1, 2, 3, 4, 5, 10};
    std::vector<int> gone;
    for (size_t i = 0; i < files.size(); ++i) {
        const bool out = keep == 1 ? !files[i]->unplayed : int(i) >= counts[std::min(keep, 7)];
        // only the files iTunes put in its Podcasts folder
        if (out && files[i]->path.find("/iTunes Music/Podcasts/") != std::string::npos)
            gone.push_back(files[i]->id);
    }
    for (int id : gone) {
        std::remove(m_library.track(id)->path.c_str());
        m_library.remove(id);
    }
}

void Podcasts::played(int id)
{
    Track *t = m_library.track(id);
    if (t && t->unplayed) {
        t->unplayed = false;
        m_library.save();
    }
}

void Podcasts::tick()
{
    // Check for new episodes (MENU 1374): every hour, day, week; manually (3)
    Settings s("itunes");
    const int every = s.integer("PodcastCheck", 1);
    if (every >= 3 || m_feeds.empty())
        return;
    static const long long seconds[] = {3600, 86400, 604800};
    const long long last = std::atoll(s.string("PodcastLastCheck", "0").c_str());
    if (std::time(nullptr) - last >= seconds[every])
        refresh();
}

} // namespace iTunes
