// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's audio CD in use (docs/itunes-reverse.md section 32): the drive watched for a disc (the
// Mac's DiskArbitration; here a poll of the drives every 2 seconds), the disc's track names asked for,
// its checked tracks imported one after another (FUN_0009932c, Import CD; FUN_0009921e stops it), each
// track's status for the list's status column, and the LCD's lines while that goes on (9003.007
// Importing “^1”, 9003.010 Time remaining: ^1 (^2); 130.004 Getting Track Names…, 130.007).
#pragma once

#include "audiocd.h"
#include "cdimport.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace iTunes {

class CDDrive {
public:
    CDDrive();
    ~CDDrive();
    void start();   // the drives watched from now on

    AudioCD *disc() const { return m_cd.get(); }
    // a disc came (true) or went (false)
    std::function<void(bool inserted)> onDisc;
    // the disc's names or a track's status changed: its rows redrawn
    std::function<void()> onChanged;
    // a track's file made: added to the library, its song's ID given back (0: not added)
    std::function<int(const std::string &path)> onImported;
    // the whole import over (Import CD and Eject ejects then)
    std::function<void(bool completed)> onImportEnded;
    // Get CD Track Names over: whether names were found
    std::function<void(bool found)> onNames;

    bool eject();
    void getNames();
    bool lookingUp() const { return m_cd && m_cd->lookingUp(); }

    // The tracks given imported in order with the Import Settings as they are now
    void import(const std::vector<size_t> &tracks);
    void stopImport();
    bool importing() const { return m_import != nullptr || !m_queue.empty(); }
    int importingTrack() const { return m_current; }   // -1 none
    // The LCD's lines and its bar (0..1) while importing or getting names; false: the song's lines
    bool status(std::string &line1, std::string &line2, double &progress) const;

private:
    void poll();
    void tick();
    void next();
    std::unique_ptr<AudioCD> m_cd;
    std::unique_ptr<CDImport> m_import;
    std::deque<size_t> m_queue;
    EncoderSettings m_settings;
    int m_current = -1;
    int m_pollTimer = 0, m_tickTimer = 0;
    bool m_completed = true;
};

} // namespace iTunes
