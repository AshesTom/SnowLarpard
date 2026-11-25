// SPDX-License-Identifier: LGPL-2.1-or-later
// iTunes 8.2.1's CD dialogs (docs/itunes-reverse.md section 32), built from their DITLs: Import Settings
// (DLOG 183) with its encoders' Custom… dialogs (AAC Encoder 5000, MP3 Encoding Options 3000, AIFF/WAV
// Encoding Options 3250), and Burn Settings (185).
#pragma once

#include "appkit.h"
#include "burn.h"
#include "cdimport.h"

#include <functional>
#include <memory>

namespace iTunes {

// Import Settings: the encoder, its setting, error correction; kept on OK
void importSettings(std::unique_ptr<AK::Window> &holder, std::function<void()> done = {});

// Burn Settings: the burner (searched for on showing), the speed, the format, the gap, Sound Check,
// CD Text; burn(settings) on Burn
void burnSettings(std::unique_ptr<AK::Window> &holder, const std::string &playlist, std::function<void(const BurnSettings &)> burn);

} // namespace iTunes
