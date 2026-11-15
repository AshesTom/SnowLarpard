// SPDX-License-Identifier: LGPL-2.1-or-later
#include "desktoppane.h"

#include "foundation.h"
#include "slideshow.h"

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <map>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace AK;

namespace {

// Measured on 10.6's window (points, from the content's top left under the toolbar): the tab pane,
// its tabs on its edge
const Rect PaneFrame{20, 34, 628, 469};
constexpr double SaverHeight = 454;   // ScreenSaverPref.nib's view: 643 x 454
const Size ThumbSize{73, 49};         // measured on 10.6

// A nib frame: Cocoa's {{x, y}, {w, h}} from the bottom of a view `height` high
Rect nib(double x, double y, double w, double h, double height) { return {x, height - y - h, w, h}; }

enum SourceTag { Removable = 1, Apple = 2 };

const std::string PicturesBundle =
    "/System/Library/PreferencePanes/DesktopScreenEffectsPref.prefPane/Contents/Resources/DesktopPictures.prefPane";

// A program's answer on its standard output, without blocking the window: the folder chooser
void runDialog(std::vector<std::string> argv, std::function<void(std::string)> done)
{
    int out[2];
    if (pipe(out) != 0)
        return done({});
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    const int err = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    if (err != 0) {
        close(out[0]);
        return done({});
    }
    fcntl(out[0], F_SETFL, O_NONBLOCK);
    auto text = std::make_shared<std::string>();
    auto timer = std::make_shared<int>(0);
    *timer = App::shared().addTimer(0.1, true, [=] {
        char buf[512];
        ssize_t n;
        while ((n = read(out[0], buf, sizeof buf)) > 0)
            text->append(buf, size_t(n));
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) != pid)
            return;
        while ((n = read(out[0], buf, sizeof buf)) > 0)
            text->append(buf, size_t(n));
        close(out[0]);
        App::shared().removeTimer(*timer);
        while (!text->empty() && (text->back() == '\n' || text->back() == '\r'))
            text->pop_back();
        done(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? *text : std::string());
    });
}

void spawnSelf(const std::vector<std::string> &extra)
{
    std::vector<std::string> argv{executablePath()};
    argv.insert(argv.end(), extra.begin(), extra.end());
    std::vector<char *> args;
    for (std::string &a : argv)
        args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid;
    posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ);
}

// The box the sources and the pictures share: white, a 165 grey edge, a line between them
struct Frame : View {
    double split = 0;
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(165));
        fill(cr, bounds().inset(1, 1), Color::gray(255));
        if (split > 0)
            fill(cr, Rect{split, 1, 1, frame().h - 2}, Color::gray(165));
    }
};

// The folder icon (CoreTypes) and the colour panel's wheel (HIToolbox), 16 points
Image folderIcon()
{
    static Image icon = [] {
        Image i = Image::load(Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns"), {16, 16});
        return i.isNull() || i.width() == 16 ? i : i.scaled(16, 16);
    }();
    return icon;
}

Image colorsIcon()
{
    static Image icon = [] {
        Image i = Image::load(Assets::path("/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/Resources/ColorPanel.png"));
        return i.isNull() ? folderIcon() : i.scaled(16, 16);
    }();
    return icon;
}

} // namespace

DesktopPane::DesktopPane()
{
    m_settings = Desktop::load();
    for (View *&page : m_pages) {
        page = add<View>();
        page->setFrame({0, 0, Width, Height});
        page->clipsToBounds = false;
    }
    buildDesktop(m_pages[0]);
    buildScreenSaver(m_pages[1]);

    // the tabs: one segmented control on the pane's top edge, 13 points of space either side of a title
    const std::vector<std::string> titles{"Desktop", "Screen Saver"};
    std::vector<double> widths;
    double total = 0;
    for (const std::string &t : titles) {
        widths.push_back(std::round(textWidth(t, Font::system()) + 26));
        total += widths.back();
    }
    m_tabs = add<SegmentedControl>(CoreUI::SegmentStyle::Tabs, titles, widths);
    m_tabs->setFrame({std::floor((Width - total) / 2), PaneFrame.y - 12, total, 24});
    m_tabs->onSelect = [this](int i) {
        showTab(i);
        Settings("systempreferences").set("desktopLastTab", i);   // com.apple.dtandsspref's last tab
    };
    showTab(Settings("systempreferences").integer("desktopLastTab", 0));
}

