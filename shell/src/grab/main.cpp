// SPDX-License-Identifier: LGPL-2.1-or-later
// Grab.app (1.5, 101) on AquaKit, as 10.6.8's (its x86_64 code, docs/reverse-status.md, section 21):
// its Grab.nib menus, its CameraWindow.nib panel, a document window for each grab, the Inspector and
// the Preferences.
//   Capture ▸ Selection (⇧⌘A)   -_variableSelection: the panel "Selection Grab", then a drag: a red
//                               outline, a tool tip with the place then the size (screencapture -G)
//   Capture ▸ Window (⇧⌘W)      "Window Grab": Choose Window, then a click on one (CameraPointer;
//                               the window without its shadow)
//   Capture ▸ Screen (⌘Z)       "Screen Grab": a click outside the panel takes the screen
//   Capture ▸ Timed Screen (⇧⌘Z) "Timed Screen Grab": Start Timer, ten seconds of the stopwatch
//                               (CameraWatch, a PiePieces slice a second at 41, 40), TimerDone at 8
// -_takePicture:: OpenShutter, the eye's flash (CameraEyeFlash, CameraWatchFlash) for a second, the
// panel gone, the grab, CloseShutter; then the picture in a new window (WindowController, "Untitled"),
// dirty until saved as TIFF (-saveToFile:). The sounds unless the Preferences' switch is off (UseSound).
// The grabs themselves are screencapture's (apps/screencapture), as the window server's were Grab's.
#include "appkit.h"
#include "controls.h"
#include "foundation.h"
#include "launchservices.h"
#include "mainmenu.h"
#include "nib.h"
#include "panels.h"
#include "text.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

using namespace AK;

namespace {

const char *const Bundle = "/Applications/Utilities/Grab.app";

std::string res(const std::string &name) { return std::string(Bundle) + "/Contents/Resources/" + name; }
std::string tr(const std::string &key) { return Assets::tr(Bundle, "Localizable", key, key); }
Image named(const std::string &name) { return Image::load(Assets::path(res(name + ".tiff"))); }

bool useSound() { return Settings("grab").boolean("UseSound", true); }

// NSSound's soundNamed: Grab's .snd (Sun audio), played by the session's sound server
void play(const std::string &name)
{
    if (!useSound())
        return;
    const std::string file = Assets::path(res(name + ".snd"));
    for (const char *player : {"pw-play", "paplay", "aplay"})
        if (Workspace::inPath(player)) {
            Task::launchDetached({player, file});
            return;
        }
}

std::string tempFile(const std::string &ext)
{
    ::mkdir(cacheDir().c_str(), 0700);
    return cacheDir() + "/grab-" + std::to_string(getpid()) + "-" + std::to_string(long(now() * 1000)) + "." + ext;
}

// ---- the document windows (WindowController, DocumentWindow.nib)

class Document;
std::vector<Document *> documents;

class Document {
public:
    Document(const Image &image, std::function<void(Event &)> menuKeys) : m_image(image)
    {
        static int untitled = 0;
        ++untitled;
        m_title = untitled == 1 ? "Untitled" : "Untitled " + std::to_string(untitled);
        const Rect visible = App::shared().visibleFrame();
        // the picture's size, at least the nib's window (160 x 129), no bigger than the screen
        const Size size{std::max(160.0, std::min(double(image.width()), visible.w - 40)),
                        std::max(129.0, std::min(double(image.height()), visible.h - 60))};
        m_window = std::make_unique<Window>(size, m_title);
        m_window->setResizable({160, 129});
        auto *scroll = m_window->contentView()->add<ScrollView>();
        scroll->setFrame({0, 0, size.w, size.h});
        m_view = scroll->document()->add<ImageView>();
        m_view->image = image;
        m_view->setFrame({0, 0, double(image.width()), double(image.height())});
        scroll->setDocumentHeight(image.height());
        m_window->onResize = [scroll](Size s) { scroll->setFrame({0, 0, s.w, s.h}); };
        m_window->setDocumentEdited(true);   // a new grab is unsaved
        m_window->onUnhandledKey = [menuKeys](Event &e) { if (menuKeys) menuKeys(e); };
        m_window->onClose = [this] { App::shared().later([this] { remove(); }); };
        documents.push_back(this);
        m_window->show();
    }

