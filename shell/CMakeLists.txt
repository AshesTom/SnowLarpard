# SPDX-License-Identifier: LGPL-2.1-or-later
# Snow Leopard's shell as Linux programs (shell/build.sh builds them into .local/build): no Mac
# code; your own 10.6 DVD only gives them resources (tools/extract.py).
cmake_minimum_required(VERSION 3.21)
project(snowleopard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets DBus Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-keysyms xcb-shape)

# What the programs share; each is a program of its own, as Dock.app and SystemUIServer.app are.
add_library(aquashell STATIC
    src/common/apps.cpp src/common/apps.h
    src/common/aqua.cpp src/common/aqua.h
    src/common/aquastyle.cpp src/common/aquastyle.h
    src/common/art.cpp src/common/art.h
    src/common/artfile.cpp src/common/artfile.h
    src/common/assets.cpp src/common/assets.h
    src/common/quartztext.h
    src/common/desktopsettings.cpp src/common/desktopsettings.h
    src/common/session.cpp src/common/session.h
    src/common/x11.cpp src/common/x11.h)
target_include_directories(aquashell PUBLIC src/common)
target_link_libraries(aquashell PUBLIC Qt6::Widgets PkgConfig::XCB)
target_compile_options(aquashell PRIVATE -Wall -Wextra)

# name -> name.app/Contents/MacOS/name, laid out as on the Mac
function(mac_app name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE aquashell)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/${name}.app/Contents/MacOS")
endfunction()

mac_app(Dock src/dock/main.cpp src/dock/dock.cpp src/dock/dock.h src/dock/dockmenu.cpp src/dock/dockmenu.h
        src/dock/stack.cpp src/dock/stack.h)
