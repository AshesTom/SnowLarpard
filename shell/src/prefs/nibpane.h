// SPDX-License-Identifier: LGPL-2.1-or-later
// A preference pane from its 10.6 nib (AquaKit's Nib: the DVD's interface, not its code). Each control
// keeps what it was set to (~/.config/snowleopard/panes/<identifier>.conf), and the panes that have a
// Linux counterpart act on it (linuxpanes.cpp: the Dock's settings, the volume, the keyboard's
// repeat...). Three panes have nibs of the older NSArchiver format: they are laid out by hand there.
#pragma once

#include "catalog.h"
#include "nib.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

class NibPane : public AK::View {
    using Nib = AK::Nib;
    using Size = AK::Size;
public:
    explicit NibPane(const PaneInfo &info);
    ~NibPane() override;
    bool loaded() const { return m_loaded; }
    AK::Nib nib;
    const PaneInfo &info;
    AK::Size size;
    std::vector<std::unique_ptr<AK::Nib>> parts;   // the nibs of its parts (Date & Time's tabs)
    bool remembered(AK::View *v) const { return m_restored.count(v) != 0; }   // set as it was left
    // a timer that goes with the pane
    void every(double seconds, std::function<void()> f);

    // a sheet over the window: its OK, Cancel... end it
    void presentSheet(AK::View *content, AK::Size size);
    // a part's nib from the bundle (kept with the pane)
    AK::Nib *loadPart(const std::string &name);
    // System Preferences' navigation, for the buttons that open another pane
    static std::function<void(const std::string &id)> openPane;

private:
    void remember();
    void connectPanels();
    void openSheet(Nib &nib, int panel, Size size);
    bool m_loaded = false;
    std::vector<int> m_timers;
    std::set<AK::View *> m_restored;
};

// The pane's view for an identifier: Desktop & Screen Saver's own, a hand-made one, or its nib's
AK::View *makePane(const PaneInfo &info, AK::Size *size);

// linuxpanes.cpp
void wirePane(NibPane &pane);
AK::View *handPane(const PaneInfo &info, AK::Size *size);   // Displays, Language & Text, Speech; null for the others
// the menu extras the panes show or hide (SystemUIServer's menuExtras, dontAutoLoad)
bool menuExtraShown(const std::string &bundle);
void showMenuExtra(const std::string &bundle, bool shown);

// timemachinepane.cpp: Time Machine's (its sheets, backupd's settings and state)
void timeMachine(NibPane &pane);

// after a control's own action, `f` (actions chain)
void then(AK::View *control, std::function<void()> f);
// a command's output (a shell line), and a program started without waiting for it
std::string run(const std::string &command);
void spawn(const std::vector<std::string> &argv);
bool onX11();
