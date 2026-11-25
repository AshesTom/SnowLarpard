// SPDX-License-Identifier: LGPL-2.1-or-later
#include "burn.h"

#include "appkit.h"
#include "foundation.h"
#include "panels.h"
#include "resources.h"
#include "runloop.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unistd.h>

using namespace AK;

namespace iTunes {

namespace {

std::string with(std::string s, const std::string &a)
{
    const size_t at = s.find("^1");
    if (at != std::string::npos)
        s.replace(at, 2, a);
    return s;
}

bool isMP3(const Track &t)
{
    const std::string &p = t.path;
    return p.size() > 4 && (p.compare(p.size() - 4, 4, ".mp3") == 0 || p.compare(p.size() - 4, 4, ".MP3") == 0);
}

bool burnable(const Track &t, int format)
{
    if (t.path.find("://") != std::string::npos || t.disabled || access(t.path.c_str(), R_OK) != 0)
        return false;
    if (format == 0)
        return !t.hasVideo;   // an audio CD: the songs' sound
    if (format == 1)
        return isMP3(t);      // 8014.016: only files already in MP3 format
    return true;
}

long long fileSize(const std::string &p)
{
    std::error_code ec;
    const auto n = std::filesystem::file_size(p, ec);
    return ec ? 0 : (long long)n;
}

const char *const CDPrograms[] = {"cdrskin", "wodim", "cdrecord"};

} // namespace

BurnSettings BurnSettings::load()
{
    Settings s("itunes");
    BurnSettings b;
    b.speed = int(s.integer("BurnSpeed", 10));
    b.format = int(s.integer("BurnFormat", 0));
    b.gap = int(s.integer("BurnGap", 2));
    b.soundCheck = s.boolean("BurnSoundCheck", false);
    b.cdText = s.boolean("BurnCDText", false);
    b.burner = s.string("BurnDevice", "");
    return b;
}

void BurnSettings::save() const
{
    Settings s("itunes");
    s.set("BurnSpeed", speed);
    s.set("BurnFormat", format);
    s.set("BurnGap", gap);
    s.set("BurnSoundCheck", soundCheck);
    s.set("BurnCDText", cdText);
    s.set("BurnDevice", burner);
}

std::string Burning::program()
{
    for (const char *p : CDPrograms)
        if (Workspace::inPath(p))
            return p;
    return Workspace::inPath("growisofs") ? "growisofs" : std::string();
}

void Burning::findBurners(std::function<void(std::vector<Burner>)> done)
{
    const std::string prog = program();
    if (prog.empty() || prog == "growisofs") {
        // growisofs has no scan: the drives the kernel has
        std::vector<Burner> out;
        if (!prog.empty())
            for (int i = 0; i < 4; ++i)
                if (access(("/dev/sr" + std::to_string(i)).c_str(), F_OK) == 0)
                    out.push_back({"/dev/sr" + std::to_string(i), "/dev/sr" + std::to_string(i)});
        App::shared().later([done, out] { done(out); });
        return;
    }
    auto task = std::make_shared<std::unique_ptr<Task>>(std::make_unique<Task>());
    Task *t = task->get();
    t->errors = Task::Errors::Merge;
    t->onExit = [task, done](int, const std::string &out) {
        // cdrskin / wodim: "0  dev='/dev/sr0'  rwrw-- :  'VENDOR' 'MODEL'"; cdrecord -scanbus:
        // "0,0,0   0) 'VENDOR  ' 'MODEL  ' 'REV ' Removable CD-ROM"
        std::vector<Burner> burners;
        static const std::regex devices(R"(dev='([^']+)'.*?:\s*'([^']*)'\s*'([^']*)')");
        static const std::regex scanbus(R"(^\s*(\d+,\d+,\d+)\s+\d+\)\s+'([^']*)'\s+'([^']*)')");
        std::istringstream in(out);
        for (std::string line; std::getline(in, line);) {
            std::smatch m;
            if (std::regex_search(line, m, devices) || std::regex_search(line, m, scanbus)) {
                std::string name = m[2].str() + " " + m[3].str();
                name.erase(std::unique(name.begin(), name.end(), [](char a, char b) { return a == ' ' && b == ' '; }), name.end());
                while (!name.empty() && name.back() == ' ')
                    name.pop_back();
                burners.push_back({m[1].str(), name});
            }
        }
        done(burners);
        App::shared().later([task] { task->reset(); });
    };
    if (!t->launch({prog, prog == "cdrecord" ? "-scanbus" : "--devices"}))
        App::shared().later([done, task] { done({}); task->reset(); });
}

std::vector<std::vector<size_t>> Burning::split(const std::vector<int> &ms, int gapSeconds, long capacity)
{
    std::vector<std::vector<size_t>> discs(1);
    long used = 0;
    for (size_t i = 0; i < ms.size(); ++i) {
        const long frames = (long(ms[i]) * 75 + 999) / 1000;
        const long gap = discs.back().empty() ? 150 : gapSeconds * 75L;
        if (!discs.back().empty() && (used + gap + frames > capacity || discs.back().size() >= 99)) {
            discs.emplace_back();
            used = 0;
        }
        used += (discs.back().empty() ? 150 : gap) + frames;
        discs.back().push_back(i);
    }
    if (discs.back().empty())
        discs.pop_back();
    return discs;
}

// ---- the burn

BurnJob::BurnJob(const std::vector<Track> &songs, const BurnSettings &settings, const std::string &name)
    : m_songs(songs), m_settings(settings), m_name(name)
{
}

BurnJob::~BurnJob()
{
    if (m_poll)
        App::shared().removeTimer(m_poll);
    m_task.reset();
    if (!m_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }
}

void BurnJob::cancel()
{
    m_task.reset();   // the program terminated
    finish(false, {});
}

void BurnJob::start()
{
    m_running = true;
    m_program = Burning::program();
    // 169.041 Checking playlist…; the songs that can't go on this kind of disc (8014.*)
    if (onStatus)
        onStatus(tr("169.041"), {}, -1);
    const int f = std::clamp(m_settings.format, 0, 2);
    std::vector<Track> ok;
    for (const Track &t : m_songs)
        if (burnable(t, f))
            ok.push_back(t);
    const int base = f * 4 + 1;   // 8014.001 audio CD, .005 MP3 CD, .009 data disc
    auto key = [](int n) { char b[16]; std::snprintf(b, sizeof b, "8014.%03d", n); return std::string(b); };
    if (ok.empty()) {
        if (onAsk)
            onAsk(tr(key(base + 2)), tr(key(base + 3)), {"OK"}, [this](int) { finish(false, {}); });
        return;
    }
    if (f == 2 && m_program == "growisofs") {
        // growisofs writes data DVDs only
    } else if (m_program.empty() || (m_program == "growisofs" && f != 2)) {
        finish(false, tr("169.022"));
        return;
    }
    const bool some = ok.size() < m_songs.size();
    m_songs = ok;
    auto go = [this] { checkMedia(); };
    if (some && onAsk)
        onAsk(tr(key(base)), tr(key(base + 1)), {"Burn", "Cancel"}, [this, go](int b) {
            if (b == 0)
                go();
            else
                finish(false, {});
        });
    else
        go();
}

void BurnJob::checkMedia()
{
    // 169.034 Checking media…: the blank disc's capacity from its ATIP (the lead-out's start)
    if (onStatus)
        onStatus(tr("169.034"), {}, -1);
    if (m_program == "growisofs") {
        m_capacity = 2295104;   // a single-layer DVD±R's 2048-byte sectors
        prepareDisc();
        return;
    }
    m_task = std::make_unique<Task>();
    m_task->errors = Task::Errors::Merge;
    m_task->onExit = [this](int status, const std::string &out) {
        static const std::regex leadOut(R"(lead out:\s*(\d+))");
        std::smatch m;
        App::shared().later([this] { m_task.reset(); });
        if (status != 0 || !std::regex_search(out, m, leadOut)) {
            // 169.007 Please insert a blank disc…: asked again every few seconds (DiskArbitration's
            // notification on the Mac)
            if (onStatus)
                onStatus(tr("169.007"), {}, -1);
            m_poll = App::shared().addTimer(3, false, [this] {
                m_poll = 0;
                checkMedia();
            });
            return;
        }
        m_capacity = std::atol(m[1].str().c_str());
        App::shared().later([this] { prepareDisc(); });
    };
    if (!m_task->launch({m_program, "-atip", "dev=" + m_settings.burner}))
        finish(false, tr("169.045"));
}

void BurnJob::prepareDisc()
{
    const int f = std::clamp(m_settings.format, 0, 2);
    if (m_discs.empty()) {
        // the discs: by time for an audio CD (FUN_00085bcc), by bytes for MP3 and data discs
        if (f == 0) {
            std::vector<int> ms;
            for (const Track &t : m_songs)
                ms.push_back(t.totalTime);
            m_discs = Burning::split(ms, std::clamp(m_settings.gap, 0, 5), m_capacity);
        } else {
            const long long room = (long long)m_capacity * 2048 - 2 * 1024 * 1024;   // the file system's own
            m_discs.emplace_back();
            long long used = 0;
            for (size_t i = 0; i < m_songs.size(); ++i) {
                const long long n = fileSize(m_songs[i].path);
                if (!m_discs.back().empty() && used + n > room) {
                    m_discs.emplace_back();
                    used = 0;
                }
                used += n;
                m_discs.back().push_back(i);
            }
        }
        if (m_discs.size() > 1 && onAsk) {
            // 169.023 / .043 / .037 with 169.024 / .044 / .038 (169.039 the count), Audio CDs (169.029),
            // MP3 CD (169.030) or Data Discs (169.040) and Cancel; "itns2987" its help
            const char *q[3][3] = {{"169.023", "169.024", "169.029"}, {"169.043", "169.044", "169.030"}, {"169.037", "169.038", "169.040"}};
            std::string more = tr(q[f][1]);
            if (m_discs.size() > 2)
                more = with(tr("169.039"), std::to_string(m_discs.size()));
            onAsk(tr(q[f][0]), more, {tr(q[f][2]), "Cancel"}, [this](int b) {
                if (b == 0)
                    App::shared().later([this] { prepareDisc(); });
                else
                    finish(false, {});
            });
            m_disc = 0;
            m_decoded = 0;
            return;
        }
    }
    // 169.013 Preparing Disc…: the songs made into what the program writes
    if (onStatus)
        onStatus(tr("169.013"), tr("169.017"), -1);
    if (m_dir.empty()) {
        char tmpl[] = "/tmp/itunes-burn-XXXXXX";
        if (const char *d = mkdtemp(tmpl))
            m_dir = d;
    }
    m_decoded = 0;
    decodeNext();
}

void BurnJob::decodeNext()
{
    const std::vector<size_t> &disc = m_discs[m_disc];
    const int f = std::clamp(m_settings.format, 0, 2);
    if (f != 0 || m_decoded >= disc.size()) {
        if (f != 0 && m_program != "growisofs") {
            // an ISO 9660 image with Joliet and Rock Ridge names of the disc's files
            const char *makers[] = {"xorriso", "genisoimage", "mkisofs"};
            std::string maker;
            for (const char *m : makers)
                if (Workspace::inPath(m)) {
                    maker = m;
                    break;
                }
            if (maker.empty()) {
                finish(false, tr("169.022"));
                return;
            }
            std::vector<std::string> args{maker};
            if (maker == "xorriso")
                args.insert(args.end(), {"-as", "mkisofs"});
            args.insert(args.end(), {"-quiet", "-r", "-J", "-V", m_name.substr(0, 32), "-graft-points", "-o", m_dir + "/disc.iso"});
            for (size_t i : disc)
                args.push_back(std::filesystem::path(m_songs[i].path).filename().string() + "=" + m_songs[i].path);
            m_task = std::make_unique<Task>();
            m_task->onExit = [this](int status, const std::string &) {
                App::shared().later([this] { m_task.reset(); });
                if (status != 0)
                    finish(false, tr("169.045"));
                else
                    App::shared().later([this] { write(); });
            };
            if (!m_task->launch(args))
                finish(false, tr("169.045"));
            return;
        }
        write();
        return;
    }
    // an audio CD's tracks: 44.1 kHz 16-bit stereo WAV; Use Sound Check as the songs' ReplayGain
    const Track &t = m_songs[disc[m_decoded]];
    char name[32];
    std::snprintf(name, sizeof name, "/%02zu.wav", m_decoded + 1);
    std::vector<std::string> args{"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", t.path, "-vn", "-ar", "44100", "-ac", "2", "-c:a", "pcm_s16le"};
    if (m_settings.soundCheck)
        args.insert(args.end(), {"-af", "volume=replaygain=track"});
    args.push_back(m_dir + name);
    if (m_settings.cdText) {
        // CD-Text from the .inf files the program reads with -useinfo
        std::ofstream inf(m_dir + std::string(name).substr(0, 3) + ".inf");
        inf << "Albumtitle='" << m_name << "'\nPerformer='" << t.artist << "'\nTracktitle='" << t.name << "'\n";
    }
    if (onStatus)
        onStatus(tr("169.013"), with(tr("169.002"), t.name), double(m_decoded) / disc.size());
    m_task = std::make_unique<Task>();
    m_task->onExit = [this](int status, const std::string &) {
        App::shared().later([this] { m_task.reset(); });
        if (status != 0) {
            finish(false, tr("169.045"));
            return;
        }
        ++m_decoded;
        App::shared().later([this] { decodeNext(); });
    };
    if (!m_task->launch(args))
        finish(false, tr("169.045"));
}

void BurnJob::write()
{
    const std::vector<size_t> &disc = m_discs[m_disc];
    const int f = std::clamp(m_settings.format, 0, 2);
    std::vector<std::string> args{m_program};
    std::vector<long long> sizes;
    if (m_program == "growisofs") {
        args.insert(args.end(), {"-dvd-compat", "-Z", m_settings.burner + "=" + m_dir + "/disc.iso"});
    } else {
        args.insert(args.end(), {"-v", "dev=" + m_settings.burner});
        // Preferred Speed (MENU 1349): 1x … 24x; Maximum Possible the program's own
        static const int speeds[] = {1, 2, 4, 6, 8, 12, 16, 20, 24};
        if (m_settings.speed >= 0 && m_settings.speed < 9)
            args.push_back("speed=" + std::to_string(speeds[m_settings.speed]));
        if (m_disc + 1 < m_discs.size())
            args.push_back("-eject");   // the next blank disc asked for
        if (f == 0) {
            args.insert(args.end(), {"-dao", "-audio", "-pad"});
            // Gap Between Songs (MENU 1352): the tracks' pregap in frames (cdrskin keeps 2 s)
            if (m_program != "cdrskin")
                args.push_back("defpregap=" + std::to_string(std::clamp(m_settings.gap, 0, 5) * 75));
            if (m_settings.cdText)
                args.insert(args.end(), {"-text", "-useinfo"});
            for (size_t i = 0; i < disc.size(); ++i) {
                char name[32];
                std::snprintf(name, sizeof name, "/%02zu.wav", i + 1);
                args.push_back(m_dir + name);
                sizes.push_back(fileSize(m_dir + name));
            }
        } else {
            args.insert(args.end(), {"-data", m_dir + "/disc.iso"});
            sizes.push_back(fileSize(m_dir + "/disc.iso"));
        }
    }
    long long total = 0;
    for (long long s : sizes)
        total += s;
    auto lineOne = [this] {
        // 169.014 Burning Disc… ^1: the disc's number when the playlist takes several (by eye)
        return with(tr("169.014"), m_discs.size() > 1 ? std::to_string(m_disc + 1) + " / " + std::to_string(m_discs.size()) : std::string());
    };
    if (onStatus)
        onStatus(lineOne(), tr("169.001"), 0);
    // the program's progress ("Track 01:   12 of   45 MB written", on carriage returns) a line each
    std::vector<std::string> sh{"sh", "-c", "{ \"$@\"; echo \"BURN-STATUS $?\"; } 2>&1 | tr '\\r' '\\n'", "sh"};
    sh.insert(sh.end(), args.begin(), args.end());
    m_task = std::make_unique<Task>();
    m_task->onLine = [this, sizes, total, lineOne, disc](const std::string &line) {
        static const std::regex progress(R"(Track\s+(\d+):\s+(\d+)\s+of\s+(\d+)\s*MB)");
        std::smatch m;
        if (line.find("BURN-STATUS") == std::string::npos && !line.empty())
            m_last = line;
        if (std::regex_search(line, m, progress) && total > 0) {
            const size_t track = size_t(std::max(1, std::atoi(m[1].str().c_str())) - 1);
            long long before = 0;
            for (size_t i = 0; i < track && i < sizes.size(); ++i)
                before += sizes[i];
            const double done = double(before) + std::atof(m[2].str().c_str()) * 1048576.0;
            const std::string what = m_settings.format == 0 && track < disc.size() ? with(tr("169.002"), m_songs[disc[track]].name) : tr("169.001");
            if (onStatus)
                onStatus(lineOne(), what, std::min(1.0, done / double(total)));
        } else if (line.find("Fixating") != std::string::npos || line.find("losing") != std::string::npos) {
            if (onStatus)
                onStatus(lineOne(), tr("169.003"), 1);
        }
    };
    m_task->onExit = [this](int, const std::string &out) {
        App::shared().later([this] { m_task.reset(); });
        const size_t at = out.rfind("BURN-STATUS ");
        const int status = at == std::string::npos ? -1 : std::atoi(out.c_str() + at + 12);
        if (status != 0) {
            // 169.046 when the program says the speed is too fast, else 169.045 with its last words
            const bool speed = m_last.find("speed") != std::string::npos;
            finish(false, speed ? tr("169.046") + "\n\n" + tr("169.047") : tr("169.045") + (m_last.empty() ? "" : "\n\n" + m_last));
            return;
        }
        if (++m_disc < m_discs.size())
            App::shared().later([this] { checkMedia(); });
        else
            finish(true, {});
    };
    if (!m_task->launch(sh))
        finish(false, tr("169.045"));
}

void BurnJob::finish(bool ok, const std::string &error)
{
    if (!m_running)
        return;
    m_running = false;
    if (m_poll) {
        App::shared().removeTimer(m_poll);
        m_poll = 0;
    }
    if (onDone)
        onDone(ok, error);
}

} // namespace iTunes