DesktopPane::~DesktopPane()
{
    if (m_thumbnails)
        App::shared().removeTimer(m_thumbnails);
}

void DesktopPane::draw(cairo_t *cr)
{
    fill(cr, bounds(), Color::gray(237));
    CoreUI::tabPane(cr, PaneFrame, controlState(*this));
}

void DesktopPane::showTab(int index)
{
    index = std::clamp(index, 0, 1);
    m_tabs->setSelected(index);
    m_pages[0]->setHidden(index != 0);
    m_pages[1]->setHidden(index != 1);
}

// ---- Desktop

void DesktopPane::buildDesktop(View *tab)
{
    // the picture now in its well, its name; how it is placed and the colour around it, for your own
    // pictures only (DSKAppleRootSource::disableCustomPlacement: Apple's are made for the screen)
    m_preview = tab->add<ImageWell>();
    m_preview->setFrame({51, 54, 128, 86});
    m_name = tab->add<Label>();
    m_name->setFrame({201, 70, 300, 17});
    m_placement = tab->add<PopUpButton>();
    m_placement->setFrame({201, 98, 176, 26});
    for (const char *p : {"Fill Screen", "Fit to Screen", "Stretch to Fill Screen", "Center", "Tile"})
        m_placement->items.push_back({p});
    m_placement->setSelected(m_settings.placement - 1);
    m_placement->onSelect = [this](int i) {
        m_settings.placement = i + 1;
        apply();
    };
    m_color = tab->add<ColorWell>();
    m_color->setFrame({381, 100, 44, 23});
    m_color->action = [this] { chooseColor(); };

    // the sources and their pictures side by side in the box (603 x 253): 165 points of sources
    auto *box = tab->add<Frame>();
    box->setFrame({34, 149, 603, 253});
    box->split = 166;
    m_sourcesScroll = box->add<ScrollView>();
    m_sourcesScroll->setFrame({1, 1, 165, 251});
    m_sourcesScroll->clipsToBounds = true;
    m_sources = m_sourcesScroll->document()->add<OutlineView>();
    m_sources->rowHeight = 20;   // measured
    m_sources->onSelect = [this](int row) { showSource(row); };
    m_picturesScroll = box->add<ScrollView>();
    m_picturesScroll->setFrame({167, 1, 603 - 168, 251});
    m_picturesScroll->lineStep = 55;
    m_pictures = m_picturesScroll->document()->add<ImageGrid>();
    m_pictures->thumbSize = ThumbSize;
    m_pictures->cellSize = {82.6, 55};   // measured: five a row, the first picture 10 points in, 7 down
    m_pictures->origin = {5.5, 4};
    m_pictures->onSelect = [this](int i) {
        m_settings.picture = m_pictures->cells[size_t(i)].data;
        m_settings.changeFolder = m_folder;
        apply();
    };

    // + and -: folders of your own
    auto *add = tab->add<Button>("+", Button::SmallSquare);
    add->setFrame({34, 409, 23, 22});
    add->action = [this] { addFolder(); };
    m_remove = tab->add<Button>("−", Button::SmallSquare);
    m_remove->setFrame({56, 409, 23, 22});
    m_remove->action = [this] {
        const int row = m_sources->selected();
        if (row < 0 || !(m_sources->rows[size_t(row)].tag & Removable))
            return;
        const std::string path = m_sources->rows[size_t(row)].data;
        auto &f = m_settings.folders;
        f.erase(std::remove(f.begin(), f.end(), path), f.end());
        Desktop::save(m_settings);
        fillSources();
    };

    // the picture changing by itself; the menu bar's translucency
    m_change = tab->add<CheckBox>("Change picture:");
    m_change->setFrame({199, 411, 130, 18});
    m_interval = tab->add<PopUpButton>();
    m_interval->setFrame({330, 407, 208, 26});
    const auto &intervals = Desktop::changeIntervals();
    for (size_t i = 1; i < intervals.size(); ++i) {
        m_interval->items.push_back({intervals[i].label});
        if (i == 2)
            m_interval->items.push_back(MenuItem::line());   // between waking from sleep and every 5 seconds
    }
    auto indexOf = [](int change) { return change <= 2 ? change - 1 : change; };   // the separator's place
    auto changeOf = [](int index) { return index <= 1 ? index + 1 : index; };
    m_change->setChecked(m_settings.change > 0);
    m_interval->setSelected(indexOf(m_settings.change > 0 ? m_settings.change : 7));
    m_interval->setEnabled(m_change->checked());
    m_random = tab->add<CheckBox>("Random order");
    m_random->setFrame({199, 431, 130, 18});
    m_random->setChecked(m_settings.random);
    m_random->setEnabled(m_change->checked());
    auto changed = [this, changeOf] {
        m_settings.change = m_change->checked() ? changeOf(m_interval->selected()) : 0;
        m_settings.random = m_random->checked();
        if (m_settings.changeFolder.empty())
            m_settings.changeFolder = m_folder;
        m_interval->setEnabled(m_change->checked());
        m_random->setEnabled(m_change->checked());
        apply();
    };
    m_change->action = changed;
    m_interval->action = changed;
    m_random->action = changed;

    m_translucent = tab->add<CheckBox>("Translucent menu bar");
    m_translucent->setFrame({199, 451, 170, 18});
    m_translucent->setChecked(Settings("menubar").boolean("AppleEnableMenuBarTransparency", true));
    m_translucent->action = [this] {
        // HIToolbox's preference; SystemUIServer follows the file
        Settings("menubar").set("AppleEnableMenuBarTransparency", m_translucent->checked());
    };

    auto *help = tab->add<Button>("", Button::Help);
    help->setFrame({612, 460, 25, 25});

    fillSources();
    updatePreview();
}

