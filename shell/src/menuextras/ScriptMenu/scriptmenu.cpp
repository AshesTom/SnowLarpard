// SPDX-License-Identifier: LGPL-2.1-or-later
// Script Menu.menu's code (AppleOSAScriptMenu, PerFile) on AquaKit's SystemUIPlugin, loaded by
// SystemUIServer as on the Mac. Reversed from 10.6's Script Menu.menu 1.2.4, x86_64, with REA
// (docs/reverse-status.md, section 33):
//
//   - -initWithBundle: (0x2d74): Scripts.pdf as a template; com.apple.scriptmenu's ShowLibraryScripts
//     (on) and PutAppScriptsFirst (off);
//   - -menu (0x26ac), made each time: "Open Scripts Folder" with its submenu (Open %@ Scripts Folder
//     for the application in front, Open User Scripts Folder, Open Computer Scripts Folder, Open
//     Network Scripts Folder when there is one); "Open %@" (the default script editor); then, each
//     after a line when not empty: the application's scripts first if PutAppScriptsFirst ("%@
//     Scripts", a disabled title, then its items), the computer's (/Library/Scripts, with
//     ShowLibraryScripts), "Network Scripts" (a submenu), the user's (~/Library/Scripts), the
//     application's last otherwise;
//   - the folders' trees (FUN_000049f3): no names starting with ".", no invisible items, not the
//     domain's Folder Action Scripts nor the user's Applications folder (the applications' scripts);
//     folders are submenus; each level sorted as the Finder sorts (UCCompareTextDefault 0x38006);
//   - PerFile -IPerFile (0x3c52): an item's kind: a folder 0, a Perl script 3 ("#!/usr/bin/perl" or
//     "#! /usr/bin/perl" on its first line), another "#!" script 4, an uncompiled AppleScript 5
//     (.applescript), a compiled one 6 (.scpt, .scptd), a droplet 7, an applet 8, an application 9,
//     a workflow 10, an executable 11, any other file 12; the extension of 5, 6, 8 to 10 hidden; the
//     bundle's icns for each;
//   - _executeFromFile (0x41b9): Shift reveals the item in the Finder; Option opens it (5 and 10 to
//     12 as the Finder would, 6 to 8 in the script editor); otherwise a folder is shown, scripts 3
//     and 4 run as tools (fork, execl; displayalert.sh's alert when that fails), the others opened.
//
// On Linux: ~/Library/Scripts, /Library/Scripts and /Network/Library/Scripts; the application's
// scripts in ~/Library/Scripts/Applications/<its name>; symbolic links followed; AppleScript has no
// interpreter here, so compiled scripts say "Unable to execute that file."; the script editor is
// TextEdit. Settings in scriptmenu.conf.
#include "systemuiplugin.h"

#include "foundation.h"
#include "launchservices.h"
#include "panels.h"
#include "runloop.h"
#include "windows.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>

using namespace AK;

namespace {

enum Kind { Folder = 0, Perl = 3, Shell = 4, Uncompiled = 5, Compiled = 6, Droplet = 7, Applet = 8, Application = 9,
            Workflow = 10, Executable = 11, Document = 12 };

struct PerFile {
    std::string path, name;
    Kind kind = Document;
    std::vector<PerFile> children;
};

bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// FUN_00003917: the first line's "#!"
int shebangKind(const std::string &path)
{
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line.rfind("#!", 0) != 0)
        return -1;
    return line.rfind("#!/usr/bin/perl", 0) == 0 || line.rfind("#! /usr/bin/perl", 0) == 0 ? Perl : Shell;
}

