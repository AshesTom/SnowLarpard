// SPDX-License-Identifier: LGPL-2.1-or-later
// Mail.app, on AquaKit (no Qt), from what docs/mail-reverse.md recovered of 10.6's Mail: the Welcome
// assistant (AssistantWindow.nib and AccountSetupAssistant.nib from the DVD), the viewer (its toolbar
// as MainWindow.toolbar lays it out, the mailboxes with Mail's icons and unread badges, the list, the
// message), the compose window; IMAP and SMTP through libcurl (net.h), the messages kept as 10.6
// keeps them (store.h).
//   Mail [mailto:address]
#include "controls.h"
#include "foundation.h"
#include "mime.h"
#include "net.h"
#include "nib.h"
#include "plist.h"
#include "store.h"
#include "textview.h"
#include "toolbar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <pwd.h>
#include <set>
#include <unistd.h>

using namespace AK;
using Preview::PList;

namespace {

const char *const Bundle = "/Applications/Mail.app";

std::string res(const std::string &name) { return Assets::path(std::string(Bundle) + "/Contents/Resources/" + name); }
std::string tr(const std::string &table, const std::string &key, const std::string &fallback)
{
    return Assets::tr(Bundle, table, key, fallback);
}

std::string fullUserName()
{
    // NSFullUserName: the account's full name
    if (passwd *pw = getpwuid(getuid())) {
        std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
        gecos = gecos.substr(0, gecos.find(','));
        return gecos.empty() ? pw->pw_name : gecos;
    }
    return {};
}

// The key's character, lowercased: from the keysym, AquaKit leaving the text empty under ⌘ and ⌃
std::string keyChar(const Event &e)
{
    if (e.keysym >= 0x20 && e.keysym < 0x7f)
        return std::string(1, char(std::tolower(int(e.keysym))));
    return e.text.empty() ? std::string() : std::string(1, char(std::tolower((unsigned char)e.text[0])));
}

// "host" or "host:port"
void splitHost(const std::string &in, std::string *host, int *port)
{
    const size_t colon = in.rfind(':');
    if (colon != std::string::npos && colon + 1 < in.size() && std::isdigit((unsigned char)in[colon + 1])) {
        *host = in.substr(0, colon);
        *port = std::atoi(in.c_str() + colon + 1);
    } else {
        *host = in;
    }
}

// =========================================================================================== Assistant

// The stamp behind the pages and its white veil (TransparentImageView, docs §15): Mail_Large aligned
// top left, not scaled; white at 75 % over the box, inset 3/4, 6 narrower and lower, radius 8
class StampView : public View {
public:
    StampView() { m_stamp = Image::load(res("Mail_Large.tiff")); }
    void draw(cairo_t *cr) override
    {
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, frame().w, frame().h);
        cairo_clip(cr);
        if (!m_stamp.isNull())
            m_stamp.draw(cr, Point{0, 0});
        cairo_restore(cr);
        // the box: 422 x 346 at (119, 5) in the view, Cocoa's coordinates (from the bottom)
        const double x = 119 + 3, w = 422 - 6, h = 346 - 6;
        const double y = frame().h - (5 + 4) - h;
        const double r = 8;
        cairo_new_path(cr);
        cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
        cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
        cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
        cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
        cairo_fill(cr);
    }
    View *hitTest(Point) override { return nullptr; }

private:
    Image m_stamp;
};

class Assistant {
public:
    // done(true) once an account is saved
    std::function<void(bool)> done;

    explicit Assistant(Mail::Worker &worker, bool adding) : m_worker(worker), m_adding(adding) {}

    bool open()
    {
        const std::string lproj = res("English.lproj/");
        if (!m_winNib.load(lproj + "AssistantWindow.nib/keyedobjects.nib", res("")) ||
            !m_pageNib.load(lproj + "AccountSetupAssistant.nib/keyedobjects.nib", res(""))) {
            std::fprintf(stderr, "Mail: the assistant's nibs are missing (run tools/extract.py on your DVD)\n");
            return false;
        }
        View *content = m_winNib.instantiate("_window");
        if (!content)
            return false;
        const Size size = m_winNib.size();
        m_window = std::make_unique<Window>(size, tr("Assistant", "TITLE_FOR_WELCOME", "Welcome to Mail"));
        View *root = m_window->contentView();
        // the stamp first: the nib's views over it (TransparentImageView at 20, 55, 548 x 355)
        auto *stamp = root->add<StampView>();
        stamp->setFrame({20, size.h - 55 - 355, 548, 355});
        root->addSubview(content);
        content->setFrame({0, 0, size.w, size.h});

        View *container = m_winNib.outlet("_assistantContainerView");
        View *top = m_pageNib.instantiate("_topView");
        if (!container || !top)
            return false;
        container->addSubview(top);
        top->setFrame(container->bounds());
        m_tabs = m_pageNib.outlet<TabView>("_tabView");
        if (m_tabs) {
            m_tabs->noTabs = true;
            m_tabs->noBorder = true;
            m_tabs->setFrame(top->bounds());
        }
        if (View *help = m_winNib.outlet("_helpButton"))
            help->setHidden(false);

        // the first page's title and description (firstPaneTitle, firstPaneDescription: Assistant.strings)
        const std::string which = m_adding ? "ADD_ACCOUNT" : "WELCOME";
        setBound("firstPaneTitle", tr("Assistant", "TITLE_FOR_" + which, "Welcome to Mail"));
        setBound("firstPaneDescription", tr("Assistant", "DESCRIPTION_FOR_" + which, ""));
        m_name = field("name", 0);
        m_email = field("emailAddress", 0);
        m_password = field("password", 0);
        m_inServer = field("incomingMailServer", 2);
        m_inUser = field("incomingUserName", 2);
        m_inPassword = field("password", 2);
        m_outServer = field("outgoingMailServer", 4);
        m_outUser = field("outgoingUserName", 4);
        m_outPassword = field("outgoingPassword", 4);
        if (m_name)
            m_name->setText(fullUserName());
        // Account Type (incomingAccountClassIndex): IMAP, the one this Mail speaks
        if (auto *type = m_pageNib.bound1<PopUpButton>("incomingAccountClassIndex", "selectedIndex")) {
            for (size_t i = 0; i < type->items.size(); ++i) {
                const bool imap = type->items[i].title == "IMAP";
                type->items[i].enabled = imap;
                if (imap)
                    type->setSelected(int(i));
            }
        }

        // the status line under the fields, with the caution sign and the spinner
        for (int page : {0, 2, 4}) {
            View *p = m_tabs ? m_tabs->page(page) : nullptr;
            if (!p)
                continue;
            auto *status = p->add<Label>(std::string(), Font::system(11));
            status->wrap = true;
            status->setFrame({60, p->frame().h - 10 - 73, 340, 73});
            m_status[page] = status;
        }
        for (View *v : m_pageNib.bound("showStatusAlert", "hidden"))
            v->setHidden(true);
        for (View *v : m_pageNib.bound("actionInProgress", "hidden"))
            v->setHidden(true);
        for (View *v : m_pageNib.bound("isEmailAddressValid", "hidden"))
            v->setHidden(true);
        for (const char *k : {"isIncomingMailServerValid", "isIncomingUserNameValid", "isOutgoingMailServerValid",
                              "isIncomingOutlookServerValid", "hideIncomingOutlookServer", "addressIsForEWSAccount"})
            for (View *v : m_pageNib.bound(k, "hidden"))
                v->setHidden(true);

        if (auto buttons = m_winNib.withAction("forward:"); !buttons.empty())
            if (auto *b = dynamic_cast<Button *>(buttons[0])) {
                m_continue = b;
                b->isDefault = true;
                b->action = [this] { forward(); };
            }
        if (auto buttons = m_winNib.withAction("back:"); !buttons.empty())
            if (auto *b = dynamic_cast<Button *>(buttons[0])) {
                m_back = b;
                b->action = [this] { back(); };
            }
        if (auto buttons = m_winNib.withAction("cancel:"); !buttons.empty())
            if (auto *b = dynamic_cast<class AK::Control *>(buttons[0]))
                b->action = [this] { finish(false); };
        m_window->onClose = [this] { finish(false); };
        m_window->onUnhandledKey = [this](Event &e) {
            if (e.keysym == 0xff0d || e.keysym == 0xff8d)
                forward();
            else if (e.keysym == 0xff1b)
                finish(false);
        };
        show(0);
        m_window->show();
        if (m_email)
            m_window->makeFirstResponder(m_email);   // _firstResponderForBasicInformation
        return true;
    }

private:
    // a value binding's text: the nib makes the bound fields Labels or (non-editable) TextFields
    void setBound(const std::string &key, const std::string &value)
    {
        if (std::getenv("MAIL_DEBUG"))
            std::fprintf(stderr, "Mail: %s -> %zu view(s), \"%s\"\n", key.c_str(), m_pageNib.bound(key).size(), value.c_str());
        for (View *v : m_pageNib.bound(key)) {
            if (auto *l = dynamic_cast<Label *>(v)) {
                l->wrap = l->frame().h > 30;
                l->setText(value);
            } else if (auto *f = dynamic_cast<TextField *>(v)) {
                f->setText(value);
            }
        }
    }

