// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's audio CD (docs/itunes-reverse.md section 32): the disc in the drive as the source under
// DEVICES ('cd  ', FUN_00168254's group 0x44, Source Icons 441's row 17), its tracks as FUN_000d6058 makes
// them from the TOC ("Track 01", kind 9000.005 "Audio CD Track", 44.1 kHz, 1411 kbps, 2352 bytes a frame,
// 75 frames a second), the sectors read for importing (libcdio-paranoia: Use error correction when
// reading Audio CDs), Eject, and Get CD Track Names. iTunes asked Gracenote's CDDB (its SDK compiled in,
// www.cddb2.cddbp.net checked first by FUN_0030e604's HostIsReachableTask): a protocol of Apple's
// registered client, not usable here; the port asks MusicBrainz' web service with the disc's TOC instead
// (a substitute, over curl), and the names stay "Track 01"… as iTunes leaves them offline.
//   A disc is noticed by polling the drives (/dev/sr*: CDROM_DRIVE_STATUS, CDROM_DISC_STATUS). For tests
// only, ITUNES_TEST_CD names a folder of 44.1 kHz 16-bit stereo WAV files read as a disc's tracks (no
// drive needed); it is never looked at otherwise.
#pragma once

#include "library.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AK {
class Task;
}

namespace iTunes {

// The CD's row in the sources (its tag) and its media kind for the columns ('cd  ')
constexpr int CDTag = -170;
constexpr int CDKind = 0x63642020;
// A CD track's row id in the list (the library's are positive)
constexpr int CDRowBase = -300000;
inline int cdRow(size_t i) { return CDRowBase - int(i); }
inline bool isCDRow(int id) { return id <= CDRowBase && id > CDRowBase - 100; }
inline size_t cdIndex(int id) { return size_t(CDRowBase - id); }

struct DiscTrack {
    Track info;            // what the list shows (name, artist, album, genre, year, number, time…)
    int lsn = 0, frames = 0;
    bool audio = true, preemphasis = false;   // FUN_000d6058: a data track flagged 4, pre-emphasis 0x100000
    bool checked = true;   // the Name column's box: imported or not
    // the status column (TrackStatusIcons 306): waiting (column 8), importing (the progress animation),
    // done (3, the green check), failed (4)
    enum Status { None, Waiting, Importing, Done, Failed } status = None;
    int libraryId = 0;     // the song made from it
};

class AudioCD {
public:
    ~AudioCD();
    // The drive with an audio (or mixed) disc in it, "" none (also when ITUNES_TEST_CD names a folder:
    // that folder)
    static std::string probe();
    // The disc's TOC read (libcdio), or the test folder's WAV files as tracks
    static std::unique_ptr<AudioCD> open(const std::string &where);

    std::string device;           // /dev/sr0, or the test folder
    bool simulated = false;
    std::string name;             // the source's name: "Audio CD" (9003.019) until track names come
    std::vector<DiscTrack> tracks;
    int leadout = 0;              // the lead-out's LSN
    std::vector<std::string> files;   // the test folder's WAV files, a track each

    // What mpv plays for track i: the test file, or cdda:// with the track's span
    std::string playURL(size_t i) const;
    bool eject();

    // Reading track i's sectors in order (2352 bytes each: 16-bit little-endian stereo at 44.1 kHz);
    // through cdparanoia's full mode with error correction, else its plain reads
    class Reader {
    public:
        virtual ~Reader() = default;
        virtual bool next(unsigned char *sector) = 0;
    };
    std::unique_ptr<Reader> reader(size_t track, bool errorCorrection) const;

    // Get CD Track Names (Advanced ▸ gnam; on insertion with General's "Automatically retrieve CD track
    // names from Internet"): MusicBrainz' disc lookup by TOC; done(found) on the run loop
    void lookup(std::function<void(bool)> done);
    bool lookingUp() const { return m_lookup != nullptr; }
    // the first release's names applied (album, artist, year, the tracks' names and artists)
    bool applyNames(const std::string &xml);

private:
    std::unique_ptr<AK::Task> m_lookup;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);   // the run loop's callbacks check it
};

} // namespace iTunes