// PerFile -IPerFile: the kind, and the name as shown
bool describe(PerFile &f)
{
    struct stat st;
    if (stat(f.path.c_str(), &st) != 0)
        return false;
    f.name = fileName(f.path);
    if (f.name.empty() || f.name[0] == '.')
        return false;
    auto strip = [&](const std::string &ext) { f.name.resize(f.name.size() - ext.size()); };
    if (S_ISDIR(st.st_mode)) {
        // packages: the applications (an applet or droplet when their PkgInfo says so), scripts
        // bundles, workflows
        if (endsWith(f.name, ".app")) {
            std::ifstream pkg(f.path + "/Contents/PkgInfo");
            std::string info;
            std::getline(pkg, info);
            f.kind = info.find("aplt") != std::string::npos ? Applet : info.find("dplt") != std::string::npos ? Droplet : Application;
            strip(".app");
        } else if (endsWith(f.name, ".scptd")) {
            f.kind = Compiled;
            strip(".scptd");
        } else if (endsWith(f.name, ".workflow")) {
            f.kind = Workflow;
            strip(".workflow");
        } else {
            f.kind = Folder;
        }
        return true;
    }
    if (endsWith(f.name, ".scpt")) {
        f.kind = Compiled;
        strip(".scpt");
    } else if (endsWith(f.name, ".applescript")) {
        f.kind = Uncompiled;
        strip(".applescript");
    } else {
        const int k = st.st_size > 4 ? shebangKind(f.path) : -1;
        // otherwise an executable (the owner's, group's or others' x bit: 0x49), or a document
        f.kind = k >= 0 ? Kind(k) : (st.st_mode & 0111) ? Executable : Document;
    }
    return true;
}

// FUN_000049f3: a folder's items, the folders' own inside them
std::vector<PerFile> tree(const std::string &dir, const std::string &skipA = {}, const std::string &skipB = {})
{
    std::vector<PerFile> out;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent *e = readdir(d)) {
        PerFile f;
        f.path = dir + "/" + e->d_name;
        if (f.path == skipA || f.path == skipB || !describe(f))
            continue;
        if (f.kind == Folder)
            f.children = tree(f.path);
        out.push_back(std::move(f));
    }
    closedir(d);
    // addMenuItems:f: sorts with FUN_000020b6 (the Finder's order)
    std::sort(out.begin(), out.end(), [](const PerFile &a, const PerFile &b) { return finderLess(a.name, b.name); });
    return out;
}

std::string userScripts() { return home() + "/Library/Scripts"; }
const std::string ComputerScripts = "/Library/Scripts", NetworkScripts = "/Network/Library/Scripts";

// GetCurrentKeyModifiers: the keyboard's state now (X's pointer query)
unsigned currentModifiers()
{
    xcb_connection_t *c = Windows::conn();
    if (!c)
        return 0;
    xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(c, xcb_query_pointer(c, Windows::root()), nullptr);
    const unsigned mask = r ? r->mask : 0;
    free(r);
    return mask;
}

} // namespace

class AppleOSAScriptMenu : public SystemUI::MenuExtra {
public:
    explicit AppleOSAScriptMenu(const std::string &bundle) : MenuExtra(bundle)
    {
        Windows::open();
        setImage(templateImage("Scripts.pdf"));
        setAlternateImage(templateImage("Scripts.pdf", true));
        Settings prefs("scriptmenu");
        m_showLibraryScripts = prefs.boolean("ShowLibraryScripts", true);
        m_putAppScriptsFirst = prefs.boolean("PutAppScriptsFirst", false);
        static const std::pair<Kind, const char *> icons[] = {
            {Folder, "Folder.icns"},          {Perl, "PerlScript.icns"},    {Shell, "ShellScript.icns"},
            {Uncompiled, "UncompiledScript.icns"}, {Compiled, "CompiledScript.icns"}, {Droplet, "Droplet.icns"},
            {Applet, "Applet.icns"},          {Application, "GenericApplication.icns"}, {Workflow, "Workflow.icns"},
            {Executable, "GenericApplication.icns"}, {Document, "GenericDocument.icns"}};
        for (const auto &[kind, file] : icons) {
            const Image i = Image::load(resourcePath(file), {16, 16});
            m_icons[kind] = i.isNull() || i.width() == 16 ? i : i.scaled(16, 16);
        }
    }

