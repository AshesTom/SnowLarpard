// SPDX-License-Identifier: LGPL-2.1-or-later
#include "bluetooth.h"

#include "aqua.h"
#include "assets.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QMenu>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <QElapsedTimer>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <climits>

static const QString res = QStringLiteral("/System/Library/CoreServices/Menu Extras/Bluetooth.menu/Contents/Resources/");

// Bluetooth.menu's Localizable.strings (English)
namespace S {
const char *const BT = "Bluetooth: ", *const On = "On", *const Off = "Off";
const char *const TurnOff = "Turn Bluetooth Off", *const TurnOn = "Turn Bluetooth On", *const Discoverable = "Discoverable";
const char *const SendFile = "Send File…", *const Browse = "Browse Device…", *const Devices = "Devices";
const char *const SetUp = "Set Up Bluetooth Device…", *const Prefs = "Open Bluetooth Preferences…";
const char *const Connect = "Connect", *const Disconnect = "Disconnect", *const Remove = "Remove This Device";
const char *const UseStereo = "Use as Audio Device (Stereo)", *const UseMono = "Use as Audio Device (Mono)";
const char *const DontUse = "Don’t Use as Audio Device";
const char *const Mouse = "Open Mouse Preferences…", *const Keyboard = "Open Keyboard Preferences…";
const char *const Trackpad = "Open Trackpad Preferences…", *const Sound = "Open Sound Preferences…";
const char *const Printer = "Open Printer Preferences…";
} // namespace S

bool BluetoothExtra::Device::has(const char *uuid16) const
{
    // the 16-bit UUIDs in the Bluetooth base UUID, as bluetoothctl lists them
    const QString full = QString("0000%1-0000-1000-8000-00805f9b34fb").arg(QLatin1String(uuid16));
    return uuids.contains(full, Qt::CaseInsensitive);
}

BluetoothExtra::BluetoothExtra(QObject *parent) : MenuExtra(parent)
{
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &BluetoothExtra::poll);
    timer->start(10000);
    poll();
}

void BluetoothExtra::poll()
{
    if (m_proc)
        return;
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this, [this] {
        QString out = QString::fromUtf8(m_proc->readAllStandardOutput());
        m_proc->deleteLater();
        m_proc = nullptr;
        parse(out);
        if (m_menu && m_menu->isVisible())
            rebuild();
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_proc->deleteLater();
        m_proc = nullptr;
    });
    // bluetoothctl waits for bluetoothd forever when it isn't there: a few seconds at most
    m_proc->start("sh", {"-c", "command -v bluetoothctl >/dev/null || exit 1; b='timeout 4 bluetoothctl';"
                               " $b show; echo @@; $b version; echo @@;"
                               // the PAN connections NetworkManager has (a bt device per phone)
                               " nmcli -t -f DEVICE,TYPE,STATE device 2>/dev/null; echo @@;"
                               " for d in $( { $b devices Paired; $b devices Connected; } 2>/dev/null | awk '/^Device/{print $2}' | sort -u);"
                               " do $b info \"$d\"; echo @@; done"});
}

