// SPDX-License-Identifier: LGPL-2.1-or-later
// SUISStartupObject (SystemUIServer 10.6.8): which menu extras the bar starts with.
//
//   -menuExtrasForStartup (0x100014550): com.apple.systemuiserver's menuExtras (bundle paths, left to
//   right), then each of Autoload.plist's entries not already there nor in dontAutoLoad whose
//   can-load method answers yes, inserted at count + 1 + -mapPosition: of its id (0x10000d7e0: the ids
//   -10000…-10024 to -1…-25, from the right);
//   -_canLoadClass: (0x100004e02): the bundle's NSPrincipalClass, one of the 36 Apple's.
//
// The preferences are ~/.config/snowleopard/systemuiserver.conf's (MenuBar reads the same list).
#pragma once

#include <string>
#include <vector>

namespace Startup {

std::vector<std::string> menuExtrasForStartup();
// -_canLoadClass:
bool canLoadClass(const std::string &principalClass);
// -writeMenuBarPlugins: the list as the bar has it
void writeMenuExtras(const std::vector<std::string> &bundles);

} // namespace Startup
