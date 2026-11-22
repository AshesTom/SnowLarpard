// SPDX-License-Identifier: LGPL-2.1-or-later
// NSColorPanel (Format ▸ Font ▸ Show Colors, ⇧⌘C), as AppKit-1038 lays it out in NSColorPanel.nib
// (205 × 322 under its toolbar): the magnifier and the colour well at the top, the picker's box,
// Opacity's slider and field, the swatches under the separator. The toolbar has the five pickers'
// pictures from AppKit's atlas (Color Wheel, Color Sliders, Color Palettes, Image Palettes, Crayons);
// the colour wheel is the picker here:
//   - NSColorPickerWheel.colorPicker's NSColorPickerWheel.nib: the wheel (169 × 192 in its bezel)
//     and the brightness slider beside it (20 × 196, its track the colour from black to full,
//     -[NSColorScaleSliderCell setDrawsTrackAsColorScaleType:])
//   - the wheel: the bundle's NSColorWheelImage.tiff drawn in the largest square, black over it at
//     1 − the brightness (-_compositeImage 0x334549); a point's hue its angle from the right,
//     counter-clockwise, its saturation its distance from the centre over the radius
//     (-_brightColorFromPoint:fullBrightness: 0x333d5e, -_pointFromColor: 0x3339af); the current
//     point marked
//   - Opacity 0 to 100 %
//   - the swatches (NSColorSwatch): chips 13 points apart; a click on one takes its colour, a colour
//     dragged from the well onto one keeps it there (kept in the shell's settings)
// The colour goes to the panel's target at once (NSColorPanel's action, changeColor:).
#pragma once

#include "appkit.h"

#include <functional>

namespace AK {

namespace ColorPanel {
// -orderFrontColorPanel:
void show();
bool isVisible();
Color color();
void setColor(const Color &c);
// Who gets the colour (the first responder's changeColor:, or the Font panel's effects bar while
// it asked for it): replaced by each who opens the panel
void setTarget(std::function<void(const Color &)> changed);
}

} // namespace AK