    TextField *field(const std::string &key, int page)
    {
        View *p = m_tabs ? m_tabs->page(page) : nullptr;
        for (View *v : m_pageNib.bound(key))
            if (auto *f = dynamic_cast<TextField *>(v)) {
                for (View *a = v; a; a = a->superview())
                    if (a == p)
                        return f;
            }
        return nullptr;
    }

    void show(int page)
    {
        m_page = page;
        if (m_tabs)
            m_tabs->select(page);
        if (m_back)
            m_back->setEnabled(!m_stack.empty());
        status("");
    }

    void status(const std::string &text)
    {
        for (auto &s : m_status)
            if (s.second)
                s.second->setText(s.first == m_page ? text : std::string());
        for (View *v : m_pageNib.bound("showStatusAlert", "hidden"))
            v->setHidden(text.empty());
    }

    void busy(bool on)
    {
        m_busy = on;
        for (View *v : m_pageNib.bound("actionInProgress", "hidden")) {
            v->setHidden(!on);
            if (auto *p = dynamic_cast<ProgressIndicator *>(v))
                on ? p->start() : p->stop();
        }
        if (m_continue)
            m_continue->setEnabled(!on);
    }

    static std::string text(TextField *f) { return f ? f->text : std::string(); }

    void forward()
    {
        if (m_busy)
            return;
        if (m_page == 0) {
            const std::string email = text(m_email);
            if (email.find('@') == std::string::npos) {
                status(tr("Assistant", "NO_EMAIL_MESSAGE", "Enter your email address."));
                return;
            }
            m_account = Mail::guessAccount(text(m_name), email);
            m_pw = text(m_password);
            if (m_inServer)
                m_inServer->setText(m_account.imapHost);
            if (m_inUser)
                m_inUser->setText(m_account.imapUser);
            if (m_inPassword)
                m_inPassword->setText(m_pw);
            check(true);
        } else if (m_page == 2) {
            if (text(m_inServer).empty()) {
                status(tr("Assistant", "NO_HOST_MESSAGE", "Enter the incoming mail server."));
                return;
            }
            if (text(m_inUser).empty()) {
                status(tr("Assistant", "NO_USER_MESSAGE", "Enter your user name."));
                return;
            }
            splitHost(text(m_inServer), &m_account.imapHost, &m_account.imapPort);
            if (text(m_inServer).find(':') == std::string::npos)
                m_account.imapPort = 993;
            m_account.imapSSL = m_account.imapPort == 993;
            m_account.imapUser = text(m_inUser);
            m_pw = text(m_inPassword);
            check(false);
        } else if (m_page == 4) {
            if (text(m_outServer).empty()) {
                status(tr("Assistant", "NO_SMTP_MESSAGE", "Enter the outgoing mail server."));
                return;
            }
            splitHost(text(m_outServer), &m_account.smtpHost, &m_account.smtpPort);
            if (text(m_outServer).find(':') == std::string::npos)
                m_account.smtpPort = 587;
            m_account.smtpSSL = m_account.smtpPort == 465;
            m_account.smtpUser = text(m_outUser).empty() ? m_account.imapUser : text(m_outUser);
            save();
        }
    }

    // -[AccountSetupAssistant _validateTab:]: the incoming server first; automatic setup when it answers
    void check(bool automatic)
    {
        busy(true);
        status(tr("Assistant", "CHECKING_INCOMING", "Checking connection to the incoming mail server…"));
        auto result = std::make_shared<Mail::Result>();
        const Mail::Account a = m_account;
        const std::string pw = m_pw;
        m_worker.run([result, a, pw] { *result = Mail::checkIncoming(a, pw); },
                     [this, result, automatic] {
                         busy(false);
                         if (result->ok) {
                             if (automatic) {
                                 save();
                             } else {
                                 if (m_outServer && m_outServer->text.empty())
                                     m_outServer->setText(m_account.smtpHost);
                                 if (m_outUser && m_outUser->text.empty())
                                     m_outUser->setText(m_account.imapUser);
                                 if (m_outPassword && m_outPassword->text.empty())
                                     m_outPassword->setText(m_pw);
                                 m_stack.push_back(m_page);   // _pushTab:
                                 show(4);
                             }
                             return;
                         }
                         if (automatic) {
                             // the manual pages, the error under the fields
                             m_stack.push_back(m_page);
                             show(2);
                         }
                         status(tr("Assistant", "CONNECTION_FAILED", "Mail can't connect to the server.") + "\n" + result->error);
                     });
    }

    void back()
    {
        if (m_stack.empty() || m_busy)
            return;
        const int p = m_stack.back();   // _popTab
        m_stack.pop_back();
        show(p);
    }

    void save()
    {
        std::vector<Mail::Account> accounts = Mail::loadAccounts();
        accounts.push_back(m_account);
        Mail::saveAccounts(accounts);
        if (!Mail::storePassword(m_account, m_pw))
            std::fprintf(stderr, "Mail: the password could not be kept in the keyring; it will be asked again\n");
        finish(true);
    }

    void finish(bool ok)
    {
        if (m_finished)
            return;
        m_finished = true;
        // the next window first: AquaKit's program ends with its last window
        if (done)
            done(ok);
        App::shared().later([this] { m_window.reset(); });
    }

