// SPDX-License-Identifier: LGPL-2.1-or-later
// What the panes do on Linux: each nib's controls (by its outlets and actions, the names 10.6's code
// knows them by) connected to the shell's settings or to the system's tools. And the three panes whose
// nibs are of the older NSArchiver format (Displays, Language & Text, Speech), laid out by hand after
// 10.6's.
#include "colorpanel.h"
#include "foundation.h"
#include "nibpane.h"

#include <algorithm>
#include <cmath>
#include <clocale>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <ctime>
#include <langinfo.h>
#include <functional>
#include <sstream>

using namespace AK;

namespace {

std::vector<std::string> lines(const std::string &text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l))
        if (!l.empty())
            out.push_back(l);
    return out;
}

// every view of a kind under a view
template <typename T> void each(View *v, const std::function<void(T *)> &f)
{
    if (auto *t = dynamic_cast<T *>(v))
        f(t);
    for (View *c : v->subviews())
        each<T>(c, f);
}

// a popup filled as the pane's code fills it; its saved choice kept if it still fits
void fillPopUp(PopUpButton *p, const std::vector<std::string> &titles, int fallback)
{
    if (!p)
        return;
    // a popup the nib left empty (its placeholder) takes the default; a real choice is kept
    const bool wasEmpty = p->items.size() <= 1;
    p->items.clear();
    for (const std::string &t : titles)
        p->items.push_back(t == "-" ? MenuItem::line() : MenuItem{t});
    if ((wasEmpty && p->selected() <= 0) || p->selected() < 0 || size_t(p->selected()) >= p->items.size() ||
        p->items[size_t(p->selected())].separator)
        p->setSelected(fallback);
    p->setNeedsDisplay();
}

bool hasBattery()
{
    return !run("ls /sys/class/power_supply/ | grep -i ^BAT").empty();
}

template <typename T> T *first(const Nib &nib, const std::string &action)
{
    for (View *v : nib.withAction(action))
        if (auto *t = dynamic_cast<T *>(v))
            return t;
    return nullptr;
}

// ---- Dock: dock.conf, which the Dock watches (com.apple.dock's keys)

void dock(NibPane &p)
{
    auto settings = std::make_shared<Settings>("dock");
    // the nib's sliders go 0..1: the Dock's sizes, 16 to 128 points
    auto *size = p.nib.outlet<Slider>("_dockSizeSlider");
    auto *mag = p.nib.outlet<Slider>("_magSizeSlider");
    auto *magOn = p.nib.outlet<CheckBox>("_magSizeCheckbox");
    auto *hide = p.nib.outlet<CheckBox>("_autoHideCheckbox");
    auto *animate = p.nib.outlet<CheckBox>("_animateCheckbox");
    auto *effect = p.nib.outlet<PopUpButton>("_effectPopup");
    auto *toApp = p.nib.outlet<CheckBox>("_minimizeToApplicationCheckbox");
    auto toValue = [](double points, const Slider *s) { return s->minValue + (points - 16) / 112 * (s->maxValue - s->minValue); };
    auto toPoints = [](const Slider *s) { return int(std::lround(16 + s->value() * 112)); };
    if (size) {
        size->setDoubleValue(toValue(settings->integer("tilesize", 64), size));
        then(size, [=] { settings->set("tilesize", toPoints(size)); });
    }
    if (mag) {
        mag->setDoubleValue(toValue(settings->integer("largesize", 96), mag));
        then(mag, [=] { settings->set("largesize", toPoints(mag)); });
    }
    if (magOn) {
        magOn->setChecked(settings->boolean("magnification", false));
        then(magOn, [=] { settings->set("magnification", magOn->checked()); });
    }
    if (hide) {
        hide->setChecked(settings->boolean("autohide", false));
        then(hide, [=] { settings->set("autohide", hide->checked()); });
    }
    if (animate) {
        animate->setChecked(settings->boolean("launchanim", true));
        then(animate, [=] { settings->set("launchanim", animate->checked()); });
    }
    if (toApp) {
        toApp->setChecked(settings->boolean("minimize-to-application", false));
        then(toApp, [=] { settings->set("minimize-to-application", toApp->checked()); });
    }
    if (effect) {
        // Genie Effect, Scale Effect (mineffect: genie, scale)
        effect->setSelected(settings->string("mineffect", "genie") == "scale" ? 1 : 0);
        then(effect, [=] { settings->set("mineffect", std::string(effect->selected() == 1 ? "scale" : "genie")); });
    }
    const std::pair<const char *, const char *> sides[] = {{"_leftOrientation", "left"}, {"_bottomOrientation", "bottom"}, {"_rightOrientation", "right"}};
    std::vector<CheckBox *> radios;
    for (auto [outlet, value] : sides)
        if (auto *r = p.nib.outlet<CheckBox>(outlet))
            radios.push_back(r);
    for (size_t i = 0; i < radios.size() && i < 3; ++i) {
        radios[i]->setChecked(settings->string("orientation", "bottom") == sides[i].second);
        CheckBox *r = radios[i];
        const std::string value = sides[i].second;
        then(r, [=] {
            for (CheckBox *o : radios)
                o->setChecked(o == r);
            settings->set("orientation", value);
        });
    }
}

// ---- Sound: PulseAudio / PipeWire through pactl

void sound(NibPane &p)
{
    auto *volume = p.nib.outlet<Slider>("_systemVolume");
    auto *mute = p.nib.outlet<CheckBox>("_muteCheckbox");
    auto read = [=] {
        const std::string v = run("pactl get-sink-volume @DEFAULT_SINK@");
        const size_t pc = v.find('%');
        if (volume && pc != std::string::npos) {
            size_t s = pc;
            while (s > 0 && std::isdigit(static_cast<unsigned char>(v[s - 1])))
                --s;
            volume->setValue(std::atoi(v.substr(s, pc - s).c_str()) / 100.0);
        }
        if (mute)
            mute->setChecked(run("pactl get-sink-mute @DEFAULT_SINK@").find("yes") != std::string::npos);
    };
    read();
    if (volume)
        then(volume, [=] { spawn({"pactl", "set-sink-volume", "@DEFAULT_SINK@", std::to_string(int(std::lround(volume->value() * 100))) + "%"}); });
    if (mute)
        then(mute, [=] { spawn({"pactl", "set-sink-mute", "@DEFAULT_SINK@", mute->checked() ? "1" : "0"}); });
    // the alert sounds, by 10.6's names
    if (auto *alerts = p.nib.outlet<TableView>("_alertList")) {
        for (const char *n : {"Basso", "Blow", "Bottle", "Frog", "Funk", "Glass", "Hero", "Morse", "Ping", "Pop", "Purr", "Sosumi", "Submarine", "Tink"})
            alerts->rows.push_back({n});
        alerts->selected = 4;
        alerts->reload();
    }
    // the output and input devices in the Output and Input tabs' lists
    std::vector<TableView *> tables;
    for (const auto &c : p.nib.controls())
        if (auto *t = dynamic_cast<TableView *>(c.view))
            if (t != p.nib.outlet<TableView>("_alertList"))
                tables.push_back(t);
    auto devices = [](const char *kind) {
        std::vector<std::vector<std::string>> rows;
        std::string name;
        for (const std::string &l : lines(run(std::string("pactl list ") + kind))) {
            const size_t d = l.find("Description: ");
            if (d != std::string::npos)
                rows.push_back({l.substr(d + 13), std::string(kind) == "sinks" ? "Built-in" : "Microphone"});
        }
        return rows;
    };
    if (tables.size() >= 1) {
        tables[0]->rows = devices("sinks");
        tables[0]->selected = 0;
        tables[0]->reload();
    }
    if (tables.size() >= 2) {
        tables[1]->rows = devices("sources");
        tables[1]->selected = 0;
        tables[1]->reload();
    }
    p.every(2, read);   // the volume set elsewhere (the menu bar's slider)
}

