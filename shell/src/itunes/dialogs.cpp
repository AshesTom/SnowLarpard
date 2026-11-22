// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dialogs.h"

#include "controls.h"
#include "resources.h"

using namespace AK;

namespace iTunes {

void getString(std::unique_ptr<Window> &holder, int dlog, const std::string &title, const std::string &prompt,
               const std::string &text, std::function<void(std::string)> done)
{
    const Size size = dialogSize(dlog);
    holder = std::make_unique<Window>(Size{size.w > 0 ? size.w : 360, size.h > 0 ? size.h : 103}, title);
    Window *w = holder.get();
    w->setStyleMask(2);
    std::unique_ptr<Window> *slot = &holder;
    w->onClose = [slot, w] {
        App::shared().later([slot, w] {
            if (slot->get() == w)
                slot->reset();
        });
    };
    View *c = w->contentView();
    TextField *field = nullptr;
    Button *ok = nullptr, *cancel = nullptr;
    int n = 0;
    for (const DialogItem &it : dialogItems(dlog)) {
        ++n;
        if (n == 1 || n == 2) {
            Button *b = c->add<Button>(it.text);
            b->setFrame(it.rect);
            (n == 1 ? ok : cancel) = b;
        } else if (n == 3) {
            c->add<Label>(prompt, Font::system(13))->setFrame(it.rect);
        } else if (n == 4) {
            field = c->add<TextField>();
            field->setFrame(Rect{it.rect.x - 3, it.rect.y - 3, it.rect.w + 6, it.rect.h + 6});
        }
    }
    if (!ok || !cancel || !field)
        return;
    ok->isDefault = true;
    field->setText(text);
    field->selectAll();
    auto finish = [w, field, done] {
        const std::string t = field->text;
        w->close();
        if (done)
            done(t);
    };
    ok->action = finish;
    field->onEnter = finish;
    cancel->action = [w] { w->close(); };
    field->onCancel = [w] { w->close(); };
    w->show();
    w->makeFirstResponder(field);
}

} // namespace iTunes
