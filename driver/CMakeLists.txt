cmake_minimum_required(VERSION 3.20)

# Driver bundle as a MODULE library (loaded by CoreAudio)
add_library(Zinkos MODULE
    ZinkosPlugin.cpp
    ZinkosDevice.cpp
)

add_dependencies(Zinkos zinkos_engine_build)

# Include the engine C header
target_include_directories(Zinkos PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/include
)

# Link against the Rust cdylib
target_link_libraries(Zinkos PRIVATE
    ${ZINKOS_ENGINE_LIB}
)

# macOS frameworks
target_link_libraries(Zinkos PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
)

# Make it a proper .driver bundle
set_target_properties(Zinkos PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    PREFIX ""
    SUFFIX ""
)

# Post-build: copy dylib into bundle and fix rpath
# Rust puts the install name under deps/ even though the symlink is one level up
set(ZINKOS_ENGINE_INSTALL_NAME "${RUST_TARGET_DIR}/deps/libzinkos_engine.dylib")

add_custom_command(TARGET Zinkos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_BUNDLE_DIR:Zinkos>/Contents/Frameworks"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ZINKOS_ENGINE_LIB}"
        "$<TARGET_BUNDLE_DIR:Zinkos>/Contents/Frameworks/libzinkos_engine.dylib"
    # Fix the dylib's own install name
    COMMAND install_name_tool -id
        "@loader_path/../Frameworks/libzinkos_engine.dylib"
        "$<TARGET_BUNDLE_DIR:Zinkos>/Contents/Frameworks/libzinkos_engine.dylib"
    # Fix the driver binary to find dylib via @loader_path
    COMMAND install_name_tool -change
        "${ZINKOS_ENGINE_INSTALL_NAME}"
        "@loader_path/../Frameworks/libzinkos_engine.dylib"
        "$<TARGET_BUNDLE_DIR:Zinkos>/Contents/MacOS/Zinkos"
    # Sign dylib first, then bundle (inner before outer)
    COMMAND codesign --force --timestamp --sign "${CODESIGN_IDENTITY}"
        "$<TARGET_BUNDLE_DIR:Zinkos>/Contents/Frameworks/libzinkos_engine.dylib"
    COMMAND codesign --force --timestamp --deep --sign "${CODESIGN_IDENTITY}"
        "$<TARGET_BUNDLE_DIR:Zinkos>"
    COMMENT "Assembling and signing Zinkos.driver bundle"
)
