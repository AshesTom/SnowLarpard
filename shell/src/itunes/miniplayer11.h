// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 11.4's MiniPlayer (Window ▸ Switch to MiniPlayer ⌥⌘M, `smin`; MiniPlayer ⌥⌘3, `minp`): a
// window of its own (MiniPlayerWindowController, MiniPlayerController, MiniPlayerFrameView;
// docs/itunes-reverse.md section 17).
#pragma once

#include "appkit.h"
#include "frame.h"

#include <functional>
#include <memory>
#include <string>

namespace iTunes {

class MiniPlayer11 {
public:
    struct Hooks {
        std::function<void()> playPause, next, previous, upNext, close, expand;
        std::function<void(double)> setVolume;
    };
    explicit MiniPlayer11(Hooks hooks);
    ~MiniPlayer11();
    void show();
    void hide();
    bool isShown() const;
    // what plays: its name, its artist and album, its artwork; playing or paused; the volume
    void setSong(const std::string &name, const std::string &artist, const AK::Image &artwork);
    void setPlaying(bool playing);
    void setVolume(double v);
    void setFloating(bool on);

private:
    class Body;
    Hooks m_hooks;
    std::unique_ptr<AK::Window> m_window;
    Body *m_body = nullptr;
};

} // namespace iTunes
