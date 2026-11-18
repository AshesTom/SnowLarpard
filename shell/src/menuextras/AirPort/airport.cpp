// SPDX-License-Identifier: LGPL-2.1-or-later
#include "airport.h"

#include "aqua.h"
#include "art.h"
#include "assets.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFontMetrics>
#include <QIcon>
#include <QMenu>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QUrl>

#include <algorithm>

using NMSettings = QMap<QString, QVariantMap>;
Q_DECLARE_METATYPE(NMSettings)

static const QString res = QStringLiteral("/System/Library/CoreServices/Menu Extras/AirPort.menu/Contents/Resources/");

// AirPort.menu's Localizable.strings (English)
namespace S {
const char *const StatusOn = "AirPort: On", *const StatusOff = "AirPort: Off", *const Scanning = "AirPort: Looking for Networks…";
const char *const TurnOff = "Turn AirPort Off", *const TurnOn = "Turn AirPort On";
const char *const NoNetwork = "No network selected", *const Devices = "Devices";
const char *const Other = "Join Other Network…", *const Create = "Create Network…", *const Prefs = "Open Network Preferences…";
const char *const NoInternet = "No Internet Connection…", *const Portal = "Requires Authentication…";
} // namespace S

// ---- what AME_AirPortNetworksOrganizer computes

int AirPortExtra::barsForSignal(int rssi)
{
    // +barsForSignal: (0xdbaf): the bars over {0, 20, 40, 60, 80} that the signal (RSSI + 100) passes
    static const int table[5] = {0, 20, 40, 60, 80};
    int s = rssi < 1 ? rssi + 100 : rssi;
    int i = 0;
    while (table[i] < s && i != 4)
        ++i;
    return i;
}

int AirPortExtra::barsForMappedTransmitRate(int rate)
{
    // +barsForMappedTransmitRate: the rate, 54 Mbit/s at most, over {0, 1, 5, 12, 36}
    static const int table[5] = {0, 1, 5, 12, 36};
    int r = qMin(rate, 54);
    int i = 0;
    while (table[i] < r && i != 4)
        ++i;
    return i;
}

int AirPortExtra::rssiForQuality(int quality)
{
    // NetworkManager's quality is the RSSI from -100 to -40 dBm, as 0 to 100 (nm_wifi_utils_level_to_quality)
    if (quality <= 0)
        return 0;
    return quality >= 100 ? -40 : int(std::lround(quality * 0.6 - 100));
}

// ---- nmcli's terse output: fields split on ':', '\:' and '\\' escaped

static QStringList terseFields(const QString &line)
{
    QStringList out{QString()};
    for (int i = 0; i < line.size(); ++i) {
        QChar c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            out.last() += line[++i];
        } else if (c == ':') {
            out.append(QString());
        } else {
            out.last() += c;
        }
    }
    return out;
}

static QIcon spinnerIcon(int step);

static QString localizedSecurity(const QString &nm)
{
    // -getLocalizedSecurityTypeForNetwork:, in 10.6's words
    if (nm.isEmpty() || nm == "--")
        return "None";
    if (nm.contains("802.1X"))
        return nm.contains("WPA2") || nm.contains("WPA3") ? "WPA2 Enterprise" : "WPA Enterprise";
    if (nm.contains("WPA2") || nm.contains("WPA3"))
        return "WPA2 Personal";
    if (nm.contains("WPA"))
        return "WPA Personal";
    if (nm.contains("WEP"))
        return "WEP";
    return nm;
}

// ---- the extra

AirPortExtra::AirPortExtra(QObject *parent) : MenuExtra(parent)
{
    qDBusRegisterMetaType<NMSettings>();
    m_joinTimeout = new QTimer(this);
    m_joinTimeout->setSingleShot(true);
    connect(m_joinTimeout, &QTimer::timeout, this, [this] { finishJoin("Connection timeout"); });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { poll(false); });
    timer->start(5000);
    // -startScanningAnimation (0x1074e): AirPortScanning1, 2, 3, 4, 3, 2 at half a second each
    // (-setImageFrameRate: 0.5), stopped after 30 seconds (-stopExtraImageAnimation)
    m_animation = new QTimer(this);
    m_animation->setInterval(500);
    connect(m_animation, &QTimer::timeout, this, [this] {
        m_frame = (m_frame + 1) % 6;
        if (m_animationClock.elapsed() >= 30000)
            m_animation->stop();
        emit changed();
    });
    // the status line's small spinning indicator (AME_AirPortMenuStatusItemView -startIndicator)
    m_spinner = new QTimer(this);
    m_spinner->setInterval(83);
    connect(m_spinner, &QTimer::timeout, this, [this] {
        m_spin = (m_spin + 1) % 12;
        if (m_status)
            m_status->setIcon(spinnerIcon(m_spin));
    });
    poll(false);
}