void BluetoothExtra::parse(const QString &out)
{
    const QStringList sections = out.split("@@\n");
    const QString show = sections.value(0);
    auto field = [](const QString &text, const QString &key) {
        auto m = QRegularExpression("^\\s*" + QRegularExpression::escape(key) + ":\\s*(.*)$", QRegularExpression::MultilineOption).match(text);
        return m.hasMatch() ? m.captured(1).trimmed() : QString();
    };
    auto controller = QRegularExpression("^Controller ([0-9A-F:]{17})", QRegularExpression::MultilineOption).match(show);
    m_controller = controller.hasMatch();
    m_address = controller.captured(1);
    m_name = field(show, "Alias");
    if (m_name.isEmpty())
        m_name = field(show, "Name");
    m_powered = field(show, "Powered") == "yes";
    m_discoverable = field(show, "Discoverable") == "yes";
    m_version = sections.value(1).trimmed().section(' ', -1);
    m_pan.clear();
    for (const QString &l : sections.value(2).split('\n', Qt::SkipEmptyParts)) {
        // "80\:99\:E7...:bt:connected"
        QString line = l;
        line.replace("\\:", "-");
        const QStringList f = line.split(':');
        if (f.size() >= 3 && f[1] == "bt")
            m_pan[QString(f[0]).replace('-', ':').toUpper()] = f[2];
    }
    m_devices.clear();
    for (int i = 3; i < sections.size(); ++i) {
        const QString &info = sections[i];
        auto head = QRegularExpression("^Device ([0-9A-F:]{17})", QRegularExpression::MultilineOption).match(info);
        if (!head.hasMatch())
            continue;
        Device d;
        d.address = head.captured(1);
        d.name = field(info, "Alias");
        if (d.name.isEmpty())
            d.name = field(info, "Name");
        d.icon = field(info, "Icon");
        d.connected = field(info, "Connected") == "yes";
        auto rssi = QRegularExpression("RSSI:.*\\((-?\\d+)\\)").match(info);
        if (rssi.hasMatch())
            d.rssi = rssi.captured(1).toInt();
        auto battery = QRegularExpression("Battery Percentage:.*\\((\\d+)\\)").match(info);
        if (battery.hasMatch())
            d.battery = battery.captured(1).toInt();
        auto uuids = QRegularExpression("UUID:.*\\(([0-9a-fA-F-]{36})\\)").globalMatch(info);
        while (uuids.hasNext())
            d.uuids << uuids.next().captured(1);
        m_devices.append(d);
    }
    emit changed();
}

void BluetoothExtra::paint(QPainter &p, const QRect &r, bool highlighted)
{
    // -checkAndSetPicture: no controller BrokenConnection, off Off, a device connected Connected, else
    // Idle; -lowBatteryNotification: lowbatt
    QString name;
    const bool low = std::any_of(m_devices.begin(), m_devices.end(), [](const Device &d) { return d.connected && d.battery >= 0 && d.battery <= 10; });
    if (!m_controller)
        name = "BlueTooth_BrokenConnection";
    else if (!m_powered)
        name = "BlueTooth_Off";
    else if (low)
        name = "lowbatt";
    else if (std::any_of(m_devices.begin(), m_devices.end(), [](const Device &d) { return d.connected; }))
        name = "BlueTooth_Connected";
    else
        name = "BlueTooth_Idle";
    QImage img = menuIcon(res + name + ".pdf", 16, p.device()->devicePixelRatioF(), highlighted);
    if (!img.isNull()) {
        drawCentered(p, r, img);
        return;
    }
    p.setPen(highlighted ? Qt::white : Qt::black);
    Aqua::drawText(p, r, Qt::AlignCenter, "B");
}

static void bluetoothctl(const QStringList &args) { QProcess::startDetached("bluetoothctl", args); }