    Mail::Worker &m_worker;
    bool m_adding;
    Nib m_winNib, m_pageNib;
    std::unique_ptr<Window> m_window;
    TabView *m_tabs = nullptr;
    TextField *m_name = nullptr, *m_email = nullptr, *m_password = nullptr, *m_inServer = nullptr, *m_inUser = nullptr,
              *m_inPassword = nullptr, *m_outServer = nullptr, *m_outUser = nullptr, *m_outPassword = nullptr;
    Button *m_continue = nullptr, *m_back = nullptr;
    std::map<int, Label *> m_status;
    std::vector<int> m_stack;
    int m_page = 0;
    bool m_busy = false, m_finished = false;
    Mail::Account m_account;
    std::string m_pw;
};

// ============================================================================================ Viewer

// The mailboxes (MailboxesOutlineViewController): source list rows 20 high (the 11 point font + 9),
// the unread badge at the right (MailboxListCell drawBadgeCenteredInRect:, docs §9)
class MailboxList : public OutlineView {
public:
    std::vector<int> badges;   // per row
    void draw(cairo_t *cr) override
    {
        OutlineView::draw(cr);
        const std::vector<int> visible = visibleRows();
        for (size_t k = 0; k < visible.size(); ++k) {
            const int row = visible[k];
            if (size_t(row) >= badges.size() || badges[size_t(row)] <= 0)
                continue;
            drawBadge(cr, frame().w - 8, double(k) * rowHeight + rowHeight / 2, badges[size_t(row)], row == selected());
        }
    }

private:
    void drawBadge(cairo_t *cr, double right, double cy, int count, bool selected)
    {
        const Font font{11, true, "Helvetica"};
        const std::string label = std::to_string(count);
        const double h = std::floor(lineHeight(font));
        // badgeWidth: the text's (or "99"'s) width + half the height, + 1
        const double w = std::max(textWidth(label, font), textWidth("99", font)) + std::ceil(h * 0.5) + 1;
        const double x = std::floor(right - w), y = std::floor(cy - h / 2), r = h / 2;
        const bool active = windowIsKey();
        Color fill = active ? Color::rgb(133, 151, 187) : Color::gray(154);       // badgeColor (Blue)
        Color ink = Color::gray(255);
        if (selected) {
            fill = Color::gray(255);
            ink = active ? (isFirstResponder() ? Color::rgb(56, 115, 191) : Color::rgb(136, 153, 188)) : Color::gray(159);
        }
        cairo_new_path(cr);
        cairo_arc(cr, x + r, y + r, r, M_PI / 2, 3 * M_PI / 2);
        cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, M_PI / 2);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, fill.r, fill.g, fill.b, fill.a);
        cairo_fill(cr);
        TextStyle style;
        style.font = font;
        style.color = ink;
        style.align = Align::Center;
        drawText(cr, label, Rect{x, y, w, h}, style);
    }
};

// The message pane: the headers (docs §11, level 1: From, Date, To, Cc, Subject), a line, the text
class MessagePane : public View {
public:
    Mail::Message message;
    bool empty = true;
    std::string placeholder;
    void draw(cairo_t *cr) override
    {
        fill(cr, bounds(), Color::gray(255));
        if (empty) {
            TextStyle s;
            s.font = Font::system(13);
            s.color = Color::gray(140);
            s.align = Align::Center;
            drawText(cr, placeholder, Rect{0, frame().h / 2 - 10, frame().w, 20}, s);
            return;
        }
        double y = 10;
        const double labelW = 70;
        TextStyle key, value;
        key.font = Font::boldSystem(12);
        key.color = Color::gray(128);
        key.align = Align::Right;
        value.font = Font::system(12);
        for (const auto &[label, header] : rows()) {
            const std::string v = header == "subject" ? message.headers.get(header) : message.headers.get(header);
            if (v.empty())
                continue;
            drawText(cr, label + ":", Rect{10, y, labelW, 16}, key);
            TextStyle vs = value;
            if (header == "subject")
                vs.font = Font::boldSystem(12);
            drawText(cr, v, Rect{10 + labelW + 6, y, frame().w - labelW - 30, 16}, vs);
            y += 17;
        }
        if (!message.attachments.empty()) {
            std::string list;
            for (const auto &a : message.attachments)
                list += (list.empty() ? "" : ", ") + a;
            drawText(cr, std::to_string(message.attachments.size()) + " attachment" +
                             (message.attachments.size() > 1 ? "s" : "") + ": " + list,
                     Rect{10 + labelW + 6, y, frame().w - labelW - 30, 16}, value);
            y += 17;
        }
        y += 6;
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        cairo_rectangle(cr, 10, y, frame().w - 20, 1);
        cairo_fill(cr);
        y += 10;
        TextStyle body;
        body.font = Font::system(12);
        body.elide = false;
        drawWrappedText(cr, message.text, Rect{14, y, frame().w - 28, 1e6}, body);
    }
    double contentHeight(double width) const
    {
        if (empty)
            return 0;
        double h = 10 + 17 * 6 + 16 + 10;
        return h + wrappedHeight(message.text, width - 28, Font::system(12)) + 20;
    }

private:
    static std::vector<std::pair<std::string, std::string>> rows()
    {
        return {{"From", "from"}, {"Subject", "subject"}, {"Date", "date"}, {"To", "to"}, {"Cc", "cc"}, {"Reply-To", "reply-to"}};
    }
};

class Composer;

class Viewer {
public:
    Viewer(Mail::Worker &worker, std::vector<Mail::Account> accounts) : m_worker(worker), m_accounts(std::move(accounts))
    {
        for (const auto &a : m_accounts)
            m_passwords.push_back(Mail::password(a));
        // MessageViewer.nib: 778 x 482, the mailbox pane 190 wide (MessageViewerContents.nib)
        m_window = std::make_unique<Window>(Size{900, 560}, "Inbox", Window::Titled);
        View *root = m_window->contentView();
        m_mailboxes = root->add<MailboxList>();
        m_list = root->add<TableView>();
        m_scroll = root->add<ScrollView>();
        m_pane = m_scroll->document()->add<MessagePane>();
        m_pane->placeholder = "No Message Selected";
        m_toolbar = root->add<Toolbar>();
        buildToolbar();
        m_window->setUnifiedToolbarHeight(int(Toolbar::Gradient));
        m_window->setShowsToolbarButton(true);
        m_window->setIcon(Image::load(res("../app.icns"), {128, 128}));
        m_window->setResizable({500, 300});
        m_window->onResize = [this](Size) { layout(); };
        m_window->onClose = [this] {
            if (onClose)
                onClose();
        };
        m_window->onUnhandledKey = [this](Event &e) { key(e); };

        m_mailboxes->background = Color::rgb(214, 221, 229);   // the source list's (AppKit's, measured)
        m_mailboxes->rowHeight = std::max(20.0, 11 + 9.0);      // _setOutlineViewRowHeight
        m_mailboxes->onSelect = [this](int row) { selectMailbox(row); };

        m_list->columns = {{"", 20}, {"From", 180}, {"Subject", 330}, {"Date Received", 130}};
        m_list->onSelect = [this](int row) { selectMessage(row); };

        layout();
        reloadMailboxes();
        m_window->show();
        refresh();
        // PollTime = 5 (Message.defaults): every five minutes
        m_poll = App::shared().addTimer(5 * 60, true, [this] { refresh(); });
    }
    ~Viewer() { App::shared().removeTimer(m_poll); }

