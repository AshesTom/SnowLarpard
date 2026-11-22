// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's string dialogs (FUN_002f055e): Get String (DLOG 10002, a one-line field) and Get Long
// String (10003, a field 64 high), chosen by the call's flag 1; item 1 OK (the default), 2 Cancel, 3 the
// prompt, 4 the field with its text.
#pragma once

#include "appkit.h"

#include <functional>
#include <memory>
#include <string>

namespace iTunes {

// The dialog in `holder` (a window of its own, replacing the one there); done(the text) on OK
void getString(std::unique_ptr<AK::Window> &holder, int dlog, const std::string &title, const std::string &prompt,
               const std::string &text, std::function<void(std::string)> done);

} // namespace iTunes
