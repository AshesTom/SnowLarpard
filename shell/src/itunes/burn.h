// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's burning (docs/itunes-reverse.md section 32): File ▸ Burn Playlist to Disc (burn) and the
// Burn Disc button open Burn Settings (DLOG 185; the old Advanced ▸ Burning tab 259 has the same items);
// Burn checks the playlist's songs (8014.*), splits them across discs when they don't fit
// (FUN_00085bcc: 75 frames a second rounded up, 99 tracks a disc; 169.023 / 169.024 asked first),
// then writes each disc, its progress in the LCD (FUN_00089b1c: 169.007 Please insert a blank disc…,
// 169.013 Preparing Disc…, 169.014 Burning Disc… with 169.002 Writing “^1”, 169.003 finishing).
//   Disc Recording is the Mac's; here the burner is a program found on the PATH when it is needed:
// cdrskin, wodim or cdrecord (CDs; also data discs with an ISO image made by xorriso, genisoimage or
// mkisofs), growisofs (data DVDs). With none, iTunes' own state: "No Disc Recording software found"
// (162.004) in Burn Settings and "Disc Recording not found…" (169.022) in the LCD; with a program but no
// drive "No supported Disc burners found" (162.002) and "Disc burner or software not found…" (169.005).
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

struct Burner {
    std::string device;   // dev= for the program
    std::string name;     // vendor and model, as the popup lists them
};

struct BurnSettings {
    int speed = 10;           // MENU 1349: 1x … 24x, -, Maximum Possible
    int format = 0;           // 0 Audio CD, 1 MP3 CD, 2 Data CD or DVD
    int gap = 2;              // MENU 1352: none, 1 … 5 seconds
    bool soundCheck = false, cdText = false;
    std::string burner;       // the device chosen
    static BurnSettings load();
    void save() const;
};

namespace Burning {
// The burning program on the PATH (cdrskin, wodim, cdrecord), "" none
std::string program();
// Its drives (`--devices`, or cdrecord's `-scanbus`): done(burners) on the run loop
void findBurners(std::function<void(std::vector<Burner>)> done);
// FUN_00085bcc's split: the tracks (their times in ms) laid on discs of `capacity` frames, each track's
// frames (ms × 75 + 999) / 1000 after its gap (the first's 2 s), at most 99 a disc; the tracks' indexes
// per disc (a track longer than a disc on one of its own)
std::vector<std::vector<size_t>> split(const std::vector<int> &ms, int gapSeconds, long capacity);
} // namespace Burning

// One burn: the playlist's songs onto one disc or more
class BurnJob {
public:
    BurnJob(const std::vector<Track> &songs, const BurnSettings &settings, const std::string &name);
    ~BurnJob();
    // the LCD's two lines and the bar (0..1, < 0 none)
    std::function<void(const std::string &, const std::string &, double)> onStatus;
    // a question (169.023…) for the window to ask: its buttons' answer given back with answer()
    std::function<void(const std::string &message, const std::string &informative, std::vector<std::string> buttons,
                       std::function<void(int)> answer)> onAsk;
    std::function<void(bool ok, const std::string &error)> onDone;
    void start();
    void cancel();
    bool running() const { return m_running; }

private:
    void checkMedia();
    void prepareDisc();
    void decodeNext();
    void write();
    void finish(bool ok, const std::string &error);
    std::vector<Track> m_songs;
    BurnSettings m_settings;
    std::string m_name, m_program, m_dir;
    std::vector<std::vector<size_t>> m_discs;
    size_t m_disc = 0, m_decoded = 0;
    long m_capacity = 359849;   // an 80-minute CD-R's lead-out (ATIP), until the disc says
    std::unique_ptr<AK::Task> m_task;
    int m_poll = 0;
    bool m_running = false;
    std::string m_last;
};

} // namespace iTunes