    std::function<void()> onClose;
    std::function<void(int account, std::string to, std::string subject, std::string body, std::string inReplyTo)> onCompose;

    void refresh()
    {
        if (m_syncing)
            return;
        m_syncing = true;
        updateTitle();
        auto pending = std::make_shared<int>(int(m_accounts.size()));
        if (m_accounts.empty())
            m_syncing = false;
        for (size_t i = 0; i < m_accounts.size(); ++i) {
            const Mail::Account a = m_accounts[i];
            const std::string pw = m_passwords[i];
            const std::string current = m_box.account == int(i) ? m_box.name : std::string();
            auto boxes = std::make_shared<std::vector<Mail::ListedMailbox>>();
            auto error = std::make_shared<std::string>();
            m_worker.run(
                [a, pw, current, boxes, error] {
                    Mail::Result r = Mail::listMailboxes(a, pw, boxes.get());
                    if (!r.ok) {
                        *error = r.error;
                        return;
                    }
                    // INBOX always, and the mailbox on screen
                    std::set<std::string> sync = {"INBOX"};
                    if (!current.empty())
                        sync.insert(current);
                    for (const std::string &box : sync) {
                        std::vector<std::pair<unsigned, unsigned>> remote;
                        r = Mail::uidsAndFlags(a, pw, box, &remote);
                        if (!r.ok) {
                            *error = r.error;
                            continue;
                        }
                        std::set<unsigned> onServer;
                        const std::vector<unsigned> local = Mail::storedUids(a, box);
                        const std::set<unsigned> have(local.begin(), local.end());
                        for (auto [uid, flags] : remote) {
                            onServer.insert(uid);
                            if (have.count(uid)) {
                                Mail::setFlags(Mail::messagePath(a, box, uid), flags);
                                continue;
                            }
                            std::string raw;
                            if (!Mail::fetchMessage(a, pw, box, uid, &raw).ok)
                                continue;
                            // the fetch marked it read on the server: unread it stays
                            if (!(flags & Mail::Read))
                                Mail::storeFlags(a, pw, box, uid, false, "\\Seen");
                            Mail::saveMessage(a, box, uid, raw, flags);
                        }
                        for (unsigned uid : local)
                            if (!onServer.count(uid))
                                Mail::removeMessage(Mail::messagePath(a, box, uid));
                    }
                },
                [this, i, boxes, error, pending] {
                    if (!error->empty())
                        std::fprintf(stderr, "Mail: %s: %s\n", m_accounts[i].description().c_str(), error->c_str());
                    if (!boxes->empty())
                        m_listed[int(i)] = *boxes;
                    if (--*pending == 0) {
                        m_syncing = false;
                        reloadMailboxes();
                        reloadMessages();
                        updateTitle();
                    }
                });
        }
    }

    void sent(int account, const std::string &raw)
    {
        // a copy in the account's Sent mailbox, on the server and here
        const Mail::Account a = m_accounts[size_t(account)];
        const std::string pw = m_passwords[size_t(account)];
        std::string sentBox = "Sent";
        for (const auto &m : m_listed[account])
            if (Mail::kindOf(m.name, m.flags) == Mail::Kind::Sent)
                sentBox = m.name;
        m_worker.run([a, pw, sentBox, raw] { Mail::appendMessage(a, pw, sentBox, raw, "\\Seen"); }, [this] { refresh(); });
    }

    const std::vector<Mail::Account> &accounts() const { return m_accounts; }
    const std::vector<std::string> &passwords() const { return m_passwords; }
    Window *window() const { return m_window.get(); }

private:
    struct Box {
        int account = -1;
        std::string name;
    };

    // ---- the toolbar (MainWindow.toolbar, docs §4)
    void buildToolbar()
    {
        const PList plist = PList::load(res("MainWindow.toolbar"));
        std::map<std::string, const PList *> byId;
        for (const PList &group : plist["toolbar array"].array) {
            if (group.type == PList::Array) {
                for (const PList &item : group.array)
                    byId.emplace(item.str("identifier"), &item);
            } else {
                byId.emplace(group.str("identifier"), &group);
            }
        }
        for (const PList &id : plist["default set"].array) {
            const std::string identifier = id.string;
            if (identifier == "NSToolbarFlexibleSpaceItem") {
                m_toolbar->addItem(identifier, {}, nullptr, 0);
                continue;
            }
            auto found = byId.find(identifier);
            if (found == byId.end())
                continue;
            const PList &item = *found->second;
            if (item.integer("tag") == 100) {   // the search field (toolbar:itemForItemIdentifier:...)
                auto *field = new SearchField;
                field->onChange = [this](const std::string &t) {
                    m_filter = t;
                    reloadMessages();
                };
                m_toolbar->addItem(identifier, tr("Localizable", item.str("title"), "Search"), field, 140, 220);
                continue;
            }
            std::vector<const PList *> segs;
            if (!item["segments"].isNull())
                for (const PList &s : item["segments"].array)
                    segs.push_back(&s);
            else
                segs.push_back(&item);
            std::vector<Image> images;
            std::vector<double> widths;
            std::vector<std::string> actions;
            std::string label;
            for (const PList *s : segs) {
                Image img = Image::load(res(s->str("image") + ".png"));
                images.push_back(img);
                widths.push_back(std::max(32.0, double(img.width()) + 6));
                actions.push_back(s->str("identifier"));
                const std::string l = tr("Localizable", s->str("title"), s->str("title"));
                label += (label.empty() ? "" : "   ") + l;
            }
            auto *sc = new SegmentedControl(CoreUI::SegmentStyle::Textured, std::vector<std::string>(images.size()), widths);
            sc->momentary = true;
            sc->glyph = [images](cairo_t *cr, int i, const Rect &r, bool enabled) {
                const Image &img = images[size_t(i)];
                if (img.isNull())
                    return;
                img.draw(cr, Point{std::floor(r.x + (r.w - img.width()) / 2), std::floor(r.y + (r.h - img.height()) / 2)},
                         enabled ? 1 : 0.35);
            };
            sc->segmentEnabled.assign(actions.size(), true);
            for (size_t i = 0; i < actions.size(); ++i)
                if (actions[i] == "showNoteEditor:" || actions[i] == "createToDo:")
                    sc->segmentEnabled[i] = false;   // Notes and To Dos: not yet
            sc->onSelect = [this, actions](int i) { perform(actions[size_t(i)]); };
            double total = 0;
            for (double w : widths)
                total += w;
            m_toolbar->addItem(identifier, label, sc, total);
        }
    }

    void perform(const std::string &action)
    {
        if (action == "checkNewMail:")
            refresh();
        else if (action == "deleteMessages:")
            deleteSelected();
        else if (action == "markAsJunkMail:")
            moveSelected(Mail::Kind::Junk);
        else if (action == "replyMessage:" || action == "replyAllMessage:")
            reply(action == "replyAllMessage:");
        else if (action == "forwardMessage:")
            forward();
        else if (action == "showComposeWindow:")
            compose();
    }

