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
mac_app(SystemUIServer src/menubar/main.cpp src/menubar/menubar.cpp src/menubar/menubar.h
        src/menubar/extras.cpp src/menubar/extras.h src/menubar/appmenu.cpp src/menubar/appmenu.h
        src/menubar/menubarfilter.cpp src/menubar/menubarfilter.h
        src/menubar/aquakitmenus.cpp src/menubar/aquakitmenus.h)
target_link_libraries(SystemUIServer PRIVATE Qt6::DBus Qt6::Network)
mac_app(Finder src/finder/main.cpp src/finder/desktop.cpp src/finder/desktop.h)
pkg_check_modules(XCB_SAVER REQUIRED IMPORTED_TARGET xcb-screensaver)
target_link_libraries(Finder PRIVATE PkgConfig::XCB_SAVER)

# AquaKit: AppKit and CoreUI for the Mac applications, without Qt (cairo, pango), on Wayland and X11
pkg_check_modules(AQUAKIT REQUIRED IMPORTED_TARGET xcb xcb-keysyms cairo cairo-xcb pangocairo fontconfig xkbcommon libturbojpeg libtiff-4)
add_library(aquakit STATIC
    src/aquakit/appkit.cpp src/aquakit/appkit.h
    src/aquakit/archive.cpp src/aquakit/archive.h
    src/aquakit/nib.cpp src/aquakit/nib.h
    src/aquakit/mainmenu.cpp src/aquakit/mainmenu.h
    src/aquakit/controls.cpp src/aquakit/controls.h
    src/aquakit/coreui.cpp src/aquakit/coreui.h
    src/aquakit/foundation.cpp src/aquakit/foundation.h
    src/aquakit/graphics.cpp src/aquakit/graphics.h
    src/aquakit/platform.h src/aquakit/platform_x11.cpp
    src/aquakit/text.cpp src/aquakit/text.h
    src/aquakit/toolbar.cpp src/aquakit/toolbar.h
    src/aquakit/typedstream.cpp)
target_include_directories(aquakit PUBLIC src/aquakit)
target_include_directories(aquakit PRIVATE src/common)   # quartztext.h, shared with the Qt programs
target_link_libraries(aquakit PUBLIC PkgConfig::AQUAKIT)
target_compile_options(aquakit PRIVATE -Wall -Wextra)
set_target_properties(aquakit PROPERTIES AUTOMOC OFF)

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
    target_sources(aquakit PRIVATE src/aquakit/platform_wayland.cpp "${gen}/xdg-shell-protocol.c" "${gen}/xdg-shell-client-protocol.h")
    target_include_directories(aquakit PRIVATE "${gen}")
    target_link_libraries(aquakit PUBLIC PkgConfig::AQUAKIT_WAYLAND)
    target_compile_definitions(aquakit PRIVATE AQUAKIT_WAYLAND)
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
               src/finder/browser/progress.h)
target_link_libraries(FinderWindows PRIVATE aquakit)
target_compile_options(FinderWindows PRIVATE -Wall -Wextra)
set_target_properties(FinderWindows PROPERTIES AUTOMOC OFF RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps/Finder.app/Contents/MacOS")

# The window manager: a program of its own, as the Mac's WindowServer is
pkg_check_modules(XCB_COMPOSITOR REQUIRED IMPORTED_TARGET xcb-composite xcb-damage xcb-render xcb-xfixes)
add_executable(WindowServer src/windowserver/main.cpp src/windowserver/windowserver.cpp src/windowserver/windowserver.h
               src/windowserver/compositor.cpp src/windowserver/compositor.h)
target_link_libraries(WindowServer PRIVATE aquashell PkgConfig::XCB_COMPOSITOR)
target_compile_options(WindowServer PRIVATE -Wall -Wextra)
set_target_properties(WindowServer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/apps")
