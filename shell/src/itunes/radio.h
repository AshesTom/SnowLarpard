// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's radio tuner (docs/itunes-reverse.md section 29): the Radio source's genres and their
// streams, from the tuning service iTunes asked (FUN_000bbbbc: "GET http://pri.kts-af.net/xml/index.xml?"
// over HTTP/1.0, "sid=…&" when it had a session; a genre's streams with FUN_000bc368's "tuning_id=%1&
// show_field[]=description&show_field[]=long_description&show_field[]=num_users&show_field[]=max_users").
// Its answer (FUN_000bb846): kb:menu_record (menu_id, name, shoutcast_count) for the genres,
// kb:station_record (station, station_url_record/url, bandwidth_kbps, status_code, description,
// long_description, num_users, max_users) for the streams. The service answers no more (Apple's 404):
// iTunes' own error then, STR# 154.010.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AK {
class Task;
}

namespace iTunes {

struct RadioStream {
    std::string name, url, description;
    int kbps = 0;
};
struct RadioGenre {
    std::string id, name;
    int count = 0;
    bool loaded = false;
    std::vector<RadioStream> streams;
};

class RadioTuner {
public:
    RadioTuner();
    ~RadioTuner();
    static constexpr const char *Service = "http://pri.kts-af.net/xml/index.xml?";
    const std::vector<RadioGenre> &genres() const { return m_genres; }
    bool loaded() const { return m_loaded; }
    void refresh();                  // the genres (154.002 "Refreshing Category List")
    void open(size_t genre);         // a genre's streams (154.003 "Refreshing “%1”")
    std::function<void()> onChanged;
    std::function<void(const std::string &)> onError;

private:
    void get(const std::string &query, std::function<void(bool, const std::string &)> done);
    std::vector<RadioGenre> m_genres;
    bool m_loaded = false;
    std::vector<std::unique_ptr<AK::Task>> m_tasks;
};

} // namespace iTunes
