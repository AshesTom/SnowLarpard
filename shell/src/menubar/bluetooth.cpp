// SPDX-License-Identifier: LGPL-2.1-or-later
#include "bluetooth.h"

#include "aqua.h"
#include "assets.h"

#include <QGuiApplication>
#include <QMenu>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

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
    m_devices.clear();
    for (int i = 2; i < sections.size(); ++i) {
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

QAction *BluetoothExtra::deviceItem(const Device &d)
{
    // DeviceMenuCreator -createMenuItemForDevice: its name (bold when connected), its submenu: what it
    // does (-menuItemsForDevice:), then its preferences (-preferenceItemsForDevice:); none without
    auto *sub = new QMenu(d.name);   // a window of its own, deleted with the next rebuild
    m_submenus << sub;
    const QString addr = d.address;
    auto after = [this] { QTimer::singleShot(2500, this, &BluetoothExtra::poll); };
    if (d.has("1105"))
        sub->addAction(S::SendFile, [addr] { startFirst({"bluetooth-sendto", "blueman-sendto"}, {"--device=" + addr}); });
    if (d.stereo() || d.headset()) {
        if (d.connected)
            sub->addAction(S::DontUse, [addr, after] { bluetoothctl({"disconnect", addr}); after(); });
        else
            sub->addAction(d.stereo() ? S::UseStereo : S::UseMono, [addr, after] { bluetoothctl({"connect", addr}); after(); });
    } else if (d.connected) {
        sub->addAction(S::Disconnect, [addr, after] { bluetoothctl({"disconnect", addr}); after(); });
    } else if (d.hid()) {
        sub->addAction(S::Connect, [addr, after] { bluetoothctl({"connect", addr}); after(); });
    }
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
        const bool sender = !QStandardPaths::findExecutable("bluetooth-sendto").isEmpty() ||
                            !QStandardPaths::findExecutable("blueman-sendto").isEmpty();
        m_menu->addAction(S::SendFile, [] { startFirst({"bluetooth-sendto", "blueman-sendto"}); })->setEnabled(sender);
        m_menu->addAction(S::Browse)->setEnabled(false);
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
            if (!startFirst({"blueman-assistant", "blueman-manager"}))
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
