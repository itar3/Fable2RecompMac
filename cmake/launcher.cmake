# "Fable 2.app": the launcher (tools/launcher), a settings window with a Play
# button that starts the game built here with the chosen options. It is built
# next to the game; Info.plist records the game, assets and shader dump
# folders, so the app can be moved to /Applications or the Dock.

# The extractor the launcher's "Install from ISO" runs (tools/sdk_tools/extract.cpp:
# disc images and Games on Demand packages), built next to the game.
add_executable(fable2_extract tools/sdk_tools/extract.cpp)
target_link_libraries(fable2_extract PRIVATE rex::runtime)
set_target_properties(fable2_extract PROPERTIES OUTPUT_NAME fable2-extract BUILD_RPATH "@executable_path")
add_dependencies(Fable2 fable2_extract)

# The program this build was recompiled from (generated/ and every guest
# address the renderer hooks): Fable II 0.0.1.33, a disc's default.xex with
# title update 1 applied. Its guest image has this XXH3-128 (fable2-extract
# --image-hash); the launcher installs a disc (and update) only if loading it
# gives this exact image. Every disc version checked so far (media IDs
# 05FBECEA and 716F0A0D) holds the same base program, and title update 1
# brings it to this image.
set(FABLE2_LAUNCHER_IMAGE_HASH "97bf3532efcd6bb2281dc1196172056b")
set(FABLE2_LAUNCHER_IMAGE_LABEL "Fable II 0.0.1.33 (a disc with title update 1)")

# SwiftUI's @State is a macro: the Command Line Tools' swiftc lacks its
# plugin, so Xcode's toolchain is used (through xcrun, which also finds the
# macOS SDK) when Xcode is installed.
set(FABLE2_XCODE_DEVELOPER_DIR "/Applications/Xcode.app/Contents/Developer" CACHE PATH
    "Xcode developer folder for building the launcher")
find_program(FABLE2_XCRUN xcrun)
if(NOT FABLE2_XCRUN)
    message(STATUS "fable2: xcrun not found; the launcher app is not built")
    return()
endif()
set(_fable2_swiftc "${FABLE2_XCRUN}" --sdk macosx swiftc)
if(EXISTS "${FABLE2_XCODE_DEVELOPER_DIR}")
    set(_fable2_swiftc ${CMAKE_COMMAND} -E env "DEVELOPER_DIR=${FABLE2_XCODE_DEVELOPER_DIR}" ${_fable2_swiftc})
endif()

set(FABLE2_LAUNCHER_APP "${CMAKE_BINARY_DIR}/Fable 2.app")
set(FABLE2_LAUNCHER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher/Fable2Launcher.swift")
set(FABLE2_LAUNCHER_EXE "${FABLE2_LAUNCHER_APP}/Contents/MacOS/Fable 2")
set(FABLE2_LAUNCHER_GAME_DIR "${CMAKE_BINARY_DIR}")
set(FABLE2_LAUNCHER_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(FABLE2_LAUNCHER_SHADER_DUMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shader_dump")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher/Info.plist.in"
               "${FABLE2_LAUNCHER_APP}/Contents/Info.plist" @ONLY)

add_custom_command(
    OUTPUT "${FABLE2_LAUNCHER_EXE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FABLE2_LAUNCHER_APP}/Contents/MacOS"
            "${FABLE2_LAUNCHER_APP}/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher/AppIcon.icns"
            "${FABLE2_LAUNCHER_APP}/Contents/Resources/AppIcon.icns"
    COMMAND ${_fable2_swiftc} -O -parse-as-library -target arm64-apple-macos14.0
            -o "${FABLE2_LAUNCHER_EXE}" "${FABLE2_LAUNCHER_SRC}"
    COMMAND codesign --force --sign - "${FABLE2_LAUNCHER_APP}"
    DEPENDS "${FABLE2_LAUNCHER_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher/Info.plist.in"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/launcher/AppIcon.icns"
    COMMENT "Building the Fable 2 launcher app"
    VERBATIM)
add_custom_target(fable2_launcher ALL DEPENDS "${FABLE2_LAUNCHER_EXE}")