    void key(Event &e)
    {
        const bool cmd = e.modifiers & Command, shift = e.modifiers & Shift;
        const std::string k = keyChar(e);
        if (cmd && shift && k == "n")
            refresh();                                  // Get New Mail ⇧⌘N
        else if (cmd && !shift && k == "n")
            compose();                                  // New Message ⌘N
        else if (cmd && shift && k == "r")
            reply(true);                                // Reply All ⇧⌘R
        else if (cmd && k == "r")
            reply(false);                               // Reply ⌘R
        else if (cmd && shift && k == "f")
            forward();                                  // Forward ⇧⌘F
        else if (cmd && shift && k == "u")
            toggleRead();                               // Mark As Read ⇧⌘U
        else if (cmd && k == "w") {
            if (onClose)
                onClose();
        } else if (cmd && k == "q")
            App::shared().quit();
        else if (e.keysym == 0xff08 || e.keysym == 0xffff)
            deleteSelected();                           // Delete ⌘⌫, and the key alone in the list
        else if (cmd && k >= "1" && k <= "6") {
            // Go To: ⌘1 Inbox ... (selectMailbox: tags 1–8)
            static const Mail::Kind kinds[] = {Mail::Kind::Inbox, Mail::Kind::Other, Mail::Kind::Drafts,
                                               Mail::Kind::Sent, Mail::Kind::Trash, Mail::Kind::Junk};
            const Mail::Kind want = kinds[k[0] - '1'];
            for (size_t r = 0; r < m_rows.size(); ++r)
                if (m_rows[r].account >= 0 && m_kinds[r] == want) {
                    m_mailboxes->select(int(r), true);
                    break;
                }
        }
    }

