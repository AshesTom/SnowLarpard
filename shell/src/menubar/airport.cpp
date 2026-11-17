// SPDX-License-Identifier: LGPL-2.1-or-later
#include "airport.h"

#include "aqua.h"
#include "art.h"
#include "assets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHostInfo>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

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
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { poll(false); });
    timer->start(5000);
    // the scanning waves while joining (AirPortScanning1-4)
    m_animation = new QTimer(this);
    m_animation->setInterval(300);
    connect(m_animation, &QTimer::timeout, this, [this] {
        m_frame = (m_frame + 1) % 4;
        emit changed();
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
                                       " --rescan %1")
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
    // the joined network's rate, averaged over the last three samples (-averageTxRateWithNewSample:)
    auto active = std::find_if(networks.begin(), networks.end(), [](const Network &n) { return n.active; });
    if (active != networks.end()) {
        int sample = active->rate;
        int count = 1 + (m_txSamples[1] > 0) + (m_txSamples[0] > 0);
        int older = m_txSamples[1], old = m_txSamples[0];
        m_txSamples[2] = older, m_txSamples[1] = old, m_txSamples[0] = sample;
        int avg = count == 2 ? qMax(sample, old) : count < 2 ? sample : (sample + old + older) / count;
        m_bars = active->signal ? qMax(barsForSignal(rssiForQuality(active->signal)), barsForMappedTransmitRate(avg)) : 0;
    }
    m_hardware = hardware;
    m_device = device;
    m_powerOn = power;
    m_connecting = connecting;
    m_connectivity = connectivity;
    m_profiles = profiles;
    m_networks = networks;
    if (connecting && !m_animation->isActive())
        m_animation->start();
    else if (!connecting)
        m_animation->stop();
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
    } else if (m_connecting) {
        name = QString("AirPortScanning%1").arg(m_frame + 1);
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
                if (m_scanning && m_status && m_powerOn)
                    m_status->setText(S::Scanning);
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
        m_menu->adjustSize();
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
            QString phy = n.frequency > 4900 ? (n.rate > 54 ? "802.11n" : "802.11a") : (n.rate > 54 ? "802.11n" : "802.11g");
            line("PHY Mode: " + phy);
            line("BSSID: " + n.bssid.toLower());
            line(QString("Channel: %1 (%2)").arg(n.channel).arg(n.frequency > 4900 ? "5 GHz" : "2.4 GHz"));
            line("Security: " + localizedSecurity(n.security));
            line(QString("RSSI: %1").arg(rssiForQuality(n.signal)));
            line(QString("Transmit Rate: %1").arg(n.rate));
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

// ---- joining: NetworkManager's AddAndActivateConnection, the password never on a command line

namespace {

struct Asked {
    QString name, password;
    int security = 2;   // None, WEP, WPA2 Personal
    bool ok = false;
};

// The panel AirPort shows to join a network: its question, the fields it needs, Cancel and the button
Asked ask(const QString &message, const QString &button, bool nameField, bool securityField, bool passwordField,
          const QString &defaultName = QString())
{
    Asked a;
    QDialog d;
    d.setWindowTitle("AirPort");
    auto *lay = new QVBoxLayout(&d);
    lay->setContentsMargins(20, 16, 20, 14);
    auto *text = new QLabel(message);
    text->setFont(Assets::uiFont(13, true));
    text->setWordWrap(true);
    lay->addWidget(text);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    QLineEdit *name = nullptr, *password = nullptr;
    QComboBox *security = nullptr;
    if (nameField) {
        name = new QLineEdit(defaultName);
        form->addRow("Network Name:", name);
    }
    if (securityField) {
        security = new QComboBox;
        security->addItems({"None", "WEP", "WPA2 Personal"});
        security->setCurrentIndex(passwordField ? 2 : 0);
        form->addRow("Security:", security);
    }
    if (passwordField) {
        password = new QLineEdit;
        password->setEchoMode(QLineEdit::Password);
        form->addRow("Password:", password);
        auto *show = new QCheckBox("Show password");
        QObject::connect(show, &QCheckBox::toggled, password,
                         [password](bool on) { password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
        form->addRow(QString(), show);
        if (security)
            QObject::connect(security, &QComboBox::currentIndexChanged, password, [password, show](int i) {
                password->setEnabled(i != 0);
                show->setEnabled(i != 0);
            });
    }
    lay->addLayout(form);
    auto *buttons = new QDialogButtonBox;
    buttons->addButton(QDialogButtonBox::Cancel);
    QPushButton *ok = buttons->addButton(button, QDialogButtonBox::AcceptRole);
    ok->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lay->addWidget(buttons);
    d.setMinimumWidth(420);
    if (QLineEdit *first = name ? name : password)
        first->setFocus();
    a.ok = d.exec() == QDialog::Accepted;
    if (name)
        a.name = name->text();
    if (password)
        a.password = password->text();
    if (security)
        a.security = security->currentIndex();
    return a;
}

void addAndActivate(const QString &device, const NMSettings &settings)
{
    auto bus = QDBusConnection::systemBus();
    QDBusMessage get = QDBusMessage::createMethodCall("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                                                      "org.freedesktop.NetworkManager", "GetDeviceByIpIface");
    get << device;
    auto *watch = new QDBusPendingCallWatcher(bus.asyncCall(get));
    QObject::connect(watch, &QDBusPendingCallWatcher::finished, watch, [settings](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<QDBusObjectPath> path = *w;
        if (path.isError())
            return;
        QDBusMessage add = QDBusMessage::createMethodCall("org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                                                          "org.freedesktop.NetworkManager", "AddAndActivateConnection");
        add << QVariant::fromValue(settings) << QVariant::fromValue(path.value()) << QVariant::fromValue(QDBusObjectPath("/"));
        QDBusConnection::systemBus().asyncCall(add);
    });
}

void addSecurity(NMSettings &s, int security, const QString &password)
{
    if (security == 1) {
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", "none"}, {"wep-key0", password}, {"wep-key-type", 2u}};
    } else if (security == 2) {
        s["802-11-wireless-security"] = QVariantMap{{"key-mgmt", "wpa-psk"}, {"psk", password}};
    }
}

} // namespace

void AirPortExtra::join(const Network &n)
{
    // a network NetworkManager knows, or an open one: as it is
    if (m_profiles.contains(n.ssid)) {
        QProcess::startDetached("nmcli", {"connection", "up", "id", n.ssid});
        return;
    }
    if (n.security.isEmpty()) {
        QProcess::startDetached("nmcli", {"device", "wifi", "connect", n.ssid});
        return;
    }
    // else its password, asked as AirPort asks it
    const bool wep = n.security.contains("WEP");
    Asked a = ask(QString("The network “%1” requires a %2 password.").arg(n.ssid, wep ? "WEP" : localizedSecurity(n.security).section(' ', 0, 0)),
                  "OK", false, false, true);
    if (!a.ok)
        return;
    NMSettings s;
    s["connection"] = QVariantMap{{"id", n.ssid}, {"type", "802-11-wireless"}};
    s["802-11-wireless"] = QVariantMap{{"ssid", n.ssid.toUtf8()}, {"mode", "infrastructure"}};
    addSecurity(s, wep ? 1 : 2, a.password);
    addAndActivate(m_device, s);
}

void AirPortExtra::joinOther()
{
    Asked a = ask("Find and join an AirPort network.\nEnter the name and security type of the network you want to join.", "Join", true,
                  true, true);
    if (!a.ok || a.name.isEmpty())
        return;
    NMSettings s;
    s["connection"] = QVariantMap{{"id", a.name}, {"type", "802-11-wireless"}};
    s["802-11-wireless"] = QVariantMap{{"ssid", a.name.toUtf8()}, {"mode", "infrastructure"}, {"hidden", true}};
    addSecurity(s, a.security, a.password);
    addAndActivate(m_device, s);
}

void AirPortExtra::createNetwork()
{
    // a computer-to-computer network, named after the computer; this computer shares its connection
    Asked a = ask("Create a computer-to-computer network.\nEnter the name and security type of the network you want to create.",
                  "OK", true, true, true, QHostInfo::localHostName());
    if (!a.ok || a.name.isEmpty())
        return;
    NMSettings s;
    s["connection"] = QVariantMap{{"id", a.name}, {"type", "802-11-wireless"}, {"autoconnect", false}};
    s["802-11-wireless"] = QVariantMap{{"ssid", a.name.toUtf8()}, {"mode", "ap"}};
    s["ipv4"] = QVariantMap{{"method", "shared"}};
    s["ipv6"] = QVariantMap{{"method", "ignore"}};
    addSecurity(s, a.security, a.password);
    addAndActivate(m_device, s);
}
