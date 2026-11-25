// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's importing (docs/itunes-reverse.md section 32): the encoders of Import Settings (DLOG 183,
// General's Import Settings…; the old Advanced ▸ Importing tab 258 has the same items) — AAC, AIFF, Apple
// Lossless, MP3, WAV, each with its settings (5003's High Quality, iTunes Plus, Spoken Podcast; 3006's Good,
// High, Higher Quality; 3252's Automatic; Custom… in DLOG 5000, 3000, 3250) — and the import of a CD's
// checked tracks one after another: read (AudioCD::Reader), encoded by ffmpeg into the iTunes Music folder
// (MusicFolder::destination), the file added to the library.
#pragma once

#include "audiocd.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace iTunes {

// MENU 1342's items as iTunes fills it: the encoders by name
enum Encoder { AAC, AIFF, AppleLossless, MP3, WAV, EncoderCount };

struct EncoderSettings {
    int encoder = AAC;
    int preset = 1;            // the Setting popup's item (Custom… last)
    // Custom…: the stereo bit rate (kbps), VBR, the sample rate (0 auto), channels (0 auto, 1, 2),
    // AAC's Optimize for voice, MP3's VBR quality (0 lowest – 6 highest), joint stereo, smart encoding
    // adjustments, the filter below 10 Hz, PCM's sample size (0 auto, 8, 16)
    int bitrate = 256, sampleRate = 0, channels = 0, quality = 4, sampleSize = 0;
    bool vbr = true, voice = false, joint = true, smart = true, filter = true;
    bool errorCorrection = false;   // Use error correction when reading Audio CDs

    // Settings "itunes": the encoder chosen, or `encoder`'s own setting (Import Settings' popup changed)
    static EncoderSettings load(int encoder = -1);
    void save() const;
    // The Setting popup's items for the encoder (the last Custom…)
    static std::vector<std::string> presets(int encoder);
    // The preset's values put in (Custom keeps its own)
    void applyPreset();
    // Details: the settings described as iTunes does in the box under the popup
    std::string details() const;
    std::string extension() const;   // m4a, aif, mp3, wav
    std::string kind() const;        // the library's Kind (5000.009 AAC audio file…)
    // ffmpeg's arguments for the encoder, after its input
    std::vector<std::string> ffmpegArguments() const;
};

// The import of a CD's tracks: one at a time, on a thread of its own, its progress read by the window
class CDImport {
public:
    ~CDImport();
    // track i of `cd` into `path` (its folders made); false when ffmpeg can't be started
    bool start(const AudioCD &cd, size_t track, const std::string &path, const EncoderSettings &settings);
    bool running() const { return m_thread.joinable() && !m_finished; }
    bool finished() const { return m_finished; }
    bool succeeded() const { return m_ok; }
    void cancel();
    void join();
    double progress() const { return m_total ? double(m_done) / m_total : 0; }   // 0..1
    double elapsed() const;                                                    // seconds
    std::string path;

private:
    std::thread m_thread;
    std::atomic<bool> m_finished{false}, m_ok{false}, m_cancel{false};
    std::atomic<long> m_done{0};
    long m_total = 0;
    double m_started = 0;
};

} // namespace iTunes
