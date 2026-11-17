// SPDX-License-Identifier: LGPL-2.1-or-later
#include "progress.h"

#include "files.h"

#include "controls.h"
#include "foundation.h"
#include "nib.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace AK;

namespace Finder {

namespace {

const char *const Bundle = "/System/Library/CoreServices/Finder.app";
std::string tr(const std::string &key, const std::string &fallback) { return Assets::tr(Bundle, "Localizable", key, fallback); }

std::string format(std::string f, const std::vector<std::string> &values)
{
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string mark = "^" + std::to_string(i);
        for (size_t at; (at = f.find(mark)) != std::string::npos;)
            f.replace(at, mark.size(), values[i]);
    }
    return f;
}

std::string folderName(const std::string &path) { return displayName(path.empty() ? "/" : path); }

// the operation's line: what it does, to where
std::string title(const Operation &op)
{
    const auto &src = op.sources();
    const size_t n = src.size();
    const std::string one = n ? fileName(src.front()) : std::string(), count = std::to_string(n), to = folderName(op.destination());
    const bool preparing = op.counting;
    switch (op.kind()) {
    case Operation::Copy:
        return preparing ? format(tr("CP3", "Preparing to copy to “^0”"), {to})
               : n == 1  ? format(tr("CP4", "Copying “^0” to “^1”"), {one, to})
                         : format(tr("CP_MULT_V1", "Copying ^0 items to “^1”"), {count, to});
    case Operation::Move:
        return preparing ? format(tr("MV3", "Preparing to move to “^0”"), {to})
               : n == 1  ? format(tr("MV4", "Moving “^0” to “^1”"), {one, to})
                         : format(tr("MV_MULT_V1", "Moving ^0 items to “^1”"), {count, to});
    case Operation::Duplicate: {
        const std::string here = n ? folderName(dirName(src.front())) : std::string();
        return n == 1 ? format(tr("DU3", "Duplicating “^0” to “^1”"), {one, here})
                      : format(tr("DU_MULT_V1", "Duplicating ^0 items to “^1”"), {count, here});
    }
    case Operation::Trash:
        return preparing ? tr("MT7", "Preparing to move to “Trash”")
               : n == 1  ? format(tr("MT8", "Moving “^0” to “Trash”"), {one})
                         : format(tr("MT_MULT_V1", "Moving ^0 items to “Trash”"), {count});
    case Operation::EmptyTrash:
    case Operation::SecureEmptyTrash:
        return preparing ? tr("ET1", "Preparing to empty the Trash…") : tr("ET2", "Emptying the Trash…");
    case Operation::PutBack:
        return n == 1 ? format(tr("MV4", "Moving “^0” to “^1”"), {one, folderName(dirName(Operation::trashOrigin(src.front())))})
                      : format(tr("MV_MULT_V1", "Moving ^0 items to “^1”"), {count, folderName(dirName(Operation::trashOrigin(src.front())))});
    case Operation::Compress:
        return n == 1 ? format(tr("AR3", "Compressing “^0” to “^1”"), {one, one + ".zip"})
                      : format(tr("AR_MULT_V1", "Compressing ^0 items to “^1”"), {count, tr("AR40", "Archive") + ".zip"});
    }
    return {};
}

// "About 5 seconds"... (U20-U26)
std::string remaining(double seconds)
{
    if (seconds < 7.5)
        return tr("U20", "About 5 seconds");
    if (seconds < 15)
        return tr("U21", "About 10 seconds");
    if (seconds < 50)
        return tr("U22", "Less than a minute");
    if (seconds < 90)
        return tr("U23", "About a minute");
    if (seconds < 50 * 60)
        return format(tr("U24", "About ^0 minutes"), {std::to_string(int(std::lround(seconds / 60)))});
    if (seconds < 90 * 60)
        return tr("U25", "About an hour");
    return format(tr("U26", "About ^0 hours"), {std::to_string(int(std::lround(seconds / 3600)))});
}

// the line under the bar: "12 MB of 40 MB - About 5 seconds" (PW3), or the items left (PW6)
std::string status(const Operation &op)
{
    if (!op.error().empty())
        return op.error();
    if (op.counting)
        return tr("PW2", "Estimating time remaining…");
    const long long done = op.bytesDone, total = op.bytesTotal;
    if (total <= 0)
        return format(tr("PW6", "Items remaining: ^0"), {std::to_string(std::max(0, int(op.itemsLeft)))});
    const double elapsed = now() - op.started;
    if (elapsed < 2 || done <= 0)
        return format(tr("PW3", "^0 of ^1 - ^2"), {formatBytes(done), formatBytes(total), tr("PW2", "Estimating time remaining…")});
    const double rate = done / elapsed;
    return format(tr("PW3", "^0 of ^1 - ^2"), {formatBytes(done), formatBytes(total), remaining((total - done) / std::max(rate, 1.0))});
}

struct Entry {
    std::shared_ptr<Operation> op;
    std::unique_ptr<Nib> statusNib, replaceNib;
    View *statusView = nullptr, *replaceView = nullptr;
    Label *titleText = nullptr, *statusText = nullptr, *message = nullptr;
    ProgressIndicator *bar = nullptr;
    CheckBox *applyAll = nullptr;
    bool asking = false;
    bool failed = false;   // stays until stopped (its error under the bar)
};

class CopyWindow {
public:
    static CopyWindow &shared()
    {
        static CopyWindow w;
        return w;
    }