void AirPortExtra::poll(bool rescan)
{
    if (m_proc)
        return;
    if (rescan)
        m_scanning = true;
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, [this, rescan](int code) {
        QString out = QString::fromUtf8(m_proc->readAllStandardOutput());
        m_proc->deleteLater();
        m_proc = nullptr;
        if (code == 0 || !out.isEmpty())
            parse(out);
        if (rescan)
            m_scanning = false;
        if (m_menu && m_menu->isVisible())
            refresh();
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_proc->deleteLater();
        m_proc = nullptr;
        m_hardware = false;
    });
    m_proc->start("sh", {"-c", QString("nmcli -t -f TYPE,STATE,DEVICE device; echo @@; nmcli radio wifi; echo @@;"
                                       " nmcli -t -f CONNECTIVITY general; echo @@; nmcli -t -f NAME,TYPE connection show; echo @@;"
                                       " nmcli -t -e yes -f IN-USE,SSID,MODE,CHAN,FREQ,RATE,SIGNAL,SECURITY,BSSID device wifi list"
                                       " --rescan %1; echo @@;"
                                       // the link as the driver has it: the RSSI in dBm, the rate, the MCS index
                                       " d=$(nmcli -t -f DEVICE,TYPE device | awk -F: '$2==\"wifi\"{print $1; exit}');"
                                       " [ -n \"$d\" ] && command -v iw >/dev/null && iw dev \"$d\" link 2>/dev/null")
                                   .arg(rescan ? "yes" : "no")});
}

void AirPortExtra::parse(const QString &out)
{
    const QStringList sections = out.split("@@\n");
    auto lines = [&](int i) { return i < sections.size() ? sections[i].split('\n', Qt::SkipEmptyParts) : QStringList(); };
    bool hardware = false, connecting = false;
    QString device;
    for (const QString &l : lines(0)) {
        QStringList f = terseFields(l);
        if (f.size() >= 3 && f[0] == "wifi") {
            hardware = true;
            device = f[2];
            connecting = f[1].startsWith("connecting");
            break;
        }
    }
    const bool power = lines(1).value(0).trimmed() == "enabled";
    const QString connectivity = lines(2).value(0).trimmed();
    QStringList profiles;
    for (const QString &l : lines(3)) {
        QStringList f = terseFields(l);
        if (f.size() >= 2 && f[1] == "802-11-wireless")
            profiles << f[0];
    }
    QVector<Network> networks;
    for (const QString &l : lines(4)) {
        QStringList f = terseFields(l);
        if (f.size() < 9 || f[1].isEmpty())
            continue;   // hidden networks: no name to show
        Network n;
        n.active = f[0] == "*";
        n.ssid = f[1];
        n.adhoc = f[2] == "Ad-Hoc";
        n.channel = f[3].toInt();
        n.frequency = f[4].section(' ', 0, 0).toInt();
        n.rate = f[5].section(' ', 0, 0).toInt();
        n.signal = f[6].toInt();
        n.security = f[7] == "--" ? QString() : f[7];
        n.bssid = f[8];
        networks.append(n);
    }
    // iw's link: "signal: -45 dBm", "tx bitrate: 780.0 MBit/s VHT-MCS 9 80MHz VHT-NSS 2"
    m_link = Link();
    for (const QString &l : lines(5)) {
        const QString t = l.trimmed();
        if (t.startsWith("signal:"))
            m_link.rssi = t.section(' ', 1, 1).toInt();
        else if (t.startsWith("tx bitrate:")) {
            m_link.rate = int(t.section(' ', 2, 2).toDouble());
            static const QRegularExpression mcs("\\b(HE-|VHT-|EHT-)?MCS (\\d+)");
            auto m = mcs.match(t);
            if (m.hasMatch()) {
                m_link.mcs = m.captured(2).toInt();
                const QString kind = m.captured(1);
                m_link.phy = kind == "EHT-" ? "802.11be" : kind == "HE-" ? "802.11ax" : kind == "VHT-" ? "802.11ac" : "802.11n";
            }
        }
    }
    // the joined network's rate, averaged over the last three samples (-averageTxRateWithNewSample:)
    auto active = std::find_if(networks.begin(), networks.end(), [](const Network &n) { return n.active; });
    if (active != networks.end()) {
        if (m_link.rssi)
            active->rssi = m_link.rssi;
        if (m_link.rate)
            active->rate = m_link.rate;
        int sample = active->rate;
        int count = 1 + (m_txSamples[1] > 0) + (m_txSamples[0] > 0);
        int older = m_txSamples[1], old = m_txSamples[0];
        m_txSamples[2] = older, m_txSamples[1] = old, m_txSamples[0] = sample;
        int avg = count == 2 ? qMax(sample, old) : count < 2 ? sample : (sample + old + older) / count;
        const int rssi = active->rssi ? active->rssi : rssiForQuality(active->signal);
        m_bars = rssi ? qMax(barsForSignal(rssi), barsForMappedTransmitRate(avg)) : 0;
    }
    m_hardware = hardware;
    m_device = device;
    m_powerOn = power;
    m_connectivity = connectivity;
    m_profiles = profiles;
    m_networks = networks;
    if (connecting && !m_connecting) {
        m_frame = 0;
        m_animationClock.start();
        m_animation->start();
    } else if (!connecting) {
        m_animation->stop();
    }
    m_connecting = connecting;
    emit changed();
}

