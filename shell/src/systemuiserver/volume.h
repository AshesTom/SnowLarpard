// SPDX-License-Identifier: LGPL-2.1-or-later
// AppleVolumeExtra: SystemUIServer's own (Volume.menu has its pictures only), from 10.6.8: its title
// (AppleVolumeExtraTitle -drawRect: 0x10000bf4d) Volume1.pdf to Volume4.pdf for the level, dimmed
// without an output, white while the menu is down; its menu (-menu 0x100030c59, MenuItem.nib) the
// vertical slider (30 x 104, the slider at {4, 2}, 21 x 100), Option-click the devices
// (-buildDeviceMenu:: Output Device:, Input Device:, Sound Preferences…). The sound server's (PipeWire or
// PulseAudio, through pactl) default output.
#pragma once

#include "systemuiplugin.h"

class AppleVolumeExtra : public SystemUI::MenuExtra {
public:
    explicit AppleVolumeExtra(const std::string &bundle);
    ~AppleVolumeExtra() override;
    std::vector<AK::MenuItem> menu() override;
    void willUnload() override;
    int level() const { return m_level; }
    bool muted() const { return m_muted; }
    bool available() const { return m_available; }
    void set(int percent);

private:
    void refresh();
    int m_level = 50;
    bool m_muted = false, m_available = false;
    int m_timer = 0;
    std::unique_ptr<AK::Task> m_task;
};