    // ---- the mailboxes
    void reloadMailboxes()
    {
        auto icon = [](const std::string &name) { return Image::load(res(name + ".png"), {16, 16}); };
        const Image folder = Image::load(
            Assets::path("/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns"), {16, 16});
        const Image account = Image::load(res("GenericAccount.tiff"), {16, 16});
        auto kindIcon = [&](Mail::Kind k) {
            // iconForMailboxUid:smallSize: (docs §9)
            switch (k) {
            case Mail::Kind::Inbox: return icon("InMailbox");
            case Mail::Kind::Drafts: return icon("DraftsMailbox");
            case Mail::Kind::Sent: return icon("SentMailbox");
            case Mail::Kind::Trash: return icon("TrashMailbox");
            case Mail::Kind::Junk: return icon("JunkMailbox");
            default: return folder;
            }
        };
        const Box selected = m_box;
        std::vector<OutlineRow> rows;
        m_rows.clear();
        m_kinds.clear();
        auto add = [&](OutlineRow r, Box b, Mail::Kind k) {
            rows.push_back(r);
            m_rows.push_back(b);
            m_kinds.push_back(k);
        };
        OutlineRow header;
        header.header = true;
        header.selectable = false;
        header.title = tr("Localizable", "MAILBOXES_SECTION_TITLE", "MAILBOXES");
        add(header, {}, Mail::Kind::Other);
        // the special mailboxes of every account, then each account's others
        const Mail::Kind order[] = {Mail::Kind::Inbox, Mail::Kind::Drafts, Mail::Kind::Sent, Mail::Kind::Trash, Mail::Kind::Junk};
        for (Mail::Kind k : order)
            for (size_t i = 0; i < m_accounts.size(); ++i)
                for (const auto &m : listed(int(i)))
                    if (Mail::kindOf(m.name, m.flags) == k) {
                        OutlineRow r;
                        r.title = displayName(m.name, k);
                        if (m_accounts.size() > 1)
                            r.title += " (" + m_accounts[i].description() + ")";
                        r.icon = kindIcon(k);
                        r.level = 1;
                        add(r, {int(i), m.name}, k);
                        break;
                    }
        for (size_t i = 0; i < m_accounts.size(); ++i) {
            std::vector<Mail::ListedMailbox> others;
            for (const auto &m : listed(int(i)))
                if (Mail::kindOf(m.name, m.flags) == Mail::Kind::Other)
                    others.push_back(m);
            if (others.empty())
                continue;
            OutlineRow h;
            h.header = true;
            h.selectable = false;
            h.title = m_accounts[i].description();
            h.icon = account;
            add(h, {}, Mail::Kind::Other);
            for (const auto &m : others) {
                OutlineRow r;
                r.title = m.name;
                r.icon = folder;
                r.level = 1;
                add(r, {int(i), m.name}, Mail::Kind::Other);
            }
        }
        m_mailboxes->rows = rows;
        m_mailboxes->badges.assign(rows.size(), 0);
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].account >= 0 && (m_kinds[r] == Mail::Kind::Inbox || m_kinds[r] == Mail::Kind::Other))
                m_mailboxes->badges[r] = unread(m_rows[r]);
        m_mailboxes->reload();
        int sel = -1;
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].account == selected.account && m_rows[r].name == selected.name && selected.account >= 0)
                sel = int(r);
        if (sel < 0)
            for (size_t r = 0; r < m_rows.size(); ++r)
                if (m_rows[r].account >= 0) {
                    sel = int(r);
                    break;
                }
        if (sel >= 0) {
            m_mailboxes->select(sel);
            m_box = m_rows[size_t(sel)];
        }
    }

    std::vector<Mail::ListedMailbox> listed(int account) const
    {
        auto it = m_listed.find(account);
        if (it != m_listed.end())
            return it->second;
        return {{"INBOX", ""}};   // before the first answer: the inbox already on disk
    }

    static std::string displayName(const std::string &name, Mail::Kind k)
    {
        switch (k) {
        case Mail::Kind::Inbox: return "Inbox";
        case Mail::Kind::Drafts: return "Drafts";
        case Mail::Kind::Sent: return "Sent";
        case Mail::Kind::Trash: return "Trash";
        case Mail::Kind::Junk: return "Junk";
        default: return name;
        }
    }

    int unread(const Box &b) const
    {
        int n = 0;
        for (const auto &s : Mail::summaries(m_accounts[size_t(b.account)], b.name))
            if (!(s.flags & Mail::Read))
                ++n;
        return n;
    }

    void selectMailbox(int row)
    {
        if (row < 0 || size_t(row) >= m_rows.size() || m_rows[size_t(row)].account < 0)
            return;
        const bool changed = m_rows[size_t(row)].name != m_box.name || m_rows[size_t(row)].account != m_box.account;
        m_box = m_rows[size_t(row)];
        reloadMessages();
        updateTitle();
        if (changed && m_box.name != "INBOX")
            refresh();   // its messages from the server
    }

    // ---- the messages
    void reloadMessages()
    {
        const unsigned selectedUid = selectedSummary() ? selectedSummary()->uid : 0;
        m_messages.clear();
        if (m_box.account >= 0)
            for (const auto &s : Mail::summaries(m_accounts[size_t(m_box.account)], m_box.name)) {
                if (!m_filter.empty()) {
                    auto has = [&](const std::string &field) {
                        std::string a = field, b = m_filter;
                        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                        return a.find(b) != std::string::npos;
                    };
                    if (!has(s.from) && !has(s.subject) && !has(s.to))
                        continue;
                }
                m_messages.push_back(s);
            }
        const bool sentBox = m_box.account >= 0 && kindOfBox(m_box) == Mail::Kind::Sent;
        m_list->columns[1].title = sentBox ? "To" : "From";
        m_list->rows.clear();
        int sel = -1;
        for (size_t i = 0; i < m_messages.size(); ++i) {
            const auto &s = m_messages[i];
            // the unread column's dot (column 0), the sender's name, the subject, the date
            m_list->rows.push_back({s.flags & Mail::Read ? "" : "\xE2\x97\x8F", Mail::displayName(sentBox ? s.to : s.from),
                                    s.subject.empty() ? "(No Subject)" : s.subject, Mail::listDate(s.date)});
            if (s.uid == selectedUid)
                sel = int(i);
        }
        m_list->selected = sel;
        m_list->reload();
        if (sel < 0)
            showMessage(nullptr);
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].account >= 0 && (m_kinds[r] == Mail::Kind::Inbox || m_kinds[r] == Mail::Kind::Other))
                m_mailboxes->badges[r] = unread(m_rows[r]);
        m_mailboxes->setNeedsDisplay();
        updateTitle();
    }

    Mail::Kind kindOfBox(const Box &b) const
    {
        for (const auto &m : listed(b.account))
            if (m.name == b.name)
                return Mail::kindOf(m.name, m.flags);
        return Mail::kindOf(b.name, "");
    }

    const Mail::Summary *selectedSummary() const
    {
        const int r = m_list->selected;
        return r >= 0 && size_t(r) < m_messages.size() ? &m_messages[size_t(r)] : nullptr;
    }

    void selectMessage(int row)
    {
        if (row < 0 || size_t(row) >= m_messages.size()) {
            showMessage(nullptr);
            return;
        }
        Mail::Summary &s = m_messages[size_t(row)];
        showMessage(&s);
        if (!(s.flags & Mail::Read))
            setRead(s, true);
    }

    void showMessage(const Mail::Summary *s)
    {
        m_pane->empty = s == nullptr;
        if (s)
            m_pane->message = Mail::parse(Mail::readMessage(s->path));
        m_pane->placeholder = m_messages.empty() ? std::string() : std::string("No Message Selected");
        layoutPane();
        m_scroll->scrollTo(0);
        m_pane->setNeedsDisplay();
    }

    void setRead(Mail::Summary &s, bool read)
    {
        s.flags = read ? s.flags | Mail::Read : s.flags & ~unsigned(Mail::Read);
        Mail::setFlags(s.path, s.flags);
        const Mail::Account a = m_accounts[size_t(m_box.account)];
        const std::string pw = m_passwords[size_t(m_box.account)], box = m_box.name;
        const unsigned uid = s.uid;
        m_worker.run([a, pw, box, uid, read] { Mail::storeFlags(a, pw, box, uid, read, "\\Seen"); });
        const int row = m_list->selected;
        if (row >= 0 && size_t(row) < m_list->rows.size()) {
            m_list->rows[size_t(row)][0] = read ? "" : "\xE2\x97\x8F";
            m_list->reload();
        }
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].account == m_box.account && m_rows[r].name == m_box.name)
                m_mailboxes->badges[r] = unread(m_rows[r]);
        m_mailboxes->setNeedsDisplay();
        updateTitle();
    }

    void toggleRead()
    {
        const int row = m_list->selected;
        if (row >= 0 && size_t(row) < m_messages.size())
            setRead(m_messages[size_t(row)], !(m_messages[size_t(row)].flags & Mail::Read));
    }

    void deleteSelected() { moveSelected(Mail::Kind::Trash); }

    void moveSelected(Mail::Kind to)
    {
        const Mail::Summary *s = selectedSummary();
        if (!s || m_box.account < 0)
            return;
        const Mail::Account a = m_accounts[size_t(m_box.account)];
        const std::string pw = m_passwords[size_t(m_box.account)], box = m_box.name;
        std::string target;
        for (const auto &m : listed(m_box.account))
            if (Mail::kindOf(m.name, m.flags) == to)
                target = m.name;
        const unsigned uid = s->uid;
        const std::string path = s->path;
        const bool inTarget = kindOfBox(m_box) == to;
        // the next message gets the selection (TableViewManager deleteMessages:..., the one below)
        const int row = m_list->selected;
        Mail::removeMessage(path);
        m_worker.run(
            [a, pw, box, uid, target, inTarget] {
                if (target.empty() || inTarget) {   // no Trash, or already in it: deleted for good
                    Mail::storeFlags(a, pw, box, uid, true, "\\Deleted");
                    return;
                }
                Mail::moveMessage(a, pw, box, uid, target);
            },
            [this] { refresh(); });
        reloadMessages();
        if (!m_messages.empty()) {
            const int next = std::min(row, int(m_messages.size()) - 1);
            m_list->selected = next;
            m_list->reload();
            selectMessage(next);
        }
    }

    std::string quoted(const Mail::Message &m) const
    {
        std::string out = "\n\nOn " + m.headers.get("date") + ", " + Mail::displayName(m.headers.get("from")) + " wrote:\n\n";
        size_t p = 0;
        while (p <= m.text.size()) {
            size_t e = m.text.find('\n', p);
            if (e == std::string::npos)
                e = m.text.size();
            out += "> " + m.text.substr(p, e - p) + "\n";
            p = e + 1;
        }
        return out;
    }

    void reply(bool all)
    {
        const Mail::Summary *s = selectedSummary();
        if (!s || !onCompose)
            return;
        Mail::Message m = Mail::parse(Mail::readMessage(s->path));
        std::string to = m.headers.get("reply-to").empty() ? m.headers.get("from") : m.headers.get("reply-to");
        if (all) {
            const std::string me = m_accounts[size_t(m_box.account)].email;
            for (const std::string &field : {m.headers.get("to"), m.headers.get("cc")}) {
                size_t p = 0;
                while (p < field.size()) {
                    size_t e = field.find(',', p);
                    if (e == std::string::npos)
                        e = field.size();
                    std::string one = field.substr(p, e - p);
                    if (Mail::address(one) != me && !Mail::address(one).empty())
                        to += ", " + one.substr(one.find_first_not_of(' '));
                    p = e + 1;
                }
            }
        }
        std::string subject = m.headers.get("subject");
        if (subject.rfind("Re:", 0) != 0 && subject.rfind("RE:", 0) != 0)
            subject = "Re: " + subject;
        onCompose(m_box.account, to, subject, quoted(m), m.headers.get("message-id"));
    }

    void forward()
    {
        const Mail::Summary *s = selectedSummary();
        if (!s || !onCompose)
            return;
        Mail::Message m = Mail::parse(Mail::readMessage(s->path));
        std::string body = "\n\nBegin forwarded message:\n\n";
        for (const char *h : {"From", "Date", "To", "Subject"})
            if (!m.headers.get(h).empty())
                body += std::string(h) + ": " + m.headers.get(h) + "\n";
        body += "\n" + m.text;
        onCompose(m_box.account, "", "Fwd: " + m.headers.get("subject"), body, "");
    }

    void compose()
    {
        if (onCompose)
            onCompose(m_box.account >= 0 ? m_box.account : 0, "", "", "", "");
    }

    void updateTitle()
    {
        // "Inbox (3 messages, 1 unread)": -[MessageViewer _countStringForType:...]
        std::string name = "Mail";
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].account == m_box.account && m_rows[r].name == m_box.name && m_box.account >= 0)
                name = m_mailboxes->rows[r].title;
        int unread = 0;
        for (const auto &s : m_messages)
            if (!(s.flags & Mail::Read))
                ++unread;
        std::string title = name + " (" + std::to_string(m_messages.size()) + " message" + (m_messages.size() == 1 ? "" : "s");
        if (unread)
            title += ", " + std::to_string(unread) + " unread";
        title += ")";
        if (m_syncing)
            title += " \xE2\x80\x94 Checking for new mail\xE2\x80\xA6";
        m_window->setTitle(title);
    }

    void layout()
    {
        const Size s = m_window->size();
        m_toolbar->setFrame({0, 0, s.w, Toolbar::Height});
        const double top = Toolbar::Height, side = 190;
        m_mailboxes->setFrame({0, top, side, s.h - top});
        // the list above the message (MessageViewerContents.nib: 173 of 469, about 37 %)
        const double listH = std::floor((s.h - top) * 0.37);
        m_list->setFrame({side + 1, top, s.w - side - 1, listH});
        m_scroll->setFrame({side + 1, top + listH + 1, s.w - side - 1, s.h - top - listH - 1});
        layoutPane();
    }

    void layoutPane()
    {
        const double w = m_scroll->frame().w - 15;
        const double h = std::max(m_scroll->frame().h, m_pane->contentHeight(w));
        m_pane->setFrame({0, 0, w, h});
        m_scroll->setDocumentHeight(h);
    }

    Mail::Worker &m_worker;
    std::vector<Mail::Account> m_accounts;
    std::vector<std::string> m_passwords;
    std::map<int, std::vector<Mail::ListedMailbox>> m_listed;
    std::unique_ptr<Window> m_window;
    Toolbar *m_toolbar = nullptr;
    MailboxList *m_mailboxes = nullptr;
    TableView *m_list = nullptr;
    ScrollView *m_scroll = nullptr;
    MessagePane *m_pane = nullptr;
    std::vector<Box> m_rows;
    std::vector<Mail::Kind> m_kinds;
    Box m_box;
    std::vector<Mail::Summary> m_messages;
    std::string m_filter;
    bool m_syncing = false;
    int m_poll = 0;
};

