// SPDX-License-Identifier: LGPL-2.1-or-later
#include "audiocd.h"

#include "appkit.h"
#include "panels.h"
#include "resources.h"
#include "runloop.h"

#ifdef ITUNES_CDIO
#include <cdio/cdio.h>
#include <cdio/paranoia/cdda.h>
#include <cdio/paranoia/paranoia.h>
#endif

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace AK;

namespace iTunes {

namespace {

constexpr int SectorBytes = 2352;   // CDIO_CD_FRAMESIZE_RAW: 588 stereo 16-bit samples

// "Track 01" (9000.006 with NumToString's number, a 0 put before a single digit: FUN_000d6058)
std::string trackName(int n)
{
    std::string s = tr("9000.006");
    char num[8];
    std::snprintf(num, sizeof num, "%02d", n);
    const size_t at = s.find("^1");
    if (at != std::string::npos)
        s.replace(at, 2, num);
    return s;
}

// The track as FUN_000d6058 fills it: its number and the count, the time (frames × 1000 / 75), the size
// (frames × 2352), 44100 Hz, 1411 kbps, the kind "Audio CD Track"
void fill(DiscTrack &d, int number, int count)
{
    Track &t = d.info;
    t.id = cdRow(size_t(number - 1));
    t.trackNumber = number;
    t.trackCount = count;
    t.name = trackName(number);
    t.kind = tr("9000.005");
    t.totalTime = int((long long)d.frames * 1000 / 75);
    t.size = (long long)d.frames * SectorBytes;
    t.sampleRate = 44100;
    t.bitRate = 1411;
}

// A WAV file's PCM data: its offset and length ("data" chunk), 0 when it isn't 44.1 kHz 16-bit stereo
bool wavData(const std::string &path, long &offset, long &length)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    unsigned char h[12];
    bool ok = std::fread(h, 1, 12, f) == 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4);
    bool format = false;
    while (ok) {
        unsigned char c[8];
        if (std::fread(c, 1, 8, f) != 8) {
            ok = false;
            break;
        }
        const long size = long(c[4] | c[5] << 8 | c[6] << 16 | unsigned(c[7]) << 24);
        if (!std::memcmp(c, "fmt ", 4)) {
            unsigned char fm[16];
            if (size < 16 || std::fread(fm, 1, 16, f) != 16) {
                ok = false;
                break;
            }
            const int channels = fm[2] | fm[3] << 8, bits = fm[14] | fm[15] << 8;
            const long rate = long(fm[4] | fm[5] << 8 | fm[6] << 16 | unsigned(fm[7]) << 24);
            format = (fm[0] | fm[1] << 8) == 1 && channels == 2 && bits == 16 && rate == 44100;
            std::fseek(f, size - 16 + (size & 1), SEEK_CUR);
        } else if (!std::memcmp(c, "data", 4)) {
            offset = std::ftell(f);
            length = size;
            break;
        } else
            std::fseek(f, size + (size & 1), SEEK_CUR);
    }
    std::fclose(f);
    return ok && format;
}

class FileReader : public AudioCD::Reader {
public:
    FileReader(const std::string &path, long offset, long length) : m_left(length)
    {
        m_f = std::fopen(path.c_str(), "rb");
        if (m_f)
            std::fseek(m_f, offset, SEEK_SET);
    }
    ~FileReader() override
    {
        if (m_f)
            std::fclose(m_f);
    }
    bool next(unsigned char *sector) override
    {
        if (!m_f)
            return false;
        std::memset(sector, 0, SectorBytes);   // the last sector padded with silence, as a disc's frames are whole
        const size_t want = size_t(std::min<long>(SectorBytes, std::max<long>(0, m_left)));
        const size_t got = want ? std::fread(sector, 1, want, m_f) : 0;
        m_left -= long(got);
        return got == want;
    }

private:
    FILE *m_f = nullptr;
    long m_left;
};