    void add(std::shared_ptr<Operation> op)
    {
        auto e = std::make_unique<Entry>();
        e->op = std::move(op);
        build(*e);
        m_entries.push_back(std::move(e));
        if (!m_timer)
            m_timer = App::shared().addTimer(0.1, true, [this] { tick(); });
    }

private:
    void ensureWindow()
    {
        if (m_window)
            return;
        m_window = std::make_unique<Window>(Size{399, 81}, tr("PW32", "Copy"), Window::Titled);
        m_window->onClose = [this] { m_shown = false; };
    }

    void build(Entry &e)
    {
        ensureWindow();
        const std::string res = std::string(Bundle) + "/Contents/Resources/";
        e.statusNib = std::make_unique<Nib>();
        if (e.statusNib->load(Assets::path(res + "English.lproj/ProgressStatusView2Line.nib/keyedobjects.nib"), Assets::path(res)))
            e.statusView = e.statusNib->instantiate("view");
        if (!e.statusView) {
            e.statusView = new View;
            e.statusView->setFrame({0, 0, 399, 81});
        }
        e.titleText = e.statusNib->outlet<Label>("_titleText");
        e.bar = e.statusNib->outlet<ProgressIndicator>("_progress");
        if (auto *img = e.statusNib->outlet<ImageView>("_imageView")) {
            const auto &src = e.op->sources();
            img->image = e.op->kind() == Operation::Trash || e.op->kind() == Operation::EmptyTrash || src.empty()
                             ? coreTypesIcon("TrashIcon", 32)
                             : icon(Item{fileName(src.front()), src.front(), isDir(src.front())}, 32);
            if (img->image.isNull())
                img->image = coreTypesIcon("GenericDocumentIcon", 32);
        }
        if (auto *b = e.statusNib->outlet<Button>("_cancelButton")) {
            b->kind = Button::Plain;
            b->image = CoreUI::named("StopProgressFreestanding").scaled(14, 14);
            Entry *raw = &e;
            b->action = [raw] {
                raw->op->cancel();
                raw->failed = false;
            };
        }
        // the line under the bar: a label where the nib has its text view
        walk(e.statusView, [&](View *v) {
            if (!e.statusText && v != e.statusView && v->frame().w == 310 && v->frame().h == 29) {
                e.statusText = new Label({}, Font::system(11));
                e.statusText->style.color = Color::gray(80);
                e.statusText->setFrame({5, 0, 305, 14});
                v->addSubview(e.statusText);
            }
        });
        m_window->contentView()->addSubview(e.statusView);
        layout();
    }