// ========================================================================================== Composer

// The compose window (WebMessageEditor.nib's layout: To, Cc, Subject, From over the message; its toolbar
// ComposeWindow_NewMessage.toolbar's Send)
class Composer {
public:
    std::function<void(Composer *)> onClose;
    std::function<void(int account, const std::string &raw)> onSent;

    Composer(Mail::Worker &worker, std::vector<Mail::Account> accounts, std::vector<std::string> passwords, int account,
             const std::string &to, const std::string &subject, const std::string &body, const std::string &inReplyTo)
        : m_worker(worker), m_accounts(std::move(accounts)), m_passwords(std::move(passwords)), m_account(account),
          m_inReplyTo(inReplyTo)
    {
        m_window = std::make_unique<Window>(Size{620, 520}, subject.empty() ? "New Message" : subject);
        View *root = m_window->contentView();
        m_toolbar = root->add<Toolbar>();
        // Send (TB_Send), as ComposeWindow_NewMessage.toolbar's first item
        const Image send = Image::load(res("TB_Send.png"));
        auto *sb = new SegmentedControl(CoreUI::SegmentStyle::Textured, {""}, {std::max(32.0, double(send.width()) + 6)});
        sb->momentary = true;
        sb->glyph = [send](cairo_t *cr, int, const Rect &r, bool enabled) {
            send.draw(cr, Point{std::floor(r.x + (r.w - send.width()) / 2), std::floor(r.y + (r.h - send.height()) / 2)},
                      enabled ? 1 : 0.35);
        };
        sb->onSelect = [this](int) { sendNow(); };
        m_toolbar->addItem("send:", tr("Localizable", "TOOLBAR_SEND", "Send"), sb, std::max(32.0, double(send.width()) + 6));
        m_toolbar->addItem("NSToolbarFlexibleSpaceItem", {}, nullptr, 0);
        m_window->setUnifiedToolbarHeight(int(Toolbar::Gradient));

        auto label = [&](const std::string &t) {
            auto *l = root->add<Label>(t, Font::system(12));
            l->style.align = Align::Right;
            l->style.color = Color::gray(80);
            return l;
        };
        m_toLabel = label("To:");
        m_ccLabel = label("Cc:");
        m_subjectLabel = label("Subject:");
        m_fromLabel = label("From:");
        m_to = root->add<TextField>();
        m_cc = root->add<TextField>();
        m_subject = root->add<TextField>();
        for (TextField *f : {m_to, m_cc, m_subject}) {
            f->style = TextField::Plain;
            f->font = Font::system(12);
        }
        m_from = root->add<Label>(fromLine(), Font::system(12));
        m_to->setText(to);
        m_subject->setText(subject);
        m_subject->onChange = [this](const std::string &t) { m_window->setTitle(t.empty() ? "New Message" : t); };
        m_status = root->add<Label>(std::string(), Font::system(11));
        m_status->style.color = Color::rgb(170, 30, 30);
        m_scroll = root->add<ScrollView>();
        m_body = m_scroll->document()->add<Mail::TextView>();
        m_body->setText(body);
        m_body->onHeightChanged = [this](double) { layoutBody(); };
        m_window->setResizable({400, 300});
        m_window->onResize = [this](Size) { layout(); };
        m_window->onClose = [this] { close(); };
        m_window->onUnhandledKey = [this](Event &e) {
            const std::string k = keyChar(e);
            if ((e.modifiers & Command) && (e.modifiers & Shift) && k == "d")
                sendNow();   // Send ⇧⌘D
            else if ((e.modifiers & Command) && k == "w")
                close();
        };
        layout();
        m_window->show();
        m_window->makeFirstResponder(to.empty() ? static_cast<View *>(m_to) : static_cast<View *>(m_body));
    }

private:
    std::string fromLine() const
    {
        if (m_account < 0 || size_t(m_account) >= m_accounts.size())
            return {};
        const Mail::Account &a = m_accounts[size_t(m_account)];
        return a.fullName.empty() ? a.email : a.fullName + " <" + a.email + ">";
    }

    void layout()
    {
        const Size s = m_window->size();
        m_toolbar->setFrame({0, 0, s.w, Toolbar::Height});
        double y = Toolbar::Height + 8;
        const double lx = 8, lw = 70, fx = lx + lw + 6, fw = s.w - fx - 12;
        for (auto [l, f] : std::vector<std::pair<Label *, View *>>{{m_toLabel, m_to}, {m_ccLabel, m_cc}, {m_subjectLabel, m_subject}, {m_fromLabel, m_from}}) {
            l->setFrame({lx, y + 2, lw, 17});
            f->setFrame({fx, y, fw, 21});
            y += 25;
        }
        m_status->setFrame({fx, y, fw, 16});
        y += 18;
        m_scroll->setFrame({0, y, s.w, s.h - y});
        layoutBody();
    }

