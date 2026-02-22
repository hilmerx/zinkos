cmake_minimum_required(VERSION 3.20)
project(Zinkos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Code signing identity — Apple Development certificate required for HAL plugins
# Code signing — use certificate SHA-1 hash to avoid shell quoting issues
set(CODESIGN_IDENTITY "EC58FBCD3313F422A4FE36E5B5951E5A226A5748" CACHE STRING "Codesign identity (SHA-1 hash)")

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
