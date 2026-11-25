// SPDX-License-Identifier: LGPL-2.1-or-later
#include "radio.h"

#include "appkit.h"
#include "resources.h"
#include "runloop.h"

#include <cstdlib>

using namespace AK;

namespace iTunes {

namespace {

// the text of each <tag>…</tag> inside each <record>…</record> (the answer's flat records)
std::vector<std::string> records(const std::string &xml, const std::string &record)
{
    std::vector<std::string> out;
    const std::string open = "<" + record, close = "</" + record + ">";
    for (size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at + 1)) {
        const size_t end = xml.find(close, at);
        if (end == std::string::npos)
            break;
        out.push_back(xml.substr(at, end - at));
    }
    return out;
}

std::string field(const std::string &rec, const std::string &tag)
{
    const size_t a = rec.find("<" + tag + ">");
    if (a == std::string::npos)
        return {};
    const size_t b = rec.find("</" + tag + ">", a);
    std::string v = rec.substr(a + tag.size() + 2, b == std::string::npos ? std::string::npos : b - a - tag.size() - 2);
    for (const auto &[e, c] : std::vector<std::pair<std::string, std::string>>{{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}})
        for (size_t p; (p = v.find(e)) != std::string::npos;)
            v.replace(p, e.size(), c);
    return v;
}

} // namespace

RadioTuner::RadioTuner() = default;
RadioTuner::~RadioTuner() = default;

void RadioTuner::get(const std::string &query, std::function<void(bool, const std::string &)> done)
{
    auto task = std::make_unique<Task>();
    Task *raw = task.get();
    task->onExit = [this, raw, done](int status, const std::string &out) {
        // the answer an XML document of kb: records (FUN_0072d529, expat), else the service's error
        const bool ok = status == 0 && out.find("<kb:") != std::string::npos && out.find("kb:error") == std::string::npos;
        done(ok, out);
        App::shared().later([this, raw] {
            for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it)
                if (it->get() == raw) {
                    m_tasks.erase(it);
                    break;
                }
        });
    };
    // -f: an HTTP error (Apple's 404 now) fails as iTunes' read does (a status past 299)
    if (task->launch({"curl", "-sSf", "--max-time", "30", "-H", "Accept-Language: en", std::string(Service) + query}))
        m_tasks.push_back(std::move(task));
    else
        done(false, {});
}

void RadioTuner::refresh()
{
    get("", [this](bool ok, const std::string &xml) {
        if (!ok) {
            if (onError)
                onError(tr("154.010"));
            return;
        }
        std::vector<RadioGenre> genres;
        for (const std::string &r : records(xml, "kb:menu_record")) {
            RadioGenre g;
            g.id = field(r, "kb:menu_id");
            g.name = field(r, "kb:name");
            g.count = std::atoi(field(r, "kb:shoutcast_count").c_str()) + std::atoi(field(r, "kb:pass_count").c_str());
            genres.push_back(g);
        }
        m_genres = genres;
        m_loaded = true;
        if (onChanged)
            onChanged();
    });
}

void RadioTuner::open(size_t i)
{
    if (i >= m_genres.size())
        return;
    const std::string id = m_genres[i].id;
    get("tuning_id=" + id + "&show_field%5B%5D=description&show_field%5B%5D=long_description&show_field%5B%5D=num_users&show_field%5B%5D=max_users",
        [this, id](bool ok, const std::string &xml) {
            for (RadioGenre &g : m_genres) {
                if (g.id != id)
                    continue;
                if (!ok) {
                    if (onError)
                        onError(tr("154.010"));
                    return;
                }
                g.streams.clear();
                for (const std::string &r : records(xml, "kb:station_record")) {
                    RadioStream s;
                    s.name = field(r, "kb:station");
                    s.url = field(r, "kb:url");
                    s.kbps = std::atoi(field(r, "kb:bandwidth_kbps").c_str());
                    s.description = field(r, "kb:description");
                    if (!s.url.empty())
                        g.streams.push_back(s);
                }
                g.loaded = true;
            }
            if (onChanged)
                onChanged();
        });
}

} // namespace iTunes