// ---- Keyboard and Mouse: X11's settings (xset); on Wayland the compositor's own

void keyboard(NibPane &p)
{
    auto *rate = first<Slider>(p.nib, "newKeyRepeat:");
    auto *delay = first<Slider>(p.nib, "newInitialKeyRepeat:");
    auto apply = [=] {
        if (!onX11() || !rate || !delay)
            return;
        // Key Repeat: slow (2/s) to fast (30/s); Delay Until Repeat: long (1.2 s) to short (0.2 s)
        const int r = int(std::lround(2 + rate->value() * 28)), d = int(std::lround(1200 - delay->value() * 1000));
        spawn({"xset", "r", "rate", std::to_string(d), std::to_string(r)});
    };
    if (rate)
        then(rate, apply);
    if (delay)
        then(delay, apply);
}

void mouse(NibPane &p)
{
    if (auto *speed = first<Slider>(p.nib, "newMouseSpeed:"))
        then(speed, [=] {
            if (onX11())   // the acceleration, 1 to 6
                spawn({"xset", "m", std::to_string(int(std::lround(1 + speed->value() * 5))), "4"});
        });
    if (auto *click = first<Slider>(p.nib, "newMouseClick:")) {
        // the double-click speed AquaKit's programs read
        auto settings = std::make_shared<Settings>("globaldomain");
        then(click, [=] { settings->set("com.apple.mouse.doubleClickThreshold", std::to_string(2.0 - click->value() * 1.85)); });
    }
}

// ---- Sharing: the computer's name, the services that run

void sharing(NibPane &p)
{
    std::string host = run("hostnamectl --static");
    if (host.empty())
        host = run("hostname");
    if (auto *name = first<TextField>(p.nib, "actionComputerName:"))
        name->setText(host);
    each<Label>(&p, [&](Label *l) {
        std::string t = l->text();
        const size_t at = t.find("computer.local");
        if (at != std::string::npos)
            l->setText(t.replace(at, 14, host + ".local"));
    });
    // the services: 10.6's list, each on when its Linux service runs
    if (auto *table = p.nib.outlet<TableView>("_table")) {
        if (!table->columns.empty())
            table->columns[0].checkBox = true;   // its code makes the On column's cells check boxes
        const std::pair<const char *, const char *> services[] = {
            {"DVD or CD Sharing", ""}, {"Screen Sharing", "vncserver@:1 x11vnc"}, {"File Sharing", "smb nmb"}, {"Printer Sharing", "cups-browsed"},
            {"Scanner Sharing", "saned.socket"}, {"Web Sharing", "httpd nginx"}, {"Remote Login", "sshd"}, {"Remote Management", ""},
            {"Remote Apple Events", ""}, {"Xgrid Sharing", ""}, {"Internet Sharing", ""}, {"Bluetooth Sharing", "bluetooth"}};
        table->rows.clear();
        for (auto [label, units] : services) {
            bool on = false;
            std::istringstream in(units);
            std::string u;
            while (in >> u)
                on = on || run("systemctl is-active " + u) == "active";
            table->rows.push_back({on ? "1" : "0", label});
        }
        table->selected = 0;
        table->reload();
    }
}

// ---- Date & Time: the time zone; the dates in the nib's fields are today's