    Window *window() const { return m_window.get(); }
    const Image &image() const { return m_image; }
    bool dirty() const { return m_window->isDocumentEdited(); }

    // -askToSave: before closing a dirty one
    void close()
    {
        if (!dirty()) {
            m_window->close();
            return;
        }
        Alert::beginSheet(m_window.get(), Image::load(Assets::path(res("appicon.icns")), {64, 64}),
                          format(tr("Do you want to save the document “%@”?"), m_title),
                          tr("Your changes will be lost if you don’t save the document."),
                          {tr("Save"), tr("Cancel"), tr("Don’t Save")}, [this](int b) {
                              if (b == 0)
                                  save(true);
                              else if (b == 2) {
                                  m_window->setDocumentEdited(false);
                                  m_window->close();
                              }
                          });
    }

    // -saveAs:: the panel, then -saveToFile: (TIFF)
    void save(bool thenClose = false)
    {
        SavePanel::run("Save", desktopDir() + "/" + m_title + ".tiff", {{"TIFF", {"*.tiff", "*.tif"}}}, [this, thenClose](std::string path) {
            if (path.empty())
                return;
            if (path.find('.', path.rfind('/') + 1) == std::string::npos)
                path += ".tiff";
            if (!m_image.saveTiff(path)) {
                Alert::beginSheet(m_window.get(), {}, format(tr("The document could not be saved as “%@”."), baseName(path)),
                                  tr("You may not have permission to save the document in the specified location."), {"OK"}, nullptr);
                return;
            }
            m_title = baseName(path);
            m_window->setTitle(m_title);
            m_window->setDocumentEdited(false);
            if (thenClose)
                m_window->close();
        });
    }

    // copy:: the picture on the clipboard (NSTIFFPboardType; PNG for Linux's programs)
    void copy() const
    {
        const std::string file = tempFile("png");
        if (!m_image.savePng(file))
            return;
        const bool wayland = std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY");
        const std::string cmd = wayland ? "wl-copy --type image/png < '" + file + "'; rm -f '" + file + "'"
                                        : "xclip -selection clipboard -t image/png -i '" + file + "'; rm -f '" + file + "'";
        Task::launchDetached({"sh", "-c", cmd});
    }

private:
    static std::string format(std::string f, const std::string &arg)
    {
        const size_t at = f.find("%@");
        if (at != std::string::npos)
            f.replace(at, 2, arg);
        return f;
    }
    void remove()
    {
        documents.erase(std::remove(documents.begin(), documents.end(), this), documents.end());
        // the last one closed: Grab stays in front with its menus and its keys, as on the Mac
        if (documents.empty())
            App::shared().setFrontWithoutWindows(true);
        delete this;
    }

    Image m_image;
    std::string m_title;
    std::unique_ptr<Window> m_window;
    ImageView *m_view = nullptr;
};

// ---- Grab

class Grab {
public:
    enum Mode { None, Selection, WindowMode, Screen, Timed };

    Grab()
    {
        m_menu.load(Assets::path(res("English.lproj/Grab.nib")));
        m_menu.validate = [this](const MainMenuItem &it, MenuValidation &v) { return validate(it, v); };
        m_menu.perform = [this](const MainMenuItem &it) { perform(it); };
        m_menu.publish();
        // launched alone: in front with its menus, no window (Grab opens none of its own)
        App::shared().setFrontWithoutWindows(true);
    }

