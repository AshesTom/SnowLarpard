// SPDX-License-Identifier: LGPL-2.1-or-later
// What QuickTime did for iTunes: one file played at a time (libmpv: FFmpeg's decoders, the session's
// sound server), its position and length, the volume; the end of a song and errors reported on
// AquaKit's run loop (mpv's wakeup pipe watched with the display's).
#pragma once

#include <functional>
#include <string>

struct mpv_handle;

namespace iTunes {

class Player {
public:
    Player();
    ~Player();
    bool available() const { return m_mpv != nullptr; }

    void play(const std::string &path);   // from its start
    void pause();
    void resume();
    void stop();
    bool isPlaying() const { return m_state == Playing; }
    bool isPaused() const { return m_state == Paused; }
    bool isStopped() const { return m_state == Stopped; }

    double position() const;   // seconds
    double duration() const;   // seconds, 0 when unknown
    void seek(double seconds);
    void setVolume(double v);  // 0..1, the slider's (a cubic curve, as the ear hears)
    double volume() const { return m_volume; }

    std::function<void()> onEnd;
    std::function<void(const std::string &)> onError;

private:
    void drain();
    double property(const char *name) const;
    enum State { Stopped, Playing, Paused } m_state = Stopped;
    mpv_handle *m_mpv = nullptr;
    int m_fd = -1;
    double m_volume = 1;
};

} // namespace iTunes