void dateTime(NibPane &p)
{
    // the time zone where its tab shows one
    const std::string zone = run("timedatectl show -p Timezone --value");
    each<TextField>(&p, [&](TextField *f) {
        if (!zone.empty() && f->text.find('/') != std::string::npos && f->text.find(':') == std::string::npos && f->text.size() > 10)
            f->setText(zone);
    });
    // the fields with the time follow the clock
    p.every(1, [&p] {
        char buf[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(buf, sizeof buf, "%-I:%M:%S %p", std::localtime(&now));
        each<TextField>(&p, [&](TextField *f) {
            if (!f->editable && f->text.find(':') != std::string::npos && f->text.find('M') != std::string::npos)
                f->setText(buf);
        });
    });
}

// ---- Software Update: Check Now opens the shell's Software Update (Apple's catalog, as on 10.6);
// without it, pacman's (checkupdates)

std::string softwareUpdateApp()
{
    std::string dir = dirName(executablePath());
    for (int i = 0; i < 5; ++i) {
        const std::string exe = dir + "/Software Update.app/Contents/MacOS/Software Update";
        if (isFile(exe))
            return exe;
        dir = dirName(dir);
    }
    return {};
}

void softwareUpdate(NibPane &p)
{
    if (const std::string app = softwareUpdateApp(); !app.empty()) {
        if (auto *button = first<Button>(p.nib, "checkNow:"))
            then(button, [app] { spawn({app}); });
        return;
    }
    auto *status = p.nib.outlet<Label>("lastStatusTextField");
    auto *progress = p.nib.outlet<Label>("progressTextField");
    auto settings = std::make_shared<Settings>("panes/" + p.info.id);
    if (status)
        status->setText(settings->string("lastCheck", "Software Update has not checked for updates yet."));
    if (auto *button = first<Button>(p.nib, "checkNow:"))
        then(button, [=] {
            if (progress)
                progress->setText("Checking for new software…");
            // after the window has shown it
            App::shared().later([=] {
                const std::vector<std::string> updates = lines(run("checkupdates"));
                char when[64];
                const std::time_t now = std::time(nullptr);
                std::strftime(when, sizeof when, "%A, %B %-d, %Y %-I:%M %p", std::localtime(&now));
                const std::string text = updates.empty() ? std::string("Software Update ran successfully. Your software is up to date.  ") + when
                                                         : std::to_string(updates.size()) + " updates are available.  " + when;
                if (status)
                    status->setText(text);
                settings->set("lastCheck", text);
                if (progress)
                    progress->setText("");
            });
        });
}

// ---- Network: the interfaces in the services' list

void network(NibPane &p)
{
    if (auto *table = p.nib.outlet<TableView>("o_serviceTable")) {
        table->rows.clear();
        for (const std::string &l : lines(run("ip -brief address"))) {
            std::istringstream in(l);
            std::string name, state, addr;
            in >> name >> state >> addr;
            if (name == "lo")
                continue;
            const std::string kind = name.rfind("wl", 0) == 0 ? "AirPort" : name.rfind("en", 0) == 0 || name.rfind("eth", 0) == 0 ? "Ethernet" : name;
            table->rows.push_back({kind + "  (" + name + ")", state == "UP" ? "Connected" : "Not Connected"});
        }
        table->selected = table->rows.empty() ? -1 : 0;
        table->reload();
    }
}

// ---- Print & Fax: CUPS's printers

void printFax(NibPane &p)
{
    if (auto *table = p.nib.outlet<TableView>("_printerList")) {
        table->rows.clear();
        for (const std::string &l : lines(run("lpstat -p")))
            if (l.rfind("printer ", 0) == 0) {
                std::istringstream in(l.substr(8));
                std::string name;
                in >> name;
                table->rows.push_back({name, l.find("idle") != std::string::npos ? "Idle" : "Printing"});
            }
        table->reload();
    }
}

// ---- Appearance: the lists its code fills

// AppearanceShared's settings: the global domain's (appearance.conf here), which CoreUI, AppKit and
// screencapture read
struct NamedColor {
    std::string name;
    double r, g, b;
};

bool parseRGB(const std::string &text, double &r, double &g, double &b)
{
    // _colorFromRGBString: three floats and nothing after them
    std::istringstream in(text);
    std::string rest;
    return bool(in >> r >> g >> b) && !(in >> rest);
}

std::string rgbString(double r, double g, double b)
{
    // _rgbStringFromColor: "%f %f %f", calibrated RGB
    char s[64];
    std::snprintf(s, sizeof s, "%f %f %f", r, g, b);
    return s;
}

std::vector<NamedColor> highlightColors()
{
    // NamedColor +namedColorsFromFile:bundle: highlightColors.plist's pairs (name, "r g b") in the
    // file's order (not sorted), each name through the pane's Localizable.strings; the last one is
    // "Other..." (extract.py's JSON beside the plist: its strings in order)
    const std::string pane = "/System/Library/PreferencePanes/Appearance.prefPane";
    std::vector<NamedColor> out;
    std::ifstream in(Assets::path(pane + "/Contents/Resources/highlightColors.plist.json"));
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> strings;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] != '"')
            continue;
        std::string s;
        for (++i; i < json.size() && json[i] != '"'; ++i)
            s += json[i] == '\\' && i + 1 < json.size() ? json[++i] : json[i];
        strings.push_back(s);
    }
    for (size_t i = 0; i + 1 < strings.size(); i += 2) {
        NamedColor c{Assets::tr(pane, "Localizable", strings[i], strings[i]), 0, 0, 0};
        if (parseRGB(strings[i + 1], c.r, c.g, c.b))
            out.push_back(c);
    }
    if (out.empty())   // without the DVD's file: 10.6's list
        out = {{"Graphite", 0.7804, 0.8157, 0.8588}, {"Silver", 0.7765, 0.7765, 0.7765}, {"Blue", 0.7098, 0.8353, 1.0},
               {"Gold", 0.9843, 0.9294, 0.4510}, {"Red", 1.0, 0.6941, 0.5490}, {"Orange", 1.0, 0.8235, 0.5059},
               {"Green", 0.7647, 0.9765, 0.5686}, {"Purple", 0.9137, 0.7216, 1.0}, {"Other...", 0.5, 0.5, 0.5}};
    return out;
}

Image swatch(double r, double g, double b)
{
    // -addNamedColor:toPopUp:: a 24 x 12 image, the colour filling it, a frame 2 wide in black at 0.1
    // (swatchFillPath, swatchFramePath: the same rectangle)
    Image img(24, 12);
    cairo_t *cr = cairo_create(img.surface());
    cairo_rectangle(cr, 0, 0, 24, 12);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.1);
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return img;
}