    // launched again (the Dock, the Finder): forward, its last document's window, else its menus alone
    void comeForward()
    {
        if (m_panel && m_panel->isVisible()) {
            m_panel->orderFront();
            return;
        }
        if (!documents.empty()) {
            documents.back()->window()->orderFront();
            return;
        }
        App::shared().setFrontWithoutWindows(true);
    }

private:
    // CameraWindow.nib: the panel, its eye or watch, its text and two buttons
    void setupFeedbackWindow()
    {
        if (!m_panel) {
            m_nib.load(Assets::path(res("English.lproj/CameraWindow.nib/keyedobjects.nib")), Assets::path(res("")));
            Size size{391, 144};
            View *content = nullptr;
            for (const Nib::WindowEntry &w : m_nib.windows())
                if (w.outlet == "feedbackWindow")
                    content = m_nib.instantiateWindow(w.object, &size);
            m_panel = std::make_unique<Window>(size, "");
            m_panel->setStyleMask(1);   // a titled panel without its buttons (129)
            if (content) {
                content->setFrame({0, 0, size.w, size.h});
                m_panel->contentView()->addSubview(content);
            }
            m_eye = m_nib.outlet<ImageView>("feedbackImageView");
            m_text = m_nib.outlet("feedbackText");
            m_proceed = m_nib.outlet<Button>("feedbackProceedButton");
            m_cancel = m_nib.outlet<Button>("feedbackCancelButton");
            if (m_cancel)
                m_cancel->action = [this] { cancel(); };
            m_panel->onClose = [this] { cancel(); };
            m_panel->onUnhandledKey = [this](Event &e) {
                if (e.keysym == 0xff1b)   // Escape: the panel's Cancel
                    cancel();
                else if ((e.keysym == 0xff0d || e.keysym == 0xff8d) && m_proceed && m_proceed->isDefault && m_proceed->isEnabled())
                    m_proceed->action();   // Return: the default button
            };
        }
        std::string title, button = tr("Cancel"), text, eye = "CameraEye1";
        bool proceeds = false;
        switch (m_mode) {
        case Selection:
            title = tr("Selection Grab");
            text = tr("Drag over the portion of the screen you want to capture.");
            break;
        case WindowMode:
            title = tr("Window Grab");
            button = tr("Choose Window");
            text = tr("When the window you want is ready, click Choose Window, then click the window to capture it.");
            proceeds = true;
            break;
        case Screen:
            title = tr("Screen Grab");
            text = tr("To capture the screen, click outside this window. (This window will not be included in the screen capture.)  "
                      "If you selected a pointer in Grab preferences, the pointer will be superimposed where you click.");
            break;
        case Timed:
            title = tr("Timed Screen Grab");
            button = tr("Start Timer");
            text = tr("Grab will capture the screen ten seconds after you start the timer.  (This window will not be included in the screen capture.)");
            eye = "CameraWatch";
            proceeds = true;
            break;
        default:
            return;
        }
        m_panel->setTitle(title);
        setStatusImage(eye);
        if (auto *l = dynamic_cast<Label *>(m_text))
            l->setText(text);
        else if (auto *f = dynamic_cast<TextField *>(m_text))
            f->setText(text);
        // -enableFeedbackCancel:: the left button only when the right one proceeds
        if (m_cancel) {
            m_cancel->title = proceeds ? tr("Cancel") : "";
            m_cancel->setHidden(!proceeds);
            m_cancel->setEnabled(proceeds);
        }
        if (m_proceed) {
            m_proceed->title = button;
            m_proceed->isDefault = proceeds;
            m_proceed->setEnabled(true);
            m_proceed->action = [this, proceeds] { proceeds ? proceed() : cancel(); };
            m_proceed->setNeedsDisplay();
        }
    }

    void setStatusImage(const std::string &name)
    {
        if (m_eye) {
            m_eye->image = named(name);
            m_eye->setNeedsDisplay();
        }
    }

    void showFeedback()
    {
        setupFeedbackWindow();
        m_panel->setFloatingPanel(false);
        m_panel->show();
        App::shared().setFrontWithoutWindows(false);
    }

    void hideFeedback()
    {
        if (m_panel && m_panel->isVisible())
            m_panel->close();
        if (documents.empty())
            App::shared().setFrontWithoutWindows(true);
    }

