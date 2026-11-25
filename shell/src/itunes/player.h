// SPDX-License-Identifier: LGPL-2.1-or-later
// What QuickTime did for iTunes: one file played at a time (libmpv: FFmpeg's decoders, the session's
// sound server), its position and length, the volume; the end of a song and errors reported on
// AquaKit's run loop (mpv's wakeup pipe watched with the display's).
#pragma once

#include <functional>
#include <string>

struct mpv_handle;
struct mpv_render_context;

namespace iTunes {

class Player {
public:
    Player();
    ~Player();
    bool available() const { return m_mpv != nullptr; }

    // from its start (or `from` seconds in: a bookmark); its picture too when video
    void play(const std::string &path, bool video = false, double from = 0);
    // The picture of a movie playing (docs/itunes-reverse.md section 29): libmpv's software renderer,
    // a frame drawn into a buffer (cairo's RGB24) of the size given; onFrame when a new one is ready
    bool hasVideo() const { return m_video && m_render; }
    bool renderFrame(int width, int height, int stride, void *pixels);
    void videoSize(int &width, int &height) const;
    std::function<void()> onFrame;
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
    // The Equalizer (EQWindow): a preamp and ten bands at 32, 64, 125, 250, 500 Hz, 1, 2, 4, 8 and
    // 16 kHz, in dB (±12); off: no filter
    void setEqualizer(bool on, double preamp, const double bands[10]);
    // Sound Check: the songs' own loudness (their ReplayGain tags) evened out
    void setSoundCheck(bool on);
    // The visualizer's sound: FFmpeg's astats and aspectralstats on what plays (af-metadata), read
    // each frame: the level (RMS, dB), the spectrum's centroid and roll-off (Hz) and its flux
    struct Levels {
        double rms = -100, centroid = 0, rolloff = 0, flux = 0;
    };
    void setAnalysis(bool on);
    Levels levels() const;

    std::function<void()> onEnd;
    std::function<void(const std::string &)> onError;

private:
    void drain();
    double property(const char *name) const;
    enum State { Stopped, Playing, Paused } m_state = Stopped;
    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_render = nullptr;
    int m_frames[2] = {-1, -1};   // the render context's update callback's pipe
    bool m_video = false;
    int m_fd = -1;
    double m_volume = 1;
    std::string m_eq;          // the equalizer's filters
    bool m_analysis = false;
    void applyFilters();
};

} // namespace iTunes
