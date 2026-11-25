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
# PIC: it goes into the Aqua style plugin too (src/linux/qtstyle)
set_target_properties(aquashell PROPERTIES POSITION_INDEPENDENT_CODE ON)
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
pkg_check_modules(APPKIT_DEPS REQUIRED IMPORTED_TARGET xcb xcb-keysyms xcb-cursor cairo-xcb xkbcommon)

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

# CoreServices: LaunchServices (the applications, opening them), on Linux's .desktop entries
aquakit_framework(CoreServices src/frameworks/CoreServices/launchservices.cpp src/frameworks/CoreServices/launchservices.h)
target_link_libraries(CoreServices PUBLIC CoreGraphics Foundation)

aquakit_framework(CoreUI src/frameworks/CoreUI/coreui.cpp src/frameworks/CoreUI/coreui.h)
target_link_libraries(CoreUI PUBLIC CoreGraphics Foundation)

aquakit_framework(AppKit
    src/frameworks/AppKit/appkit.cpp src/frameworks/AppKit/appkit.h
    src/frameworks/AppKit/controls.cpp src/frameworks/AppKit/controls.h
    src/frameworks/AppKit/toolbar.cpp src/frameworks/AppKit/toolbar.h
    src/frameworks/AppKit/mainmenu.cpp src/frameworks/AppKit/mainmenu.h
    src/frameworks/AppKit/nib.cpp src/frameworks/AppKit/nib.h
    src/frameworks/AppKit/textruleroptions.cpp src/frameworks/AppKit/textruleroptions.h
    src/frameworks/AppKit/pagelayout.cpp src/frameworks/AppKit/pagelayout.h
    src/frameworks/AppKit/fontpanel.cpp src/frameworks/AppKit/fontpanel.h
    src/frameworks/AppKit/colorpanel.cpp src/frameworks/AppKit/colorpanel.h
    src/frameworks/AppKit/attributedstring.cpp src/frameworks/AppKit/attributedstring.h
    src/frameworks/AppKit/textview.cpp src/frameworks/AppKit/textview.h
    src/frameworks/AppKit/panels.cpp src/frameworks/AppKit/panels.h
    src/frameworks/AppKit/rulerview.cpp src/frameworks/AppKit/rulerview.h
    src/frameworks/AppKit/platform.h src/frameworks/AppKit/platform_x11.cpp)
target_link_libraries(AppKit PUBLIC CoreUI CoreText CoreGraphics Foundation PkgConfig::APPKIT_DEPS)

add_library(aquakit INTERFACE)
target_link_libraries(aquakit INTERFACE AppKit CoreUI CoreText CoreServices CoreGraphics Foundation)

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
    # the status bar (the menu bar) as a layer: wlr-layer-shell (the window server's copy)
    set(LAYER_SHELL_XML "${CMAKE_SOURCE_DIR}/src/windowserver/wayland/protocols/wlr-layer-shell-unstable-v1.xml")
    add_custom_command(OUTPUT "${gen}/xdg-shell-client-protocol.h" "${gen}/xdg-shell-protocol.c"
                              "${gen}/wlr-layer-shell-unstable-v1-client-protocol.h" "${gen}/wlr-layer-shell-unstable-v1-protocol.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${gen}/xdg-shell-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${gen}/xdg-shell-protocol.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${LAYER_SHELL_XML}" "${gen}/wlr-layer-shell-unstable-v1-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${LAYER_SHELL_XML}" "${gen}/wlr-layer-shell-unstable-v1-protocol.c"
        DEPENDS "${XDG_SHELL_XML}" "${LAYER_SHELL_XML}")
    enable_language(C)
    target_sources(AppKit PRIVATE src/frameworks/AppKit/platform_wayland.cpp "${gen}/xdg-shell-protocol.c" "${gen}/xdg-shell-client-protocol.h"
                   "${gen}/wlr-layer-shell-unstable-v1-protocol.c" "${gen}/wlr-layer-shell-unstable-v1-client-protocol.h")
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