#ifdef ITUNES_CDIO
// cdparanoia's reads: full paranoia (overlap checks, scratch repair, never skipping) with error
// correction, else none of its fixups (FUN_003b9548's +0x2a8 preference chooses the read mode)
class DiscReader : public AudioCD::Reader {
public:
    DiscReader(const std::string &device, int lsn, bool correction)
    {
        m_cdio = cdio_open(device.c_str(), DRIVER_DEVICE);
        if (!m_cdio)
            return;
        char *messages = nullptr;
        m_drive = cdio_cddap_identify_cdio(m_cdio, CDDA_MESSAGE_FORGETIT, &messages);
        std::free(messages);
        if (!m_drive || cdio_cddap_open(m_drive) != 0)
            return;
        m_paranoia = cdio_paranoia_init(m_drive);
        cdio_paranoia_modeset(m_paranoia, correction ? PARANOIA_MODE_FULL : PARANOIA_MODE_DISABLE);
        cdio_paranoia_seek(m_paranoia, lsn, SEEK_SET);
    }
    ~DiscReader() override
    {
        if (m_paranoia)
            cdio_paranoia_free(m_paranoia);
        if (m_drive)
            cdio_cddap_close_no_free_cdio(m_drive);
        if (m_cdio)
            cdio_destroy(m_cdio);
    }
    bool next(unsigned char *sector) override
    {
        if (!m_paranoia)
            return false;
        const int16_t *data = cdio_paranoia_read_limited(m_paranoia, nullptr, 20);
        if (!data)
            return false;
        std::memcpy(sector, data, SectorBytes);
        return true;
    }

private:
    CdIo_t *m_cdio = nullptr;
    cdrom_drive_t *m_drive = nullptr;
    cdrom_paranoia_t *m_paranoia = nullptr;
};
#endif

// XML text between <tag> and its </tag> from `from`; the entities undone
std::string element(const std::string &xml, const std::string &tag, size_t from = 0, size_t *end = nullptr)
{
    const std::string open = "<" + tag;
    size_t a = xml.find(open, from);
    while (a != std::string::npos && xml[a + open.size()] != '>' && xml[a + open.size()] != ' ')
        a = xml.find(open, a + 1);
    if (a == std::string::npos)
        return {};
    a = xml.find('>', a);
    const size_t b = xml.find("</" + tag + ">", a);
    if (a == std::string::npos || b == std::string::npos)
        return {};
    if (end)
        *end = b;
    std::string v = xml.substr(a + 1, b - a - 1);
    for (const auto &[e, c] : std::vector<std::pair<std::string, std::string>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}})
        for (size_t p = 0; (p = v.find(e, p)) != std::string::npos; p += c.size())
            v.replace(p, e.size(), c);
    return v;
}

// An artist credit's names joined as MusicBrainz writes them (each name and its join phrase)
std::string credit(const std::string &xml)
{
    std::string out;
    for (size_t at = xml.find("<name-credit"); at != std::string::npos; at = xml.find("<name-credit", at + 1)) {
        size_t end = xml.find("</name-credit>", at);
        const std::string nc = xml.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const size_t jp = nc.find("joinphrase=\"");
        out += element(nc, "name");   // the credited name, before the artist's own when there is one
        if (jp != std::string::npos)
            out += nc.substr(jp + 12, nc.find('"', jp + 12) - jp - 12);
    }
    return out;
}

} // namespace

AudioCD::~AudioCD() { *m_alive = false; }

std::string AudioCD::probe()
{
    if (const char *test = std::getenv("ITUNES_TEST_CD"); test && *test)
        return test;
    for (int i = 0; i < 4; ++i) {
        const std::string dev = "/dev/sr" + std::to_string(i);
        const int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        const int drive = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
        const int disc = drive == CDS_DISC_OK ? ioctl(fd, CDROM_DISC_STATUS, 0) : -1;
        ::close(fd);
        if (disc == CDS_AUDIO || disc == CDS_MIXED)
            return dev;
    }
    return {};
}