void AirPortExtra::paint(QPainter &p, const QRect &r, bool highlighted)
{
    // -updateStatusImage: the warning, else the interface's state: off, joining, joined (its bars)
    const qreal dpr = p.device()->devicePixelRatioF();
    const bool joined = std::any_of(m_networks.begin(), m_networks.end(), [](const Network &n) { return n.active; });
    QString name;
    bool dimmed = false;
    if (!m_powerOn) {
        name = "AirPortOff";
        dimmed = !highlighted;   // -disabledImageFromMyBundle:, the alternate image plain
    } else if (m_connecting && m_animation->isActive()) {
        static const int frames[6] = {1, 2, 3, 4, 3, 2};
        name = QString("AirPortScanning%1").arg(frames[m_frame]);
    } else if (joined && (m_connectivity == "limited" || m_connectivity == "portal" || m_connectivity == "none")) {
        name = "AirPortWarning";
    } else if (joined) {
        name = QString("AirPort%1").arg(m_bars);
    } else {
        name = "AirPort0";
    }
    QImage img = menuIcon(res + name + ".pdf", 16, dpr, highlighted);
    if (img.isNull()) {
        p.setPen(highlighted ? Qt::white : Qt::black);
        Aqua::drawText(p, r, Qt::AlignCenter, QString::fromUtf8("≋"));
        return;
    }
    p.save();
    if (dimmed)
        p.setOpacity(0.5);
    drawCentered(p, r, img);
    p.restore();
}

// The lock and the bars at the right of a network's title (AME_AirPortNetworkItemView, 0x985d): 10
// points, AppKit's NSImageNameLockLockedTemplate (CoreUI's shared art 20173) in its 9 by 12, 6 more,
// the bars' 17 by 13 (AirportInMenu0-4); both at 63% (fraction 0.63), white when selected
static QImage templateImage(QImage img, bool white)
{
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(img.rect(), white ? Qt::white : Qt::black);
    return img;
}

