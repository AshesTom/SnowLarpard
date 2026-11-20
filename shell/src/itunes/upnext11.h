// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 11.4's Up Next (View ▸ Show Up Next ⌥⌘U, `PQsh`; PlayQueuePopover, PlayQueueHeaderView,
// PlayQueueCell; docs/itunes-reverse.md section 17): the songs to come, in a window of its own here
// (11.4's is a popover under the LCD).
#pragma once

#include "appkit.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iTunes {

class UpNext11 {
public:
    struct Song {
        std::string name, artist;
        AK::Image artwork;
    };
    std::function<void(size_t)> onPlay, onRemove;
    std::function<void()> onClear;

    UpNext11();
    ~UpNext11();
    void toggle();
    bool isShown() const;
    void setSongs(std::vector<Song> songs);

private:
    class Body;
    std::unique_ptr<AK::Window> m_window;
    Body *m_body = nullptr;
    std::vector<Song> m_songs;
};

} // namespace iTunes