void DesktopPane::fillSources()
{
    // DSKAppleRootSource ("Apple": its eight folders) and DSKUserFoldersRootSource ("Folders": Pictures and yours)
    auto t = [](const std::string &key, const std::string &fallback) { return Assets::tr(PicturesBundle, "Localizable", key, fallback); };
    static const std::map<std::string, std::string> keys = {
        {"Desktop Pictures", "DesktopPicturesBuiltin"}, {"Nature", "NatureBuiltin"}, {"Plants", "PlantsBuiltin"},
        {"Art", "ArtBuiltin"}, {"Black & White", "BlackAndWhiteBuiltin"}, {"Abstract", "AbstractBuiltin"},
        {"Patterns", "PatternsBuiltin"}, {"Solid Colors", "SolidColorsBuiltin"}};
    std::vector<OutlineRow> rows;
    const std::string current = dirName(Desktop::picturePath(m_settings));
    int select = -1;
    auto add = [&](int parent, const std::string &name, const std::string &path, bool removable, const Image &icon) {
        OutlineRow r;
        r.title = name;
        r.icon = icon;
        r.level = 1;
        r.parent = parent;
        r.data = path;
        r.tag = (removable ? Removable : 0) | (parent == 0 ? Apple : 0);
        rows.push_back(r);
        if (path == current && select < 0)
            select = int(rows.size()) - 1;
    };
    OutlineRow apple;
    apple.title = t("DSKAppleRootSource", "Apple");
    apple.group = true;
    apple.selectable = false;
    rows.push_back(apple);
    for (const Desktop::Source &s : Desktop::appleSources()) {
        auto k = keys.find(s.name);
        add(0, k != keys.end() ? t(k->second, s.name) : s.name, s.path, false, s.name == "Solid Colors" ? colorsIcon() : folderIcon());
    }
    OutlineRow folders;
    folders.title = t("DSKUserFoldersRootSource", "Folders");
    folders.group = true;
    folders.selectable = false;
    const int foldersRow = int(rows.size());
    rows.push_back(folders);
    add(foldersRow, "Pictures", picturesDir(), false, folderIcon());
    for (const std::string &f : m_settings.folders)
        add(foldersRow, fileName(f), f, true, folderIcon());
    rows[size_t(foldersRow)].expanded = select > foldersRow;
    m_sources->rows = rows;
    m_sources->reload();
    m_sources->select(-1);
    m_sources->select(select >= 0 ? select : 2, true);
}

void DesktopPane::showSource(int row)
{
    const std::string path = row >= 0 ? m_sources->rows[size_t(row)].data : std::string();
    const int tag = row >= 0 ? m_sources->rows[size_t(row)].tag : 0;
    m_remove->setEnabled(tag & Removable);
    // Apple's pictures are made for the screen: no placement for them (disableCustomPlacement)
    const bool custom = !(tag & Apple);
    m_placement->setHidden(!custom);
    m_color->setHidden(!custom);
    m_pictures->cells.clear();
    m_pictures->selected = -1;
    m_pending.clear();
    m_folder = path;
    const std::string current = Desktop::picturePath(m_settings);
    for (const std::string &p : Desktop::pictures(path)) {
        m_pictures->cells.push_back({Image(), p, baseName(p)});
        m_pending.push_back(p);
    }
    m_picturesScroll->setDocumentHeight(m_pictures->contentHeight());
    m_picturesScroll->scrollTo(0);
    m_pictures->setFrame(m_picturesScroll->document()->bounds());
    if (!m_thumbnails && !m_pending.empty())
        m_thumbnails = App::shared().addTimer(0, true, [this] { loadThumbnails(); });
}

