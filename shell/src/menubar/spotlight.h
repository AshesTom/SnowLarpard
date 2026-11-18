// SPDX-License-Identifier: LGPL-2.1-or-later
// Spotlight's place in the menu bar (Spotlight's own, not a .menu bundle): its field
#pragma once

#include "menuextra.h"

class SpotlightExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit SpotlightExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 28; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QWidget *popup() override;
private:
    QWidget *m_popup = nullptr;
};