void appearance(NibPane &p)
{
    auto settings = std::make_shared<Settings>("appearance");
    // Appearance: the nib's items, tags 1 (Blue, aqua_blue.tiff) and 6 (Graphite, graphite.tiff);
    // -aquaColorChanged: → -setAquaInterface: the tag (anything but 1 or 6 is 1) as
    // AppleAquaColorVariant, then AppleAquaColorVariantChanged (CoreUI's _CUIGetTheme reads it again)
    if (auto *aqua = p.nib.outlet<PopUpButton>("aquaColorPopUp")) {
        const std::string pane = "/System/Library/PreferencePanes/Appearance.prefPane/Contents/Resources/";
        for (MenuItem &it : aqua->items)
            if (it.tag == 1 || it.tag == 6)
                it.image = Image::load(Assets::path(pane + (it.tag == 1 ? "aqua_blue.tiff" : "graphite.tiff")));
        const int variant = settings->integer("AppleAquaColorVariant", 1);
        for (size_t i = 0; i < aqua->items.size(); ++i)
            if (aqua->items[i].tag == variant)
                aqua->setSelected(int(i));
        then(aqua, [settings, aqua] {
            const int s = aqua->selected();
            int tag = s >= 0 && size_t(s) < aqua->items.size() ? aqua->items[size_t(s)].tag : 1;
            if (tag != 1 && tag != 6)
                tag = 1;
            settings->set("AppleAquaColorVariant", tag);
        });
    }
    // Highlight Color: the named colours with their swatches; the one equal to AppleHighlightColor
    // ("0.709800 0.835300 1.000000" unset) selected, else the last ("Other...") given that colour.
    // -newHighlightColor:: the item's colour as AppleHighlightColor; the last opens the colour panel
    // (-applyColorClicked: sets it), starting from AppleOtherHighlightColor
    if (auto *hl = p.nib.outlet<PopUpButton>("highlightColorPopUp")) {
        auto colors = std::make_shared<std::vector<NamedColor>>(highlightColors());
        double r = 0.7098, g = 0.8353, b = 1.0;
        parseRGB(settings->string("AppleHighlightColor", "0.709800 0.835300 1.000000"), r, g, b);
        double orr, og, ob;
        if (parseRGB(settings->string("AppleOtherHighlightColor"), orr, og, ob))
            colors->back() = {colors->back().name, orr, og, ob};
        else
            settings->set("AppleOtherHighlightColor", rgbString(colors->back().r, colors->back().g, colors->back().b));
        hl->items.clear();
        for (const NamedColor &c : *colors) {
            MenuItem it{c.name};
            it.image = swatch(c.r, c.g, c.b);
            hl->items.push_back(it);
        }
        int chosen = int(colors->size()) - 1;
        for (size_t i = 0; i < colors->size(); ++i) {
            const NamedColor &c = (*colors)[i];
            if (std::fabs(c.r - r) < 1e-4 && std::fabs(c.g - g) < 1e-4 && std::fabs(c.b - b) < 1e-4) {
                chosen = int(i);
                break;
            }
        }
        if (chosen == int(colors->size()) - 1) {
            colors->back() = {colors->back().name, r, g, b};   // -updateItem:withColor:
            hl->items.back().image = swatch(r, g, b);
        }
        hl->setSelected(chosen);
        hl->setNeedsDisplay();
        then(hl, [settings, colors, hl] {
            const int s = hl->selected();
            if (s < 0 || size_t(s) >= colors->size())
                return;
            const NamedColor c = (*colors)[size_t(s)];
            settings->set("AppleHighlightColor", rgbString(c.r, c.g, c.b));
            if (size_t(s) + 1 != colors->size())
                return;
            ColorPanel::setColor(Color{c.r, c.g, c.b, 1});
            ColorPanel::setTarget([settings, colors, hl](const Color &picked) {
                // -applyColorClicked:: the Other item's colour, and the highlight
                colors->back() = {colors->back().name, picked.r, picked.g, picked.b};
                hl->items.back().image = swatch(picked.r, picked.g, picked.b);
                hl->setNeedsDisplay();
                settings->set("AppleOtherHighlightColor", rgbString(picked.r, picked.g, picked.b));
                settings->set("AppleHighlightColor", rgbString(picked.r, picked.g, picked.b));
            });
            ColorPanel::show();
        });
    }
    for (const char *o : {"recentAppsPopUpButton", "recentDocsPopUpButton", "recentServersPopUpButton"}) {
        auto *b = p.nib.outlet<PopUpButton>(o);
        fillPopUp(b, {"None", "5", "10", "15", "20", "30", "50"}, 2);
        if (b && !p.remembered(b))
            b->setSelected(2);   // 10.6's: ten of each
    }
}

// ---- Exposé & Spaces: the keys and mouse buttons of its shortcuts

void expose(NibPane &p)
{
    // the shortcuts' popups are the ones the nib leaves empty: keys on the left, mouse buttons on the right
    std::vector<PopUpButton *> empty;
    each<PopUpButton>(&p, [&](PopUpButton *b) {
        if (b->items.size() == 1 && b->items[0].title == "–")
            empty.push_back(b);
    });
    std::sort(empty.begin(), empty.end(), [](PopUpButton *a, PopUpButton *b) { return a->toWindow({0, 0}).y < b->toWindow({0, 0}).y; });
    std::vector<std::string> keys{"\u2013"}, buttons{"\u2013", "Secondary Mouse Button", "Middle Mouse Button", "Mouse Button 4", "Mouse Button 5"};
    for (int f = 1; f <= 19; ++f)
        keys.push_back("F" + std::to_string(f));
    // the keys' column on the left: All windows F9, Application windows F10, Show Desktop F11, Dashboard F12
    double middle = 0;
    for (PopUpButton *b : empty)
        middle += b->toWindow({0, 0}).x / double(empty.size());
    int row = 0;
    for (PopUpButton *b : empty) {
        if (b->toWindow({0, 0}).x < middle)
            fillPopUp(b, keys, 9 + std::min(row++, 3));
        else
            fillPopUp(b, buttons, 0);
    }
}


// ---- the menu extras the panes show or hide (com.apple.systemuiserver's menuExtras; dontAutoLoad
// keeps Autoload.plist from bringing one back), which SystemUIServer follows

const char *const ClockExtra = "/System/Library/CoreServices/Menu Extras/Clock.menu";
const char *const BatteryExtra = "/System/Library/CoreServices/Menu Extras/Battery.menu";

bool menuExtraShown(const std::string &bundle)
{
    const std::vector<std::string> list = Settings("systemuiserver").list("menuExtras");
    const std::vector<std::string> dont = Settings("systemuiserver").list("dontAutoLoad");
    if (std::find(list.begin(), list.end(), bundle) != list.end())
        return true;
    return list.empty() && std::find(dont.begin(), dont.end(), bundle) == dont.end();   // Autoload's
}

void showMenuExtra(const std::string &bundle, bool shown)
{
    Settings prefs("systemuiserver");
    std::vector<std::string> list = prefs.list("menuExtras"), dont = prefs.list("dontAutoLoad");
    list.erase(std::remove(list.begin(), list.end(), bundle), list.end());
    dont.erase(std::remove(dont.begin(), dont.end(), bundle), dont.end());
    if (shown) {
        // the clock at the right end, the battery left of it (Autoload.plist's places)
        auto clock = std::find(list.begin(), list.end(), std::string(ClockExtra));
        list.insert(bundle == ClockExtra ? list.end() : clock, bundle);
    } else {
        dont.push_back(bundle);
    }
    prefs.set("dontAutoLoad", dont);
    prefs.set("menuExtras", list);
}

// ---- Date & Time's Clock tab (ClockPref.nib's bindings): com.apple.menuextra.clock's DateFormat as
// the checkboxes make it, IsAnalog, FlashDateSeparators