    void start(Mode mode)
    {
        if (m_mode != None)
            return;
        m_mode = mode;
        showFeedback();
        if (mode == Selection)   // -variableSelection: the drag once the panel is on the screen
            App::shared().addTimer(0.3, false, [this] { takePicture(); });
        else if (mode == Screen)   // a click outside the panel
            proceed();
    }

    void cancel()
    {
        if (m_timer) {
            App::shared().removeTimer(m_timer);
            m_timer = 0;
        }
        m_task.reset();
        m_mode = None;
        hideFeedback();
    }

    // -proceed:
    void proceed()
    {
        if (m_mode == Timed) {
            if (m_proceed)
                m_proceed->setEnabled(false);
            m_tick = 0;
            m_timer = App::shared().addTimer(1, true, [this] { timerTicked(); });
        } else if (m_mode == WindowMode) {
            setStatusImage("CameraEye2");
            if (m_proceed)
                m_proceed->setEnabled(false);
            takePicture();
        } else if (m_mode == Screen) {
            m_panel->onOutsideClick = [this](Event &) {
                m_panel->onOutsideClick = nullptr;
                takePicture();
            };
            m_panel->grabInput();
        }
    }

    // -timerTicked:: a slice of the pie a second, TimerDone from the eighth
    void timerTicked()
    {
        if (m_tick < 11) {
            const Image pie = named("PiePieces"), watch = named("CameraWatch");
            if (m_eye && !watch.isNull()) {
                Image face(watch.width(), watch.height());
                cairo_t *cr = cairo_create(face.surface());
                watch.draw(cr, Point{0, 0});
                if (!pie.isNull() && m_tick < 10)   // fromRect (17 × tick, 0, 17, 17) to (41, 40) from the bottom
                    pie.draw(cr, Rect{17.0 * m_tick, 0, 17, 17}, Rect{41, double(watch.height()) - 40 - 17, 17, 17});
                cairo_destroy(cr);
                m_eye->image = face;
                m_eye->setNeedsDisplay();
            }
            if (m_tick == 8)
                play("TimerDone");
            ++m_tick;
            return;
        }
        App::shared().removeTimer(m_timer);
        m_timer = 0;
        takePicture();
    }

    // -_takePicture:: the shutter, the flash, then the grab by screencapture
    void takePicture()
    {
        std::vector<std::string> args{LaunchServices::appsDir() + "/screencapture", "-x", "-tpng"};
        const std::string file = tempFile("png");
        auto grab = [this, args, file]() mutable {
            if (m_mode == Selection)
                args.insert(args.begin() + 1, {"-G", "-s"});
            else if (m_mode == WindowMode)
                args.insert(args.begin() + 1, {"-G", "-w"});
            args.push_back(file);
            m_task = std::make_unique<Task>();
            m_task->onExit = [this, file](int, const std::string &) {
                App::shared().later([this, file] { finish(file); });
            };
            if (!m_task->launch(args))
                cancel();
        };
        if (m_mode == Selection || m_mode == WindowMode) {
            grab();   // the panel stays while the pointer chooses
            return;
        }
        // the screen: the shutter opens, the eye flashes a second, the panel goes, then the grab
        play("OpenShutter");
        setStatusImage(m_mode == Timed ? "CameraWatchFlash" : "CameraEyeFlash");
        App::shared().addTimer(1, false, [this, grab]() mutable {
            if (m_panel && m_panel->isVisible())
                m_panel->close();
            App::shared().addTimer(0.2, false, grab);   // the panel off the screen first
        });
    }

    void finish(const std::string &file)
    {
        m_task.reset();
        const Image image = Image::load(file);
        ::unlink(file.c_str());
        const Mode mode = m_mode;
        m_mode = None;
        if (image.isNull()) {   // given up (Escape, a click without a drag)
            hideFeedback();
            return;
        }
        if (mode == Selection || mode == WindowMode) {
            play("OpenShutter");
            setStatusImage("CameraEyeFlash");
            App::shared().addTimer(1, false, [this, image] {
                play("CloseShutter");
                hideFeedback();
                newDocument(image);
            });
            return;
        }
        play("CloseShutter");
        hideFeedback();
        newDocument(image);
    }