// NSProgressIndicator's small spinning style (16 points): twelve spokes, the leading one darkest
static QIcon spinnerIcon(int step)
{
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap px(QSize(16, 16) * dpr);
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(8, 8);
    for (int i = 0; i < 12; ++i) {
        const int age = (step - i + 12) % 12;
        QPen pen(QColor(0, 0, 0, qMax(40, 230 - age * 20)), 1.6, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.save();
        p.rotate(i * 30);
        p.drawLine(QPointF(0, -3.4), QPointF(0, -6.6));
        p.restore();
    }
    p.end();
    return QIcon(px);
}

static QIcon networkIcon(bool secure, int bars)
{
    const qreal dpr = qApp->devicePixelRatio();
    const QSize size(10 + 9 + 6 + 17, 13);
    QIcon icon;
    for (bool selected : {false, true}) {
        QPixmap px(size * dpr);
        px.setDevicePixelRatio(dpr);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setOpacity(selected ? 1.0 : 0.63);
        if (secure) {
            QImage lock;
            for (int rep = 0; rep < 4 && lock.isNull(); ++rep)   // its first representation with pixels
                lock = Art::shared(20173, rep);
            if (!lock.isNull()) {
                lock = templateImage(lock, selected);
                p.drawImage(QPointF(10 + (9 - lock.width()) / 2.0, (13 - lock.height()) / 2.0), lock);
            }
        }
        QImage signal = menuIcon(res + QString("AirportInMenu%1.pdf").arg(qBound(0, bars, 4)), 13, dpr, selected);
        if (!signal.isNull())
            p.drawImage(QPointF(25, 0), signal);
        p.end();
        icon.addPixmap(px, selected ? QIcon::Selected : QIcon::Normal);
    }
    return icon;
}

QAction *AirPortExtra::networkItem(const Network &n, bool current, int bars)
{
    // -menuItemForNetworkRef: the name, checked when joined (handleJoinCurrentNetwork:)
    auto *a = new QAction(n.ssid, m_menu);
    a->setCheckable(current);
    a->setChecked(current);
    a->setIcon(networkIcon(!n.security.isEmpty(), bars));
    Network copy = n;
    connect(a, &QAction::triggered, this, [this, copy, current] {
        if (!current)
            join(copy);
    });
    return a;
}

QMenu *AirPortExtra::menu()
{
    if (!m_menu) {
        m_menu = new QMenu;
        m_menu->setProperty("aquaTrailingIcons", true);
        connect(m_menu, &QMenu::aboutToShow, this, [this] {
            m_option = QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier;
            rebuild();
            // -menu: the scan in the background, "Looking for Networks…" if it lasts 1.5 seconds
            poll(true);
            QTimer::singleShot(1500, m_menu, [this] {
                // -setScanningStatus: (0x4285): the title, and the status view's indicator going
                if (m_scanning && m_status && m_powerOn) {
                    m_status->setText(S::Scanning);
                    m_spinner->start();
                }
            });
        });
    }
    return m_menu;
}

void AirPortExtra::rebuild()
{
    // AirPortExtraImplementation -menu (0x5284): the status, the power, then the networks (-updateMenu)
    const QList<QAction *> old = m_menu->actions();
    m_list.clear();
    m_tail = nullptr;
    // the status line is a view of its own on the Mac, as wide whatever it says: the menu keeps the
    // width "Looking for Networks…" needs
    m_menu->setMinimumWidth(QFontMetrics(Assets::uiFont(14)).horizontalAdvance(S::Scanning) + 21 + 22);
    m_spinner->stop();
    m_status = m_menu->addAction(m_powerOn ? (m_scanning ? S::Scanning : S::StatusOn) : S::StatusOff);
    m_status->setEnabled(false);
    const bool joined = std::any_of(m_networks.begin(), m_networks.end(), [](const Network &n) { return n.active; });
    if (m_powerOn && joined && (m_connectivity == "limited" || m_connectivity == "portal" || m_connectivity == "none")) {
        // the connection's warning, AlertTriangleSmall at its right (AirPortMenuBadgeView)
        QAction *w = m_menu->addAction(m_connectivity == "portal" ? S::Portal : S::NoInternet, [] { openPreferencePane("network"); });
        QImage tri = menuIcon(res + "AlertTriangleSmall.pdf", 11, qApp->devicePixelRatio(), false);
        if (!tri.isNull())
            w->setIcon(QIcon(QPixmap::fromImage(tri)));
    }
    m_menu->addAction(m_powerOn ? S::TurnOff : S::TurnOn, [this] {
        QProcess::startDetached("nmcli", {"radio", "wifi", m_powerOn ? "off" : "on"});
        m_powerOn = !m_powerOn;
        emit changed();
        QTimer::singleShot(1500, this, [this] { poll(false); });
    });
    m_menu->addSeparator();
    m_builtPowerOn = m_powerOn;
    if (m_powerOn) {
        m_tail = m_menu->addSeparator();
        fillNetworks();
        m_menu->addAction(S::Other, this, &AirPortExtra::joinOther);
        m_menu->addAction(S::Create, this, &AirPortExtra::createNetwork);
    }
    m_menu->addAction(S::Prefs, [] { openPreferencePane("network"); });
    removeActions(m_menu, old);
    Aqua::prepareMenu(m_menu);
}

void AirPortExtra::refresh()
{
    // a scan's results with the menu down: its networks in place (-updateMenu), the rest as it is
    if (m_builtPowerOn != m_powerOn || !m_tail) {
        rebuild();
    } else {
        m_status->setText(m_scanning ? S::Scanning : S::StatusOn);
        if (!m_scanning) {
            // -updateMenu: -stopIndicator
            m_spinner->stop();
            m_status->setIcon(QIcon());
        }
        for (QAction *a : std::as_const(m_list)) {
            m_menu->removeAction(a);
            a->deleteLater();
        }
        m_list.clear();
        fillNetworks();
    }
    // the menu keeps its right edge on the screen
    QTimer::singleShot(0, m_menu, [this] {
        if (!m_menu->isVisible() || !m_menu->screen())
            return;
        m_menu->resize(m_menu->sizeHint());   // not adjustSize(): it keeps a window to 2/3 of the screen
        const QRect screen = m_menu->screen()->geometry();
        if (m_menu->geometry().right() > screen.right())
            m_menu->move(screen.right() + 1 - m_menu->width(), m_menu->y());
    });
}

void AirPortExtra::fillNetworks()
{
    auto add = [this](QAction *a) {
        m_menu->insertAction(m_tail, a);
        m_list << a;
        return a;
    };
    auto disabled = [&](const QString &title) {
        auto *a = new QAction(title, m_menu);
        a->setEnabled(false);
        return add(a);
    };
    // the network joined, checked, then its details with the Option key (PHY mode, BSSID, channel...),
    // else "No network selected"
    auto current = std::find_if(m_networks.begin(), m_networks.end(), [](const Network &n) { return n.active && !n.adhoc; });
    if (current != m_networks.end()) {
        add(networkItem(*current, true, m_bars));
        if (m_option) {
            const Network &n = *current;
            auto line = [&](const QString &text) { disabled("      " + text); };
            // the PHY mode the driver gives (HT, VHT, HE...), else from the band and the rate
            QString phy = m_link.phy;
            if (phy.isEmpty())
                phy = n.frequency > 4900 ? (n.rate > 54 ? "802.11n" : "802.11a") : (n.rate > 54 ? "802.11n" : "802.11g");
            line("PHY Mode: " + phy);
            line("BSSID: " + n.bssid.toLower());
            line(QString("Channel: %1 (%2)").arg(n.channel).arg(n.frequency > 4900 ? "5 GHz" : "2.4 GHz"));
            line("Security: " + localizedSecurity(n.security));
            line(QString("RSSI: %1").arg(n.rssi ? n.rssi : rssiForQuality(n.signal)));
            line(QString("Transmit Rate: %1").arg(n.rate));
            if (m_link.mcs >= 0)
                line(QString("MCS Index: %1").arg(m_link.mcs));
        }
    } else {
        disabled(S::NoNetwork);
    }
    // the networks found, by name (+sectionTitlesForGroup: with no grouping: "name", then the
    // computer-to-computer ones under Devices), each name once: its strongest
    QVector<Network> found;
    for (const Network &n : m_networks) {
        if (current != m_networks.end() && n.ssid == current->ssid && !n.adhoc)
            continue;
        auto same = std::find_if(found.begin(), found.end(), [&](const Network &o) { return o.ssid == n.ssid && o.adhoc == n.adhoc; });
        if (same == found.end())
            found.append(n);
        else if (n.signal > same->signal)
            *same = n;
    }
    std::sort(found.begin(), found.end(), [](const Network &a, const Network &b) { return a.ssid.localeAwareCompare(b.ssid) < 0; });
    for (const Network &n : found)
        if (!n.adhoc)
            add(networkItem(n, false, barsForSignal(rssiForQuality(n.signal))));
    bool devices = false;
    for (const Network &n : found) {
        if (!n.adhoc)
            continue;
        if (!devices) {
            auto *sep = new QAction(m_menu);
            sep->setSeparator(true);
            add(sep);
            disabled(S::Devices);
            devices = true;
        }
        add(networkItem(n, n.active, barsForSignal(rssiForQuality(n.signal))));
    }
}

// ---- joining: AirPort's panels (privateframeworks/Apple80211/clientdialogs.cpp, ClientDialogs' nibs), then NetworkManager's
// AddAndActivateConnection2 over D-Bus, the password never on a command line

namespace {

// the security type's settings (the panels' tags: -getLocalizedNameOfSecurityType:)
void addSecurity(NMSettings &s, int type, const QString &password, const QString &username)
{
    switch (type) {
    case 1:   // WEP Password: a passphrase
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", "none"}, {"wep-key0", password}, {"wep-key-type", 2u}};
        break;
    case 2:   // WEP 40/128-bit hex
    case 3:   // WEP 40/128-bit ASCII
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", "none"}, {"wep-key0", password}, {"wep-key-type", 1u}};
        break;
    case 4:   // LEAP
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", "ieee8021x"}, {"auth-alg", "leap"}, {"leap-username", username},
                                                    {"leap-password", password}};
        break;
    case 5:
    case 7:
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", "wpa-psk"}, {"psk", password}};
        break;
    case 6:
    case 8:
    case 9:   // Enterprise and 802.1X WEP: PEAP / MSCHAPv2, as 10.6's "Automatic" profile mostly was
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", type == 9 ? "ieee8021x" : "wpa-eap"}};
        s["802-1x"] = QVariantMap{{"eap", QStringList{"peap", "ttls"}}, {"identity", username}, {"password", password},
                                  {"phase2-auth", "mschapv2"}};
        break;
    default:
        break;
    }
}

// kAirPortConnectionFailed*: NMActiveConnectionStateReason
QString failure(uint reason)
{
    switch (reason) {
    case 9:   // NO_SECRETS
    case 10:  // LOGIN_FAILED
        return "Invalid password";
    case 6:   // CONNECT_TIMEOUT
        return "Connection timeout";
    default:
        return "Connection failed";
    }
}

} // namespace