void clockPart(Nib &nib)
{
    Settings prefs("menuextra.clock");
    // the clock's own default (SystemUIServer's clock.cpp): the day and the time in the locale's clock
    const char *previous = std::setlocale(LC_TIME, nullptr);
    const std::string keep = previous ? previous : "C";
    std::setlocale(LC_TIME, "");
    const std::string tfmt = nl_langinfo(T_FMT);
    std::setlocale(LC_TIME, keep.c_str());
    const bool twelve = tfmt.find("%p") != std::string::npos || tfmt.find("%r") != std::string::npos || tfmt.find("%I") != std::string::npos;
    const std::string format = prefs.string("DateFormat", twelve ? "EEE h:mm a" : "EEE H:mm");
    auto box = [&](const char *key) { return nib.bound1<CheckBox>(key); };
    CheckBox *visible = box("isClockMenuExtraVisible"), *date = box("showDate"), *seconds = box("showSeconds"),
             *ampm = box("showAMPM"), *weekday = box("showDayOfTheWeek"), *flash = box("flashDateSeparators"),
             *h24 = box("use24Hours");
    auto *style = nib.bound1<RadioGroup>("isAnalog", "selectedIndex");
    if (!visible)
        return;
    visible->setChecked(menuExtraShown(ClockExtra));
    if (date) date->setChecked(format.find('d') != std::string::npos);
    if (seconds) seconds->setChecked(format.find('s') != std::string::npos);
    if (ampm) ampm->setChecked(format.find('a') != std::string::npos);
    if (weekday) weekday->setChecked(format.find("EEE") != std::string::npos);
    if (h24) h24->setChecked(format.find('H') != std::string::npos);
    if (flash) flash->setChecked(prefs.boolean("FlashDateSeparators", false));
    if (style) style->select(prefs.boolean("IsAnalog", false) ? 1 : 0);
    auto update = [=] {
        const bool on = visible->checked(), analog = style && style->selected() == 1;
        for (CheckBox *c : {date, seconds, ampm, weekday, flash, h24})
            if (c)
                c->setEnabled(on && !analog);
        if (ampm)
            ampm->setEnabled(on && !analog && !(h24 && h24->checked()));
        if (style)
            style->setEnabled(on);
        // the format as ClockPref builds it: the day, the date, the time, AM/PM
        std::string f;
        if (weekday && weekday->checked())
            f += "EEE ";
        if (date && date->checked())
            f += "MMM d ";
        const bool twentyFour = h24 && h24->checked();
        f += twentyFour ? "H:mm" : "h:mm";
        if (seconds && seconds->checked())
            f += ":ss";
        if (!twentyFour && ampm && ampm->checked())
            f += " a";
        Settings p("menuextra.clock");
        p.set("DateFormat", f);
        p.set("IsAnalog", analog);
        p.set("FlashDateSeparators", flash && flash->checked());
    };
    for (CheckBox *c : {date, seconds, ampm, weekday, flash, h24})
        if (c)
            c->onToggle = [update](CheckBox *) { update(); };
    visible->onToggle = [update](CheckBox *c) {
        showMenuExtra(ClockExtra, c->checked());
        update();
    };
    if (style)
        style->onSelect = [update](int) { update(); };
    // the enabled states, without writing the preferences yet
    const bool on = visible->checked(), analog = style && style->selected() == 1;
    for (CheckBox *c : {date, seconds, ampm, weekday, flash, h24})
        if (c)
            c->setEnabled(on && !analog);
    if (ampm && h24)
        ampm->setEnabled(on && !analog && !h24->checked());
}

// ---- Energy Saver: what this computer has (a battery? two graphics cards: no)

void energy(NibPane &p)
{
    // Show battery status in the menu bar (-changedMenuExtraCheckbox:): Battery.menu in the bar
    for (View *v : p.nib.withAction("changedMenuExtraCheckbox:"))
        if (auto *c = dynamic_cast<CheckBox *>(v)) {
            c->setChecked(menuExtraShown(BatteryExtra));
            c->onToggle = [](CheckBox *b) { showMenuExtra(BatteryExtra, b->checked()); };
        }
    for (const char *o : {"_gfxmuxMatrix", "_graphicsLabel", "_batteryHealthAlertIcon"})
        if (View *v = p.nib.outlet(o))
            v->setHidden(true);
    const bool battery = hasBattery();
    const std::string charge = battery ? run("cat /sys/class/power_supply/BAT*/capacity | head -1") : std::string();
    for (const char *o : {"_batteryStatusMessageForPowerTab", "_batteryStatusMessageForBatteryTab", "_upsStatusMessageForUPSTab"})
        if (auto *l = p.nib.outlet<Label>(o)) {
            l->setHidden(!battery || std::string(o).find("Battery") == std::string::npos);
            l->setText("Current battery charge: " + charge + "%");
        }
    // a computer without a battery: the power adapter's settings alone, without the tabs
    if (auto *tabs = p.nib.outlet<TabView>("_detailsTabView")) {
        // a UPS's tab only with one
        if (run("ls /sys/class/power_supply/ | grep -i ups").empty() && tabs->count() > 2)
            tabs->setTabLabel(2, "");
        // the graphics switching's warning sign, above the tabs
        each<ImageView>(&p, [&](ImageView *i) {
            if (i->toWindow({0, 0}).y < tabs->toWindow({0, 0}).y)
                i->setHidden(true);
        });
        if (!battery) {
            tabs->noTabs = true;
            if (tabs->count() > 1)
                tabs->select(1);   // Power Adapter
            tabs->setFrame(tabs->frame());
        }
    }
}

// ---- Spotlight: the categories of its results

void spotlight(NibPane &p)
{
    TableView *table = nullptr;
    each<TableView>(&p, [&](TableView *t) {
        if (!table)
            table = t;
    });
    if (!table)
        return;
    static const char *const categories[] = {"Applications", "System Preferences", "Documents", "Folders", "Mail Messages", "Contacts",
                                              "Events & To Do Items", "Images", "PDF Documents", "Webpages", "Music", "Movies",
                                              "Fonts", "Presentations", "Spreadsheets", "Developer"};
    table->rows.clear();
    size_t nameColumn = 0;
    for (size_t c = 0; c < table->columns.size(); ++c)
        if (table->columns[c].width > table->columns[nameColumn].width)
            nameColumn = c;
    int n = 1;
    for (const char *c : categories) {
        std::vector<std::string> row(std::max<size_t>(1, table->columns.size()));
        for (size_t col = 0; col < table->columns.size(); ++col)
            if (table->columns[col].checkBox)
                row[col] = "1";
        if (!table->columns.empty() && nameColumn > 0 && !table->columns[0].checkBox)
            row[0] = std::to_string(n);
        row[nameColumn] = c;
        table->rows.push_back(row);
        ++n;
    }
    table->reload();
}

// ---- Bluetooth: bluetoothctl

void bluetooth(NibPane &p)
{
    const std::string show = run("bluetoothctl show");
    const bool there = show.find("Controller") != std::string::npos;
    const bool powered = show.find("Powered: yes") != std::string::npos, visible = show.find("Discoverable: yes") != std::string::npos;
    std::string name;
    for (const std::string &l : lines(show))
        if (l.find("Name: ") != std::string::npos)
            name = l.substr(l.find("Name: ") + 6);
    // the sentence its code writes at the top
    each<Label>(&p, [&](Label *l) {
        if (l->text().empty() && l->wrap && l->frame().w > 300)
            l->setText(!there ? "Bluetooth: No Bluetooth controller was found."
                              : std::string("Bluetooth: ") + (powered ? "On" : "Off") + (name.empty() ? "" : "\nName: " + name));
    });
    each<CheckBox>(&p, [&](CheckBox *c) {
        if (c->title == "On") {
            c->setChecked(powered);
            c->setEnabled(there);
            then(c, [c] { spawn({"bluetoothctl", "power", c->checked() ? "on" : "off"}); });
        } else if (c->title == "Discoverable") {
            c->setChecked(visible);
            c->setEnabled(there);
            then(c, [c] { spawn({"bluetoothctl", "discoverable", c->checked() ? "on" : "off"}); });
        }
    });
}