std::unique_ptr<AudioCD> AudioCD::open(const std::string &where)
{
    auto cd = std::make_unique<AudioCD>();
    cd->device = where;
    cd->name = tr("9003.019");
    const char *test = std::getenv("ITUNES_TEST_CD");
    if (test && where == test) {
        // the test disc: its WAV files in their names' order, a track each, laid out from LSN 0
        cd->simulated = true;
        DIR *d = opendir(where.c_str());
        if (!d)
            return nullptr;
        for (dirent *e; (e = readdir(d));) {
            const std::string n = e->d_name;
            if (n.size() > 4 && (n.substr(n.size() - 4) == ".wav" || n.substr(n.size() - 4) == ".WAV"))
                cd->files.push_back(where + "/" + n);
        }
        closedir(d);
        std::sort(cd->files.begin(), cd->files.end());
        int lsn = 0;
        for (const std::string &f : cd->files) {
            long off = 0, len = 0;
            if (!wavData(f, off, len))
                continue;
            DiscTrack t;
            t.lsn = lsn;
            t.frames = int((len + SectorBytes - 1) / SectorBytes);
            lsn += t.frames;
            cd->tracks.push_back(t);
        }
        cd->leadout = lsn;
    } else {
#ifdef ITUNES_CDIO
        CdIo_t *p = cdio_open(where.c_str(), DRIVER_DEVICE);
        if (!p)
            return nullptr;
        const track_t first = cdio_get_first_track_num(p), n = cdio_get_num_tracks(p);
        if (first == CDIO_INVALID_TRACK || n == CDIO_INVALID_TRACK) {
            cdio_destroy(p);
            return nullptr;
        }
        for (track_t i = first; i < first + n; ++i) {
            DiscTrack t;
            t.lsn = cdio_get_track_lsn(p, i);
            const lsn_t next = cdio_get_track_lsn(p, i + 1 < first + n ? track_t(i + 1) : track_t(CDIO_CDROM_LEADOUT_TRACK));
            t.frames = int(next - t.lsn);
            t.audio = cdio_get_track_format(p, i) == TRACK_FORMAT_AUDIO;
            t.preemphasis = cdio_get_track_preemphasis(p, i) == CDIO_TRACK_FLAG_TRUE;
            cd->tracks.push_back(t);
        }
        cd->leadout = cdio_get_track_lsn(p, CDIO_CDROM_LEADOUT_TRACK);
        cdio_destroy(p);
        // an Enhanced CD's data session after the audio: the last audio track ends 11 400 frames before it
        if (cd->tracks.size() > 1 && !cd->tracks.back().audio && cd->tracks[cd->tracks.size() - 2].audio)
            cd->tracks[cd->tracks.size() - 2].frames = std::max(0, cd->tracks[cd->tracks.size() - 2].frames - 11400);
        // the data tracks are not shown (FUN_000d6058 flags them 4; the CD playlist leaves them out)
        cd->tracks.erase(std::remove_if(cd->tracks.begin(), cd->tracks.end(), [](const DiscTrack &t) { return !t.audio; }),
                         cd->tracks.end());
#endif
    }
    if (cd->tracks.empty())
        return nullptr;
    for (size_t i = 0; i < cd->tracks.size(); ++i) {
        fill(cd->tracks[i], int(i + 1), int(cd->tracks.size()));
        cd->tracks[i].info.album = cd->name;
    }
    return cd;
}

std::string AudioCD::playURL(size_t i) const
{
    if (simulated)
        return i < files.size() ? files[i] : std::string();
    return "cdda://";
}

bool AudioCD::eject()
{
    if (simulated)
        return true;
#ifdef ITUNES_CDIO
    return cdio_eject_media_drive(device.c_str()) == DRIVER_OP_SUCCESS;
#else
    return Task::launchDetached({"eject", device});
#endif
}

std::unique_ptr<AudioCD::Reader> AudioCD::reader(size_t track, bool errorCorrection) const
{
    if (track >= tracks.size())
        return nullptr;
    if (simulated) {
        long off = 0, len = 0;
        if (track >= files.size() || !wavData(files[track], off, len))
            return nullptr;
        return std::make_unique<FileReader>(files[track], off, len);
    }
#ifdef ITUNES_CDIO
    return std::make_unique<DiscReader>(device, tracks[track].lsn, errorCorrection);
#else
    (void)errorCorrection;
    return nullptr;
#endif
}

