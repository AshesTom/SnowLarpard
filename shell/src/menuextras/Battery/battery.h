// SPDX-License-Identifier: LGPL-2.1-or-later
// Battery.menu: the charge (/sys/class/power_supply)
#pragma once

#include "menuextra.h"

class BatteryExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit BatteryExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 34; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QMenu *menu() override;
    bool visible() const override { return !m_battery.isEmpty(); }
private:
    void refresh();
    QString m_battery;
    int m_percent = 100;
    QString m_status;
    QMenu *m_menu = nullptr;
};
