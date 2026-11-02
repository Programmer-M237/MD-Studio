# =============================================================================
#  MD Studio - a VS Code style Markdown editor built with Qt 6 Widgets
#  Build:   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# =============================================================================
cmake_minimum_required(VERSION 3.21)
project(MDStudio VERSION 1.0.0 LANGUAGES CXX)
if(WIN32)
    enable_language(RC)   # needed to compile the .exe icon resource below
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)   # embeds the math font (resources/resources.qrc)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---- Application icon --------------------------------------------------------
#  Windows : resources/icons/mdstudio.ico embedded into MDStudio.exe via an .rc file
#  macOS   : resources/icons/mdstudio.icns copied into the .app bundle
#  All     : PNGs embedded in resources.qrc and set as the window icon in main.cpp
set(MDSTUDIO_APP_ICON_SOURCES "")
if(WIN32)
    set(MDSTUDIO_ICO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mdstudio.ico")
    configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    list(APPEND MDSTUDIO_APP_ICON_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
elseif(APPLE)
    set(MDSTUDIO_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mdstudio.icns")
    set_source_files_properties("${MDSTUDIO_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    list(APPEND MDSTUDIO_APP_ICON_SOURCES "${MDSTUDIO_ICNS}")
endif()

add_executable(MDStudio WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/MainWindow.cpp      src/MainWindow.h
    src/MarkdownEditor.cpp  src/MarkdownEditor.h
    src/MarkdownHighlighter.cpp src/MarkdownHighlighter.h
    src/PreviewView.cpp     src/PreviewView.h
    src/Exporter.cpp        src/Exporter.h
    src/FindBar.cpp         src/FindBar.h
    src/QuickPick.cpp       src/QuickPick.h
    src/SettingsDialog.cpp  src/SettingsDialog.h
    src/Settings.cpp        src/Settings.h
    src/Theme.cpp           src/Theme.h
    src/MathRenderer.cpp    src/MathRenderer.h    src/MathGlyphData.h
    src/OutlineParser.h
    resources/resources.qrc
    src/ZipWriter.h
    ${MDSTUDIO_APP_ICON_SOURCES}
)
if(APPLE)
    set_target_properties(MDStudio PROPERTIES MACOSX_BUNDLE_ICON_FILE "mdstudio.icns")
endif()
target_link_libraries(MDStudio PRIVATE Qt6::Widgets)
if(MSVC)
    # Sources are UTF-8 (Unicode symbols in menus/exports); also use standard C++ conformance.
    target_compile_options(MDStudio PRIVATE /utf-8 /permissive- /W3)
endif()
target_compile_definitions(MDStudio PRIVATE MDSTUDIO_VERSION="${PROJECT_VERSION}")