// IOBluetoothDevice -readRSSIForDevice: HCI_Read_RSSI (OGF 0x05, OCF 0x0005) on the connection's handle,
// through a raw HCI socket: the kernel lets anyone send that one (hci_sock's security filter); the
// handle from HCIGETCONNINFO. The BR/EDR value is relative to the golden receive range (0: within
// it), as the Mac shows it. INT_MIN when it can't be read.
static int readRSSI(const QString &address, int dev = 0)
{
    constexpr int AfBluetooth = 31, BtprotoHci = 1, SolHci = 0, HciFilter = 2;
    constexpr unsigned long HciGetConnInfo = 0x800448d5;   // _IOR('H', 213, int)
    const QStringList bytes = address.split(':');
    if (bytes.size() != 6)
        return INT_MIN;
    const int s = socket(AfBluetooth, SOCK_RAW | SOCK_CLOEXEC, BtprotoHci);
    if (s < 0)
        return INT_MIN;
    struct {
        sa_family_t family;
        unsigned short dev, channel;
    } sa{AfBluetooth, static_cast<unsigned short>(dev), 0};
    int rssi = INT_MIN;
    if (bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof sa) == 0) {
        // struct hci_conn_info_req: the address (little endian), the link type, then hci_conn_info
        unsigned char req[8 + 16] = {};
        for (int i = 0; i < 6; ++i)
            req[i] = static_cast<unsigned char>(bytes[5 - i].toUInt(nullptr, 16));
        int handle = -1;
        for (unsigned char type : {0x01, 0x80}) {   // ACL, LE
            req[6] = type;
            if (ioctl(s, HciGetConnInfo, req) == 0) {
                handle = req[8] | (req[9] << 8);
                break;
            }
        }
        struct {
            uint32_t typeMask;
            uint32_t eventMask[2];
            uint16_t opcode;
        } filter{1u << 4, {1u << 0x0e, 0}, 0};   // events: Command Complete
        const uint16_t opcode = (0x05 << 10) | 0x0005;
        const unsigned char cmd[] = {0x01, uint8_t(opcode & 0xff), uint8_t(opcode >> 8), 2, uint8_t(handle & 0xff), uint8_t(handle >> 8)};
        if (handle >= 0 && setsockopt(s, SolHci, HciFilter, &filter, sizeof filter) == 0 && write(s, cmd, sizeof cmd) == ssize_t(sizeof cmd)) {
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 500) {
                pollfd p{s, POLLIN, 0};
                if (poll(&p, 1, int(500 - clock.elapsed())) <= 0)
                    break;
                unsigned char ev[260];
                const ssize_t n = read(s, ev, sizeof ev);
                // 0x04 0x0e len ncmd opcode(2) status handle(2) rssi
                if (n >= 10 && ev[0] == 0x04 && ev[1] == 0x0e && (ev[4] | (ev[5] << 8)) == opcode) {
                    if (ev[6] == 0)
                        rssi = static_cast<int8_t>(ev[9]);
                    break;
                }
            }
        }
    }
    close(s);
    return rssi;
}

QAction *BluetoothExtra::deviceItem(const Device &d)
{
    // DeviceMenuCreator -createMenuItemForDevice: its name (bold when connected), its submenu: what it
    // does (-menuItemsForDevice:), then its preferences (-preferenceItemsForDevice:); none without
    auto *sub = new QMenu(d.name);   // a window of its own, deleted with the next rebuild
    m_submenus << sub;
    const QString addr = d.address;
    auto after = [this] { QTimer::singleShot(2500, this, &BluetoothExtra::poll); };
    if (d.has("1105"))
        sub->addAction(S::SendFile, [addr] {
            if (!startFirst({"bluedevil-sendfile"}, {"-k", "bluetooth://" + addr}))
                startFirst({"bluetooth-sendto", "blueman-sendto"}, {"--device=" + addr});
        });
    if (d.has("1106"))
        // -handleBrowseDevice: Bluetooth File Exchange's browser; here the desktop's OBEX FTP (KIO's obexftp:)
        sub->addAction(S::Browse, [addr] {
            QString dashed = addr;
            dashed.replace(':', '-');
            startFirst({"kioclient", "kioclient5"}, {"exec", "obexftp:/" + dashed + "/"});
        });
    if (d.stereo() || d.headset()) {
        if (d.connected)
            sub->addAction(S::DontUse, [addr, after] { bluetoothctl({"disconnect", addr}); after(); });
        else
            sub->addAction(d.stereo() ? S::UseStereo : S::UseMono, [addr, after] { bluetoothctl({"connect", addr}); after(); });
    }
    // -createConnectPANMenuItemForDevice: (0x92b5) a phone's network (NAP, 0x1116): Connect to Network,
    // Disconnect from Network when joined, disabled while it changes
    bool panActive = false;
    if (d.has("1116")) {
        const QString state = m_pan.value(addr.toUpper());
        panActive = state == "connected";
        const bool changing = state.startsWith("connecting") || state.startsWith("deactivating");
        QAction *pan = sub->addAction(panActive ? "Disconnect from Network" : "Connect to Network", [addr, panActive, after] {
            QProcess::startDetached("nmcli", {"device", panActive ? "disconnect" : "connect", addr});
            after();
        });
        pan->setEnabled(!changing);
    }
    // Connect: a HID device not connected; Disconnect: connected, not in use as audio or network
    if (d.hid() && !d.connected)
        sub->addAction(S::Connect, [addr, after] { bluetoothctl({"connect", addr}); after(); });
    if (d.connected && !panActive && !d.stereo() && !d.headset())
        sub->addAction(S::Disconnect, [addr, after] { bluetoothctl({"disconnect", addr}); after(); });
    if (m_option)
        sub->addAction(S::Remove, [addr, after] { bluetoothctl({"remove", addr}); after(); });
    QList<QAction *> prefs;
    auto pref = [&](const QString &title, const QString &pane) {
        auto *a = new QAction(title, sub);
        connect(a, &QAction::triggered, this, [pane] { openPreferencePane(pane); });
        prefs << a;
    };
    auto info = [&](const QString &title) {
        auto *a = new QAction(title, sub);
        a->setEnabled(false);
        prefs << a;
    };
    // with the Option key: its RSSI (-startRSSIReadForDevice:, the HCI command; else BlueZ's, from a
    // scan) and its address
    if (m_option && d.connected) {
        int rssi = readRSSI(d.address);
        if (rssi == INT_MIN && d.rssi)
            rssi = d.rssi;
        if (rssi != INT_MIN)
            info(QString("RSSI: %1 dBm").arg(rssi));
    }
    if (m_option)
        info("Address: " + d.address);
    if (d.connected && d.battery > 0)
        info(QString("Battery Level: %1%").arg(d.battery));
    if (d.icon == "input-mouse")
        pref(S::Mouse, "mouse");
    else if (d.icon == "input-tablet")
        pref(S::Trackpad, "trackpad");
    if (d.icon == "input-keyboard")
        pref(S::Keyboard, "keyboard");
    if (d.icon == "printer")
        pref(S::Printer, "printfax");
    if (d.stereo() || d.headset())
        pref(S::Sound, "sound");
    if (!prefs.isEmpty()) {
        if (!sub->actions().isEmpty())
            sub->addSeparator();
        sub->addActions(prefs);
    }
    if (sub->actions().isEmpty()) {
        m_submenus.removeOne(sub);
        delete sub;
        return nullptr;
    }
    QAction *a = sub->menuAction();
    if (d.connected)
        a->setFont(Assets::uiFont(14, true));   // +boldSystemFontOfSize:14
    return a;
}

