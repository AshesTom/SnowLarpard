// SPDX-License-Identifier: LGPL-2.1-or-later
// The Podcasts source's own views (docs/itunes-reverse.md section 29): the placard under the list
// (Placards.nib's window 216) and the list's podcast cells — the disclosure triangle, the GET buttons,
// the blue dot of the episodes not yet played.
#pragma once

#include "appkit.h"
#include "controls.h"

#include <functional>
#include <string>

namespace iTunes {

// Placards.nib, window 216: a pane 46 high (HIView 3220 on PBUT 1034, the placard gradient) under the
// list; Unsubscribe (3221, `PCSB`; a rounded bevel button 88 × 22 at 20, 12, TXTS 301), Settings… (3222,
// `PCSe`, 80 × 22 at 118), Report a Concern (3223, `PCRC`, TXTS 544, and the arrow PBUT 650, 3224: the
// Store's podcasts only), Podcast Directory (3225 and its arrow 3226, `STPD`: the library's), Refresh
// (3227, `PCRB`, 76 × 22, 18 from the right); the right-hand ones bound to the right (bindingRightKind 2)
class PodcastPlacard : public AK::View {
public:
    static constexpr double Height = 46;
    PodcastPlacard();
    std::function<void(const std::string &)> onCommand;
    void setSubscribed(bool subscribed);   // FUN_0013b678: 3221's title, STR# 163.081 / .080
    void setPodcastChosen(bool chosen);    // a podcast chosen
    void draw(cairo_t *cr) override;
    void setFrame(const AK::Rect &r) override;

private:
    class Link;
    AK::Button *m_unsubscribe, *m_settings, *m_refresh;
    Link *m_directory;
};

// A podcast's Name cell (the list's "Podcast" column): its triangle (PBUT 314 TrackDisclosure: rows 0
// closed, 2 open, 3 to 5 the same white on a chosen row) at 4 in, the name 18 in; at its right end the
// GET buttons (Localized.rsrc's PBUT 672 "Get All Podcasts", 50 × 13; 674 "Get Podcast", 31 × 13; their
// second cells pressed) — places by eye
enum PodcastButton { NoButton = 0, GetAll = 672, Get = 674 };
AK::Rect podcastTriangle(const AK::Rect &cell);
AK::Rect podcastButton(const AK::Rect &cell, int button);
void drawPodcastName(cairo_t *cr, const AK::Rect &cell, const std::string &name, bool header, bool open, bool selected,
                     int button, bool pressed, const AK::TextStyle &style);
// The status column's marks (TrackStatusIcons, PBUT 306: 12 × 12 cells, the second row on a chosen row):
// column 12 the blue dot (not played), 7 the arrows going round (downloading)
void drawPodcastStatus(cairo_t *cr, const AK::Rect &cell, int column, bool selected);

} // namespace iTunes
