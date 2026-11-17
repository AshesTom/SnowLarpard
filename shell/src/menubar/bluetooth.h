// SPDX-License-Identifier: LGPL-2.1-or-later
// The Bluetooth menu extra (Bluetooth.menu, AppleBluetoothExtra and its DeviceMenuCreator), on BlueZ
// (bluetoothctl): the controller's state, Discoverable, the devices with their submenus.
// What 10.6.8's Bluetooth.menu does is in docs/reverse-status.md ("AirPort and Bluetooth menu extras").
#pragma once

#include "extras.h"

#include <QVector>

class QAction;
class QProcess;

class BluetoothExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit BluetoothExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 26; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QMenu *menu() override;
    bool visible() const override { return m_controller; }

    struct Device {
        QString address, name, icon;
        QStringList uuids;
        bool connected = false;
        int battery = -1;
        bool has(const char *uuid16) const;
        bool hid() const { return has("1124") || has("1812"); }
        bool stereo() const { return has("110b"); }
        bool headset() const { return has("1108") || has("111e"); }
    };

private:
    void poll();
    void parse(const QString &out);
    void rebuild();
    QAction *deviceItem(const Device &d);

    QMenu *m_menu = nullptr;
    QList<QMenu *> m_submenus;
    QProcess *m_proc = nullptr;
    bool m_controller = false, m_powered = false, m_discoverable = false, m_option = false;
    QString m_name, m_address, m_version;
    QVector<Device> m_devices;
};