QMenu *BluetoothExtra::menu()
{
    if (!m_menu) {
        m_menu = new QMenu;
        connect(m_menu, &QMenu::aboutToShow, this, [this] {
            m_option = QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier;
            rebuild();
            poll();
        });
    }
    return m_menu;
}

void BluetoothExtra::rebuild()
{
    // AppleBluetoothExtra -menu (0x2509) and -createOnOffMenuItems: (0x2e42)
    const QList<QAction *> old = m_menu->actions();
    const QList<QMenu *> oldSubmenus = m_submenus;
    m_submenus.clear();
    auto disabled = [this](const QString &title) { m_menu->addAction(title)->setEnabled(false); };
    if (m_option) {
        // with the Option key: the stack's version, the controller's name and address
        disabled("Version: " + m_version);
        if (!m_name.isEmpty())
            disabled("Name: " + m_name);
        if (!m_address.isEmpty())
            disabled("Address: " + m_address.toUpper());
        // Bluetooth Explorer, PacketLogger and the Diagnostics Utility are only there with the
        // developer tools in /Developer: here the report alone, between separators
        m_menu->addSeparator();
        m_menu->addAction("Create Diagnostics Report on Desktop…", this, &BluetoothExtra::report);
        m_menu->addSeparator();
    }
    disabled(QString(S::BT) + (m_powered ? S::On : S::Off));
    m_menu->addAction(m_powered ? S::TurnOff : S::TurnOn, [this] {
        bluetoothctl({"power", m_powered ? "off" : "on"});
        m_powered = !m_powered;
        emit changed();
        QTimer::singleShot(1500, this, &BluetoothExtra::poll);
    });
    if (m_powered) {
        // -createDiscoverableMenuItems:
        QAction *disc = m_menu->addAction(S::Discoverable, [this] {
            bluetoothctl({"discoverable", m_discoverable ? "off" : "on"});
            m_discoverable = !m_discoverable;
        });
        disc->setCheckable(true);
        disc->setChecked(m_discoverable);
        // -createBatteryWarningMenus: a device's low battery, its preferences
        QVector<Device> low;
        for (const Device &d : m_devices)
            if (d.connected && d.battery >= 0 && d.battery <= 10)
                low << d;
        if (!low.isEmpty()) {
            m_menu->addSeparator();
            for (const Device &d : low) {
                QString pane = d.icon == "input-mouse" ? "mouse" : d.icon == "input-tablet" ? "trackpad" : "keyboard";
                m_menu->addAction(QString("%1 has a low battery.").arg(d.name), [pane] { openPreferencePane(pane); });
            }
        }
        m_menu->addSeparator();
        // Bluetooth File Exchange's two: the desktop's Bluetooth file sender, if one is installed
        const bool sender = !QStandardPaths::findExecutable("bluedevil-sendfile").isEmpty() ||
                            !QStandardPaths::findExecutable("bluetooth-sendto").isEmpty() ||
                            !QStandardPaths::findExecutable("blueman-sendto").isEmpty();
        m_menu->addAction(S::SendFile, [] { startFirst({"bluedevil-sendfile", "bluetooth-sendto", "blueman-sendto"}); })->setEnabled(sender);
        // Browse Device…: Bluetooth File Exchange's device chooser; here the desktop's (KIO's bluetooth:)
        const bool browser = !QStandardPaths::findExecutable("kioclient").isEmpty() || !QStandardPaths::findExecutable("kioclient5").isEmpty();
        m_menu->addAction(S::Browse, [] { startFirst({"kioclient", "kioclient5"}, {"exec", "bluetooth:/"}); })->setEnabled(browser);
        m_menu->addSeparator();
        // -createDeviceMenuItems: the connected first, then by name
        QVector<Device> devices = m_devices;
        std::stable_sort(devices.begin(), devices.end(), [](const Device &a, const Device &b) {
            if (a.connected != b.connected)
                return a.connected;
            return a.name.localeAwareCompare(b.name) < 0;
        });
        QList<QAction *> items;
        for (const Device &d : devices)
            if (QAction *a = deviceItem(d))
                items << a;
        if (!items.isEmpty()) {
            disabled(S::Devices);
            m_menu->addActions(items);
            m_menu->addSeparator();
        }
        m_menu->addAction(S::SetUp, [] {
            if (!startFirst({"bluedevil-wizard", "blueman-assistant", "blueman-manager"}))
                openPreferencePane("bluetooth");
        });
    } else {
        m_menu->addSeparator();
    }
    m_menu->addAction(S::Prefs, [] { openPreferencePane("bluetooth"); });
    removeActions(m_menu, old);
    for (QMenu *sub : oldSubmenus)
        sub->deleteLater();
    Aqua::prepareMenu(m_menu);
}

// -handleOpenBluetoothReporter: IOBluetooth's BluetoothReporter, run as an administrator, leaves its
// report on the desktop; here the controller's and the devices' states, BlueZ's log and the kernel's,
// asked for with pkexec, in a file on the Desktop
void BluetoothExtra::report()
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString file = desktop + "/Bluetooth Report " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH.mm.ss") + ".txt";
    auto *proc = new QProcess(this);
    proc->setStandardOutputFile(file);
    connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
    const QString script =
        "echo '== bluetoothctl show'; bluetoothctl show; echo; echo '== btmgmt info'; btmgmt info 2>&1;"
        " for d in $(bluetoothctl devices | awk '/^Device/{print $2}'); do echo; echo \"== bluetoothctl info $d\"; bluetoothctl info \"$d\"; done;"
        " echo; echo '== rfkill'; rfkill list 2>&1; echo; echo '== bluetoothd'; journalctl -b -u bluetooth --no-pager -n 300 2>&1;"
        " echo; echo '== kernel'; dmesg 2>/dev/null | grep -i -E 'bluetooth|btusb|hci' | tail -n 200";
    proc->start("pkexec", {"sh", "-c", script});
}

SYSTEMUI_MENU_EXTRA(BluetoothExtra)
