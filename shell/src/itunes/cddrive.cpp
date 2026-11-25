// SPDX-License-Identifier: LGPL-2.1-or-later
#include "cddrive.h"

#include "appkit.h"
#include "musicfolder.h"
#include "resources.h"
#include "runloop.h"

#include <algorithm>
#include <cstdio>

using namespace AK;

namespace iTunes {

namespace {

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

} // namespace

CDDrive::CDDrive() = default;

CDDrive::~CDDrive()
{
    if (m_pollTimer)
        App::shared().removeTimer(m_pollTimer);
    if (m_tickTimer)
        App::shared().removeTimer(m_tickTimer);
}

void CDDrive::start()
{
    if (m_pollTimer)
        return;
    m_pollTimer = App::shared().addTimer(2, true, [this] { poll(); });
    App::shared().later([this] { poll(); });
}

void CDDrive::poll()
{
    if (importing())
        return;   // the drive busy reading: the disc stays as it is
    const std::string where = AudioCD::probe();
    if (m_cd && where == m_cd->device)
        return;
    const bool had = m_cd != nullptr;
    m_cd.reset();
    if (!where.empty())
        m_cd = AudioCD::open(where);
    if ((had || m_cd) && onDisc)
        onDisc(m_cd != nullptr);
}

bool CDDrive::eject()
{
    if (!m_cd)
        return false;
    stopImport();
    const bool ok = m_cd->eject();
    // the test disc stays in its folder: taken away until the variable names another
    m_cd.reset();
    if (onDisc)
        onDisc(false);
    return ok;
}

void CDDrive::getNames()
{
    if (!m_cd || m_cd->lookingUp())
        return;
    if (onChanged)
        onChanged();
    AudioCD *cd = m_cd.get();
    cd->lookup([this, cd](bool found) {
        if (m_cd.get() != cd)
            return;
        if (onChanged)
            onChanged();
        if (onNames)
            onNames(found);
    });
}

void CDDrive::import(const std::vector<size_t> &tracks)
{
    if (!m_cd)
        return;
    m_settings = EncoderSettings::load();
    for (size_t i : tracks)
        if (i < m_cd->tracks.size() && std::find(m_queue.begin(), m_queue.end(), i) == m_queue.end() && int(i) != m_current) {
            m_queue.push_back(i);
            m_cd->tracks[i].status = DiscTrack::Waiting;
        }
    m_completed = true;
    if (!m_tickTimer)
        m_tickTimer = App::shared().addTimer(0.25, true, [this] { tick(); });
    if (!m_import)
        next();
    if (onChanged)
        onChanged();
}

void CDDrive::stopImport()
{
    if (!importing())
        return;
    m_completed = false;
    for (size_t i : m_queue)
        if (m_cd && i < m_cd->tracks.size())
            m_cd->tracks[i].status = DiscTrack::None;
    m_queue.clear();
    if (m_import) {
        m_import->cancel();
        m_import->join();
        if (m_cd && m_current >= 0 && size_t(m_current) < m_cd->tracks.size())
            m_cd->tracks[size_t(m_current)].status = DiscTrack::None;
        m_import.reset();
    }
    m_current = -1;
    if (onChanged)
        onChanged();
    if (onImportEnded)
        onImportEnded(false);
}

void CDDrive::next()
{
    m_import.reset();
    m_current = -1;
    while (m_cd && !m_queue.empty()) {
        const size_t i = m_queue.front();
        m_queue.pop_front();
        DiscTrack &d = m_cd->tracks[i];
        // the song's place in the iTunes Music folder (section 31) with the encoder's extension
        Track t = d.info;
        t.mediaKind = 1;
        const std::string to = MusicFolder::destination(t, m_settings.extension());
        auto job = std::make_unique<CDImport>();
        if (to.empty() || !job->start(*m_cd, i, to, m_settings)) {
            d.status = DiscTrack::Failed;
            m_completed = false;
            continue;
        }
        d.status = DiscTrack::Importing;
        m_current = int(i);
        m_import = std::move(job);
        return;
    }
    if (m_tickTimer) {
        App::shared().removeTimer(m_tickTimer);
        m_tickTimer = 0;
    }
    if (onChanged)
        onChanged();
    if (onImportEnded)
        onImportEnded(m_completed);
}

void CDDrive::tick()
{
    if (!m_import || !m_import->finished())
        return;
    m_import->join();
    if (m_cd && m_current >= 0 && size_t(m_current) < m_cd->tracks.size()) {
        DiscTrack &d = m_cd->tracks[size_t(m_current)];
        const int id = m_import->succeeded() && onImported ? onImported(m_import->path) : 0;
        d.status = id ? DiscTrack::Done : DiscTrack::Failed;
        d.libraryId = id;
        if (!id)
            m_completed = false;
    }
    next();
    if (onChanged)
        onChanged();
}

bool CDDrive::status(std::string &line1, std::string &line2, double &progress) const
{
    if (m_cd && m_cd->lookingUp()) {
        // 130.004 Getting Track Names…, 130.006 Accessing Gracenote Media Database (here MusicBrainz')
        line1 = tr("130.004");
        line2 = tr("130.007");
        progress = -1;
        return true;
    }
    if (!m_import || !m_cd || m_current < 0)
        return false;
    const DiscTrack &d = m_cd->tracks[size_t(m_current)];
    line1 = with(tr("9003.007"), d.info.name);
    progress = m_import->progress();
    // the time left at the speed so far, the speed as a multiple of playing (FUN_0009932c's LCD: "^1 (^2)")
    const double elapsed = m_import->elapsed();
    const double audio = progress * d.frames / 75.0;
    if (elapsed < 1 || audio <= 0) {
        line2 = tr("23988.001");   // Estimating time remaining…
        return true;
    }
    const double speed = audio / elapsed;
    const double left = (1 - progress) * d.frames / 75.0 / speed;
    char x[16];
    std::snprintf(x, sizeof x, "%.1fx", speed);
    line2 = with(tr("9003.010"), formatTime(int(left * 1000)), x);
    return true;
}

} // namespace iTunes
