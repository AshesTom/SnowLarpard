// SPDX-License-Identifier: LGPL-2.1-or-later
// Spotlight's item: Search.bundle's MDStatusItem, which SystemUIServer loads first, at the bar's right
// edge (-loadMatadorSearch). Its magnifier is Search.bundle's MDSearchMenuIcon.pdf, drawn white on
// Spotlight's blue while open; its menu the "Spotlight" field over the results, the left column
// (MDMenu_LeftPattern) naming each group, "Show All" and "Spotlight Preferences…" (MDMenuStrings).
// The index is for later: it finds the applications (LaunchServices).
#pragma once

#include "systemuiplugin.h"

#include <memory>

class SpotlightWindow;

class SpotlightExtra : public SystemUI::MenuExtra {
public:
    explicit SpotlightExtra(const std::string &bundle);
    ~SpotlightExtra() override;
    bool slidesFromMenus() const override { return false; }
    void openField();
    bool isOpen() const;

private:
    std::unique_ptr<SpotlightWindow> m_window;
};
