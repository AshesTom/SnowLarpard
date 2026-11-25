// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes DJ's views (docs/itunes-reverse.md section 30): its placard under the list (Placards.nib's
// "iTunes DJ"), its first page (Placards.nib's "iTunes DJ Intro", `PShf`) and its settings
// (PartyShuffleSettings.nib).
#pragma once

#include "appkit.h"
#include "controls.h"
#include "dj.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iTunes {

// Placards.nib's "iTunes DJ": HIView 3200 on PBUT 1034, 594 × 46: "Source:" (3201, TXTS 544, right
// justified, 9, 15, 52 × 14), the source's popup (3202, `PSSP`, PBUT 786 with TXTS 510, 68, 13,
// 170 × 20), Settings… (3222, `PSSS`) and Refresh (3209, `PSRB`), rounded bevel buttons 80 × 22 at
// 408 and 496, TXTS 301, bound to the right
class DJPlacard : public AK::View {
public:
    static constexpr double Height = 46;
    DJPlacard();
    std::function<void(const std::string &)> onCommand;
    // the popup's items (Music, then the playlists) and the one chosen; onSource(index) when one is
    std::function<void(int)> onSource;
    void setSources(const std::vector<std::string> &names, int chosen);
    void draw(cairo_t *cr) override;
    void setFrame(const AK::Rect &r) override;

private:
    class SourcePopup;
    SourcePopup *m_popup;
    AK::Button *m_settings, *m_refresh;
};

// "iTunes DJ Intro": a box view (com.apple.itunes.uiboxview, 795 × 517) whose pane (0, 65, 795 × 350)
// holds "Welcome to iTunes DJ" (TXTS 1210, 20, 36), its line (1211, 20, 94, 420 × 60), the pictures
// 9203 (508, 10), 9201 (20, 234) and 9202 (407, 234), "Always in Control" and "Make a Request" (1212, at
// 95 and 479, 234) with their texts (1213, 256), the link "Learn more about iTunes DJ and Remote" (1214,
// `PInf`, 20, 318, and its arrow PBUT 671 at 280, 322) and Continue (`PDon`, 90 × 22 at 685, 318)
class DJIntro : public AK::View {
public:
    DJIntro();
    std::function<void(const std::string &)> onCommand;
    void draw(cairo_t *cr) override;
    void setFrame(const AK::Rect &r) override;
    void mouseDown(AK::Event &e) override;
    void mouseUp(AK::Event &e) override;

private:
    AK::Point origin() const;   // the pane's top left
    AK::Button *m_continue;
    bool m_linkDown = false;
};

// PartyShuffleSettings.nib's window ("iTunes DJ Settings", 543 × 356, a movable modal): Display (3,
// 107, 26), popup 4 "recently played songs" (5), popup 6 "upcoming songs" (7), box 8 "Play higher rated
// songs more often", a separator (9), box 10 "Allow guests to request songs…", "Welcome Message:" (11)
// over its field (12, 435 × 47), box 13 "Enable voting" and its small text (14), box 15 "Require
// password:" and its field (16), the help button (17), Cancel (2) and OK (1). FUN_004cb81c fills it,
// FUN_002b0286 keeps it on OK
class DJSettingsWindow {
public:
    void show(const DJSettings &now, std::function<void(const DJSettings &)> done);

private:
    std::unique_ptr<AK::Window> m_window;
};

} // namespace iTunes
