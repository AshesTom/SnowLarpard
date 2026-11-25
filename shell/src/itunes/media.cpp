// SPDX-License-Identifier: LGPL-2.1-or-later
// The library's media kinds (docs/itunes-reverse.md section 29): what a file is to iTunes 8.2.1 — music,
// a movie, a TV show, a music video, an audiobook, a podcast episode — and the distinguished kinds'
// playlists the LIBRARY sources show.
//
//   FUN_0017b984 (a track's media kind, cached by FUN_00181e2e): the kind byte its tags give (MP4's
//   'stik': 1, 3, 4 music; 2 audiobook 8; 5 0x10; 6 music video 0x20; 7, 8 podcast 4; 9 movie 2; 10 TV
//   show 0x40; 11 0x80; 14 ringtone 0x4000); none: a track with video (+0x3C bit 0x8000) is a movie
//   when nothing else says what it is (0xE62 clear); else by the file's type ('M4B ', 'AA  ', 'AAX ' an
//   audiobook; 'M4R ' a ringtone; 'PDF ' a booklet); the podcast flag (0x10000000) adds 4; music else.
//   FUN_002e68aa: each distinguished playlist a smart one on that field (0x3c).
#include "library.h"

#include "foundation.h"

#include <taglib/fileref.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sys/stat.h>

