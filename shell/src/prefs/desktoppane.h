// SPDX-License-Identifier: LGPL-2.1-or-later
// Desktop & Screen Saver: its two tabs, on AquaKit. The Desktop tab as measured on 10.6 (its nib is the
// secondary displays' window), the Screen Saver tab as ScreenSaverPref.nib lays it; docs/reverse-status.md
// section 7.
#pragma once

#include "controls.h"
#include "desktop.h"

class Slideshow;

class DesktopPane : public AK::View {
public:
    DesktopPane();
    ~DesktopPane() override;
    void showTab(int index);
    void draw(cairo_t *cr) override;
    static constexpr double Width = 668, Height = 520;

private:
    void buildDesktop(AK::View *tab);
    void buildScreenSaver(AK::View *tab);
    void fillSources();
    void showSource(int row);
    void loadThumbnails();
    void apply();
    void updatePreview();
    void chooseColor();
    void addFolder();

    Desktop::Settings m_settings;
    AK::SegmentedControl *m_tabs = nullptr;
    AK::View *m_pages[2] = {};
    // Desktop
    AK::ImageWell *m_preview = nullptr;
    AK::Label *m_name = nullptr;
    AK::PopUpButton *m_placement = nullptr, *m_interval = nullptr;
    AK::ColorWell *m_color = nullptr;
    AK::Button *m_remove = nullptr;
    AK::OutlineView *m_sources = nullptr;
    AK::ScrollView *m_sourcesScroll = nullptr, *m_picturesScroll = nullptr;
    AK::ImageGrid *m_pictures = nullptr;
    AK::CheckBox *m_change = nullptr, *m_random = nullptr, *m_translucent = nullptr;
    std::vector<std::string> m_pending;   // thumbnails still to make
    int m_thumbnails = 0;
    std::string m_folder;
    // Screen Saver
    AK::OutlineView *m_savers = nullptr;
    Slideshow *m_saverPreview = nullptr;
};
