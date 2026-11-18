// SPDX-License-Identifier: LGPL-2.1-or-later
// NSNib for AquaKit: a 10.6 compiled nib (keyedobjects.nib, a keyed archive: archive.h; or the older
// objects.nib, a typedstream: typedstream.cpp) made into
// AquaKit's views, as -[NSNib instantiateNibWithOwner:topLevelObjects:] makes Cocoa's. The nibs are the
// DVD's, read at run time; their code is not: a program connects to the controls by the outlets and
// actions the nib names.
//
//   IB.objectdata (NSIBObjectData): NSRoot the File's Owner, NSConnections its outlets
//   (NSNibOutletConnector: source, destination, label) and actions (NSNibControlConnector: the control,
//   its target, the selector); windows (NSWindowTemplate: NSWindowRect, NSWindowView the content)
//   views: NSFrame "{{x, y}, {w, h}}" from the superview's bottom left (flipped ones from the top),
//   NSSubviews, NSvFlags (0x80000000 hidden); a control's cell (NSCell) holds its look:
//   NSCellFlags 0x80000000 on, 0x10000000 editable, 0x00400000 bezeled, 0x00100000 one line;
//   NSCellFlags2 bits 26-28 the alignment (1 right, 2 centre), bits 17-18 the size (1 small);
//   buttons: bezel (NSButtonFlags2 & 7) | ((NSButtonFlags2 & 0x20) >> 2), NSAlternateImage NSSwitch /
//   NSRadioButton for check boxes and radio buttons
#pragma once

#include "archive.h"
#include "controls.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace AK {

class Nib {
public:
    // `resources`: the bundle's Resources folder (the nib's images are there)
    bool load(const std::string &file, const std::string &resources);
    bool isNull() const { return m_archive.isNull(); }

    // The view the owner's `outlet` window holds (a pane's "_window"), or the first window's; the
    // caller owns it. Its size is the window's content's.
    View *instantiate(const std::string &outlet = "_window");
    Size size() const { return m_size; }

    // The nib's windows (its sheets and panels besides the main one): each with the owner's outlet
    // that names it, its title and size, and the actions of its controls
    struct WindowEntry {
        int object = -1;
        std::string outlet, title;
        Size size;
        std::vector<std::string> actions;
    };
    std::vector<WindowEntry> windows() const;
    int outletObject(const std::string &name) const;   // the archived object an owner's outlet names
    std::vector<int> outletObjects(const std::string &name) const;   // every object's outlets of that name
    // A window's content, its controls added to controls() (the caller owns it)
    View *instantiateWindow(int object, Size *size);

    // After instantiate: the owner's outlets that are views, the controls and their actions
    View *outlet(const std::string &name) const;
    template <typename T> T *outlet(const std::string &name) const { return dynamic_cast<T *>(outlet(name)); }
    struct Connection {
        View *view;
        std::string action;   // the selector, "setMagnification:"
        int tag = 0;
        std::string title;
    };
    const std::vector<Connection> &controls() const { return m_controls; }
    std::vector<View *> withAction(const std::string &selector) const;
    View *withTitle(const std::string &title) const;
    View *withTag(int tag) const;

    // The views bound (NSNibBindingConnector) to a key path ending with `keyPath`, for that binding
    // ("value", "selectedIndex", "selectedValue", "hidden"...): the nib's controls by what they show
    std::vector<View *> bound(const std::string &keyPath, const std::string &binding = "value") const;
    template <typename T> T *bound1(const std::string &keyPath, const std::string &binding = "value") const
    {
        for (View *v : bound(keyPath, binding))
            if (auto *t = dynamic_cast<T *>(v))
                return t;
        return nullptr;
    }

    Image image(const std::string &name) const;   // a picture of the bundle (or System Preferences')

private:
    View *build(int obj, double parentHeight, bool parentFlipped);
    View *buildControl(int obj, const std::string &cls, const Rect &frame);
    void buildChildren(int obj, View *into, double height, bool flipped);
    Rect frameOf(int obj, double parentHeight, bool parentFlipped) const;
    Font fontOf(int cell) const;
    Color colorOf(int color, const Color &fallback) const;
    std::string classOf(int obj) const;   // NSClassSwapper: the class it stands for
    std::string customClassOf(int obj) const;
    void collectViews(int obj, std::vector<int> &out) const;

    Archive m_archive;
    std::string m_resources;
    int m_owner = -1;
    std::map<std::string, int> m_ownerOutlets;
    std::multimap<std::string, int> m_allOutlets;   // every object's outlets
    std::map<int, std::string> m_actions;   // control -> selector
    struct Binding {
        int source;
        std::string binding, keyPath;
    };
    std::vector<Binding> m_bindings;
    std::map<int, View *> m_views;          // archived object -> view
    std::vector<Connection> m_controls;
    Size m_size;
};

// "{{x, y}, {w, h}}", "{w, h}"
Rect parseRect(const std::string &s);

} // namespace AK
