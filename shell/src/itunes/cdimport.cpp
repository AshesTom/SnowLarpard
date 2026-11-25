// SPDX-License-Identifier: LGPL-2.1-or-later
#include "cdimport.h"

#include "appkit.h"
#include "foundation.h"
#include "resources.h"
#include "runloop.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace AK;

namespace iTunes {

namespace {

std::string key(int encoder, const char *field) { return "Import." + std::to_string(encoder) + "." + field; }

std::string with(std::string s, const std::string &a, const std::string &b = {})
{
    size_t at = s.find("^1");
    if (at != std::string::npos)
        s.replace(at, 2, a);
    at = s.find("^2");
    if (at != std::string::npos)
        s.replace(at, 2, b);
    return s;
}

// 3000's / 5000's sample rates (Hz) as the popups list them after Auto
const int Rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

std::string khz(int hz)
{
    char b[16];
    std::snprintf(b, sizeof b, "%.3f", hz / 1000.0);
    return with(tr("5003.012"), b);
}

} // namespace

EncoderSettings EncoderSettings::load(int encoder)
{
    Settings s("itunes");
    EncoderSettings e;
    e.encoder = std::clamp(encoder >= 0 ? encoder : int(s.integer("ImportEncoder", AAC)), 0, EncoderCount - 1);
    // the AAC encoder's own configuration at first: iTunes Plus (FUN_00200926 copies "iTunesPlus", the
    // table at 0xabf76c: Better, iTunesPlus, Podcast, Custom); MP3 High Quality, the PCM ones Automatic
    const int first[EncoderCount] = {1, 0, 0, 1, 0};
    e.preset = int(s.integer(key(e.encoder, "preset"), first[e.encoder]));
    e.errorCorrection = s.boolean("ImportErrorCorrection", false);
    const int n = int(presets(e.encoder).size());
    e.preset = std::clamp(e.preset, 0, n - 1);
    if (e.preset == n - 1 && n > 1) {   // Custom…: its own values
        // DLOG 5000's Default Settings (FUN_00200586): 256 kbps stereo (128 mono), VBR
        e.bitrate = int(s.integer(key(e.encoder, "bitrate"), e.encoder == MP3 ? 160 : 256));
        e.vbr = s.boolean(key(e.encoder, "vbr"), e.encoder == AAC);
        e.sampleRate = int(s.integer(key(e.encoder, "rate"), 0));
        e.channels = int(s.integer(key(e.encoder, "channels"), 0));
        e.quality = int(s.integer(key(e.encoder, "quality"), 4));
        e.sampleSize = int(s.integer(key(e.encoder, "size"), 0));
        e.voice = s.boolean(key(e.encoder, "voice"), false);
        e.joint = s.boolean(key(e.encoder, "joint"), true);
        e.smart = s.boolean(key(e.encoder, "smart"), true);
        e.filter = s.boolean(key(e.encoder, "filter"), true);
    } else
        e.applyPreset();
    return e;
}

void EncoderSettings::save() const
{
    Settings s("itunes");
    s.set("ImportEncoder", encoder);
    s.set(key(encoder, "preset"), preset);
    s.set("ImportErrorCorrection", errorCorrection);
    if (preset == int(presets(encoder).size()) - 1 && presets(encoder).size() > 1) {
        s.set(key(encoder, "bitrate"), bitrate);
        s.set(key(encoder, "vbr"), vbr);
        s.set(key(encoder, "rate"), sampleRate);
        s.set(key(encoder, "channels"), channels);
        s.set(key(encoder, "quality"), quality);
        s.set(key(encoder, "size"), sampleSize);
        s.set(key(encoder, "voice"), voice);
        s.set(key(encoder, "joint"), joint);
        s.set(key(encoder, "smart"), smart);
        s.set(key(encoder, "filter"), filter);
    }
}

std::vector<std::string> EncoderSettings::presets(int encoder)
{
    switch (encoder) {
    case AAC: return {tr("5003.001"), tr("5003.002"), tr("5003.003"), tr("5003.004")};
    case MP3: return {tr("3006.001"), tr("3006.002"), tr("3006.003"), tr("3006.004")};
    case AppleLossless: return {tr("5003.015")};
    default: return {tr("3252.001"), tr("3252.002")};
    }
}

void EncoderSettings::applyPreset()
{
    sampleRate = 0;
    channels = 0;
    sampleSize = 0;
    voice = false;
    vbr = false;
    joint = true;
    smart = true;
    filter = true;
    if (encoder == AAC) {
        // High Quality 128 kbps; iTunes Plus 256 kbps VBR (the Custom dialog's defaults, FUN_00200586);
        // Spoken Podcast 64 kbps, optimized for voice (its rate by Apple's documentation, not read)
        const int rates[3] = {128, 256, 64};
        bitrate = rates[std::clamp(preset, 0, 2)];
        vbr = preset == 1;
        voice = preset == 2;
    } else if (encoder == MP3) {
        const int rates[3] = {128, 160, 192};
        bitrate = rates[std::clamp(preset, 0, 2)];
    }
}

std::string EncoderSettings::details() const
{
    // the strings iTunes puts together for the Details box (5003.*, 3002.*, 3251.*), in that order
    std::string s;
    if (encoder == AAC) {
        s = with(tr("5003.009"), std::to_string(bitrate / 2), std::to_string(bitrate));
        if (sampleRate)
            s += tr("5003.005") + khz(sampleRate);
        if (vbr)
            s += tr("5003.005") + tr("5003.013");
        if (voice)
            s += tr("5003.005") + tr("5003.014");
        s += tr("5003.005") + tr("5003.008") + tr("5003.006");
    } else if (encoder == MP3) {
        s = with(tr("3002.001"), std::to_string(bitrate / 2), std::to_string(bitrate));
        s += tr("3002.003") + tr(joint ? "3002.007" : "3002.006");
        if (vbr)
            s += tr("3002.003") + with(tr("3002.008"), tr("3003.00" + std::to_string(std::clamp(quality, 0, 6) + 1)));
        s += tr("3002.003") + tr("3002.013") + tr("3002.011");
    } else if (encoder == AppleLossless) {
        s = tr("5003.016") + tr("5003.006");
    } else if (preset == 0) {
        s = tr("3251.006") + tr("3251.007");
    } else {
        s = sampleRate ? khz(sampleRate) : tr("3252.001");
        if (sampleSize)
            s += tr("3251.001") + tr(sampleSize == 8 ? "3251.004" : "3251.005");
        if (channels)
            s += tr("3251.001") + tr(channels == 1 ? "3251.002" : "3251.003");
        s += tr("3251.007");
    }
    return s;
}

std::string EncoderSettings::extension() const
{
    switch (encoder) {
    case AAC:
    case AppleLossless: return "m4a";
    case AIFF: return "aif";
    case MP3: return "mp3";
    default: return "wav";
    }
}

std::string EncoderSettings::kind() const
{
    switch (encoder) {
    case AAC: return tr("5000.009");
    case AppleLossless: return tr("5000.017");
    case AIFF: return tr("3200.001");
    case MP3: return tr("3101.001");
    default: return tr("3200.002");
    }
}

std::vector<std::string> EncoderSettings::ffmpegArguments() const
{
    std::vector<std::string> a;
    if (sampleRate)
        a.insert(a.end(), {"-ar", std::to_string(sampleRate)});
    if (channels)
        a.insert(a.end(), {"-ac", std::to_string(channels)});
    const std::string kbps = std::to_string(channels == 1 ? bitrate / 2 : bitrate) + "k";
    switch (encoder) {
    case AAC:
        // FFmpeg's AAC encoder at the bit rate (its VBR mode is experimental: VBR is the same bit rate);
        // Optimize for voice as a high-pass filter (a substitute for Apple's voice filtering)
        a.insert(a.end(), {"-c:a", "aac", "-b:a", kbps});
        if (voice)
            a.insert(a.end(), {"-af", "highpass=f=80"});
        a.insert(a.end(), {"-f", "ipod"});
        break;
    case AppleLossless:
        a.insert(a.end(), {"-c:a", "alac", "-f", "ipod"});
        break;
    case AIFF:
        a.insert(a.end(), {"-c:a", sampleSize == 8 ? "pcm_s8" : "pcm_s16be", "-f", "aiff"});
        break;
    case MP3:
        a.insert(a.end(), {"-c:a", "libmp3lame"});
        if (vbr)   // the guaranteed minimum is the bit rate; the quality LAME's (Highest 0 … Lowest 9)
            a.insert(a.end(), {"-q:a", std::to_string(int(std::lround(9 - std::clamp(quality, 0, 6) * 1.5)))});
        else
            a.insert(a.end(), {"-b:a", kbps});
        a.insert(a.end(), {"-joint_stereo", joint ? "1" : "0"});
        if (filter)
            a.insert(a.end(), {"-af", "highpass=f=10"});
        a.insert(a.end(), {"-id3v2_version", "3", "-f", "mp3"});
        break;
    default:
        a.insert(a.end(), {"-c:a", sampleSize == 8 ? "pcm_u8" : "pcm_s16le", "-f", "wav"});
    }
    return a;
}

// ---- the import of a track

CDImport::~CDImport()
{
    cancel();
    join();
}

void CDImport::cancel() { m_cancel = true; }

void CDImport::join()
{
    if (m_thread.joinable())
        m_thread.join();
}

double CDImport::elapsed() const { return now() - m_started; }

bool CDImport::start(const AudioCD &cd, size_t track, const std::string &to, const EncoderSettings &settings)
{
    if (track >= cd.tracks.size() || m_thread.joinable())
        return false;
    std::shared_ptr<AudioCD::Reader> reader(cd.reader(track, settings.errorCorrection).release());
    if (!reader)
        return false;
    path = to;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(to).parent_path(), ec);
    // the song's fields into its tags (the library reads them back when it adds the file)
    const Track &t = cd.tracks[track].info;
    // the sectors as they come: 16-bit little-endian stereo at 44.1 kHz on ffmpeg's standard input
    std::vector<std::string> args = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", "pipe:0"};
    auto meta = [&](const char *k, const std::string &v) {
        if (!v.empty())
            args.insert(args.end(), {"-metadata", std::string(k) + "=" + v});
    };
    meta("title", t.name);
    meta("artist", t.artist);
    meta("album_artist", t.albumArtist);
    meta("album", t.album);
    meta("genre", t.genre);
    meta("composer", t.composer);
    meta("date", t.year ? std::to_string(t.year) : std::string());
    meta("track", std::to_string(t.trackNumber) + "/" + std::to_string(t.trackCount));
    if (t.discCount)
        meta("disc", std::to_string(t.discNumber) + "/" + std::to_string(t.discCount));
    if (t.compilation)
        meta("compilation", "1");
    for (const std::string &s : settings.ffmpegArguments())
        args.push_back(s);
    args.push_back(to);

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    std::vector<char *> argv;
    for (std::string &s : args)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int err = posix_spawnp(&pid, "ffmpeg", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[0]);
    if (err != 0) {
        ::close(fds[1]);
        return false;
    }
    m_total = cd.tracks[track].frames;
    m_done = 0;
    m_finished = false;
    m_ok = false;
    m_cancel = false;
    m_started = now();
    const int out = fds[1];
    m_thread = std::thread([this, reader, out, pid] {
        signal(SIGPIPE, SIG_IGN);
        unsigned char sector[2352];
        bool ok = true;
        for (long i = 0; i < m_total && ok; ++i) {
            if (m_cancel || !reader->next(sector)) {
                ok = false;
                break;
            }
            for (size_t w = 0; w < sizeof sector && ok;) {
                const ssize_t n = ::write(out, sector + w, sizeof sector - w);
                if (n <= 0)
                    ok = false;
                else
                    w += size_t(n);
            }
            m_done = i + 1;
        }
        ::close(out);
        if (!ok)
            kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        m_ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!m_ok) {
            std::error_code ec;
            std::filesystem::remove(path, ec);   // a half-written file not left in the music folder
        }
        m_finished = true;
    });
    return true;
}

} // namespace iTunes