void DesktopPane::loadThumbnails()
{
    // a few at a time, the window staying responsive
    for (int n = 0; n < 3 && !m_pending.empty(); ++n) {
        const std::string path = m_pending.front();
        m_pending.erase(m_pending.begin());
        Image thumb = Desktop::thumbnail(path, ThumbSize);
        for (auto &c : m_pictures->cells)
            if (c.data == path)
                c.image = thumb;
    }
    m_pictures->setNeedsDisplay();
    if (m_pending.empty()) {
        App::shared().removeTimer(m_thumbnails);
        m_thumbnails = 0;
    }
}

void DesktopPane::apply()
{
    Desktop::save(m_settings);   // the Finder's desktop, the Dock and the menu bar follow the file
    updatePreview();
}

void DesktopPane::updatePreview()
{
    // the screen, small, inside the well
    m_preview->image = Desktop::render(m_settings, {232, 146}).scaled(116, 73);
    m_preview->setNeedsDisplay();
    m_name->setText(baseName(Desktop::picturePath(m_settings)));
    // the colour well: for a picture that doesn't cover the screen
    m_color->setEnabled(m_settings.placement == Desktop::FitToScreen || m_settings.placement == Desktop::Center ||
                        m_settings.placement == Desktop::Tile);
    m_color->color = m_settings.color;
    m_color->setNeedsDisplay();
}

void DesktopPane::chooseColor()
{
    // the colours of 10.6's Solid Colors collection, each its picture's own
    std::vector<MenuItem> items;
    std::vector<Color> colors;
    for (const Desktop::Source &s : Desktop::appleSources()) {
        if (s.name != "Solid Colors")
            continue;
        for (const std::string &p : Desktop::pictures(s.path)) {
            Image px = Image::load(p, {1, 1});
            if (px.isNull())
                continue;
            px = px.scaled(1, 1);
            cairo_surface_flush(px.surface());
            const uint32_t v = *reinterpret_cast<uint32_t *>(cairo_image_surface_get_data(px.surface()));
            colors.push_back(Color::rgb(int(v >> 16 & 0xFF), int(v >> 8 & 0xFF), int(v & 0xFF)));
            items.push_back({baseName(p)});
            items.back().checked = colors.back().name() == m_settings.color.name();
        }
    }
    if (items.empty())
        return;
    Menu::popUp(window(), m_color->toWindow({0, m_color->frame().h}), items, 120, Font::system(), [this, colors](int i) {
        if (i < 0)
            return;
        m_settings.color = colors[size_t(i)];
        apply();
    });
}

void DesktopPane::addFolder()
{
    auto added = [this](std::string dir) {
        if (dir.empty() || !isDir(dir) || std::find(m_settings.folders.begin(), m_settings.folders.end(), dir) != m_settings.folders.end())
            return;
        m_settings.folders.push_back(dir);
        Desktop::save(m_settings);
        fillSources();
    };
    // the desktop's own folder chooser (the Mac's open panel is for later)
    runDialog({"zenity", "--file-selection", "--directory", "--title=Choose a folder", "--filename=" + picturesDir() + "/"},
              [added](std::string dir) {
                  if (!dir.empty())
                      return added(dir);
                  runDialog({"kdialog", "--getexistingdirectory", picturesDir()}, added);
              });
}

// ---- Screen Saver