    std::vector<MenuItem> menu() override
    {
        Settings prefs("scriptmenu");   // com.apple.scriptmenu's update notification: read again
        m_showLibraryScripts = prefs.boolean("ShowLibraryScripts", true);
        m_putAppScriptsFirst = prefs.boolean("PutAppScriptsFirst", false);

        const std::string app = frontApplication();
        const std::string appDir = app.empty() ? std::string() : userScripts() + "/Applications/" + app;
        const std::vector<PerFile> appFiles = appDir.empty() ? std::vector<PerFile>() : tree(appDir);
        const std::vector<PerFile> network = tree(NetworkScripts);

        std::vector<MenuItem> m;
        MenuItem open{tr("Open Scripts Folder")};
        if (!app.empty())
            open.submenu.push_back(openFolderItem(format(tr("Open %@ Scripts Folder"), app), appDir, true));
        open.submenu.push_back(openFolderItem(tr("Open User Scripts Folder"), userScripts(), true));
        open.submenu.push_back(openFolderItem(tr("Open Computer Scripts Folder"), ComputerScripts, false));
        if (!network.empty())
            open.submenu.push_back(openFolderItem(tr("Open Network Scripts Folder"), NetworkScripts, false));
        m.push_back(open);
        m.push_back(launchPrefsItem());

        if (m_putAppScriptsFirst && !appFiles.empty())
            addAppScripts(m, app, appFiles);
        if (m_showLibraryScripts) {
            const std::vector<PerFile> local = tree(ComputerScripts, ComputerScripts + "/Folder Action Scripts");
            if (!local.empty()) {
                m.push_back(MenuItem::line());
                addItems(m, local);
            }
        }
        if (!network.empty()) {
            m.push_back(MenuItem::line());
            MenuItem net{tr("Network Scripts")};
            addItems(net.submenu, network);
            m.push_back(net);
        }
        const std::vector<PerFile> user = tree(userScripts(), userScripts() + "/Applications", userScripts() + "/Folder Action Scripts");
        if (!user.empty()) {
            m.push_back(MenuItem::line());
            addItems(m, user);
        }
        if (!m_putAppScriptsFirst && !appFiles.empty())
            addAppScripts(m, app, appFiles);
        return m;
    }

private:
    std::string tr(const std::string &key) const { return localized(key, key); }
    static std::string format(std::string f, const std::string &arg)
    {
        const size_t at = f.find("%@");
        if (at != std::string::npos)
            f.replace(at, 2, arg);
        return f;
    }

    // _GetFrontProcess: the active window's application, else the one in front without a window
    static std::string frontApplication()
    {
        const xcb_window_t w = Windows::activeWindow();
        LaunchServices::App a;
        if (w)
            a = LaunchServices::forWindow(Windows::wmClass(w), Windows::wmInstance(w), Windows::pid(w));
        if (!a.valid())
            if (const uint32_t pid = Windows::frontApp())
                a = LaunchServices::forPid(pid);
        return a.valid() ? a.name : std::string();
    }

    // -makeOpenScriptsFolderItem:domain: / -openScriptsFolderHandler: (createAppScriptsFolder for
    // the application's and the user's)
    static MenuItem openFolderItem(const std::string &title, const std::string &dir, bool create)
    {
        MenuItem item{title};
        item.action = [dir, create] {
            if (create)
                makePath(dir);
            if (isDir(dir))
                Workspace::openFile(dir);
        };
        return item;
    }

    // -makeLaunchPrefsItem: "Open %@", the default script editor's name
    MenuItem launchPrefsItem() const
    {
        const std::string editor = LaunchServices::appsDir() + "/TextEdit.app/Contents/MacOS/TextEdit";
        const bool have = isFile(editor);
        MenuItem item{format(tr("Open %@"), have ? std::string("TextEdit") : tr("Default Script Editor"))};
        item.action = [editor, have] {
            if (have)
                Task::launchDetached({editor});
            else
                beep();   // AudioServicesPlayAlertSound
        };
        return item;
    }

