cmake_minimum_required(VERSION 3.20)
project(Zinkos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Code signing — Apple Development certificate required for HAL plugins.
# Auto-detect the first valid signing identity, or pass your own:
#   cmake -DCODESIGN_IDENTITY="your-cert-hash-or-name" ..
if(NOT DEFINED CODESIGN_IDENTITY OR CODESIGN_IDENTITY STREQUAL "")
    execute_process(
        COMMAND bash -c "security find-identity -v -p codesigning | grep 'Apple Development\\|Developer ID' | head -1 | sed 's/.*\\\"\\(.*\\)\\\".*/\\1/'"
        OUTPUT_VARIABLE CODESIGN_IDENTITY
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(CODESIGN_IDENTITY STREQUAL "")
        message(FATAL_ERROR
            "No Apple Development certificate found.\n"
            "You need a (free) Apple Developer account. Then:\n"
            "  1. Open Xcode → Settings → Accounts → add your Apple ID\n"
            "  2. Manage Certificates → + → Apple Development\n"
            "  3. Re-run cmake\n"
            "Or pass manually: cmake -DCODESIGN_IDENTITY=\"your cert name\" ..")
    endif()
endif()
message(STATUS "Code signing identity: ${CODESIGN_IDENTITY}")

# Determine Rust build profile and target directory
option(ZINKOS_RELEASE "Build Rust in release mode" OFF)
if(ZINKOS_RELEASE)
    set(RUST_PROFILE "release")
    set(CARGO_BUILD_FLAG "--release")
else()
    set(RUST_PROFILE "debug")
    set(CARGO_BUILD_FLAG "")
endif()

set(RUST_TARGET_DIR "${CMAKE_SOURCE_DIR}/target/${RUST_PROFILE}")
set(ZINKOS_ENGINE_LIB "${RUST_TARGET_DIR}/libzinkos_engine.dylib")

# Custom command to build the Rust engine
add_custom_command(
    OUTPUT ${ZINKOS_ENGINE_LIB}
    COMMAND cargo build ${CARGO_BUILD_FLAG} -p zinkos-engine
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building Rust engine (${RUST_PROFILE})"
    VERBATIM
)

add_custom_target(zinkos_engine_build DEPENDS ${ZINKOS_ENGINE_LIB})

# Build the driver
add_subdirectory(driver)

# Install target
set(DRIVER_INSTALL_DIR "/Library/Audio/Plug-Ins/HAL")
install(DIRECTORY "$<TARGET_BUNDLE_DIR:Zinkos>" DESTINATION "${DRIVER_INSTALL_DIR}")
