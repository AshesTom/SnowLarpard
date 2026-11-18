// SPDX-License-Identifier: LGPL-2.1-or-later
// The AirPort menu extra (AirPort.menu, AirPortExtraImplementation), on NetworkManager (nmcli): the
// bars of the network joined, the networks found, Turn AirPort Off, Join Other Network…
// What 10.6.8's AirPort.menu does is in docs/reverse-status.md ("AirPort and Bluetooth menu extras").
#pragma once

#include "menuextra.h"

#include <QMap>
#include <QElapsedTimer>
#include <QVector>

class QAction;
class QProcess;
class QTimer;

class AirPortExtra : public MenuExtra {
    Q_OBJECT
public:
    explicit AirPortExtra(QObject *parent);
    int width(const QFontMetrics &) const override { return 32; }
    void paint(QPainter &p, const QRect &r, bool highlighted) override;
    QMenu *menu() override;
    bool visible() const override { return m_hardware; }

    struct Network {
        QString ssid, security, bssid;
        int signal = 0;   // NetworkManager's 0-100
        int rssi = 0;     // dBm, from the driver (iw) for the network joined
        int channel = 0, frequency = 0, rate = 0;
        bool active = false, adhoc = false;
    };
    // AME_AirPortNetworksOrganizer +barsForSignal: (RSSI in dBm) and +barsForMappedTransmitRate: (Mbit/s)
    static int barsForSignal(int rssi);
    static int barsForMappedTransmitRate(int rate);
    static int rssiForQuality(int quality);

private:
    void poll(bool rescan);
    void parse(const QString &out);
    void rebuild();
    void refresh();
    void fillNetworks();
    QAction *networkItem(const Network &n, bool current, int bars);
    void join(const Network &n);
    void joinOther();
    void createNetwork();
    void runPanel(const QStringList &args);
    void panelAccepted(const QMap<QString, QString> &fields);
    void panelFailed(const QString &error);
    void finishJoin(const QString &error);

private slots:
    void joinStateChanged(uint state, uint reason);

private:

    QMenu *m_menu = nullptr;
    QAction *m_status = nullptr, *m_tail = nullptr;   // the status line; the separator under the networks
    QList<QAction *> m_list;                          // the networks' lines
    bool m_builtPowerOn = false;
    QProcess *m_proc = nullptr;
    QTimer *m_animation = nullptr, *m_spinner = nullptr;
    QElapsedTimer m_animationClock;
    int m_spin = 0;
    struct Link {
        int rssi = 0, rate = 0, mcs = -1;
        QString phy;
    } m_link;
    bool m_hardware = false, m_powerOn = false, m_connecting = false, m_scanning = false;
    bool m_option = false;
    QString m_device, m_connectivity;
    QVector<Network> m_networks;
    QStringList m_profiles;     // NetworkManager's Wi-Fi connections (their names)
    // the panel (Apple80211's ClientDialogs) and the network it joins
    QProcess *m_panel = nullptr;
    QString m_panelOutput, m_joinSettings, m_joinActive;
    bool m_panelHidden = false;
    QTimer *m_joinTimeout = nullptr;
    int m_txSamples[3] = {0, 0, 0};
    int m_bars = 0, m_frame = 0;
};
