// SPDX-License-Identifier: LGPL-2.1-or-later
#include "player.h"

#include "appkit.h"

#include <mpv/client.h>
#include <mpv/render.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace iTunes {

Player::Player()
{
    // libmpv wants the C locale's numbers
    std::setlocale(LC_NUMERIC, "C");
    m_mpv = mpv_create();
    if (!m_mpv)
        return;
    // sound alone unless a movie plays (its picture through the render API: no window of mpv's), no
    // terminal, no configuration of the user's mpv
    mpv_set_option_string(m_mpv, "vid", "no");
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "audio-client-name", "iTunes");
    if (mpv_initialize(m_mpv) < 0) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }
    m_fd = mpv_get_wakeup_pipe(m_mpv);
    AK::App::shared().watch(m_fd, [this] { drain(); });
    // the software renderer (section 29); its update callback (another thread) writes to a pipe the
    // run loop watches
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (::pipe(m_frames) == 0 && mpv_render_context_create(&m_render, m_mpv, params) >= 0) {
        mpv_render_context_set_update_callback(
            m_render, [](void *fd) { const char c = 1; (void)!::write(*static_cast<int *>(fd), &c, 1); }, &m_frames[1]);
        AK::App::shared().watch(m_frames[0], [this] {
            char buf[64];
            while (::read(m_frames[0], buf, sizeof buf) == sizeof buf) {
            }
            if (m_render && (mpv_render_context_update(m_render) & MPV_RENDER_UPDATE_FRAME) && onFrame)
                onFrame();
        });
    } else
        m_render = nullptr;
}

bool Player::renderFrame(int width, int height, int stride, void *pixels)
{
    if (!m_render || width <= 0 || height <= 0)
        return false;
    int size[2] = {width, height};
    size_t pitch = size_t(stride);
    mpv_render_param params[] = {{MPV_RENDER_PARAM_SW_SIZE, size},
                                 {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>("bgr0")},
                                 {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
                                 {MPV_RENDER_PARAM_SW_POINTER, pixels},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    return mpv_render_context_render(m_render, params) >= 0;
}

void Player::videoSize(int &width, int &height) const
{
    width = int(property("dwidth"));
    height = int(property("dheight"));
}

Player::~Player()
{
    if (!m_mpv)
        return;
    AK::App::shared().unwatch(m_fd);
    if (m_render) {
        AK::App::shared().unwatch(m_frames[0]);
        mpv_render_context_free(m_render);
    }
    mpv_terminate_destroy(m_mpv);
}

void Player::drain()
{
    char buf[64];
    while (::read(m_fd, buf, sizeof buf) > 0) {
    }
    while (true) {
        mpv_event *e = mpv_wait_event(m_mpv, 0);
        if (!e || e->event_id == MPV_EVENT_NONE)
            break;
        if (e->event_id != MPV_EVENT_END_FILE)
            continue;
        const auto *end = static_cast<mpv_event_end_file *>(e->data);
        // a file replaced by the next (play()) or stopped by us: nothing to report
        if (end->reason == MPV_END_FILE_REASON_STOP || end->reason == MPV_END_FILE_REASON_QUIT)
            continue;
        m_state = Stopped;
        if (end->reason == MPV_END_FILE_REASON_ERROR) {
            const std::string text = mpv_error_string(end->error);
            if (onError)
                AK::App::shared().later([this, text] { onError(text); });
        } else if (onEnd) {
            AK::App::shared().later(onEnd);
        }
    }
}

void Player::play(const std::string &path, bool video)
{
    if (!m_mpv)
        return;
    m_video = video && m_render;
    mpv_set_property_string(m_mpv, "vid", m_video ? "auto" : "no");
    setVolume(m_volume);
    mpv_set_property_string(m_mpv, "pause", "no");
    const char *cmd[] = {"loadfile", path.c_str(), "replace", nullptr};
    mpv_command(m_mpv, cmd);
    m_state = Playing;
}

void Player::pause()
{
    if (!m_mpv || m_state != Playing)
        return;
    mpv_set_property_string(m_mpv, "pause", "yes");
    m_state = Paused;
}

void Player::resume()
{
    if (!m_mpv || m_state != Paused)
        return;
    mpv_set_property_string(m_mpv, "pause", "no");
    m_state = Playing;
}

void Player::stop()
{
    if (!m_mpv)
        return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command(m_mpv, cmd);
    m_state = Stopped;
}

double Player::property(const char *name) const
{
    double v = 0;
    if (!m_mpv || m_state == Stopped || mpv_get_property(m_mpv, name, MPV_FORMAT_DOUBLE, &v) < 0)
        return 0;
    return v;
}

double Player::position() const { return property("time-pos"); }
double Player::duration() const { return property("duration"); }

void Player::seek(double seconds)
{
    if (!m_mpv || m_state == Stopped)
        return;
    double s = std::max(0.0, seconds);
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &s);
}

void Player::setVolume(double v)
{
    m_volume = std::clamp(v, 0.0, 1.0);
    if (!m_mpv)
        return;
    // mpv's volume is cubic itself (0 to 100)
    double percent = m_volume * 100;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &percent);
}

} // namespace iTunes

namespace iTunes {

void Player::setEqualizer(bool on, double preamp, const double bands[10])
{
    if (!m_mpv)
        return;
    // FFmpeg's peaking filters an octave wide at iTunes' ten frequencies, the preamp a gain before them
    static const int freq[10] = {32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    std::string af;
    if (on) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "lavfi-volume=volume=%.2fdB", preamp);
        af = buf;
        for (int i = 0; i < 10; ++i) {
            if (bands[i] == 0)
                continue;
            std::snprintf(buf, sizeof buf, ",lavfi-equalizer=f=%d:width_type=o:width=1:gain=%.2f", freq[i], bands[i]);
            af += buf;
        }
    }
    m_eq = af;
    applyFilters();
}

void Player::applyFilters()
{
    if (!m_mpv)
        return;
    std::string af = m_eq;
    if (m_analysis)
        af += std::string(af.empty() ? "" : ",") + "@vis:lavfi=[astats=metadata=1:reset=1,aspectralstats=measure=centroid+rolloff+flux]";
    mpv_set_property_string(m_mpv, "af", af.c_str());
}

void Player::setAnalysis(bool on)
{
    if (on == m_analysis)
        return;
    m_analysis = on;
    applyFilters();
}

Player::Levels Player::levels() const
{
    Levels l;
    if (!m_mpv || !m_analysis || m_state != Playing)
        return l;
    auto get = [&](const char *key, double &out) {
        char *s = mpv_get_property_string(m_mpv, (std::string("af-metadata/vis/") + key).c_str());
        if (s) {
            out = std::atof(s);
            mpv_free(s);
        }
    };
    get("lavfi.astats.Overall.RMS_level", l.rms);
    get("lavfi.aspectralstats.1.centroid", l.centroid);
    get("lavfi.aspectralstats.1.rolloff", l.rolloff);
    get("lavfi.aspectralstats.1.flux", l.flux);
    return l;
}

} // namespace iTunes

namespace iTunes {

void Player::setSoundCheck(bool on)
{
    if (m_mpv)
        mpv_set_property_string(m_mpv, "replaygain", on ? "track" : "no");
}

} // namespace iTunes
