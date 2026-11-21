// SPDX-License-Identifier: LGPL-2.1-or-later
// NSPageLayout (File ▸ Page Setup…), as 10.6 shows it: PrintingPrivate's PrintCocoaUI.bundle
// (CocoaPageSetupWindow.nib, PMPrintWindowController) holding the Page Attributes panel of
// PrintingCocoaPDEs.bundle (PageAttributes.nib, PMPageAttributes), reversed from their x86_64 code:
//   - Settings: Page Attributes, then Save as Default (the page format kept as the default one)
//   - Format For: Any Printer (no printers here); Paper Size: the generic printer's papers
//     (PrintCore's Generic.ppd: US Letter, US Legal, A4, B5, JIS B5 and their Small kinds), then
//     Manage Custom Sizes… (not here); under it PMPaperCreatePaperSizeString's
//     "%d.%02d by %d.%02d inches" (centimetres where the locale is metric)
//   - Orientation: Portrait or Landscape (-saveValuesAndReturnError: 0x9e89: the matrix's column)
//   - Scale: a whole percentage; out of 3…10000 OK alerts "The value in the “Scale” field is out of
//     range." "Please enter a number between 1 and 10000." (-putUpPageRangeAlert:range: 0x3f9f)
//   - the sheet as tall as the panel needs (the PDE view's place grown to it)
#pragma once

#include "appkit.h"

#include <functional>
#include <string>

namespace AK {

// NSPrintInfo's page format: the paper (portrait, points), its orientation and the scale
struct PrintInfo {
    std::string paperName = "Letter";   // the PPD's name ("Letter", "A4", "JISB5")
    double paperWidth = 612, paperHeight = 792;
    bool landscape = false;
    double scale = 100;                  // percent
    // the page as laid out: the paper turned for landscape
    double pageWidth() const { return landscape ? paperHeight : paperWidth; }
    double pageHeight() const { return landscape ? paperWidth : paperHeight; }
};

namespace PageLayout {
// The page format saved by Save as Default, else the locale's paper (A4 or US Letter…)
PrintInfo defaultPrintInfo();
// -[NSPageLayout beginSheetWithPrintInfo:modalForWindow:…]: done(true, the new format) on OK
void beginSheet(Window *window, const PrintInfo &info, std::function<void(bool, const PrintInfo &)> done);
}

} // namespace AK