    // -addAppScripts: a line, "%@ Scripts" disabled, the items
    void addAppScripts(std::vector<MenuItem> &m, const std::string &app, const std::vector<PerFile> &files) const
    {
        m.push_back(MenuItem::line());
        MenuItem title{format(tr("%@ Scripts"), app)};
        title.enabled = false;
        m.push_back(title);
        addItems(m, files);
    }

    // -addMenuItems:f:, -addMenuItemsToArray:f:
    void addItems(std::vector<MenuItem> &m, const std::vector<PerFile> &files) const
    {
        for (const PerFile &f : files) {
            MenuItem item{f.name};
            const auto icon = m_icons.find(f.kind);
            if (icon != m_icons.end())
                item.image = icon->second;
            if (f.kind == Folder) {
                addItems(item.submenu, f.children);
            } else {
                PerFile copy = f;
                item.action = [this, copy] { execute(copy); };
            }
            m.push_back(item);
        }
    }

    // _executeFromFile
    void execute(const PerFile &f) const
    {
        const unsigned mods = currentModifiers();
        if (mods & XCB_MOD_MASK_SHIFT) {
            // the Finder's 'misc' 'slct': the item shown in its folder
            Workspace::openFile(dirName(f.path));
            return;
        }
        if (mods & XCB_MOD_MASK_1) {   // Option
            switch (f.kind) {
            case Perl: case Shell: case Uncompiled: case Workflow: case Executable: case Document:
                if (!LaunchServices::open(f.path))
                    error(tr("Unable to open that file."));
                return;
            case Compiled: case Droplet: case Applet: {
                const std::string editor = LaunchServices::appsDir() + "/TextEdit.app/Contents/MacOS/TextEdit";
                if (!isFile(editor) || !Task::launchDetached({editor, f.path}))
                    error(tr("Unable to open that file."));
                return;
            }
            default: error(tr("Unable to open that file.")); return;
            }
        }
        switch (f.kind) {
        case Folder: Workspace::openFile(f.path); break;   // _revealFolder
        case Perl:
        case Shell: runTool(f.path); break;
        case Compiled: error(tr("Unable to execute that file.")); break;   // AppleScriptRunner's 'dosc'
        case Executable:
            runTool(f.path);
            break;
        default:
            if (!LaunchServices::open(f.path))
                error(tr("Unable to open that file."));
        }
    }

    // FUN_000031f3: fork and execl the script; its failure told as displayalert.sh would
    void runTool(const std::string &path) const
    {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0)
            return;
        const pid_t pid = fork();
        if (pid == 0) {
            setsid();
            if (fork() == 0) {   // the tool on its own, not SystemUIServer's child
                execl(path.c_str(), path.c_str(), static_cast<char *>(nullptr));
                const int err = errno;
                (void)!write(fds[1], &err, sizeof err);
            }
            _exit(0);
        }
        close(fds[1]);
        if (pid > 0)
            waitpid(pid, nullptr, 0);
        int err = 0;
        const bool failed = read(fds[0], &err, sizeof err) == ssize_t(sizeof err);
        close(fds[0]);
        if (pid < 0 || failed) {
            char msg[1200];
            std::snprintf(msg, sizeof msg, "Error:  Couldn't execute the tool %s; errno is %d (%s).", path.c_str(), err,
                          std::strerror(err));
            Alert::run(Image(), "Script Menu", msg, {"OK"}, [](int) {});
        }
    }

    // FUN_0000342e: "Error:  %@"
    void error(const std::string &what) const
    {
        Alert::run(Image(), format(tr("Error:  %@"), what), "", {"OK"}, [](int) {});
    }

    static void beep()
    {
        if (xcb_connection_t *c = Windows::conn()) {
            xcb_bell(c, 0);
            xcb_flush(c);
        }
    }

    bool m_showLibraryScripts = true, m_putAppScriptsFirst = false;
    std::map<Kind, Image> m_icons;
};

SYSTEMUI_REGISTER_EXTRA("AppleOSAScriptMenu", AppleOSAScriptMenu);