# iTunes.app (8.2.1), on AquaKit: its xMNU menus, its browser window and LCD from iTunes.rsrc, the
# library in ~/Music/iTunes/iTunes Music Library.xml, libmpv's playback, TagLib's tags
# (docs/itunes-reverse.md)
pkg_check_modules(ITUNES_DEPS IMPORTED_TARGET mpv taglib)
if(ITUNES_DEPS_FOUND)
    add_executable(iTunes src/itunes/main.cpp src/itunes/resources.cpp src/itunes/resources.h
                   src/itunes/library.cpp src/itunes/library.h src/itunes/media.cpp src/itunes/librarysources.cpp src/itunes/librarysources.h
                   src/itunes/podcasts.cpp src/itunes/podcasts.h src/itunes/podcastui.cpp src/itunes/podcastui.h
                   src/itunes/player.cpp src/itunes/player.h
                   src/itunes/store.cpp src/itunes/store.h src/itunes/frame.cpp src/itunes/frame.h
                   src/itunes/equalizer.cpp src/itunes/equalizer.h src/itunes/dialogs.cpp src/itunes/dialogs.h
                   src/itunes/getinfo.cpp src/itunes/getinfo.h
                   src/itunes/grid.cpp src/itunes/grid.h
                   src/itunes/smartplaylist.cpp src/itunes/smartplaylist.h
                   src/itunes/coverflow.cpp src/itunes/coverflow.h
                   src/itunes/viewoptions.cpp src/itunes/viewoptions.h
                   src/itunes/preferences.cpp src/itunes/preferences.h
                   src/itunes/visualizer.cpp src/itunes/visualizer.h
                   src/itunes/miniplayer11.cpp src/itunes/miniplayer11.h
                   src/itunes/upnext11.cpp src/itunes/upnext11.h)
    target_link_libraries(iTunes PRIVATE aquakit PkgConfig::ITUNES_DEPS)
    target_compile_options(iTunes PRIVATE -Wall -Wextra)
    set_target_properties(iTunes PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/iTunes.app/Contents/MacOS")
    # iTunes 11.4's Store: its pages in a process of their own (Qt's web engine, off screen), built
    # when Qt WebEngine is installed (docs/itunes-reverse.md, section 8)
    find_package(Qt6 6.4 QUIET COMPONENTS WebEngineWidgets Network)
    if(Qt6WebEngineWidgets_FOUND)
        add_executable(iTunesStore src/itunes/storeprocess.cpp)
        target_link_libraries(iTunesStore PRIVATE Qt6::WebEngineWidgets Qt6::Network)
        set_target_properties(iTunesStore PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/iTunes.app/Contents/MacOS")
    endif()
endif()

# Grab.app (Utilities), on AquaKit: Grab 1.5's panel, timer, documents, Inspector and Preferences; its
# grabs are screencapture's (docs/reverse-status.md, section 21)
add_executable(Grab src/grab/main.cpp)
target_link_libraries(Grab PRIVATE aquakit)
target_compile_options(Grab PRIVATE -Wall -Wextra)
set_target_properties(Grab PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Grab.app/Contents/MacOS")

# Activity Monitor.app (Utilities), on AquaKit: its window, tabs and charts, the processes and the system
# from /proc (docs/reverse-status.md, section 22)
add_executable(ActivityMonitor src/activitymonitor/main.cpp)
target_link_libraries(ActivityMonitor PRIVATE aquakit LinuxGlue)
target_compile_options(ActivityMonitor PRIVATE -Wall -Wextra)
set_target_properties(ActivityMonitor PROPERTIES AUTOMOC OFF OUTPUT_NAME "Activity Monitor"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Activity Monitor.app/Contents/MacOS")

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
                   src/photobooth/effects.cpp src/photobooth/effects.h src/photobooth/library.cpp src/photobooth/backdrops.cpp src/photobooth/library.h)
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

# SystemUIPlugin.framework on AquaKit: NSMenuExtra, NSMenuExtraView (src/systemuiplugin), a library of
# its own as the private framework is; the frameworks' symbols are SystemUIServer's (which links them
# whole and exports them), as AppKit is one copy in a process on the Mac
add_library(SystemUIPlugin SHARED src/systemuiplugin/systemuiplugin.cpp src/systemuiplugin/systemuiplugin.h)
target_include_directories(SystemUIPlugin PUBLIC src/systemuiplugin)
target_include_directories(SystemUIPlugin PRIVATE $<TARGET_PROPERTY:AppKit,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_options(SystemUIPlugin PRIVATE -Wall -Wextra)
set_target_properties(SystemUIPlugin PROPERTIES AUTOMOC OFF PREFIX "" SUFFIX "" OUTPUT_NAME SystemUIPlugin
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/PrivateFrameworks/SystemUIPlugin.framework")

# The Aqua style for the session's Linux Qt 6 applications (src/linux/qtstyle): AquaStyle as a style
# plugin, apps/Linux/qt6/styles/aquastyle.so; the session sets QT_PLUGIN_PATH and QT_STYLE_OVERRIDE=Aqua.
# aquashell inside it, its symbols hidden (it lives in other programs)
add_library(AquaStylePlugin MODULE src/linux/qtstyle/aquastyleplugin.cpp src/linux/qtstyle/aqua.json)
target_link_libraries(AquaStylePlugin PRIVATE aquashell)
target_compile_options(AquaStylePlugin PRIVATE -Wall -Wextra)
target_link_options(AquaStylePlugin PRIVATE "LINKER:--exclude-libs,ALL")
set_target_properties(AquaStylePlugin PROPERTIES PREFIX "" OUTPUT_NAME aquastyle CXX_VISIBILITY_PRESET hidden
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Linux/qt6/styles")

# The Wayland window server's private calls (src/linux/windowserverclient, sl_window_server_v1: the
# shell's CGS), for the Qt programs (X11:: on Wayland), LinuxGlue and AquaKit: wayland-client alone
set(WS_CLIENT_XML "${CMAKE_SOURCE_DIR}/src/windowserver/wayland/protocols/snowleopard-window-server-v1.xml")
if(AQUAKIT_WAYLAND_FOUND AND WAYLAND_SCANNER)
    set(wscgen "${CMAKE_BINARY_DIR}/windowserver-client")
    file(MAKE_DIRECTORY "${wscgen}")
    add_custom_command(OUTPUT "${wscgen}/snowleopard-window-server-v1-client-protocol.h" "${wscgen}/snowleopard-window-server-v1-protocol.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${WS_CLIENT_XML}" "${wscgen}/snowleopard-window-server-v1-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${WS_CLIENT_XML}" "${wscgen}/snowleopard-window-server-v1-protocol.c"
        DEPENDS "${WS_CLIENT_XML}")
    enable_language(C)
    add_library(WindowServerClient STATIC src/linux/windowserverclient.cpp src/linux/windowserverclient.h
                "${wscgen}/snowleopard-window-server-v1-protocol.c" "${wscgen}/snowleopard-window-server-v1-client-protocol.h")
    target_include_directories(WindowServerClient PUBLIC src/linux PRIVATE "${wscgen}")
    target_link_libraries(WindowServerClient PUBLIC PkgConfig::AQUAKIT_WAYLAND)
    target_compile_definitions(WindowServerClient PUBLIC SNOWLEOPARD_WAYLAND)
    set_target_properties(WindowServerClient PROPERTIES AUTOMOC OFF POSITION_INDEPENDENT_CODE ON)
    target_compile_options(WindowServerClient PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
    # the Qt programs: X11:: on the Wayland window server, their shell windows as layer surfaces
    find_package(LayerShellQt QUIET)
    if(LayerShellQt_FOUND)
        target_link_libraries(aquashell PUBLIC WindowServerClient LayerShellQt::Interface)
        message(STATUS "The shell's Qt programs: X11 and Wayland (LayerShellQt)")
    else()
        message(STATUS "The shell's Qt programs: X11 only (no LayerShellQt)")
    endif()
endif()

# Linux's glue, not a Mac framework (src/linux): the session's windows (EWMH) and D-Bus (sd-bus) on
# Foundation's run loop, for SystemUIServer (and the Dock, later)
pkg_check_modules(LINUXGLUE_DEPS REQUIRED IMPORTED_TARGET xcb libsystemd)
add_library(LinuxGlue STATIC src/linux/windows.cpp src/linux/windows.h src/linux/dbus.cpp src/linux/dbus.h)
target_include_directories(LinuxGlue PUBLIC src/linux)
target_link_libraries(LinuxGlue PUBLIC Foundation PkgConfig::LINUXGLUE_DEPS)
if(TARGET WindowServerClient)
    target_link_libraries(LinuxGlue PUBLIC WindowServerClient)
    if(AQUAKIT_WAYLAND_FOUND)
        target_link_libraries(AppKit PUBLIC WindowServerClient)
    endif()
endif()
target_compile_options(LinuxGlue PRIVATE -Wall -Wextra)
set_target_properties(LinuxGlue PROPERTIES AUTOMOC OFF)

# SystemUIServer.app on AquaKit: the menu bar and its extras (src/systemuiserver); the frameworks linked
# whole and exported, for SystemUIPlugin and the .menu bundles it loads. Desktop & Screen Saver's
# desktop picture (prefs/desktop.cpp) for the bar's filter.
add_executable(SystemUIServer src/systemuiserver/main.cpp src/systemuiserver/menubar.cpp src/systemuiserver/menubar.h
               src/systemuiserver/menutoolbar.cpp src/systemuiserver/menutoolbar.h
               src/systemuiserver/menubarfilter.cpp src/systemuiserver/menubarfilter.h
               src/systemuiserver/startup.cpp src/systemuiserver/startup.h src/systemuiserver/session.cpp src/systemuiserver/session.h
               src/systemuiserver/dbusmenu.cpp src/systemuiserver/dbusmenu.h
               src/systemuiserver/aquakitmenus.cpp src/systemuiserver/aquakitmenus.h
               src/systemuiserver/findermenus.cpp src/systemuiserver/findermenus.h
               src/systemuiserver/spotlight.cpp src/systemuiserver/spotlight.h
               src/systemuiserver/clock.cpp src/systemuiserver/clock.h src/systemuiserver/battery.cpp src/systemuiserver/battery.h
               src/systemuiserver/volume.cpp src/systemuiserver/volume.h
               src/systemuiserver/screencapture.cpp src/systemuiserver/screencapture.h
               src/prefs/desktop.cpp src/prefs/desktop.h src/prefs/catalog.cpp src/prefs/catalog.h)
target_include_directories(SystemUIServer PRIVATE src/systemuiserver src/prefs)
target_link_libraries(SystemUIServer PRIVATE SystemUIPlugin
                      "$<LINK_LIBRARY:WHOLE_ARCHIVE,AppKit,CoreUI,CoreText,CoreServices,CoreGraphics,Foundation,LinuxGlue>" ${CMAKE_DL_LIBS})
target_compile_options(SystemUIServer PRIVATE -Wall -Wextra)
set_target_properties(SystemUIServer PROPERTIES AUTOMOC OFF ENABLE_EXPORTS ON
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/SystemUIServer.app/Contents/MacOS"
                      BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/../../../PrivateFrameworks/SystemUIPlugin.framework")

# The menu extras with code of their own (Clock, Battery and Volume are SystemUIServer's), each a
# bundle SystemUIServer loads: Menu Extras/<Name>.menu/Contents/MacOS/<Name>. Built against the
# frameworks' headers only: their code is SystemUIServer's, one copy in the process as on the Mac.
function(menu_extra name)
    add_library(${name}Menu MODULE ${ARGN})
    target_include_directories(${name}Menu PRIVATE src/menuextras/${name})
    target_link_libraries(${name}Menu PRIVATE SystemUIPlugin "$<COMPILE_ONLY:AppKit>" "$<COMPILE_ONLY:LinuxGlue>")
    target_compile_options(${name}Menu PRIVATE -Wall -Wextra)
    set_target_properties(${name}Menu PROPERTIES AUTOMOC OFF PREFIX "" SUFFIX "" OUTPUT_NAME ${name}
                          LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Menu Extras/${name}.menu/Contents/MacOS"
                          BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/../../../../PrivateFrameworks/SystemUIPlugin.framework")
endfunction()
menu_extra(AirPort src/menuextras/AirPort/airport.cpp src/menuextras/AirPort/airport.h)
menu_extra(Bluetooth src/menuextras/Bluetooth/bluetooth.cpp src/menuextras/Bluetooth/bluetooth.h)
menu_extra(Eject src/menuextras/Eject/eject.cpp)
menu_extra(User src/menuextras/User/user.cpp)
menu_extra(Displays src/menuextras/Displays/displays.cpp)
menu_extra(TextInput src/menuextras/TextInput/textinput.cpp)
menu_extra(Spaces src/menuextras/Spaces/spaces.cpp)
menu_extra(TimeMachine src/menuextras/TimeMachine/timemachine.cpp)
menu_extra(UniversalAccess src/menuextras/UniversalAccess/universalaccess.cpp)
# Universal Access reads the X server's AccessX controls (XKB) on a connection of its own
pkg_check_modules(XCB_XKB REQUIRED IMPORTED_TARGET xcb-xkb)
target_link_libraries(UniversalAccessMenu PRIVATE PkgConfig::XCB_XKB)
# Script Menu.menu: its bundle and library named with a space
menu_extra(ScriptMenu src/menuextras/ScriptMenu/scriptmenu.cpp)
target_link_libraries(ScriptMenuMenu PRIVATE "$<COMPILE_ONLY:CoreServices>")
set_target_properties(ScriptMenuMenu PROPERTIES OUTPUT_NAME "Script Menu"
                      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Menu Extras/Script Menu.menu/Contents/MacOS")
menu_extra(ExpressCard src/menuextras/ExpressCard/expresscard.cpp)
menu_extra(WWAN src/menuextras/WWAN/wwan.cpp)
# PPP, PPPoE and VPN: NetworkMenusCommon.framework's AppleNetworkMenuExtra compiled into each
foreach(extra IN ITEMS VPN PPP PPPoE)
    string(TOLOWER ${extra} file)
    menu_extra(${extra} src/menuextras/${extra}/${file}.cpp src/menuextras/NetworkMenusCommon/networkmenu.cpp)
    target_include_directories(${extra}Menu PRIVATE src/menuextras/NetworkMenusCommon)
endforeach()

# Apple80211.framework's ClientDialogs.bundle: AirPort's panels from its nibs, on AquaKit; the AirPort
# menu extra starts it
add_executable(ClientDialogs src/privateframeworks/Apple80211/clientdialogs.cpp)
target_link_libraries(ClientDialogs PRIVATE aquakit)
target_compile_options(ClientDialogs PRIVATE -Wall -Wextra)
set_target_properties(ClientDialogs PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY
                      "${CMAKE_BINARY_DIR}/apps/PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle/Contents/MacOS")

# loginwindow's About This Mac, Log Out, Restart, Shut Down and Force Quit, on AquaKit (their nibs from
# the DVD)
add_executable(loginwindow src/loginwindow/main.cpp src/loginwindow/logout.cpp src/loginwindow/logout.h src/loginwindow/forcequit.cpp)
target_link_libraries(loginwindow PRIVATE aquakit LinuxGlue PkgConfig::XCB)
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

# Each component's Info.plist (its source folder's): Apple's identifier and version, and what it
# reproduces (SLReproducesSystem, SLReproducesBundle), so that each can be brought to a later release
# on its own; copied where the Mac has it
function(component_info source destination)
    configure_file("${CMAKE_SOURCE_DIR}/src/${source}/Info.plist" "${CMAKE_BINARY_DIR}/apps/${destination}/Info.plist" COPYONLY)
endfunction()

# Help Viewer.app: the help books' pages through Qt's web engine (WebKit's on the Mac); built when Qt
# WebEngine is installed
find_package(Qt6 6.4 QUIET COMPONENTS WebEngineWidgets PrintSupport)
if(Qt6WebEngineWidgets_FOUND AND Qt6PrintSupport_FOUND)
    mac_app(HelpViewer src/helpviewer/main.cpp)
    target_link_libraries(HelpViewer PRIVATE Qt6::WebEngineWidgets Qt6::PrintSupport)
    component_info(helpviewer "HelpViewer.app/Contents")
    message(STATUS "Help Viewer: Qt WebEngine")
else()
    message(STATUS "Help Viewer: not built (no Qt WebEngine)")
endif()

# The window manager: a program of its own, as the Mac's WindowServer is
pkg_check_modules(XCB_COMPOSITOR REQUIRED IMPORTED_TARGET xcb-composite xcb-damage xcb-render xcb-xfixes xcb-cursor)
add_executable(WindowServer src/windowserver/main.cpp src/windowserver/windowserver.cpp src/windowserver/windowserver.h
               src/windowserver/frame.cpp src/windowserver/frame.h
               src/windowserver/compositor.cpp src/windowserver/compositor.h src/windowserver/spaces.cpp)
target_link_libraries(WindowServer PRIVATE aquashell PkgConfig::XCB_COMPOSITOR)
target_compile_options(WindowServer PRIVATE -Wall -Wextra)
set_target_properties(WindowServer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps")

# /usr/sbin/screencapture: the screen shots, of the screen, a selection or a window (SystemUIServer's
# hot keys, Grab), to a file or the clipboard; a program of its own as the Mac's
# backupd.bundle: Time Machine's engine (docs/reverse-status.md, section 37); the menu extra starts it
add_executable(backupd src/backupd/main.cpp src/backupd/thinning.cpp src/backupd/thinning.h)
target_link_libraries(backupd PRIVATE Foundation)
target_compile_options(backupd PRIVATE -Wall -Wextra)
set_target_properties(backupd PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/backupd.bundle/Contents/Resources")

# /Applications/Time Machine.app: the launcher and the browser ("Enter Time Machine", Backup.framework's
# scene and the Finder's part of it: docs/timemachine-reverse.md, docs/timemachine-host.md), on Qt and
# legacy OpenGL; the menu extra and the Dock start it
find_package(Qt6 6.4 REQUIRED COMPONENTS OpenGL)
add_executable(TimeMachine src/timemachine/main.cpp src/timemachine/starfield.cpp src/timemachine/starfield.h
               src/timemachine/snapshots.cpp src/timemachine/snapshots.h
               src/windowserver/frame.cpp src/windowserver/frame.h)
target_include_directories(TimeMachine PRIVATE src/windowserver)
target_link_libraries(TimeMachine PRIVATE aquashell Qt6::OpenGL)
target_compile_options(TimeMachine PRIVATE -Wall -Wextra)
set_target_properties(TimeMachine PROPERTIES OUTPUT_NAME "Time Machine"
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Time Machine.app/Contents/MacOS")

add_executable(screencapture src/screencapture/main.cpp)
target_link_libraries(screencapture PRIVATE aquashell PkgConfig::XCB_COMPOSITOR)
target_compile_options(screencapture PRIVATE -Wall -Wextra)
set_target_properties(screencapture PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps")

component_info(dock "Dock.app/Contents")
component_info(finder "Finder.app/Contents")
component_info(systemuiserver "SystemUIServer.app/Contents")
component_info(loginwindow "loginwindow.app/Contents")
component_info(softwareupdate "Software Update.app/Contents")
component_info(prefs "System Preferences.app/Contents")
component_info(calculator "Calculator.app/Contents")
component_info(stickies "Stickies.app/Contents")
component_info(textedit "TextEdit.app/Contents")
component_info(grab "Grab.app/Contents")
if(ITUNES_DEPS_FOUND)
    component_info(itunes "iTunes.app/Contents")
endif()
component_info(activitymonitor "Activity Monitor.app/Contents")
component_info(timemachine "Time Machine.app/Contents")
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
foreach(extra IN ITEMS Clock Battery Volume AirPort Bluetooth Eject User Displays TextInput Spaces TimeMachine UniversalAccess VPN PPP PPPoE ExpressCard WWAN)
    component_info(menuextras/${extra} "Menu Extras/${extra}.menu/Contents")
endforeach()
component_info(menuextras/ScriptMenu "Menu Extras/Script Menu.menu/Contents")
component_info(systemuiplugin "PrivateFrameworks/SystemUIPlugin.framework/Resources")
component_info(privateframeworks/Apple80211 "PrivateFrameworks/Apple80211.framework/Resources/ClientDialogs.bundle/Contents")
foreach(framework IN ITEMS Foundation AppKit CoreGraphics CoreText)
    component_info(frameworks/${framework} "Frameworks/${framework}.framework/Resources")
endforeach()
component_info(frameworks/CoreUI "PrivateFrameworks/CoreUI.framework/Resources")

# The window server of the Wayland session (src/windowserver/wayland): a Wayland compositor on Qt
# Wayland Compositor and Qt Quick (docs/architecture.md says why), WindowServer's title bars, shadows
# and genie (frame.cpp) on Wayland, and an Xwayland of its own for the X11 programs. Its protocols'
# server code is generated here: xdg-shell and xdg-decoration (its own, not Qt's, which refuses the
# layer surfaces' popups), wlr-layer-shell, KDE's appmenu, and its private one for the shell
# (snowleopard-window-server-v1, the CGS calls). Their C symbols and Qt classes renamed (sl_,
# SLWaylandServer) so that they never meet Qt's own copies of xdg-shell.
find_package(Qt6 6.5 QUIET COMPONENTS WaylandCompositor Quick)
pkg_check_modules(WAYLAND_SERVER IMPORTED_TARGET wayland-server)
find_program(QT_WAYLAND_SCANNER qtwaylandscanner PATHS /usr/lib/qt6 /usr/lib/qt6/libexec /usr/libexec/qt6 ${QT6_INSTALL_PREFIX}/libexec)
set(QT_WAYLAND_PROTOCOLS /usr/share/qt6/wayland/protocols)
if(Qt6WaylandCompositor_FOUND AND Qt6Quick_FOUND AND WAYLAND_SERVER_FOUND AND WAYLAND_SCANNER AND QT_WAYLAND_SCANNER
   AND EXISTS "${QT_WAYLAND_PROTOCOLS}/xdg-shell/xdg-shell.xml")
    enable_language(C)
    set(wsgen "${CMAKE_BINARY_DIR}/windowserver-wayland")
    file(MAKE_DIRECTORY "${wsgen}")
    set(ws_protocol_sources)
    function(windowserver_protocol xml name)
        set(rename -e "s/\\b\\(z\\?xdg_[a-z0-9_]*_interface\\)\\b/sl_\\1/g"
                   -e "s/QtWaylandServer/SLWaylandServer/g")
        add_custom_command(OUTPUT "${wsgen}/wayland-${name}-server-protocol.h" "${wsgen}/${name}-protocol.c"
                                  "${wsgen}/qwayland-server-${name}.h" "${wsgen}/qwayland-server-${name}.cpp"
            COMMAND "${WAYLAND_SCANNER}" server-header "${xml}" "${wsgen}/wayland-${name}-server-protocol.h"
            COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${wsgen}/${name}-protocol.c"
            COMMAND "${QT_WAYLAND_SCANNER}" server-header "${xml}" > "${wsgen}/qwayland-server-${name}.h"
            COMMAND "${QT_WAYLAND_SCANNER}" server-code "${xml}" > "${wsgen}/qwayland-server-${name}.cpp"
            COMMAND sed -i ${rename} "${wsgen}/wayland-${name}-server-protocol.h" "${wsgen}/${name}-protocol.c"
                    "${wsgen}/qwayland-server-${name}.h" "${wsgen}/qwayland-server-${name}.cpp"
            DEPENDS "${xml}" VERBATIM)
        list(APPEND ws_protocol_sources "${wsgen}/${name}-protocol.c" "${wsgen}/qwayland-server-${name}.cpp"
                                        "${wsgen}/qwayland-server-${name}.h")
        set(ws_protocol_sources ${ws_protocol_sources} PARENT_SCOPE)
    endfunction()
    windowserver_protocol("${QT_WAYLAND_PROTOCOLS}/xdg-shell/xdg-shell.xml" xdg-shell)
    windowserver_protocol("${QT_WAYLAND_PROTOCOLS}/xdg-decoration/xdg-decoration-unstable-v1.xml" xdg-decoration-unstable-v1)
    windowserver_protocol("${QT_WAYLAND_PROTOCOLS}/appmenu/appmenu.xml" appmenu)
    windowserver_protocol("${CMAKE_SOURCE_DIR}/src/windowserver/wayland/protocols/wlr-layer-shell-unstable-v1.xml" wlr-layer-shell-unstable-v1)
    windowserver_protocol("${CMAKE_SOURCE_DIR}/src/windowserver/wayland/protocols/snowleopard-window-server-v1.xml" snowleopard-window-server-v1)
    pkg_check_modules(WINDOWSERVER_WAYLAND_DEPS REQUIRED IMPORTED_TARGET xcb xcb-composite xcb-icccm xcb-res xcb-cursor)
    add_executable(WindowServer-wayland
        src/windowserver/wayland/main.cpp
        src/windowserver/wayland/server.cpp src/windowserver/wayland/server.h
        src/windowserver/wayland/window.cpp src/windowserver/wayland/window.h
        src/windowserver/wayland/xdgshell.cpp src/windowserver/wayland/xdgshell.h
        src/windowserver/wayland/layershell.cpp src/windowserver/wayland/layershell.h
        src/windowserver/wayland/shellprotocol.cpp src/windowserver/wayland/shellprotocol.h
        src/windowserver/wayland/effects.cpp src/windowserver/wayland/effects.h
        src/windowserver/wayland/xwayland.cpp src/windowserver/wayland/xwayland.h
        src/windowserver/wayland/imageitem.h
        src/windowserver/frame.cpp src/windowserver/frame.h
        ${ws_protocol_sources})
    target_include_directories(WindowServer-wayland PRIVATE "${wsgen}" src/windowserver src/windowserver/wayland)
    target_link_libraries(WindowServer-wayland PRIVATE aquashell Qt6::WaylandCompositor Qt6::Quick PkgConfig::WAYLAND_SERVER
                          PkgConfig::WINDOWSERVER_WAYLAND_DEPS)
    target_compile_options(WindowServer-wayland PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
    set_target_properties(WindowServer-wayland PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps")
    message(STATUS "WindowServer: X11 and Wayland")
else()
    message(STATUS "WindowServer: X11 only (no Qt Wayland Compositor, Qt Quick, wayland-server or qtwaylandscanner)")
endif()