    void newDocument(const Image &image)
    {
        App::shared().setFrontWithoutWindows(false);
        new Document(image, [this](Event &e) { m_menu.performKeyEquivalent(e); });
        updateInspector();
    }

    Document *keyDocument() const
    {
        for (Document *d : documents)
            if (d->window()->isKey())
                return d;
        return documents.empty() ? nullptr : documents.back();
    }

    // InspectorPanel.nib: the key document's width, height and depth
    void showInspector()
    {
        if (!m_inspector) {
            m_inspectorNib.load(Assets::path(res("English.lproj/InspectorPanel.nib/keyedobjects.nib")), Assets::path(res("")));
            Size size{260, 184};
            View *content = nullptr;
            for (const Nib::WindowEntry &w : m_inspectorNib.windows())
                if (w.outlet == "inspectorPanel")
                    content = m_inspectorNib.instantiateWindow(w.object, &size);
            m_inspector = std::make_unique<Window>(size, "Image Inspector");
            m_inspector->setFloatingPanel(true);
            if (content)
                m_inspector->contentView()->addSubview(content);
            m_inspector->onClose = [this] { Settings("grab").set("InspectorWasActive", false); };
        }
        Settings("grab").set("InspectorWasActive", true);
        updateInspector();
        m_inspector->show();
    }

    void updateInspector()
    {
        if (!m_inspector)
            return;
        const Document *d = keyDocument();
        auto set = [this](const char *outlet, const std::string &value) {
            View *v = m_inspectorNib.outlet(outlet);
            if (auto *l = dynamic_cast<Label *>(v))
                l->setText(value);
            else if (auto *f = dynamic_cast<TextField *>(v))
                f->setText(value);
        };
        set("widthText", d ? std::to_string(d->image().width()) : "---");
        set("heightText", d ? std::to_string(d->image().height()) : "---");
        set("depthText", d ? "Millions of Colors (32 bit)" : "---");
    }

    // CursorTypes.nib: the pointer to superimpose (none here yet) and the sound switch
    void showPreferences()
    {
        if (!m_prefs) {
            m_prefsNib.load(Assets::path(res("English.lproj/CursorTypes.nib/keyedobjects.nib")), Assets::path(res("")));
            Size size{226, 186};
            View *content = nullptr;
            for (const Nib::WindowEntry &w : m_prefsNib.windows())
                if (w.outlet == "panel")
                    content = m_prefsNib.instantiateWindow(w.object, &size);
            m_prefs = std::make_unique<Window>(size, "Preferences");
            m_prefs->setFloatingPanel(true);
            if (content)
                m_prefs->contentView()->addSubview(content);
            if (auto *sound = m_prefsNib.outlet<CheckBox>("soundSwitch")) {
                sound->setChecked(useSound());
                sound->onToggle = [](CheckBox *c) { Settings("grab").set("UseSound", c->checked()); };
            }
        }
        m_prefs->show();
    }

    void showAbout()
    {
        if (!m_about) {
            const auto info = Assets::plistStrings(std::string(Bundle) + "/Contents/Info.plist");
            auto get = [&info](const char *k) {
                auto it = info.find(k);
                return it == info.end() ? std::string() : it->second;
            };
            m_about = std::make_unique<Window>(Size{285, 196}, "");
            View *c = m_about->contentView();
            auto *icon = c->add<ImageView>();
            icon->image = Image::load(Assets::path(res("appicon.icns")), {64, 64});
            icon->setFrame({110, 16, 64, 64});
            auto *name = c->add<Label>("Grab", Font::boldSystem(14));
            name->style.align = Align::Center;
            name->setFrame({0, 94, 285, 18});
            auto *version = c->add<Label>("Version " + get("CFBundleShortVersionString") + " (" + get("CFBundleVersion") + ")", Font::system(10));
            version->style.align = Align::Center;
            version->setFrame({0, 118, 285, 14});
            // -showStandardAboutPanel:: CopyrightStartYear 1995
            auto *copyright = c->add<Label>("Copyright © 1995–2009 Apple Inc.", Font::system(10));
            copyright->style.align = Align::Center;
            copyright->setFrame({0, 160, 285, 14});
        }
        m_about->show();
    }

