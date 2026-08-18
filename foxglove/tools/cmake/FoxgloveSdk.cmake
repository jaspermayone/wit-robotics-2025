# Downloads the prebuilt Foxglove C++ SDK for the host platform and exposes it
# as the `foxglove_cpp` target.
#
# The SDK ships one archive per platform. Each archive contains the prebuilt
# Rust/C core library plus the C++ wrapper sources, which are compiled here
# against the host toolchain.
#
# To move to a new SDK version:
#   1. Change FOXGLOVE_SDK_VERSION below.
#   2. Refresh every hash:
#      gh api repos/foxglove/foxglove-sdk/releases/tags/sdk/v<version> \
#        --jq '.assets[] | "\(.name) \(.digest)"'

include(FetchContent)

set(FOXGLOVE_SDK_VERSION "0.26.0" CACHE STRING "Foxglove SDK version to download")

# Map the host platform to a release asset plus its SHA256.
if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_fg_target "aarch64-apple-darwin")
        set(_fg_sha256 "aae2ccfebcce4b6ff72e366912a92d6576f6100e79cdf71d79e4288977add0f0")
    else()
        set(_fg_target "x86_64-apple-darwin")
        set(_fg_sha256 "4e592b4fe447b9fab912706f863a75833cf6bedc5c17d3ea474d4531eec722ae")
    endif()
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(_fg_target "aarch64-pc-windows-msvc")
        set(_fg_sha256 "aaafd94b67ce9de06eea616f247e49c0b11a2f78cf26b7cb1e1ae45f264a4067")
    else()
        set(_fg_target "x86_64-pc-windows-msvc")
        set(_fg_sha256 "335fcaa4d2637a529d6a47739feb944defcd393b9ccdb8a2fe2901747dbff863")
    endif()
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_fg_target "aarch64-unknown-linux-gnu")
        set(_fg_sha256 "0710f2ec5abc3954acf6203b93a96768dccd19d742af9b78fcd8fbbba5f6225a")
    else()
        set(_fg_target "x86_64-unknown-linux-gnu")
        set(_fg_sha256 "8fb03b061127788ad708918d186d74d4fdb58718b11cff97c9d605513ab57cc6")
    endif()
else()
    message(FATAL_ERROR "No Foxglove SDK archive is published for this platform.")
endif()

set(_fg_url
    "https://github.com/foxglove/foxglove-sdk/releases/download/sdk%2Fv${FOXGLOVE_SDK_VERSION}/foxglove-v${FOXGLOVE_SDK_VERSION}-cpp-${_fg_target}.zip")

message(STATUS "Foxglove SDK v${FOXGLOVE_SDK_VERSION} for ${_fg_target}")

FetchContent_Declare(
    foxglove_sdk
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL "${_fg_url}"
    URL_HASH "SHA256=${_fg_sha256}"
)
FetchContent_MakeAvailable(foxglove_sdk)

# The archive unpacks into a single `foxglove/` directory holding the CMake
# package config at lib/cmake/foxglove-sdk/.
find_package(foxglove-sdk CONFIG REQUIRED
    HINTS "${foxglove_sdk_SOURCE_DIR}/foxglove" "${foxglove_sdk_SOURCE_DIR}")

# STATIC keeps the core library inside the executables, so the tools run
# without an rpath or a loose shared library next to them.
foxglove_sdk_add_cpp_library(foxglove_cpp TYPE STATIC REMOTE_ACCESS OFF)

# The SDK's own platform links declare Security and CoreFoundation, but the
# prebuilt static library also carries the Rust `sysinfo` crate, which calls
# into IOKit. Add it here so the link step resolves those symbols.
if(APPLE)
    target_link_libraries(foxglove_cpp INTERFACE "-framework IOKit")
endif()

unset(_fg_target)
unset(_fg_sha256)
unset(_fg_url)