// the panel's program, beside SystemUIServer
void AirPortExtra::runPanel(const QStringList &args)
{
    if (m_panel)
        return;   // one panel at a time, as ClientController shows them
    m_panel = new QProcess(this);
    m_panelOutput.clear();
    connect(m_panel, &QProcess::readyReadStandardOutput, this, [this] {
        m_panelOutput += QString::fromUtf8(m_panel->readAllStandardOutput());
        if (!m_panelOutput.contains("\nend\n") && !m_panelOutput.startsWith("end\n"))
            return;
        QMap<QString, QString> fields;
        for (const QString &line : m_panelOutput.split('\n', Qt::SkipEmptyParts)) {
            if (line == "end")
                break;
            const int eq = line.indexOf('=');
            if (eq > 0)
                fields[line.left(eq)] = QUrl::fromPercentEncoding(line.mid(eq + 1).toUtf8());
        }
        m_panelOutput.clear();
        panelAccepted(fields);
    });
    connect(m_panel, &QProcess::finished, this, [this] {
        m_panel->deleteLater();
        m_panel = nullptr;
    });
    // Apple80211.framework's ClientDialogs.bundle, among the private frameworks beside SystemUIServer.app
    const QString program = QFileInfo(QCoreApplication::applicationDirPath() +
                                      "/../../../PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle/Contents/MacOS/ClientDialogs")
                                .absoluteFilePath();
    m_panel->start(program, args);
}