    bool validate(const MainMenuItem &it, MenuValidation &)
    {
        const std::string &a = it.action;
        if (a == "variableSelection:" || a == "windowSelection:" || a == "screenSelection:" || a == "timedSelection:")
            return m_mode == None;   // -_enableCameraItems:
        if (a == "save:" || a == "saveAs:" || a == "copy:" || a == "performClose:" || a == "performMiniaturize:")
            return keyDocument() != nullptr;
        return a == "terminate:" || a == "hide:" || a == "hideOtherApplications:" || a == "unhideAllApplications:" ||
               a == "orderFrontInspectorPanel:" || a == "orderFrontPrefsPanel:" || a == "showStandardAboutPanel:";
    }

    void perform(const MainMenuItem &it)
    {
        const std::string &a = it.action;
        Document *d = keyDocument();
        if (a == "variableSelection:")
            start(Selection);
        else if (a == "windowSelection:")
            start(WindowMode);
        else if (a == "screenSelection:")
            start(Screen);
        else if (a == "timedSelection:")
            start(Timed);
        else if (a == "orderFrontInspectorPanel:")
            showInspector();
        else if (a == "orderFrontPrefsPanel:")
            showPreferences();
        else if (a == "showStandardAboutPanel:")
            showAbout();
        else if (a == "save:" || a == "saveAs:") {
            if (d)
                d->save();
        } else if (a == "copy:") {
            if (d)
                d->copy();
        } else if (a == "performClose:") {
            if (d)
                d->close();
        } else if (a == "performMiniaturize:") {
            if (d)
                d->window()->miniaturize();
        } else if (a == "terminate:")
            terminate();
    }

    // -applicationShouldTerminate:: unsaved grabs, reviewed or not
    void terminate()
    {
        const bool dirty = std::any_of(documents.begin(), documents.end(), [](Document *d) { return d->dirty(); });
        if (!dirty) {
            App::shared().quit();
            return;
        }
        Alert::run(Image::load(Assets::path(res("appicon.icns")), {64, 64}),
                   tr("You have documents with unsaved changes.  Do you want to review these changes before quitting?"),
                   tr("If you don’t review your documents, all your changes will be lost."),
                   {tr("Review Unsaved"), tr("Cancel"), tr("Quit Anyway")}, [](int b) {
                       if (b == 2)
                           App::shared().quit();
                       else if (b == 0)
                           for (Document *d : std::vector<Document *>(documents))
                               if (d->dirty()) {
                                   d->close();
                                   break;
                               }
                   });
    }

    MainMenu m_menu;
    Nib m_nib, m_inspectorNib, m_prefsNib;
    std::unique_ptr<Window> m_panel, m_inspector, m_prefs, m_about;
    ImageView *m_eye = nullptr;
    View *m_text = nullptr;
    Button *m_proceed = nullptr, *m_cancel = nullptr;
    Mode m_mode = None;
    int m_timer = 0, m_tick = 0;
    std::unique_ptr<Task> m_task;
};

} // namespace

int main()
{
    App &app = App::shared();
    setProgramName("Grab");
    if (!app.open()) {
        std::fprintf(stderr, "Grab: no display (Wayland or X11)\n");
        return 1;
    }
    FontManager::registerSystemFonts();
    if (!Assets::exists(res("English.lproj/Grab.nib/keyedobjects.nib"))) {
        std::fprintf(stderr, "Grab: its files are not in the assets (tools/extract.py takes them from the DVD)\n");
        return 1;
    }
    if (Instance::handToRunning("Grab", {}))
        return 0;
    Grab grab;
    Instance::listen("Grab", [&grab](std::vector<std::string>) { grab.comeForward(); });
    return app.run();
}