void DesktopPane::buildScreenSaver(View *outer)
{
    auto *tab = outer->add<View>();   // ScreenSaverPref.nib's view, inside the tab pane
    tab->setFrame({PaneFrame.x + 2, PaneFrame.y + 12, 624, SaverHeight});
    tab->clipsToBounds = false;
    const double H = SaverHeight;
    auto *title = tab->add<Label>("Screen Savers", Font::system(11));
    title->setFrame(nib(8, 435, 120, 14, H));
    auto *previewTitle = tab->add<Label>("Preview", Font::system(11));
    previewTitle->setFrame(nib(262, 435, 60, 14, H));

    // the savers: slideshows of the Desktop Pictures collections and of your Pictures folder, under
    // PICTURES as 10.6 lists its picture savers
    auto *box = tab->add<Frame>();
    box->setFrame(nib(11, 186, 238, 246, H));
    auto *scroll = box->add<ScrollView>();
    scroll->setFrame({1, 1, 236, 244});
    m_savers = scroll->document()->add<OutlineView>();
    m_savers->rowHeight = 18;
    OutlineRow heading;
    heading.title = "PICTURES";
    heading.header = true;
    heading.selectable = false;
    m_savers->rows.push_back(heading);
    const std::string chosen = ScreenSaver::folder();
    int select = -1;
    auto addSaver = [&](const std::string &name, const std::string &path) {
        OutlineRow r;
        r.title = name;
        r.data = path;
        r.level = 1;
        r.parent = 0;
        m_savers->rows.push_back(r);
        if (path == chosen)
            select = int(m_savers->rows.size()) - 1;
    };
    for (const Desktop::Source &s : Desktop::appleSources())
        if (s.name != "Solid Colors")
            addSaver(s.name, s.path);
    addSaver("Pictures Folder", picturesDir());
    m_savers->indent = 4;
    m_savers->reload();

    auto *frame = tab->add<Frame>();
    frame->setFrame(nib(262, 186, 347, 246, H));
    m_saverPreview = frame->add<Slideshow>();
    m_saverPreview->setFrame({1, 1, 345, 244});
    m_savers->onSelect = [this](int row) {
        const std::string path = m_savers->rows[size_t(row)].data;
        ScreenSaver::setFolder(path);
        m_saverPreview->setFolder(path);
    };
    m_savers->select(select >= 0 ? select : 2, true);

    auto *options = tab->add<Button>("Options…");
    options->setFrame(nib(408, 148, 103, 32, H));
    options->setEnabled(false);
    auto *test = tab->add<Button>("Test");
    test->setFrame(nib(511, 148, 103, 32, H));
    test->action = [] { spawnSelf({"--screensaver"}); };
    for (auto [label, y, w] : {std::tuple{"Use random screen saver", 136.0, 190.0}, std::tuple{"Show with clock", 113.0, 123.0},
                               std::tuple{"Main screen only", 90.0, 188.0}}) {
        auto *c = tab->add<CheckBox>(label);
        c->setFrame(nib(9, y, w, 18, H));
        c->setEnabled(false);
    }

    // "Start screen saver:" 3, 5, 15, 30 minutes, 1 and 2 hours, never: the slider's stops
    auto *startLabel = tab->add<Label>("Start screen saver:");
    startLabel->setFrame(nib(262, 115, 140, 17, H));
    static const int stops[] = {3 * 60, 5 * 60, 15 * 60, 30 * 60, 3600, 7200, 0};
    static const char *const names[] = {"3", "5", "15", "30", "1hr", "2hr", "Never"};
    static const double labelX[] = {265, 320, 372, 426, 480, 533, 582};
    auto *slider = tab->add<Slider>();
    slider->setFrame(nib(261, 87, 347, 25, H));
    slider->ticks = 7;
    slider->onlyTicks = true;
    auto *when = tab->add<Label>("", Font::system(11));
    when->setFrame(nib(458, 115, 148, 13, H));
    when->style.align = Align::Right;
    for (int i = 0; i < 7; ++i) {
        auto *l = tab->add<Label>(names[i], Font::system(10));
        l->setFrame(nib(labelX[i], 73, 40, 13, H));
    }
    int at = 6;
    for (int i = 0; i < 7; ++i)
        if (stops[i] == ScreenSaver::idleSeconds())
            at = i;
    slider->setValue(at / 6.0);
    auto describe = [when](int i) {
        const int s = stops[i];
        when->setText(!s ? "Never" : s < 3600 ? std::to_string(s / 60) + " minutes" : std::to_string(s / 3600) + (s > 3600 ? " hours" : " hour"));
    };
    describe(at);
    slider->onChange = [describe](double v) {
        const int i = int(std::lround(v * 6));
        ScreenSaver::setIdleSeconds(stops[i]);   // the Finder watches the idle time
        describe(i);
    };

    auto *corners = tab->add<Button>("Hot Corners…");
    corners->setFrame(nib(5, 13, 131, 32, H));
    corners->setEnabled(false);
    auto *help = tab->add<Button>("", Button::Help);
    help->setFrame(nib(588, 16, 25, 25, H));
}