void AudioCD::lookup(std::function<void(bool)> done)
{
    // the TOC as MusicBrainz takes it: the first and last track, the lead-out and each track's offset,
    // in frames from the disc's start (the LSN plus the 150-frame lead-in); "-" asks by TOC alone
    std::string toc = "1+" + std::to_string(tracks.size()) + "+" + std::to_string(leadout + 150);
    for (const DiscTrack &t : tracks)
        toc += "+" + std::to_string(t.lsn + 150);
    const std::string url = "https://musicbrainz.org/ws/2/discid/-?toc=" + toc + "&cdstubs=no&inc=artist-credits+recordings";
    if (simulated) {
        // the test disc: the answer kept in its folder (musicbrainz.xml), the web service not asked
        std::string xml;
        if (FILE *f = std::fopen((device + "/musicbrainz.xml").c_str(), "rb")) {
            char buf[65536];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
                xml.append(buf, n);
            std::fclose(f);
        }
        m_lookup = std::make_unique<Task>();   // a moment of "Getting Track Names…"
        App::shared().addTimer(1, false, [this, alive = m_alive, done, xml] {
            if (!*alive)
                return;
            m_lookup.reset();
            done(applyNames(xml));
        });
        return;
    }
    if (!Workspace::inPath("curl")) {
        App::shared().later([done] { done(false); });
        return;
    }
    m_lookup = std::make_unique<Task>();
    m_lookup->onExit = [this, done](int status, const std::string &out) {
        const bool found = status == 0 && applyNames(out);
        App::shared().later([this, alive = m_alive, done, found] {
            if (!*alive)
                return;
            m_lookup.reset();
            done(found);
        });
    };
    if (!m_lookup->launch({"curl", "-sSf", "--max-time", "30", "-A", "iTunes/8.2.1 (CD lookup)", url})) {
        m_lookup.reset();
        App::shared().later([done] { done(false); });
    }
}

bool AudioCD::applyNames(const std::string &xml)
{
    // the first release with a medium of as many tracks (130.010's choice among several matches: the first)
    for (size_t r = xml.find("<release "); r != std::string::npos; r = xml.find("<release ", r + 1)) {
        const size_t rEnd = xml.find("</release>", r);
        const std::string rel = xml.substr(r, rEnd == std::string::npos ? std::string::npos : rEnd - r);
        const std::string album = element(rel, "title");
        const size_t media = rel.find("<medium-list");
        const std::string artist = credit(rel.substr(0, media));
        const std::string date = element(rel.substr(0, media), "date");
        for (size_t m = rel.find("<medium>", media); m != std::string::npos; m = rel.find("<medium>", m + 1)) {
            const size_t mEnd = rel.find("</medium>", m);
            const std::string med = rel.substr(m, mEnd == std::string::npos ? std::string::npos : mEnd - m);
            const size_t tl = med.find("<track-list count=\"");
            if (tl == std::string::npos || std::atoi(med.c_str() + tl + 19) != int(tracks.size()))
                continue;
            size_t at = tl;
            for (DiscTrack &d : tracks) {
                at = med.find("<track ", at + 1);
                if (at == std::string::npos)
                    break;
                const size_t tEnd = med.find("</track>", at);
                const std::string tk = med.substr(at, tEnd == std::string::npos ? std::string::npos : tEnd - at);
                const size_t rec = tk.find("<recording");
                // the track's own title and credit when it has them, else its recording's
                std::string title = element(tk.substr(0, rec), "title");
                if (title.empty() && rec != std::string::npos)
                    title = element(tk, "title", rec);
                std::string who = credit(tk.substr(0, rec));
                if (who.empty() && rec != std::string::npos)
                    who = credit(tk.substr(rec));
                d.info.name = title.empty() ? d.info.name : title;
                d.info.artist = who.empty() ? artist : who;
                d.info.album = album;
                d.info.albumArtist = who.empty() || who == artist ? std::string() : artist;
                d.info.compilation = artist == "Various Artists";
                d.info.year = date.size() >= 4 ? std::atoi(date.c_str()) : 0;
                const size_t disc = med.find("<position>");
                const int discNo = disc != std::string::npos && disc < tl ? std::atoi(med.c_str() + disc + 10) : 0;
                const size_t count = rel.find("<medium-list count=\"");
                const int discs = count != std::string::npos ? std::atoi(rel.c_str() + count + 20) : 0;
                if (discs > 1) {
                    d.info.discNumber = discNo;
                    d.info.discCount = discs;
                }
            }
            name = album.empty() ? name : album;
            return true;
        }
    }
    return false;
}

} // namespace iTunes