    void ask(Entry &e, const Operation::Conflict &c)
    {
        // ProgressReplaceView: the message, Stop, Don't Replace, Replace, Apply to All
        const std::string res = std::string(Bundle) + "/Contents/Resources/";
        e.replaceNib = std::make_unique<Nib>();
        if (!e.replaceNib->load(Assets::path(res + "English.lproj/ProgressReplaceView.nib/keyedobjects.nib"), Assets::path(res)))
            return;
        e.replaceView = e.replaceNib->instantiate("view");
        if (!e.replaceView)
            return;
        std::string text;
        const bool ext = !c.extension.empty();
        const std::string stem = ext ? c.name.substr(0, c.name.size() - c.extension.size() - 1) : c.name;
        if (c.age < 0)
            text = ext ? format(tr("PE64", ""), {stem, c.extension}) : format(tr("PE63", ""), {c.name});
        else if (c.age > 0)
            text = ext ? format(tr("PE62", ""), {stem, c.extension}) : format(tr("PE61", ""), {c.name});
        else
            text = ext ? format(tr("PE32", ""), {stem, c.extension}) : format(tr("PE1", ""), {c.name});
        walk(e.replaceView, [&](View *v) {
            if (!e.message && v != e.replaceView && v->frame().w >= 329 && v->frame().h >= 49) {
                e.message = new Label(text, Font::system(11));
                e.message->wrap = true;
                e.message->setFrame({0, 0, v->frame().w, v->frame().h});
                v->addSubview(e.message);
            }
        });
        if (auto *img = e.replaceNib->outlet<ImageView>("_imageView"))
            img->image = coreTypesIcon(c.folder ? "GenericFolderIcon" : "GenericDocumentIcon", 32);
        e.applyAll = e.replaceNib->outlet<CheckBox>("_applyAllCheckbox");
        Entry *raw = &e;
        walk(e.replaceView, [&](View *v) {
            auto *b = dynamic_cast<Button *>(v);
            if (!b)
                return;
            const Operation::Answer a = b->title == "Replace" ? Operation::Replace : b->title == "Stop" ? Operation::Stop : Operation::Skip;
            if (b->title == "Replace")
                b->isDefault = true;
            b->action = [this, raw, a] {
                raw->op->answer(a, raw->applyAll && raw->applyAll->checked());
                raw->replaceView->setHidden(true);
                raw->statusView->setHidden(false);
                raw->asking = false;
                layout();
            };
        });
        m_window->contentView()->addSubview(e.replaceView);
        e.statusView->setHidden(true);
        e.asking = true;
        layout();
        show();
    }

    void tick()
    {
        bool changed = false;
        for (auto &e : m_entries) {
            Operation &op = *e->op;
            Operation::Conflict c;
            if (!e->asking && op.conflict(c)) {
                if (e->replaceView) {
                    m_window->contentView()->removeSubview(e->replaceView);
                    e->replaceView = nullptr;
                    e->message = nullptr;
                }
                ask(*e, c);
            }
            if (e->titleText)
                e->titleText->setText(title(op));
            if (e->statusText)
                e->statusText->setText(status(op));
            if (e->bar) {
                const long long total = op.bytesTotal;
                e->bar->indeterminate = op.counting;
                e->bar->maxValue = 100;
                e->bar->value = total > 0 ? 100.0 * double(op.bytesDone) / double(total)
                                          : 100.0 * (1 - double(std::max(0, int(op.itemsLeft))) / std::max<size_t>(1, op.sources().size()));
                if (op.counting && !e->bar->animating)
                    e->bar->start();
                else if (!op.counting && e->bar->animating)
                    e->bar->stop();
                e->bar->setNeedsDisplay();
            }
            if (op.finished && !op.error().empty() && !op.cancelled && !e->failed) {
                e->failed = true;   // its error stays until the stop button is clicked
                show();
            }
        }
        // the ended ones go (an error stays until stopped)
        for (size_t i = 0; i < m_entries.size();) {
            Entry &e = *m_entries[i];
            if (e.op->finished && !e.failed) {
                if (Progress::onFinished)
                    Progress::onFinished(*e.op);
                m_window->contentView()->removeSubview(e.statusView);
                if (e.replaceView)
                    m_window->contentView()->removeSubview(e.replaceView);
                m_entries.erase(m_entries.begin() + long(i));
                changed = true;
            } else if (e.failed && e.op->cancelled) {
                e.failed = false;   // stopped: it goes next time
                ++i;
            } else {
                ++i;
            }
        }
        if (changed)
            layout();
        // shown when one has taken more than a second
        for (auto &e : m_entries)
            if (!m_shown && now() - e->op->started > 1.0)
                show();
        if (m_entries.empty()) {
            if (m_shown)
                m_window->close();
            m_shown = false;
            App::shared().removeTimer(m_timer);
            m_timer = 0;
        }
        if (m_window)
            m_window->contentView()->setNeedsDisplay();
    }

    void layout()
    {
        double y = 0;
        for (auto &e : m_entries) {
            View *v = e->asking && e->replaceView ? e->replaceView : e->statusView;
            v->setFrame({0, y, 399, v->frame().h});
            y += v->frame().h;
        }
        if (m_window && y > 0)
            m_window->setContentSize({399, y});
    }

    void show()
    {
        if (m_shown || !m_window)
            return;
        m_window->show();
        m_shown = true;
    }

    static void walk(View *v, const std::function<void(View *)> &f)
    {
        f(v);
        for (View *c : v->subviews())
            walk(c, f);
    }

    std::unique_ptr<Window> m_window;
    std::vector<std::unique_ptr<Entry>> m_entries;
    int m_timer = 0;
    bool m_shown = false;
};

} // namespace

void Progress::follow(std::shared_ptr<Operation> op) { CopyWindow::shared().add(std::move(op)); }

} // namespace Finder