void AirPortExtra::panelAccepted(const QMap<QString, QString> &f)
{
    const QString ssid = f.value("ssid");
    const int type = f.value("security").toInt();
    NMSettings s;
    QVariantMap wireless{{"ssid", ssid.toUtf8()}, {"mode", "infrastructure"}};
    QVariantMap connection{{"id", ssid}, {"type", "802-11-wireless"}};
    if (f.contains("channel")) {
        // Create Network…: a computer-to-computer network; this computer shares its connection
        wireless["mode"] = "ap";
        const int channel = f.value("channel").toInt();
        if (channel > 0) {
            wireless["band"] = channel > 14 ? "a" : "bg";
            wireless["channel"] = uint(channel);
        }
        connection["autoconnect"] = false;
        s["ipv4"] = QVariantMap{{"method", "shared"}};
        s["ipv6"] = QVariantMap{{"method", "ignore"}};
    } else if (f.value("hidden", m_panelHidden ? "1" : "0") == "1") {
        wireless["hidden"] = true;   // a network named in Join Other: maybe one that doesn't broadcast
    }
    s["connection"] = connection;
    s["802-11-wireless"] = wireless;
    addSecurity(s, type, f.value("password"), f.value("username"));
    const bool remember = f.value("remember", "1") == "1";

    auto bus = QDBusConnection::systemBus();
    QDBusMessage get = QDBusMessage::createMethodCall("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                                                      "org.freedesktop.NetworkManager", "GetDeviceByIpIface");
    get << m_device;
    QDBusMessage device = bus.call(get);
    if (device.type() != QDBusMessage::ReplyMessage || device.arguments().isEmpty()) {
        panelFailed("Connection failed");
        return;
    }
    // Remember this network: kept on disk; else for this time only
    QDBusMessage add = QDBusMessage::createMethodCall("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                                                      "org.freedesktop.NetworkManager", "AddAndActivateConnection2");
    add << QVariant::fromValue(s) << device.arguments().first() << QVariant::fromValue(QDBusObjectPath("/"))
        << QVariantMap{{"persist", remember ? "disk" : "volatile"}};
    auto *watch = new QDBusPendingCallWatcher(bus.asyncCall(add), this);
    connect(watch, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QDBusObjectPath, QDBusObjectPath, QVariantMap> r = *w;
        if (r.isError()) {
            panelFailed("Connection failed");
            return;
        }
        m_joinSettings = r.argumentAt<0>().path();
        m_joinActive = r.argumentAt<1>().path();
        QDBusConnection::systemBus().connect("org.freedesktop.NetworkManager", m_joinActive,
                                             "org.freedesktop.NetworkManager.Connection.Active", "StateChanged", this,
                                             SLOT(joinStateChanged(uint, uint)));
        m_joinTimeout->start(45000);
    });
}

void AirPortExtra::joinStateChanged(uint state, uint reason)
{
    // NM_ACTIVE_CONNECTION_STATE: 2 activated, 4 deactivated
    if (state == 2) {
        finishJoin(QString());
    } else if (state == 4) {
        finishJoin(failure(reason));
    }
}

void AirPortExtra::finishJoin(const QString &error)
{
    m_joinTimeout->stop();
    if (!m_joinActive.isEmpty())
        QDBusConnection::systemBus().disconnect("org.freedesktop.NetworkManager", m_joinActive,
                                                "org.freedesktop.NetworkManager.Connection.Active", "StateChanged", this,
                                                SLOT(joinStateChanged(uint, uint)));
    if (!error.isEmpty() && !m_joinSettings.isEmpty()) {
        // a network that couldn't be joined isn't remembered
        QDBusConnection::systemBus().asyncCall(QDBusMessage::createMethodCall(
            "org.freedesktop.NetworkManager", m_joinSettings, "org.freedesktop.NetworkManager.Settings.Connection", "Delete"));
    }
    m_joinActive.clear();
    m_joinSettings.clear();
    if (error.isEmpty()) {
        if (m_panel)
            m_panel->write("done\n");
    } else {
        panelFailed(error);
    }
    poll(false);
}

void AirPortExtra::panelFailed(const QString &error)
{
    if (m_panel)
        m_panel->write(("error " + error + "\n").toUtf8());
}

void AirPortExtra::join(const Network &n)
{
    // -handleJoinSelectedNetwork: a network NetworkManager knows, or an open one: as it is
    if (m_profiles.contains(n.ssid)) {
        QProcess::startDetached("nmcli", {"connection", "up", "id", n.ssid});
        return;
    }
    if (n.security.isEmpty()) {
        QProcess::startDetached("nmcli", {"device", "wifi", "connect", n.ssid});
        return;
    }
    // else AirPortClientDialogPassword: the type as -showAuthViewForACSecurityType: has it
    int type = 7;
    if (n.security.contains("802.1X"))
        type = n.security.contains("WPA2") || n.security.contains("WPA3") ? 8 : 6;
    else if (n.security.contains("WEP"))
        type = 1;
    else if (!n.security.contains("WPA2") && !n.security.contains("WPA3") && n.security.contains("WPA"))
        type = 5;
    m_panelHidden = false;
    QStringList args{"password", n.ssid, QString::number(type)};
    if (m_option)
        args << "--option";
    runPanel(args);
}

void AirPortExtra::joinOther()
{
    // -handleJoinOtherNetwork: AirPortClientDialogJoinNetwork
    m_panelHidden = true;
    runPanel({"join"});
}

void AirPortExtra::createNetwork()
{
    // -handleCreateIBSSNetwork: AirPortClientDialogCreateIBSS
    m_panelHidden = false;
    runPanel({"create"});
}

SYSTEMUI_MENU_EXTRA(AirPortExtra)
