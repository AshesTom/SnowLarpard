// SPDX-License-Identifier: LGPL-2.1-or-later
// Volume.menu: the output's volume (pactl), a slider in its menu
#pragma once

#include "menuextra.h"

class VolumeExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit VolumeExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 30; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QWidget *popup() override;
    bool visible() const override { return m_available; }
private:
    void refresh();
    void set(int percent);
    int m_level = 50;
    bool m_muted = false, m_available = false;
    QWidget *m_popup = nullptr;
};