// ---- Accounts: the people who log in on this computer

struct AccountsList : View {
    struct Row {
        std::string title, detail;
        Image icon;
        bool header = false;
    };
    std::vector<Row> rows;
    int selected = 1;
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(255));
        double y = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row &r = rows[i];
            if (r.header) {
                TextStyle t;
                t.font = Font::boldSystem(11);
                t.color = Color::rgb(112, 126, 140);
                drawText(cr, r.title, Rect{8, y, frame().w - 10, 20}, t);
                y += 20;
                continue;
            }
            const bool sel = int(i) == selected;
            if (sel)
                fill(cr, Rect{0, y, frame().w, 40}, windowIsKey() ? Color::rgb(56, 117, 215) : Color::gray(212));
            if (!r.icon.isNull())
                r.icon.draw(cr, Rect{8, y + 4, 32, 32});
            TextStyle t;
            t.color = sel && windowIsKey() ? Color::gray(255) : Color::gray(0);
            drawText(cr, r.title, Rect{46, y + 4, frame().w - 50, 17}, t);
            t.font = Font::system(11);
            t.color = sel && windowIsKey() ? Color::gray(255) : Color::gray(110);
            drawText(cr, r.detail, Rect{46, y + 20, frame().w - 50, 15}, t);
            y += 40;
        }
    }
    void mouseDown(Event &e) override
    {
        double y = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const double h = rows[i].header ? 20 : 40;
            if (!rows[i].header && e.location.y >= y && e.location.y < y + h) {
                selected = int(i);
                setNeedsDisplay();
            }
            y += h;
        }
    }
};

void accounts(NibPane &p)
{
    const std::string user = run("id -un");
    std::string full = run("getent passwd " + user + " | cut -d: -f5 | cut -d, -f1");
    if (full.empty())
        full = user;
    const bool admin = run("id -Gn").find("wheel") != std::string::npos;
    if (auto *f = first<TextField>(p.nib, "fullNameDidEndEditing:"))
        f->setText(full);
    for (View *v : p.nib.withAction("selectUserAction:")) {
        auto *list = v->add<AccountsList>();
        list->setFrame(v->bounds());
        const std::string types = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/";
        list->rows = {{"My Account", "", {}, true},
                      {full, admin ? "Admin" : "Standard", Image::load(Assets::path(types + "UserIcon.icns"), {32, 32})},
                      {"Other Accounts", "", {}, true},
                      {"Guest Account", "Sharing only", Image::load(Assets::path(types + "GuestUserIcon.icns"), {32, 32})},
                      {"Login Options", "", p.nib.image("LoginOptions")}};
    }
}

// ---- Startup Disk: the systems this computer can start

void startupDisk(NibPane &p)
{
    View *content = p.nib.outlet("contentView");
    if (!content)
        return;
    // the system that runs: its disk, by its label or device
    std::string root = run("findmnt -no SOURCE /");
    root = root.substr(0, root.find('['));   // a btrfs subvolume: its device
    std::string label = run("lsblk -no LABEL " + root);
    if (label.empty())
        label = run("lsblk -no PARTLABEL " + root);
    std::string system = run(". /etc/os-release; echo \"$NAME\"");
    auto *icon = content->add<ImageView>();
    icon->image = p.nib.image("StartupDiskPref");
    if (icon->image.isNull())
        icon->image = p.info.icon;
    icon->setFrame({20, 14, 64, 64});
    auto *name = content->add<Label>((system.empty() ? std::string("Linux") : system) + "\non " + (label.empty() ? fileName(root) : label),
                                     Font::system(11));
    name->wrap = true;
    name->style.align = Align::Center;
    name->setFrame({4, 80, 96, 30});
}

// ---- Network: the selected interface's state, in the box its code fills

// Network's Advanced… panel: ANPMainTabs.nib's window, its tabs the views of ANPAdvanced<Tab>View.nib
void networkAdvanced(NibPane &p, const std::string &service)
{
    Nib *main = p.loadPart("ANPMainTabs");
    if (!main)
        return;
    const int window = main->outletObject("o_advancedTabWindow");
    Size size;
    View *content = main->instantiateWindow(window, &size);
    if (!content)
        return;
    if (auto *tabs = main->outlet<TabView>("o_advancedViewTabView")) {
        tabs->keepPageFrames = false;
        if (tabs->count() > 0)
            tabs->setTabLabel(0, "");   // the code's empty item
        const bool airport = service.find("AirPort") != std::string::npos;
        std::vector<std::pair<std::string, std::string>> pages = {{"TCPIP", "TCP/IP"}, {"DNS", "DNS"}, {"WINS", "WINS"},
                                                                  {"8021x", "802.1X"}, {"Proxy", "Proxies"}, {"Ethernet", "Ethernet"}};
        const std::string dev = service.substr(service.find('(') + 1, service.find(')') - service.find('(') - 1);
        for (const auto &[nib, label] : pages) {
            Nib *part = p.loadPart("ANPAdvanced" + nib + "View");
            View *page = part ? part->instantiate("o_view") : nullptr;
            if (!page)
                continue;
            tabs->addTab(airport && label == "Ethernet" ? "AirPort" : label, page);
            if (nib != "TCPIP")
                continue;
            // the interface's addresses, as its configuration shows them
            auto set = [part](const char *outlet, const std::string &text) {
                if (auto *f = part->outlet<TextField>(outlet))
                    f->setText(text);
                else if (auto *l = part->outlet<Label>(outlet))
                    l->setText(text);
            };
            const std::string v4 = run("ip -4 -o address show " + dev + " | awk '{print $4}' | head -1");
            if (!v4.empty()) {
                const int bits = std::atoi(v4.substr(v4.find('/') + 1).c_str());
                const uint32_t mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
                set("o_ipAddressTextField", v4.substr(0, v4.find('/')));
                set("o_subnetMaskTextField", std::to_string(mask >> 24) + '.' + std::to_string(mask >> 16 & 255) + '.' +
                                                 std::to_string(mask >> 8 & 255) + '.' + std::to_string(mask & 255));
            }
            set("o_routerTextField", run("ip -4 route show default dev " + dev + " | awk '{print $3}' | head -1"));
            const std::string v6 = run("ip -6 -o address show " + dev + " scope global | awk '{print $4}' | head -1");
            set("o_ipv6AddressTextField", v6.substr(0, v6.find('/')));
            set("o_ipv6PrefixLengthTextField", v6.find('/') == std::string::npos ? std::string() : v6.substr(v6.find('/') + 1));
            set("o_ipv6RouterAddressTextField", run("ip -6 route show default dev " + dev + " | awk '{print $3}' | head -1"));
        }
        tabs->select(1);
        tabs->setFrame(tabs->frame());
    }
    for (const char *o : {"o_serviceNameField", "o_serviceNameTextField"})
        if (auto *l = main->outlet<Label>(o))
            l->setText(service.substr(0, service.find("  ")));
    p.presentSheet(content, size);
}