# SystemUIPlugin.framework: NSMenuExtra and what the menu extras share, a library of its own (as the
# private framework is) used by SystemUIServer and by the .menu bundles it loads
add_library(SystemUIPlugin SHARED src/systemuiplugin/menuextra.cpp src/systemuiplugin/menuextra.h)
target_include_directories(SystemUIPlugin PUBLIC src/systemuiplugin src/common)
target_link_libraries(SystemUIPlugin PUBLIC Qt6::Widgets)
target_compile_definitions(SystemUIPlugin PRIVATE SYSTEMUIPLUGIN_LIBRARY)
target_compile_options(SystemUIPlugin PRIVATE -Wall -Wextra)
set_target_properties(SystemUIPlugin PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME SystemUIPlugin
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/PrivateFrameworks/SystemUIPlugin.framework")

# SystemUIServer.app: the menu bar. It hands the shell's shared code (aquashell, whole) to the bundles
# it loads, as a bundle loader does; its libraries found beside it ($ORIGIN), wherever install.sh
# copies the programs
add_executable(SystemUIServer src/menubar/main.cpp src/menubar/menubar.cpp src/menubar/menubar.h
        src/menubar/spotlight.cpp src/menubar/spotlight.h src/menubar/appmenu.cpp src/menubar/appmenu.h
        src/menubar/menubarfilter.cpp src/menubar/menubarfilter.h src/menubar/findermenus.cpp src/menubar/findermenus.h
        src/menubar/aquakitmenus.cpp src/menubar/aquakitmenus.h)
target_link_libraries(SystemUIServer PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,aquashell>" SystemUIPlugin Qt6::DBus Qt6::Network
                      PkgConfig::XCB ${CMAKE_DL_LIBS})
target_compile_options(SystemUIServer PRIVATE -Wall -Wextra)
set_target_properties(SystemUIServer PROPERTIES ENABLE_EXPORTS ON
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/SystemUIServer.app/Contents/MacOS"
                      BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/../../../PrivateFrameworks/SystemUIPlugin.framework")

# The menu extras, each a bundle SystemUIServer loads: Menu Extras/<Name>.menu/Contents/MacOS/<Name>
function(menu_extra name)
    add_library(${name}Menu MODULE ${ARGN})
    target_link_libraries(${name}Menu PRIVATE SystemUIPlugin)
    target_include_directories(${name}Menu PRIVATE src/menuextras/${name})
    target_compile_options(${name}Menu PRIVATE -Wall -Wextra)
    set_target_properties(${name}Menu PROPERTIES PREFIX "" SUFFIX "" OUTPUT_NAME ${name}
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Menu Extras/${name}.menu/Contents/MacOS"
                          BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/../../../../PrivateFrameworks/SystemUIPlugin.framework")
endfunction()
menu_extra(Clock src/menuextras/Clock/clock.cpp src/menuextras/Clock/clock.h)
menu_extra(Battery src/menuextras/Battery/battery.cpp src/menuextras/Battery/battery.h)
menu_extra(Volume src/menuextras/Volume/volume.cpp src/menuextras/Volume/volume.h)
menu_extra(AirPort src/menuextras/AirPort/airport.cpp src/menuextras/AirPort/airport.h)
target_link_libraries(AirPortMenu PRIVATE Qt6::DBus)
menu_extra(Bluetooth src/menuextras/Bluetooth/bluetooth.cpp src/menuextras/Bluetooth/bluetooth.h)
mac_app(Finder src/finder/main.cpp src/finder/desktop.cpp src/finder/desktop.h)
pkg_check_modules(XCB_SAVER REQUIRED IMPORTED_TARGET xcb-screensaver)
target_link_libraries(Finder PRIVATE PkgConfig::XCB_SAVER)

# AquaKit: the Mac's frameworks, without Qt (cairo, pango), on Wayland and X11, one reimplementation
# of each, shared by the programs as on the Mac, each its own library so that it can follow macOS's
# releases on its own (src/frameworks/<Framework>):
#   Foundation     Assets (the DVD's files), .strings, Settings, NSKeyedUnarchiver and NSUnarchiver,
#                  NSRunLoop and NSTask (runloop)
#   CoreGraphics   images and their colour (Quartz's drawing, ImageIO's readers, CGPDF's pages)
#   CoreText       text as Quartz draws it with font smoothing, the DVD's fonts (CTFontManager)
#   CoreUI         the controls', menus' and windows' art (ArtFile.bin, SArtFile.bin)
#   AppKit         windows, views, controls, menus, toolbars, nibs, NSTextView and RTF / RTFD
#                  (NSAttributedString's additions); the display (X11, Wayland)
# `aquakit` is their umbrella, as Cocoa.framework is Foundation's and AppKit's.
function(aquakit_framework name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC src/frameworks/${name})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    set_target_properties(${name} PROPERTIES AUTOMOC OFF)
endfunction()
pkg_check_modules(COREGRAPHICS_DEPS REQUIRED IMPORTED_TARGET cairo libturbojpeg libtiff-4 lcms2 zlib poppler-glib)
pkg_check_modules(CORETEXT_DEPS REQUIRED IMPORTED_TARGET pangocairo fontconfig)
pkg_check_modules(APPKIT_DEPS REQUIRED IMPORTED_TARGET xcb xcb-keysyms cairo-xcb xkbcommon)

aquakit_framework(Foundation
    src/frameworks/Foundation/foundation.cpp src/frameworks/Foundation/foundation.h
    src/frameworks/Foundation/archive.cpp src/frameworks/Foundation/archive.h
    src/frameworks/Foundation/typedstream.cpp
    src/frameworks/Foundation/runloop.cpp src/frameworks/Foundation/runloop.h)

aquakit_framework(CoreGraphics src/frameworks/CoreGraphics/graphics.cpp src/frameworks/CoreGraphics/graphics.h)
target_link_libraries(CoreGraphics PUBLIC Foundation PkgConfig::COREGRAPHICS_DEPS)

aquakit_framework(CoreText src/frameworks/CoreText/text.cpp src/frameworks/CoreText/text.h)
target_include_directories(CoreText PRIVATE src/common)   # quartztext.h, shared with the Qt programs
target_link_libraries(CoreText PUBLIC CoreGraphics Foundation PkgConfig::CORETEXT_DEPS)

aquakit_framework(CoreUI src/frameworks/CoreUI/coreui.cpp src/frameworks/CoreUI/coreui.h)
target_link_libraries(CoreUI PUBLIC CoreGraphics Foundation)

aquakit_framework(AppKit
    src/frameworks/AppKit/appkit.cpp src/frameworks/AppKit/appkit.h
    src/frameworks/AppKit/controls.cpp src/frameworks/AppKit/controls.h
    src/frameworks/AppKit/toolbar.cpp src/frameworks/AppKit/toolbar.h
    src/frameworks/AppKit/mainmenu.cpp src/frameworks/AppKit/mainmenu.h
    src/frameworks/AppKit/nib.cpp src/frameworks/AppKit/nib.h
    src/frameworks/AppKit/attributedstring.cpp src/frameworks/AppKit/attributedstring.h
    src/frameworks/AppKit/textview.cpp src/frameworks/AppKit/textview.h
    src/frameworks/AppKit/panels.cpp src/frameworks/AppKit/panels.h
    src/frameworks/AppKit/rulerview.cpp src/frameworks/AppKit/rulerview.h
    src/frameworks/AppKit/platform.h src/frameworks/AppKit/platform_x11.cpp)
target_link_libraries(AppKit PUBLIC CoreUI CoreText CoreGraphics Foundation PkgConfig::APPKIT_DEPS)

add_library(aquakit INTERFACE)
target_link_libraries(aquakit INTERFACE AppKit CoreUI CoreText CoreGraphics Foundation)

# Wayland: xdg-shell's XML from wayland-protocols (or the copy Qt ships), turned into C by wayland-scanner
pkg_check_modules(AQUAKIT_WAYLAND IMPORTED_TARGET wayland-client wayland-cursor)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner)
find_file(XDG_SHELL_XML NAMES xdg-shell.xml
          PATHS "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell" /usr/share/wayland-protocols/stable/xdg-shell
                /usr/share/qt6/wayland/protocols/xdg-shell NO_DEFAULT_PATH)
if(AQUAKIT_WAYLAND_FOUND AND WAYLAND_SCANNER AND XDG_SHELL_XML)
    set(gen "${CMAKE_BINARY_DIR}/wayland")
    file(MAKE_DIRECTORY "${gen}")
    add_custom_command(OUTPUT "${gen}/xdg-shell-client-protocol.h" "${gen}/xdg-shell-protocol.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${gen}/xdg-shell-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${gen}/xdg-shell-protocol.c"
        DEPENDS "${XDG_SHELL_XML}")
    enable_language(C)
    target_sources(AppKit PRIVATE src/frameworks/AppKit/platform_wayland.cpp "${gen}/xdg-shell-protocol.c" "${gen}/xdg-shell-client-protocol.h")
    target_include_directories(AppKit PRIVATE "${gen}")
    target_link_libraries(AppKit PUBLIC PkgConfig::AQUAKIT_WAYLAND)
    target_compile_definitions(AppKit PRIVATE AQUAKIT_WAYLAND)
    message(STATUS "AquaKit: Wayland and X11 (${XDG_SHELL_XML})")
else()
    message(STATUS "AquaKit: X11 only (no wayland-client, wayland-scanner or xdg-shell.xml)")
endif()

# System Preferences.app (a name with a space: the target has none), on AquaKit
add_executable(SystemPreferences src/prefs/main.cpp src/prefs/desktoppane.cpp src/prefs/desktoppane.h
               src/prefs/desktop.cpp src/prefs/desktop.h src/prefs/slideshow.cpp src/prefs/slideshow.h
               src/prefs/catalog.cpp src/prefs/catalog.h src/prefs/showall.cpp src/prefs/showall.h
               src/prefs/nibpane.cpp src/prefs/nibpane.h src/prefs/linuxpanes.cpp)
target_link_libraries(SystemPreferences PRIVATE aquakit)
target_compile_options(SystemPreferences PRIVATE -Wall -Wextra)
set_target_properties(SystemPreferences PROPERTIES OUTPUT_NAME "System Preferences" AUTOMOC OFF
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/System Preferences.app/Contents/MacOS")

# Preview.app, on AquaKit: PDFs drawn by poppler (its cairo glue), pictures by AquaKit
pkg_check_modules(POPPLER IMPORTED_TARGET poppler-glib)
if(POPPLER_FOUND)
    add_executable(Preview src/preview/main.cpp src/preview/document.cpp src/preview/document.h
                   src/preview/pdfview.cpp src/preview/pdfview.h src/preview/sidebar.cpp src/preview/sidebar.h
                   src/preview/plist.cpp src/preview/plist.h)
    target_link_libraries(Preview PRIVATE aquakit PkgConfig::POPPLER)
    target_compile_options(Preview PRIVATE -Wall -Wextra)
    set_target_properties(Preview PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Preview.app/Contents/MacOS")
else()
    message(STATUS "Preview: not built (no poppler-glib)")
endif()

# Calculator.app, on AquaKit: Calculator 4.5's algebraic engine and its Basic and Scientific keys
# (docs/reverse-status.md, section 15)
add_executable(Calculator src/calculator/main.cpp src/calculator/engine.cpp src/calculator/engine.h
               src/calculator/programmer.cpp src/calculator/programmer.h)
target_link_libraries(Calculator PRIVATE aquakit)
target_compile_options(Calculator PRIVATE -Wall -Wextra)
set_target_properties(Calculator PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Calculator.app/Contents/MacOS")

# Stickies.app, on AquaKit: Stickies 7.0's database, notes, colours, Arrange, Find, Import and Export
# (docs/reverse-status.md, section 16)
add_executable(Stickies src/stickies/main.cpp src/stickies/database.cpp src/stickies/database.h)
target_link_libraries(Stickies PRIVATE aquakit)
target_compile_options(Stickies PRIVATE -Wall -Wextra)
set_target_properties(Stickies PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Stickies.app/Contents/MacOS")

# TextEdit.app, on AquaKit: TextEdit 1.6's documents, rich and plain text, its window, menus and
# preferences (docs/reverse-status.md, section 18)
add_executable(TextEdit src/textedit/main.cpp)
target_link_libraries(TextEdit PRIVATE aquakit)
target_compile_options(TextEdit PRIVATE -Wall -Wextra)
set_target_properties(TextEdit PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/TextEdit.app/Contents/MacOS")

# Mail.app, on AquaKit: IMAP and SMTP by libcurl, the passwords in the keyring by libsecret
# (docs/mail-reverse.md); Preview's reader of NeXT plists for its *.toolbar files
pkg_check_modules(MAIL IMPORTED_TARGET libcurl libsecret-1)
if(MAIL_FOUND)
    add_executable(Mail src/mail/main.cpp src/mail/mime.cpp src/mail/mime.h src/mail/store.cpp src/mail/store.h
                   src/mail/net.cpp src/mail/net.h src/mail/textview.cpp src/mail/textview.h
                   src/preview/plist.cpp src/preview/plist.h)
    target_include_directories(Mail PRIVATE src/preview)
    target_link_libraries(Mail PRIVATE aquakit PkgConfig::MAIL)
    target_compile_options(Mail PRIVATE -Wall -Wextra)
    set_target_properties(Mail PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Mail.app/Contents/MacOS")
else()
    message(STATUS "Mail: not built (no libcurl or libsecret-1)")
endif()

# Photo Booth.app, on AquaKit: the camera through Video4Linux, its effects (the system's compositions)
# done on the processor (docs/reverse-status.md, section 14)
include(CheckIncludeFile)
check_include_file(linux/videodev2.h HAVE_V4L2)
if(HAVE_V4L2)
    add_executable(PhotoBooth src/photobooth/main.cpp src/photobooth/camera.cpp src/photobooth/camera.h
                   src/photobooth/effects.cpp src/photobooth/effects.h src/photobooth/library.cpp src/photobooth/library.h)
    target_link_libraries(PhotoBooth PRIVATE aquakit)
    target_compile_options(PhotoBooth PRIVATE -Wall -Wextra)
    set_target_properties(PhotoBooth PROPERTIES AUTOMOC OFF OUTPUT_NAME "Photo Booth"
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Photo Booth.app/Contents/MacOS")
else()
    message(STATUS "Photo Booth: not built (no linux/videodev2.h)")
endif()

# Software Update, on AquaKit (its nibs from the DVD, the updates from Apple's catalog through
# tools/softwareupdate.py)
add_executable(SoftwareUpdate src/softwareupdate/main.cpp)
target_link_libraries(SoftwareUpdate PRIVATE aquakit)
target_compile_options(SoftwareUpdate PRIVATE -Wall -Wextra)
set_target_properties(SoftwareUpdate PROPERTIES AUTOMOC OFF OUTPUT_NAME "Software Update"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Software Update.app/Contents/MacOS")

# Apple80211.framework's ClientDialogs.bundle: AirPort's panels from its nibs, on AquaKit; the AirPort
# menu extra starts it
add_executable(ClientDialogs src/privateframeworks/Apple80211/clientdialogs.cpp)
target_link_libraries(ClientDialogs PRIVATE aquakit)
target_compile_options(ClientDialogs PRIVATE -Wall -Wextra)
set_target_properties(ClientDialogs PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY
                      "${CMAKE_BINARY_DIR}/apps/PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle/Contents/MacOS")

# loginwindow's About This Mac, on AquaKit (its nib from the DVD)
add_executable(loginwindow src/loginwindow/main.cpp)
target_link_libraries(loginwindow PRIVATE aquakit)
target_compile_options(loginwindow PRIVATE -Wall -Wextra)
set_target_properties(loginwindow PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/loginwindow.app/Contents/MacOS")

# The Finder's windows, on AquaKit (the desktop stays the Qt Finder's): FinderWindows in Finder.app,
# its windows named "Finder"
add_executable(FinderWindows src/finder/browser/main.cpp src/finder/browser/files.cpp src/finder/browser/files.h
               src/finder/browser/iconview.cpp src/finder/browser/iconview.h src/finder/browser/sidebar.cpp
               src/finder/browser/sidebar.h src/finder/browser/listview.cpp src/finder/browser/listview.h
               src/finder/browser/columnview.cpp src/finder/browser/columnview.h
               src/finder/browser/flowview.cpp src/finder/browser/flowview.h
               src/finder/browser/dsstore.cpp src/finder/browser/dsstore.h
               src/finder/browser/viewoptions.cpp src/finder/browser/viewoptions.h
               src/finder/browser/infowindow.cpp src/finder/browser/infowindow.h
               src/finder/browser/fileops.cpp src/finder/browser/fileops.h src/finder/browser/progress.cpp
               src/finder/browser/progress.h src/finder/browser/quicklook.cpp src/finder/browser/quicklook.h
               src/finder/browser/goto.cpp src/finder/browser/goto.h
               src/finder/browser/search.cpp src/finder/browser/search.h
               src/finder/browser/labels.cpp src/finder/browser/labels.h
               src/finder/browser/preferences.cpp src/finder/browser/preferences.h)
target_link_libraries(FinderWindows PRIVATE aquakit)
# the desktop's icons (Finder.app's Qt desktop) draw with the Finder windows' own code: the items, their
# icons from CoreTypes, the Desktop folder's .DS_Store, the labels
target_sources(Finder PRIVATE src/finder/desktopicons.cpp src/finder/desktopicons.h src/finder/browser/files.cpp
               src/finder/browser/dsstore.cpp src/finder/browser/labels.cpp)
target_include_directories(Finder PRIVATE src/finder/browser)
target_link_libraries(Finder PRIVATE aquakit Qt6::Network)
add_dependencies(Finder FinderWindows)

if(POPPLER_FOUND)
    # Quick Look's PDFs: the first page
    target_link_libraries(FinderWindows PRIVATE PkgConfig::POPPLER)
    target_compile_definitions(FinderWindows PRIVATE FINDER_POPPLER)
endif()
target_compile_options(FinderWindows PRIVATE -Wall -Wextra)
set_target_properties(FinderWindows PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Finder.app/Contents/MacOS")

# The window manager: a program of its own, as the Mac's WindowServer is
pkg_check_modules(XCB_COMPOSITOR REQUIRED IMPORTED_TARGET xcb-composite xcb-damage xcb-render xcb-xfixes)
add_executable(WindowServer src/windowserver/main.cpp src/windowserver/windowserver.cpp src/windowserver/windowserver.h
               src/windowserver/compositor.cpp src/windowserver/compositor.h)
target_link_libraries(WindowServer PRIVATE aquashell PkgConfig::XCB_COMPOSITOR)
target_compile_options(WindowServer PRIVATE -Wall -Wextra)
set_target_properties(WindowServer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps")

# Each component's Info.plist (its source folder's): Apple's identifier and version, and what it
# reproduces (SLReproducesSystem, SLReproducesBundle), so that each can be brought to a later release
# on its own; copied where the Mac has it
function(component_info source destination)
    configure_file("${CMAKE_SOURCE_DIR}/src/${source}/Info.plist" "${CMAKE_BINARY_DIR}/apps/${destination}/Info.plist" COPYONLY)
endfunction()
component_info(dock "Dock.app/Contents")
component_info(finder "Finder.app/Contents")
component_info(menubar "SystemUIServer.app/Contents")
component_info(loginwindow "loginwindow.app/Contents")
component_info(softwareupdate "Software Update.app/Contents")
component_info(prefs "System Preferences.app/Contents")
component_info(calculator "Calculator.app/Contents")
component_info(stickies "Stickies.app/Contents")
component_info(textedit "TextEdit.app/Contents")
foreach(app IN ITEMS Preview Mail PhotoBooth)
    if(TARGET ${app})
        string(TOLOWER ${app} dir)
        set(bundle ${app})
        if(app STREQUAL "PhotoBooth")
            set(bundle "Photo Booth")
        endif()
        component_info(${dir} "${bundle}.app/Contents")
    endif()
endforeach()
foreach(extra IN ITEMS Clock Battery Volume AirPort Bluetooth)
    component_info(menuextras/${extra} "Menu Extras/${extra}.menu/Contents")
endforeach()
component_info(systemuiplugin "PrivateFrameworks/SystemUIPlugin.framework/Resources")
component_info(privateframeworks/Apple80211 "PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle/Contents")
foreach(framework IN ITEMS Foundation AppKit CoreGraphics CoreText)
    component_info(frameworks/${framework} "Frameworks/${framework}.framework/Resources")
endforeach()
component_info(frameworks/CoreUI "PrivateFrameworks/CoreUI.framework/Resources")