    void layoutBody()
    {
        const double w = m_scroll->frame().w - 15;
        m_body->setFrame({0, 0, w, std::max(m_scroll->frame().h, m_body->contentHeight())});
        const double h = std::max(m_scroll->frame().h, m_body->contentHeight());
        m_body->setFrame({0, 0, w, h});
        m_scroll->setDocumentHeight(h);
    }

    static std::vector<std::string> addresses(const std::string &field)
    {
        std::vector<std::string> out;
        size_t p = 0;
        while (p < field.size()) {
            size_t e = field.find(',', p);
            if (e == std::string::npos)
                e = field.size();
            const std::string a = Mail::address(field.substr(p, e - p));
            if (!a.empty())
                out.push_back(a);
            p = e + 1;
        }
        return out;
    }

    void sendNow()
    {
        if (m_sending || m_account < 0 || size_t(m_account) >= m_accounts.size())
            return;
        std::vector<std::string> rcpts = addresses(m_to->text);
        for (const auto &c : addresses(m_cc->text))
            rcpts.push_back(c);
        if (rcpts.empty()) {
            m_status->setText("The message has no recipient.");
            return;
        }
        const Mail::Account a = m_accounts[size_t(m_account)];
        // the message: UTF-8 text, 8 bit
        char id[64];
        std::snprintf(id, sizeof id, "<%ld.%d@%s>", long(time(nullptr)), int(getpid()),
                      a.email.substr(a.email.find('@') + 1).c_str());
        std::string raw;
        raw += "From: " + (a.fullName.empty() ? a.email : Mail::encodeWord(a.fullName) + " <" + a.email + ">") + "\r\n";
        raw += "To: " + m_to->text + "\r\n";
        if (!m_cc->text.empty())
            raw += "Cc: " + m_cc->text + "\r\n";
        raw += "Subject: " + Mail::encodeWord(m_subject->text) + "\r\n";
        raw += "Date: " + Mail::formatDate(time(nullptr)) + "\r\n";
        raw += std::string("Message-Id: ") + id + "\r\n";
        if (!m_inReplyTo.empty())
            raw += "In-Reply-To: " + m_inReplyTo + "\r\nReferences: " + m_inReplyTo + "\r\n";
        raw += "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n";
        raw += "X-Mailer: Snow Leopard Mail\r\n\r\n";
        std::string body = m_body->text;
        for (size_t p = 0; (p = body.find('\n', p)) != std::string::npos; p += 2)
            body.replace(p, 1, "\r\n");
        raw += body + "\r\n";
        m_sending = true;
        m_status->style.color = Color::gray(90);
        m_status->setText("Sending…");
        const std::string pw = m_passwords[size_t(m_account)];
        auto result = std::make_shared<Mail::Result>();
        m_worker.run([a, pw, rcpts, raw, result] { *result = Mail::sendMessage(a, pw, a.email, rcpts, raw); },
                     [this, result, raw] {
                         m_sending = false;
                         if (!result->ok) {
                             m_status->style.color = Color::rgb(170, 30, 30);
                             m_status->setText("The message could not be sent: " + result->error);
                             return;
                         }
                         if (onSent)
                             onSent(m_account, raw);
                         close();
                     });
    }

    void close()
    {
        if (m_closing)
            return;
        m_closing = true;
        App::shared().later([this] {
            if (onClose)
                onClose(this);
        });
    }

    Mail::Worker &m_worker;
    std::vector<Mail::Account> m_accounts;
    std::vector<std::string> m_passwords;
    int m_account;
    std::string m_inReplyTo;
    std::unique_ptr<Window> m_window;
    Toolbar *m_toolbar = nullptr;
    Label *m_toLabel = nullptr, *m_ccLabel = nullptr, *m_subjectLabel = nullptr, *m_fromLabel = nullptr, *m_from = nullptr,
          *m_status = nullptr;
    TextField *m_to = nullptr, *m_cc = nullptr, *m_subject = nullptr;
    ScrollView *m_scroll = nullptr;
    Mail::TextView *m_body = nullptr;
    bool m_sending = false, m_closing = false;
};

// =============================================================================================== App

class MailApp {
public:
    explicit MailApp(std::string mailto) : m_mailto(std::move(mailto)) {}

    void start()
    {
        std::vector<Mail::Account> accounts = Mail::loadAccounts();
        if (accounts.empty()) {
            assistant(false);
            return;
        }
        openViewer(accounts);
    }

private:
    void assistant(bool adding)
    {
        m_assistant = std::make_unique<Assistant>(m_worker, adding);
        m_assistant->done = [this](bool ok) {
            std::vector<Mail::Account> accounts = Mail::loadAccounts();
            if (!ok && accounts.empty() && !m_viewer) {
                App::shared().quit();
                return;
            }
            if (!m_viewer && !accounts.empty())
                openViewer(accounts);
            // the assistant goes once its window is gone (it closes it itself, later)
            App::shared().later([this] { App::shared().later([this] { m_assistant.reset(); }); });
        };
        if (!m_assistant->open())
            App::shared().quit(1);
    }

    void openViewer(const std::vector<Mail::Account> &accounts)
    {
        m_viewer = std::make_unique<Viewer>(m_worker, accounts);
        m_viewer->onClose = [] { App::shared().quit(); };   // the viewer closed: 10.6's Mail stays, ours quits
        m_viewer->onCompose = [this](int account, std::string to, std::string subject, std::string body, std::string inReplyTo) {
            compose(account, to, subject, body, inReplyTo);
        };
        if (!m_mailto.empty()) {
            std::string to = m_mailto.rfind("mailto:", 0) == 0 ? m_mailto.substr(7) : m_mailto;
            to = to.substr(0, to.find('?'));
            compose(0, to, "", "", "");
            m_mailto.clear();
        }
    }

    void compose(int account, const std::string &to, const std::string &subject, const std::string &body,
                 const std::string &inReplyTo)
    {
        auto c = std::make_unique<Composer>(m_worker, m_viewer->accounts(), m_viewer->passwords(), account, to, subject, body,
                                            inReplyTo);
        c->onClose = [this](Composer *w) {
            m_composers.erase(std::remove_if(m_composers.begin(), m_composers.end(), [w](const auto &p) { return p.get() == w; }),
                              m_composers.end());
        };
        c->onSent = [this](int a, const std::string &raw) {
            if (m_viewer)
                m_viewer->sent(a, raw);
        };
        m_composers.push_back(std::move(c));
    }

    Mail::Worker m_worker;
    std::string m_mailto;
    std::unique_ptr<Assistant> m_assistant;
    std::unique_ptr<Viewer> m_viewer;
    std::vector<std::unique_ptr<Composer>> m_composers;
};

} // namespace

int main(int argc, char **argv)
{
    App &app = App::shared();
    std::string mailto;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "mailto:", 7) == 0 || std::strchr(argv[i], '@'))
            mailto = argv[i];
    if (!app.open()) {
        std::fprintf(stderr, "Mail: no display (Wayland or X11)\n");
        return 1;
    }
    Assets::registerFonts();
    MailApp mail(mailto);
    mail.start();
    return app.run();
}
