// SPDX-License-Identifier: LGPL-2.1-or-later
// A smart playlist's rules as iTunes keeps them (docs/itunes-reverse.md sections 11 and 31): the plist's
// "Smart Info" (FUN_00192dfa writes it, FUN_00193322 reads it: 0x70 bytes) and "Smart Criteria" (the SLst
// list: FUN_0037c336 writes it, FUN_0037c8ba reads it), both big-endian <data>.
#pragma once

#include "library.h"

#include <string>

namespace iTunes {

// Smart Info: [0] live updating, [1] the rules used (the Match box), [2] the limit, [3] its unit (1
// minutes, 2 MB, 3 items, 4 hours, 5 GB), [4..7] the order it selects by, [8..11] the limit's number,
// [12] match only checked items, [13] the order reversed (the "least" ones), [14] a flag kept as read
std::string smartInfoBlob(const SmartInfo &info);
bool readSmartInfo(const std::string &blob, SmartInfo &info);

// Smart Criteria: a 0x88-byte header ('SLst', 1, 1, the rule count at 8, byte 15 any rather than all),
// then per rule a 0x38-byte header (field, operator, a sub-list byte at 8, a disabled byte at 9, the
// data's length at 0x34) and its data, padded to an even length: UTF-16 big-endian text, a nested SLst,
// or a 0x44-byte numeric record (FUN_0037c268: six 64-bit values, from, a count, a multiplier, to, a
// count, a multiplier, then 20 bytes). A list read keeps its bytes (info.criteria) and is written back
// as it was while its rules are unchanged (nested lists and fields the port does not offer survive)
std::string smartCriteriaBlob(const SmartInfo &info);
bool readSmartCriteria(const std::string &blob, SmartInfo &info);

std::string base64Encode(const std::string &bytes);
std::string base64Decode(const std::string &text);

} // namespace iTunes