void networkDetails(NibPane &p)
{
    auto *table = p.nib.outlet<TableView>("o_serviceTable");
    // the service's settings: in the tab view its code fills
    View *box = p.nib.outlet("o_simpleSetupTabView");
    if (!table || !box)
        return;
    auto *status = box->add<Label>("", Font::boldSystem(13));
    status->setFrame({120, 20, 300, 18});
    auto *detail = box->add<Label>("", Font::system(11));
    detail->wrap = true;
    detail->setFrame({120, 42, 300, 60});
    auto *statusTitle = box->add<Label>("Status:");
    statusTitle->style.align = Align::Right;
    statusTitle->setFrame({10, 20, 104, 18});
    auto *addressTitle = box->add<Label>("IP Address:");
    addressTitle->style.align = Align::Right;
    addressTitle->setFrame({10, 110, 104, 18});
    auto *address = box->add<Label>("");
    address->setFrame({120, 110, 300, 18});
    auto update = [=](int row) {
        if (row < 0 || size_t(row) >= table->rows.size())
            return;
        const std::string service = table->rows[size_t(row)][0];
        const std::string dev = service.substr(service.find('(') + 1, service.find(')') - service.find('(') - 1);
        const bool up = table->rows[size_t(row)].size() > 1 && table->rows[size_t(row)][1] == "Connected";
        status->setText(up ? "Connected" : "Not Connected");
        const std::string ip = run("ip -4 -brief address show " + dev + " | awk '{print $3}'");
        detail->setText(up ? service.substr(0, service.find("  ")) + " is currently active and has the IP address " + ip + "."
                           : "The cable for " + service.substr(0, service.find("  ")) + " is not plugged in.");
        address->setText(ip);
    };
    then(table, [=] { update(table->selected); });
    update(table->selected);
    if (auto *advanced = first<Button>(p.nib, "a_showAdvancedSettings:"))
        then(advanced, [&p, table] {
            const std::string service = table->selected >= 0 ? table->rows[size_t(table->selected)][0] : std::string("Ethernet");
            App::shared().later([&p, service] { networkAdvanced(p, service); });
        });
    fillPopUp(p.nib.outlet<PopUpButton>("o_locationPopUp"), {"Automatic", "-", "Edit Locations\u2026"}, 0);
}

// ---- Date & Time: its three tabs are panes of their own (Date & Time, Time Zone, Clock)

void dateTimeParts(NibPane &p)
{
    auto *tabs = p.nib.outlet<TabView>("_tabView");
    if (!tabs)
        return;
    tabs->keepPageFrames = false;
    for (const char *part : {"DateTime", "TimeZone", "ClockPref"}) {
        const std::string bundle = p.info.bundle + "/Contents/Resources/" + part + ".prefPane";
        const auto info = Assets::plistStrings(bundle + "/Contents/Info.plist");
        const auto strings = Assets::plistStrings(bundle + "/Contents/Resources/English.lproj/InfoPlist.strings");
        auto nibName = info.find("NSMainNibFile");
        if (nibName == info.end())
            continue;
        auto nib = std::make_unique<Nib>();
        const std::string resources = Assets::path(bundle + "/Contents/Resources");
        if (!nib->load(resources + "/English.lproj/" + nibName->second + ".nib/keyedobjects.nib", resources))
            continue;
        View *view = nib->instantiate("_window");
        if (!view)
            continue;
        std::string label = part;
        for (const char *k : {"NSPrefPaneIconLabel", "CFBundleName"})
            if (strings.count(k) && !strings.at(k).empty()) {
                label = strings.at(k);
                break;
            }
        std::replace(label.begin(), label.end(), '\n', ' ');
        tabs->addTab(label, view);
        if (std::string(part) == "ClockPref")
            clockPart(*nib);
        p.parts.push_back(std::move(nib));
    }
    tabs->select(0);
    tabs->setFrame(tabs->frame());
}

} // namespace

void wirePane(NibPane &p)
{
    const std::string &id = p.info.id;
    if (id == "com.apple.preference.dock")
        dock(p);
    else if (id == "com.apple.preference.sound")
        sound(p);
    else if (id == "com.apple.preference.keyboard")
        keyboard(p);
    else if (id == "com.apple.preference.mouse")
        mouse(p);
    else if (id == "com.apple.preferences.sharing")
        sharing(p);
    else if (id == "com.apple.preference.datetime") {
        dateTimeParts(p);
        dateTime(p);
    }
    else if (id == "com.apple.preferences.softwareupdate")
        softwareUpdate(p);
    else if (id == "com.apple.preference.network")
        network(p);
    else if (id == "com.apple.preference.printfax")
        printFax(p);
    else if (id == "com.apple.preference.general")
        appearance(p);
    else if (id == "com.apple.preference.expose")
        expose(p);
    else if (id == "com.apple.preference.energysaver")
        energy(p);
    else if (id == "com.apple.preference.spotlight")
        spotlight(p);
    else if (id == "com.apple.preferences.Bluetooth")
        bluetooth(p);
    else if (id == "com.apple.preferences.users")
        accounts(p);
    else if (id == "com.apple.preference.startupdisk")
        startupDisk(p);
    if (id == "com.apple.preference.network")
        networkDetails(p);
}

// ---- the panes whose main nib is of the older format: put together from their parts' nibs, or by hand