namespace iTunes {

namespace {

std::string ext(const std::string &path)
{
    const size_t dot = path.rfind('.'), slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    std::string e = path.substr(dot + 1);
    for (char &c : e)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// An MPEG-4 / QuickTime file's tracks (ISO 14496-12's boxes): whether one is a video track (its
// 'hdlr' handler 'vide'), and its 'tkhd' width and height (16.16 fixed)
struct VideoInfo {
    bool video = false;
    int width = 0, height = 0;
};

uint32_t be32(const unsigned char *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void scanBoxes(std::ifstream &f, uint64_t from, uint64_t to, int depth, VideoInfo &info, int &w, int &h)
{
    uint64_t at = from;
    while (at + 8 <= to && depth < 8) {
        unsigned char head[16];
        f.seekg(std::streamoff(at));
        if (!f.read(reinterpret_cast<char *>(head), 8))
            return;
        uint64_t size = be32(head);
        const std::string type(reinterpret_cast<char *>(head + 4), 4);
        uint64_t body = at + 8;
        if (size == 1) {
            if (!f.read(reinterpret_cast<char *>(head + 8), 8))
                return;
            size = uint64_t(be32(head + 8)) << 32 | be32(head + 12);
            body = at + 16;
        } else if (size == 0)
            size = to - at;
        if (size < 8 || at + size > to)
            return;
        if (type == "moov" || type == "trak" || type == "mdia") {
            int tw = w, th = h;
            if (type == "trak")
                tw = th = 0;
            scanBoxes(f, body, at + size, depth + 1, info, tw, th);
            if (type == "trak" && info.video && !info.width) {
                info.width = tw;
                info.height = th;
            }
            if (type == "mdia") {
                w = tw;
                h = th;
            }
        } else if (type == "tkhd") {
            // version 0: 84 bytes, width and height its last eight; version 1: 96
            unsigned char b[100];
            const uint64_t n = std::min<uint64_t>(size - (body - at), sizeof b);
            f.seekg(std::streamoff(body));
            if (f.read(reinterpret_cast<char *>(b), std::streamsize(n)) && n >= 84) {
                const size_t off = b[0] == 1 ? 88 : 76;
                if (off + 8 <= n) {
                    w = int(be32(b + off) >> 16);
                    h = int(be32(b + off + 4) >> 16);
                }
            }
        } else if (type == "hdlr") {
            unsigned char b[12];
            f.seekg(std::streamoff(body));
            if (f.read(reinterpret_cast<char *>(b), 12) && std::memcmp(b + 8, "vide", 4) == 0)
                info.video = true;
        }
        at += size;
    }
}

VideoInfo videoInfo(const std::string &path)
{
    VideoInfo info;
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return info;
    f.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(f.tellg());
    int w = 0, h = 0;
    scanBoxes(f, 0, size, 0, info, w, h);
    return info;
}

} // namespace

bool inDistinguished(const Track &t, int kind)
{
    const int k = t.mediaKind;
    switch (kind) {
    case Music: return (k & 0x1121b1) != 0 && !(k & 4);
    case Movies: return (k & 0x8006) == 2;
    case TVShows: return (k & 0x8044) == 0x40;
    case Audiobooks: return (k & 0x800c) == 8;
    case Ringtones: return (k & 0x4000) != 0;
    case Rentals: return (k & 0x8002) == 0x8002;
    case Podcasts: return (k & 4) != 0;
    case Applications: return (k & 0x20000) != 0;
    }
    return false;
}

std::vector<int> Library::items(int kind) const
{
    std::vector<int> out;
    for (const auto &[id, t] : m_tracks)
        if (inDistinguished(t, kind))
            out.push_back(id);
    return out;
}

void Library::readMedia(Track &t) const
{
    const std::string e = ext(t.path);
    int stik = -1;
    bool podcast = t.mediaKind & 4;
    if (e == "m4a" || e == "m4b" || e == "m4v" || e == "mp4" || e == "mov" || e == "m4p" || e == "m4r") {
        if (e != "m4a" && e != "m4b" && e != "m4r") {
            const VideoInfo v = videoInfo(t.path);
            t.hasVideo = v.video;
            t.videoWidth = v.width;
            t.videoHeight = v.height;
        }
        TagLib::MP4::File f(t.path.c_str(), false);
        if (f.isValid() && f.tag()) {
            const TagLib::MP4::Tag *tag = f.tag();
            auto text = [&](const char *k) {
                return tag->contains(k) && !tag->item(k).toStringList().isEmpty() ? tag->item(k).toStringList().front().to8Bit(true) : std::string();
            };
            if (tag->contains("stik"))
                stik = tag->item("stik").toByte();
            if (tag->contains("pcst") && tag->item("pcst").toBool())
                podcast = true;
            t.series = text("tvsh");
            t.episode = text("tven");
            if (tag->contains("tvsn"))
                t.season = tag->item("tvsn").toInt();
            if (tag->contains("tves"))
                t.episodeOrder = tag->item("tves").toInt();
            t.description = text("desc");
            t.longDescription = text("ldes");
            t.category = text("catg");
            if (t.podcastURL.empty())
                t.podcastURL = text("purl");
            if (t.guid.empty())
                t.guid = text("egid");
        }
    }
    // FUN_0017b984
    int k = 0;
    switch (stik) {
    case 1: case 3: case 4: k = 1; break;
    case 2: k = 8; break;
    case 5: k = 0x10; break;
    case 6: k = 0x20; break;
    case 7: case 8: k = 4; break;
    case 9: k = 2; break;
    case 10: k = 0x40; break;
    case 11: k = 0x80; break;
    case 14: k = 0x4000; break;
    default: break;
    }
    if (t.hasVideo && !(k & 0xe62))
        k |= 2;
    else if (!k) {
        if (e == "m4b" || e == "aa" || e == "aax")
            k = 8;
        else if (e == "m4r")
            k = 0x4000;
        else if (e == "pdf")
            k = 0x10;
    }
    if (podcast)
        k |= 4;
    t.mediaKind = k ? k : 1;
    // Get Info's Options, "Remember playback position": on for the spoken and the long ones (by eye: the
    // kinds iTunes 8 sets it for, its default not read)
    t.rememberBookmark = (t.mediaKind & (2 | 4 | 8 | 0x40)) != 0;
    if (t.hasVideo)
        t.kind = e == "mov" ? "QuickTime movie file" : e == "m4p" || e == "m4v" ? "MPEG-4 video file" : "MPEG-4 video file";
}

int Library::addEpisode(const Track &episode)
{
    for (const auto &[id, t] : m_tracks)
        if ((t.mediaKind & 4) && t.podcastURL == episode.podcastURL &&
            ((!episode.guid.empty() && t.guid == episode.guid) || (!episode.path.empty() && t.path == episode.path)))
            return 0;
    Track t = episode;
    t.id = m_nextId++;
    t.mediaKind = 4 | (t.hasVideo ? 2 : 0);
    t.unplayed = true;
    t.rememberBookmark = true;
    if (t.dateAdded.empty()) {
        char buf[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        t.dateAdded = buf;
    }
    m_tracks[t.id] = t;
    for (Playlist &p : m_playlists)
        if (p.master)
            p.items.push_back(t.id);
    m_addedKinds |= t.mediaKind;
    return t.id;
}

void Library::setFile(int id, const std::string &path)
{
    Track *t = track(id);
    if (!t)
        return;
    t->path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0)
        t->size = st.st_size;
    const int kind = t->mediaKind;
    TagLib::FileRef f(path.c_str());
    if (!f.isNull() && f.audioProperties()) {
        t->totalTime = f.audioProperties()->lengthInMilliseconds();
        t->bitRate = f.audioProperties()->bitrate();
        t->sampleRate = f.audioProperties()->sampleRate();
    }
    const std::string e = ext(path);
    if (e == "mp4" || e == "m4v" || e == "mov") {
        const VideoInfo v = videoInfo(path);
        t->hasVideo = v.video;
        t->videoWidth = v.width;
        t->videoHeight = v.height;
    }
    t->mediaKind = (kind & 4) | (t->hasVideo ? 2 : 0);
    if (!t->mediaKind)
        t->mediaKind = 1;
    t->kind = t->hasVideo ? (e == "mov" ? "QuickTime movie file" : "MPEG-4 video file")
              : e == "mp3"  ? "MPEG audio file"
              : e == "m4a" || e == "mp4" || e == "aac" ? "AAC audio file"
                                         : t->kind;
}

} // namespace iTunes
