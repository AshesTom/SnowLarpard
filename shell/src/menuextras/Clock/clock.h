// SPDX-License-Identifier: LGPL-2.1-or-later
// Clock.menu: the day and the time, the date in its menu
#pragma once

#include "menuextra.h"

class ClockExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit ClockExtra(QObject *parent);
    int width(const QFontMetrics &fm) const override;
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QMenu *menu() override;
private:
    QString text() const;
    QMenu *m_menu = nullptr;
};