namespace {

Label *label(View *in, const std::string &text, const Rect &r, Align align = Align::Left, Font font = Font::system())
{
    auto *l = in->add<Label>(text, font);
    l->style.align = align;
    l->setFrame(r);
    return l;
}

// A pane whose tabs are other bundles' nibs (Displays' monitor panels, Language & Text's own panes):
// its code (not here) puts them together; their nibs kept with the pane
struct TabbedPane : View {
    std::vector<std::unique_ptr<Nib>> nibs;
    TabView *tabs = nullptr;
    Nib *add(const std::string &label, const std::string &bundle, const std::string &nibName, const std::string &outlet)
    {
        auto nib = std::make_unique<Nib>();
        const std::string resources = Assets::path(bundle + "/Contents/Resources");
        if (!nib->load(resources + "/English.lproj/" + nibName + ".nib/keyedobjects.nib", resources))
            return nullptr;
        View *page = nib->instantiate(outlet);
        if (!page)
            return nullptr;
        tabs->addTab(label, page);
        nibs.push_back(std::move(nib));
        return nibs.back().get();
    }
    // the window's size for the tallest tab
    void finish(Size *size)
    {
        double h = 0;
        for (const auto &n : nibs)
            h = std::max(h, n->size().h);
        *size = {668, std::max(300.0, h + 100)};
        setFrame({0, 0, size->w, size->h});
        tabs->keepPageFrames = false;
        tabs->setFrame({13, 10, 642, size->h - 50});
        tabs->select(0);
    }
};

TabbedPane *tabbed()
{
    auto *pane = new TabbedPane;
    pane->clipsToBounds = false;
    pane->tabs = pane->View::add<TabView>();
    return pane;
}

// Displays: the Display panel (resolutions, refresh rate) and the Color panel (profiles); X11's xrandr
View *displays(Size *size)
{
    auto *pane = tabbed();
    const std::string panels = "/System/Library/MonitorPanels/";
    Nib *display = pane->add("Display", panels + "Display.monitorPanel", "Display", "_view");
    Nib *color = pane->add("Color", panels + "Profile.monitorPanel", "ProfilePanel", "_view");
    pane->finish(size);
    if (display) {
        // the first output and its modes; the one in use selected
        std::string output;
        std::vector<std::string> modes, rates;
        int current = -1;
        for (const std::string &l : lines(onX11() ? run("xrandr --query") : std::string())) {
            if (l.find(" connected") != std::string::npos && output.empty()) {
                output = l.substr(0, l.find(' '));
                continue;
            }
            if (output.empty() || l.size() < 4 || l[0] != ' ')
                continue;
            std::istringstream in(l);
            std::string mode, rate;
            in >> mode;
            if (mode.find('x') == std::string::npos)
                continue;
            if (l.find('*') != std::string::npos) {
                current = int(modes.size());
                while (in >> rate)
                    rates.push_back(std::to_string(int(std::lround(std::atof(rate.c_str())))) + " Hertz");
            }
            modes.push_back(mode.substr(0, mode.find('x')) + " × " + mode.substr(mode.find('x') + 1));
        }
        if (modes.empty()) {
            const Rect s = App::shared().screenFrame();
            modes.push_back(std::to_string(int(s.w)) + " × " + std::to_string(int(s.h)));
            current = 0;
        }
        if (auto *table = display->outlet<TableView>("mResolutionList")) {
            table->rows.clear();
            for (const std::string &m : modes)
                table->rows.push_back({m});
            table->selected = current;
            table->reload();
            table->onSelect = [table, output](int i) {
                if (output.empty() || i < 0)
                    return;
                std::string m = table->rows[size_t(i)][0];
                const size_t x = m.find(" × ");
                if (x != std::string::npos)
                    m = m.substr(0, x) + "x" + m.substr(x + 4);
                spawn({"xrandr", "--output", output, "--mode", m});
            };
        }
        if (rates.empty())
            rates.push_back("60 Hertz");
        fillPopUp(display->outlet<PopUpButton>("mRefreshList"), rates, 0);
        if (auto *detect = display->outlet<Button>("mDetectButton"))
            detect->action = [] {
                if (onX11())
                    spawn({"xrandr", "--auto"});
            };
    }
    if (color)
        if (auto *table = color->outlet<TableView>("_profileTable")) {
            // the colour profiles installed (ICC), else the usual ones
            table->rows.clear();
            for (const std::string &dir : {home() + "/.local/share/icc", std::string("/usr/share/color/icc")})
                for (const std::string &f : files(dir))
                    table->rows.push_back({baseName(f)});
            if (table->rows.empty())
                table->rows = {{"Color LCD"}, {"sRGB IEC61966-2.1"}, {"Generic RGB Profile"}};
            table->selected = 0;
            table->reload();
        }
    return pane;
}

// Language & Text: its four panes (Language, Text, Formats, Input Sources), the languages installed
View *language(Size *size)
{
    auto *pane = tabbed();
    const std::string intl = "/System/Library/PreferencePanes/Localization.prefPane/Contents/Resources/";
    Nib *languages = pane->add("Language", intl + "IntlLanguages.prefPane", "IntlLanguages", "_window");
    pane->add("Text", intl + "IntlTexts.prefPane", "IntlTexts", "_window");
    pane->add("Formats", intl + "IntlFormats.prefPane", "IntlFormats", "_window");
    pane->add("Input Sources", intl + "IntlKeyboard.prefPane", "IntlKeyboard", "_window");
    pane->finish(size);
    TableView *table = languages ? languages->outlet<TableView>("mLanguageTableView") : nullptr;
    if (!table)
        return pane;
    static const std::pair<const char *, const char *> names[] = {
        {"en", "English"}, {"fr", "Français"}, {"de", "Deutsch"}, {"ja", "日本語"}, {"es", "Español"}, {"it", "Italiano"},
        {"nl", "Nederlands"}, {"sv", "Svenska"}, {"nb", "Norsk Bokmål"}, {"da", "Dansk"}, {"fi", "Suomi"}, {"pt", "Português"},
        {"zh", "中文"}, {"ko", "한국어"}, {"ru", "Русский"}, {"pl", "Polski"}};
    // the session's language first, then the others installed, then the rest of 10.6's
    std::vector<std::string> codes;
    if (const char *lang = std::getenv("LANG"))
        codes.push_back(std::string(lang).substr(0, 2));
    for (const std::string &l : lines(run("locale -a")))
        if (l.size() >= 2 && l != "C" && l != "POSIX" && l.rfind("C.", 0) != 0)
            codes.push_back(l.substr(0, 2));
    for (auto [code, name] : names)
        codes.push_back(code);
    std::vector<std::string> seen;
    table->rows.clear();
    for (const std::string &c : codes)
        if (std::find(seen.begin(), seen.end(), c) == seen.end())
            for (auto [code, name] : names)
                if (c == code) {
                    table->rows.push_back({name});
                    seen.push_back(c);
                }
    table->selected = 0;
    table->reload();
    fillPopUp(languages->outlet<PopUpButton>("mSortPopUp"), {table->rows.empty() ? std::string("English") : table->rows[0][0]}, 0);
    return pane;
}

} // namespace

View *handPane(const PaneInfo &info, Size *size)
{
    if (info.id == "com.apple.preference.displays")
        return displays(size);
    if (info.id == "com.apple.Localization")
        return language(size);
    return nullptr;
}
