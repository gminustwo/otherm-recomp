cmake_minimum_required(VERSION 3.25)

# Dawn's pinned macOS artifacts target 12.0. Set the same floor before project()
# initializes the Apple toolchain so direct developer CMake invocations cannot
# accidentally inherit the running SDK's deployment version. This cache entry
# is harmless on non-Apple platforms and remains overridable by a caller.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Minimum macOS version supported" FORCE)
endif()
project(otherm_recomp)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit Clang toolchain is required")
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES OTHERM_OSX_ARCHITECTURE_COUNT)
    if(OTHERM_OSX_ARCHITECTURE_COUNT GREATER 1)
        message(FATAL_ERROR
            "One macOS architecture per build directory; "
            "configure separate arm64 and x86_64 build directories")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 _otherm_osx_arch)
    set(CMAKE_SYSTEM_PROCESSOR "${_otherm_osx_arch}" CACHE STRING "Target processor architecture" FORCE)
    set(CMAKE_SYSTEM_PROCESSOR "${_otherm_osx_arch}")
endif()

if(WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(OTHERM_PLATFORM_WINDOWS TRUE)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|arm64|ARM64)$")
    set(OTHERM_PLATFORM_MACOS TRUE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        set(OTHERM_PLATFORM_MACOS_X86_64 TRUE)
    else()
        set(OTHERM_PLATFORM_MACOS_ARM64 TRUE)
    endif()
elseif(ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    # Cross-built with the NDK. The game is a shared library (libmain.so) that SDL's Android
    # activity loads; see cmake/PublicProducts.cmake.
    set(OTHERM_PLATFORM_ANDROID TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|aarch64|arm64|ARM64)$")
    set(OTHERM_PLATFORM_LINUX TRUE)
else()
    message(FATAL_ERROR
        "Supported: 64-bit LLVM-MinGW Clang on Windows, native Linux x86_64/aarch64, "
        "Apple Clang on macOS x86_64/arm64, or the NDK for Android arm64-v8a")
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "Only Release builds are supported")
endif()

option(OTHERM_BUILD_PRODUCTS "Build the game (the translated product targets)" ON)
option(OTHERM_BUILD_PSQ_TESTS "Build focused PSQ ISA tests" OFF)

# Preprocessor definitions that belong to this project's own code (the runtime,
# the translated shards and the product glue) and to nothing else. They are
# applied directory-scoped, immediately after the aurora add_subdirectory() call,
# so every target created afterwards (otherm_runtime_common, the translated shard
# libraries and the products in cmake/PublicProducts.cmake) inherits them while
# aurora and its third-party tree stay unaffected.
set(OTHERM_PROJECT_COMPILE_DEFINITIONS NOMINMAX)

set(CMAKE_CXX_STANDARD 17)

# The two Crypto++ configuration macros are load-bearing for ABI: they shape the
# structs and inline code the headers generate, so a first-party TU including
# <cryptopp/...> must see exactly what the archive was compiled with.
set(OTHERM_CRYPTOPP_INTERFACE_DEFINITIONS CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)

# Riivolution XML is parsed with pugixml, matching the parser used by Dolphin.
# The four upstream files from v1.15 (ee86beb30e4973f5feffe3ce63bfa4fbadf72f38)
# are vendored so builds remain usable with FETCHCONTENT_FULLY_DISCONNECTED.
add_library(otherm_pugixml STATIC third_party/pugixml/pugixml.cpp)
add_library(otherm::pugixml ALIAS otherm_pugixml)
target_include_directories(otherm_pugixml PUBLIC third_party/pugixml)
target_compile_features(otherm_pugixml PUBLIC cxx_std_17)
set_target_properties(otherm_pugixml PROPERTIES UNITY_BUILD OFF)

# POSIX x86-64 guest-fiber scheduling (runtime/src/host_context.cpp) needs a symmetric
# stackful-coroutine primitive to stand in for Win32 Fibers. libco's co_switch() transfers
# directly to any other created coroutine, matching SwitchToFiber's semantics exactly (unlike
# asymmetric resume/yield coroutine libraries, which would need every call site restructured).
# Vendored from upstream (higan-emu/libco @ e18e09d, 2019-10-16, ISC license; valgrind.h is
# separately BSD-style licensed, see third_party/libco/LICENSE). Windows keeps native Fibers;
# Apple Silicon uses the project's x18-safe AArch64 assembly backend, while Intel macOS uses
# libco's existing System V AMD64 backend.
if(OTHERM_PLATFORM_LINUX OR OTHERM_PLATFORM_MACOS_X86_64)
    add_library(otherm_libco STATIC third_party/libco/libco.c)
    add_library(otherm::libco ALIAS otherm_libco)
    target_include_directories(otherm_libco PUBLIC third_party/libco)
    set_target_properties(otherm_libco PROPERTIES UNITY_BUILD OFF)
endif()

# Runtime configuration is real TOML, parsed by toml11 rather than a project-
# specific line parser. Keep it header-only and vendored so disconnected release
# builds have exactly the same parser as developer builds.
add_library(otherm_toml11 INTERFACE)
add_library(otherm::toml11 ALIAS otherm_toml11)
target_include_directories(otherm_toml11 SYSTEM INTERFACE third_party/toml11)
target_compile_features(otherm_toml11 INTERFACE cxx_std_17)

# Wii ES signatures use the standard binary-field sect233r1 curve. Crypto++
# supplies that curve, SHA-1, ECDSA and an OS-seeded CSPRNG. The complete 8.9.0
# source distribution is vendored. Assembly and algorithm-specific SIMD units
# are disabled to keep the library portable and independent of the runtime's
# x86-64-v3 policy; the static linker retains only the algorithms we reference.
file(GLOB OTHERM_CRYPTOPP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/third_party/cryptopp/*.cpp")
list(FILTER OTHERM_CRYPTOPP_SOURCES EXCLUDE REGEX
    "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
list(FILTER OTHERM_CRYPTOPP_SOURCES EXCLUDE REGEX
    "(_avx|_ppc|_simd|_sse)\\.cpp$")
add_library(otherm_cryptopp STATIC ${OTHERM_CRYPTOPP_SOURCES})
add_library(otherm::cryptopp ALIAS otherm_cryptopp)
target_include_directories(otherm_cryptopp SYSTEM PUBLIC third_party)
target_compile_features(otherm_cryptopp PUBLIC cxx_std_17)
target_compile_definitions(otherm_cryptopp PUBLIC ${OTHERM_CRYPTOPP_INTERFACE_DEFINITIONS})
target_compile_options(otherm_cryptopp PRIVATE -w)
set_target_properties(otherm_cryptopp PROPERTIES UNITY_BUILD OFF)
if(OTHERM_PLATFORM_ANDROID)
    # On Android Crypto++ asks the NDK's cpufeatures helper which ARM extensions exist.
    set(OTHERM_NDK_CPUFEATURES_DIR "${ANDROID_NDK}/sources/android/cpufeatures")
    target_sources(otherm_cryptopp PRIVATE "${OTHERM_NDK_CPUFEATURES_DIR}/cpu-features.c")
    target_include_directories(otherm_cryptopp PRIVATE "${OTHERM_NDK_CPUFEATURES_DIR}")
endif()

# TLS for non-Windows guest network HLE (runtime/src/hle/net/network_ssl.cpp) - the Windows path
# uses Schannel (a Windows-only OS API), which has no equivalent on Linux/Android, so this project
# needs its own TLS library there. mbed TLS was chosen over OpenSSL specifically because it cross-
# compiles cleanly for Android with nothing beyond a plain C toolchain (no perl/asm build-script
# dependency the way OpenSSL's build has), matching how this project already prefers toolchain-
# simple libraries (see Crypto++ above, similarly stripped of ASM/SIMD for portability).
# The build fetches a pinned upstream tarball with a checked SHA-256.
# Bump both values in cmake/MbedTLSPin.cmake when updating; the hash comes from
# upstream's signed `mbedtls-<version>-sha256sum.txt` release asset.
#
# The alias exists on every platform so the link lines in cmake/PublicProducts.cmake stay
# platform-independent, but it is only populated where network_ssl.cpp actually compiles the mbed
# TLS path (`#ifndef _WIN32`). Windows keeps Schannel and must not fetch anything: its builds run
# with FETCHCONTENT_FULLY_DISCONNECTED=ON against the offline dependency set prepared by
# tools/fetch-dependencies.ps1, so an unconditional fetch would fail a clean configure there
# and would also add a dependency Windows never links.
add_library(otherm_mbedtls INTERFACE)
add_library(otherm::mbedtls ALIAS otherm_mbedtls)
if(NOT OTHERM_PLATFORM_WINDOWS)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/MbedTLSPin.cmake")
    include(FetchContent)
    FetchContent_Declare(otherm_mbedtls_upstream
        URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${OTHERM_MBEDTLS_VERSION}/mbedtls-${OTHERM_MBEDTLS_VERSION}.tar.bz2"
        URL_HASH SHA256=${OTHERM_MBEDTLS_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    # Subproject mode already disables testing and codegen; it still enables programs.
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(otherm_mbedtls_upstream)
    target_link_libraries(otherm_mbedtls INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

set(OTHERM_TRANSLATED_COMPILE_JOBS 0 CACHE STRING
    "Cap on concurrently compiling translated shard TUs via a Ninja job pool (0 = uncapped). \
Scheduling only - never affects output bytes, so it is deliberately outside the canonical flag fingerprint.")
set(OTHERM_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Optional self-contained C++/WinRT include directory")

set(OTHERM_AURORA_DIR "${CMAKE_CURRENT_LIST_DIR}/../aurora")

# The translator's explicit float casts are PPC single-precision rounding points.
# Fast-math may erase them and change guest-visible integer conversions.
set(OTHERM_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)

# kpad.cpp derives the accelerometer values the game reads (magnitude, speed, acc_vertical) in
# native float math, under the translated code's FP policy.
set(OTHERM_PPC_SEMANTIC_RUNTIME_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/fpu_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/hle/input/kpad.cpp")
set_source_files_properties(${OTHERM_PPC_SEMANTIC_RUNTIME_SOURCES} PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "${OTHERM_TRANSLATED_PPC_FP_OPTIONS}")
# The sampling profiler includes <windows.h>; keep it out of the unity batches.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/guest_profiler.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# Other M's button prompts compile stb_truetype (static functions, STBTT_ macros); keep them out of the unity batches.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/button_icons.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# Match the optimization policy of the translated callers as well as their FP policy.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp" APPEND PROPERTY
    COMPILE_OPTIONS -O2 -fno-slp-vectorize)

# ----------------------------------------------------------------------
# Third-party: aurora (provides SDL3 + GPU backends)
# ----------------------------------------------------------------------
if(NOT EXISTS ${OTHERM_AURORA_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "aurora is missing: ${OTHERM_AURORA_DIR}")
endif()
set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
if(OTHERM_PLATFORM_WINDOWS)
    set(DAWN_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER ON CACHE BOOL "" FORCE)
elseif(OTHERM_PLATFORM_MACOS)
    set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
else()
    set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
    set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
    set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
endif()
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Provide a tiny stub for DXProgrammableCapture when the SDK/PIX headers are
# missing (common on MinGW). Dawn only includes the header; no symbols are
# referenced when PIX isn't present.
if(OTHERM_PLATFORM_WINDOWS)
    set(OTHERM_DX_STUB_DIR "${CMAKE_BINARY_DIR}/aurora_dx_stubs")
    if(NOT EXISTS "${OTHERM_DX_STUB_DIR}/DXProgrammableCapture.h")
        file(MAKE_DIRECTORY ${OTHERM_DX_STUB_DIR})
        file(WRITE "${OTHERM_DX_STUB_DIR}/DXProgrammableCapture.h"
"#pragma once\n// Stubbed PIX capture header for Dawn; no functionality when PIX isn't present.\n")
    endif()
endif()
# Deliberately NOT injected project-wide. Only a from-source Dawn build ever
# includes DXProgrammableCapture.h, and this tree consumes Dawn as a prebuilt
# package (AURORA_DAWN_PROVIDER=package), so the old
# include_directories()/add_compile_options(-I...) pair only added a dead
# -I to every one of the ~500 third-party and runtime compile lines. The
# aurora_* targets that do want it get it from the
# target_include_directories() call further down; a from-source Dawn build
# should add it to the Dawn targets the same way.

# Keep SDL3 lean when aurora is enabled.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)

# MinGW/WSL can choke on SDL precompiled headers; disable PCH for aurora/SDL.
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
# Isolate aurora build products to a subdirectory under the main build dir.
add_subdirectory(${OTHERM_AURORA_DIR} ${CMAKE_BINARY_DIR}/aurora-build EXCLUDE_FROM_ALL)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

# Keep upstream Aurora split: several sources intentionally use file-local
# names that collide under this repo's aggressive unity build settings.
set(AURORA_TARGETS aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi)
foreach(t ${AURORA_TARGETS})
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES UNITY_BUILD OFF)
        target_compile_options(${t} PRIVATE -O3 -ffast-math -w -pipe)
        if(OTHERM_PLATFORM_WINDOWS)
            target_include_directories(${t} PRIVATE ${OTHERM_DX_STUB_DIR})
        endif()
        # Aurora's own sources include Windows headers and call std::min/max;
        # they relied on the old project-wide NOMINMAX that no longer leaks
        # into this subtree, so the define is applied per target here.
        target_compile_definitions(${t} PRIVATE NOMINMAX)
    endif()
endforeach()

# Frame interpolation filters guest NaN/Inf matrices out of a std::sort
# comparator, which makes its std::isfinite guards load-bearing for memory
# safety, not just match quality. -ffinite-math-only (part of -ffast-math
# above) folds those guards to constants and poisons NaN float arguments
# via nofpclass, so this one file opts back out of the finite-math
# assumption; the file #errors if built without this. Source-scoped
# COMPILE_OPTIONS append after the target options, so this overrides the
# target's -ffast-math for exactly this TU.
if(TARGET aurora_gx)
    set_source_files_properties("${OTHERM_AURORA_DIR}/lib/gx/frame_interpolation.cpp"
        TARGET_DIRECTORY aurora_gx
        PROPERTIES COMPILE_OPTIONS "-fno-finite-math-only")
endif()

# Everything from here down is this project's own code, so this is where the
# project definitions enter the directory scope. A directory-scoped definition
# seeds the COMPILE_DEFINITIONS of targets as they are created, which means:
#   * aurora and its third-party tree were added above -> unaffected;
#   * otherm_runtime_common, the translated shard libraries and the products in
#     cmake/PublicProducts.cmake are all created below (that file is include()d,
#     not add_subdirectory()d, so it shares this directory scope) -> they pick
#     the definitions up exactly as before.
if(OTHERM_PROJECT_COMPILE_DEFINITIONS)
    add_compile_definitions(${OTHERM_PROJECT_COMPILE_DEFINITIONS})
endif()

# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build (measured
# at ~55 ms across the 100+ globs this build already re-verifies for SDL), and it
# buys correctness: without it, a newly added src/*.cpp that is not referenced by
# a registration file is silently never compiled and never errors. The stale-glob
# failure mode is worth far more than the milliseconds.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
if(OTHERM_PLATFORM_MACOS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory.cpp")
    if(OTHERM_PLATFORM_MACOS_ARM64)
        # HostContext's Apple Silicon backend is implemented in a small assembly
        # companion. It must be part of the product runtime as well as the
        # standalone context test; otherwise the final executable is missing
        # otherm_co_init/otherm_co_switch at link time.
        enable_language(ASM)
        list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
    endif()
else()
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
    if(OTHERM_PLATFORM_ANDROID)
        # Android shares HostContext's AArch64 assembly backend with Apple Silicon, in its ELF
        # spelling (and without touching x18, the thread's shadow-call-stack register).
        enable_language(ASM)
        list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/android/co_switch.S")
    endif()
endif()
# src/android/: the app's side of the Android build (the Java activity's JNI calls, the disc image read through nod).
if(NOT OTHERM_PLATFORM_ANDROID)
    list(FILTER SOURCES EXCLUDE REGEX "/src/android/")
endif()
file(GLOB OTHERM_ANDROID_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/android/*.cpp")
set_source_files_properties(${OTHERM_ANDROID_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
# nod's C library (github.com/encounter/nod, MIT / Apache-2.0), built by tools/build-android.ps1: the app sets the game
# up from the disc image the system's picker hands over, in every format, read through its file descriptor.
set(OTHERM_LIBNOD_DIR "" CACHE PATH "Android: the folder with nod's include/nod.h and lib/libnod.so")
if(OTHERM_PLATFORM_ANDROID)
    if(NOT EXISTS "${OTHERM_LIBNOD_DIR}/include/nod.h" OR NOT EXISTS "${OTHERM_LIBNOD_DIR}/lib/libnod.so")
        message(FATAL_ERROR "OTHERM_LIBNOD_DIR has no include/nod.h and lib/libnod.so: '${OTHERM_LIBNOD_DIR}' (tools/build-android.ps1 builds it)")
    endif()
    add_library(otherm_nod SHARED IMPORTED)
    set_target_properties(otherm_nod PROPERTIES
        IMPORTED_LOCATION "${OTHERM_LIBNOD_DIR}/lib/libnod.so"
        IMPORTED_SONAME "libnod.so"
        INTERFACE_INCLUDE_DIRECTORIES "${OTHERM_LIBNOD_DIR}/include")
    add_library(otherm::nod ALIAS otherm_nod)
endif()
set(OTHERM_PLATFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/platform/host_platform.cpp")
set(OTHERM_BASE_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/base_product.cpp")
# The region-module loader is its own executable (cmake/PublicProducts.cmake, OTHERM_ENGINE_MODULE), not runtime code.
list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/product/engine_loader.cpp")
# The host ISA guard is the one translation unit that must not receive the
# x86-64-v3 flags, so it gets its own object library instead of riding along in
# otherm_runtime_common. See cmake/PublicProducts.cmake and the file's own header.
set(OTHERM_CPU_BASELINE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/host_cpu_baseline.cpp")
list(REMOVE_ITEM SOURCES ${OTHERM_BASE_PRODUCT_SOURCE} ${OTHERM_CPU_BASELINE_SOURCE} ${OTHERM_PLATFORM_SOURCE})

# This deliberately small library contains host services that are safe to
# validate before guest memory and fiber work makes a full runtime build viable.
add_library(otherm_platform STATIC "${OTHERM_PLATFORM_SOURCE}")
target_include_directories(otherm_platform PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(otherm_platform PUBLIC cxx_std_17)
set_target_properties(otherm_platform PROPERTIES UNITY_BUILD OFF)

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
enable_testing()
if(OTHERM_BUILD_PSQ_TESTS)
    add_executable(otherm_psq_helpers_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/psq_helpers_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp")
    target_include_directories(otherm_psq_helpers_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/tests/psq_memory"
        "${CMAKE_CURRENT_LIST_DIR}/include/isa"
        "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(otherm_psq_helpers_tests PRIVATE cxx_std_17)
    target_compile_options(otherm_psq_helpers_tests PRIVATE
        -O2 ${OTHERM_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_compile_options(otherm_psq_helpers_tests PRIVATE -march=x86-64-v3)
    endif()
    set_target_properties(otherm_psq_helpers_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME otherm_psq_helpers_tests COMMAND otherm_psq_helpers_tests)
    add_test(NAME otherm_psq_reserved_tests COMMAND "${CMAKE_COMMAND}"
        "-DPSQ_TEST_EXECUTABLE=$<TARGET_FILE:otherm_psq_helpers_tests>"
        -P "${CMAKE_CURRENT_LIST_DIR}/tests/psq_reserved_tests.cmake")
endif()

add_executable(otherm_platform_paths_tests "${CMAKE_CURRENT_LIST_DIR}/tests/platform_paths_tests.cpp")
target_link_libraries(otherm_platform_paths_tests PRIVATE otherm_platform)
target_compile_features(otherm_platform_paths_tests PRIVATE cxx_std_17)
add_test(NAME otherm_platform_paths_tests COMMAND otherm_platform_paths_tests)

add_executable(otherm_runtime_config_tests "${CMAKE_CURRENT_LIST_DIR}/tests/runtime_config_tests.cpp")
target_include_directories(otherm_runtime_config_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/toml11")
target_compile_features(otherm_runtime_config_tests PRIVATE cxx_std_20)
target_link_libraries(otherm_runtime_config_tests PRIVATE otherm_platform)
add_test(NAME otherm_runtime_config_tests COMMAND otherm_runtime_config_tests)

add_executable(otherm_nand_save_tests "${CMAKE_CURRENT_LIST_DIR}/tests/nand_save_tests.cpp")
target_include_directories(otherm_nand_save_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(otherm_nand_save_tests PRIVATE cxx_std_17)
add_test(NAME otherm_nand_save_tests COMMAND otherm_nand_save_tests)

add_executable(otherm_nand_settings_tests "${CMAKE_CURRENT_LIST_DIR}/tests/nand_settings_tests.cpp")
find_package(Threads REQUIRED)
target_link_libraries(otherm_nand_settings_tests PRIVATE Threads::Threads)
target_include_directories(otherm_nand_settings_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(otherm_nand_settings_tests PRIVATE cxx_std_17)
add_test(NAME otherm_nand_settings_tests COMMAND otherm_nand_settings_tests)

add_executable(otherm_sc_serial_tests "${CMAKE_CURRENT_LIST_DIR}/tests/sc_serial_tests.cpp")
target_include_directories(otherm_sc_serial_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(otherm_sc_serial_tests PRIVATE cxx_std_17)
add_test(NAME otherm_sc_serial_tests COMMAND otherm_sc_serial_tests)

# The input expression engine is self-contained, so it can be exercised without
# linking the runtime or SDL.
add_executable(otherm_input_expr_tests
    "${CMAKE_CURRENT_LIST_DIR}/tests/test_expr.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/input_expr.cpp")
target_include_directories(otherm_input_expr_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(otherm_input_expr_tests PRIVATE cxx_std_17)
add_test(NAME otherm_input_expr_tests COMMAND otherm_input_expr_tests)

# Dolphin Wii Remote profiles (Other M's controls, Metroid.ini) on top of the same engine.
add_executable(otherm_wiimote_profile_tests
    "${CMAKE_CURRENT_LIST_DIR}/tests/wiimote_profile_tests.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/wiimote_profile.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/input_expr.cpp")
target_include_directories(otherm_wiimote_profile_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(otherm_wiimote_profile_tests PRIVATE cxx_std_17)
add_test(NAME otherm_wiimote_profile_tests COMMAND otherm_wiimote_profile_tests
    "${CMAKE_CURRENT_LIST_DIR}/../project/controls/Metroid.ini")

# HostContext deliberately keeps the platform-specific context primitive out
# of fiber_manager.cpp. Exercise the Linux libco handoff directly so future
# refactors cannot silently remove its headers, implementation, or link edge.
if(OTHERM_PLATFORM_LINUX)
    add_executable(otherm_linux_host_context_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp")
    target_include_directories(otherm_linux_host_context_tests PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/third_party/libco")
    target_compile_features(otherm_linux_host_context_tests PRIVATE cxx_std_17)
    target_link_libraries(otherm_linux_host_context_tests PRIVATE otherm::libco)
    add_test(NAME otherm_linux_host_context_tests COMMAND otherm_linux_host_context_tests)
endif()

if(OTHERM_PLATFORM_MACOS)
    # Exercise the public host-memory contracts separately from translated products.
    if(OTHERM_PLATFORM_MACOS_ARM64)
        # Apple Silicon's context ABI is implemented by the local assembly backend.
        enable_language(ASM)
        add_executable(otherm_macos_context_abi_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/macos_context_abi_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
        target_compile_features(otherm_macos_context_abi_tests PRIVATE cxx_std_17)
        add_test(NAME otherm_macos_context_abi_tests COMMAND otherm_macos_context_abi_tests)

        add_executable(otherm_macos_host_context_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
        target_include_directories(otherm_macos_host_context_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
        # Save-state continuations: rebuild native fiber stacks from guest state only.
        add_executable(otherm_save_state_continuation_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/save_state_continuation_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
        target_include_directories(otherm_save_state_continuation_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
        target_compile_features(otherm_save_state_continuation_tests PRIVATE cxx_std_20)
        add_test(NAME otherm_save_state_continuation_tests COMMAND otherm_save_state_continuation_tests)
    else()
        # Intel macOS follows the same System V AMD64 libco path as Linux.
        add_executable(otherm_macos_host_context_tests
            "${CMAKE_CURRENT_LIST_DIR}/tests/host_context_tests.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/host_context.cpp")
        target_include_directories(otherm_macos_host_context_tests PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include"
            "${CMAKE_CURRENT_LIST_DIR}/third_party/libco")
        target_link_libraries(otherm_macos_host_context_tests PRIVATE otherm::libco)
    endif()
    target_compile_features(otherm_macos_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME otherm_macos_host_context_tests COMMAND otherm_macos_host_context_tests)

    add_executable(otherm_macos_guest_flat_memory_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/macos_guest_flat_memory_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
    target_include_directories(otherm_macos_guest_flat_memory_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_features(otherm_macos_guest_flat_memory_tests PRIVATE cxx_std_17)
    add_test(NAME otherm_macos_guest_flat_memory_tests COMMAND otherm_macos_guest_flat_memory_tests)

endif()

# The translator emits the complete, content-addressed source graph. Consuming
# this one manifest keeps configure independent of the generated function files.
if(OTHERM_BUILD_PRODUCTS)
    set(OTHERM_TRANSLATED_SHARD_MANIFEST
        "${CMAKE_CURRENT_LIST_DIR}/../generated/build_shards/shards.cmake"
        CACHE FILEPATH "Translator-owned aggregate shard manifest")
    if(NOT EXISTS "${OTHERM_TRANSLATED_SHARD_MANIFEST}")
        message(FATAL_ERROR
            "Missing translator-owned shard manifest: ${OTHERM_TRANSLATED_SHARD_MANIFEST}. "
            "Run Translator.Cli emit-build-shards first; see translator/README.md.")
    endif()
    include("${OTHERM_TRANSLATED_SHARD_MANIFEST}")
    message(STATUS
        "Translator graph: ${OTHERM_BASE_FUNCTION_COUNT} base functions")

    set(OTHERM_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PublicProducts.cmake")
else()
    if(OTHERM_PLATFORM_MACOS)
        # Compile-only audit of native runtime sources. It deliberately avoids
        # translated products until their host dependencies are portable.
        # These sources depend on generated/RuntimeConfig.h, which is emitted for
        # a particular game by the translator and is intentionally unavailable
        # in this platform-only configuration.
        set(OTHERM_MACOS_NATIVE_AUDIT_SOURCES ${SOURCES})
        list(REMOVE_ITEM OTHERM_MACOS_NATIVE_AUDIT_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/src/abi_bridge.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/hle/os/os_alarm.cpp")
        add_library(otherm_macos_native_compile OBJECT ${OTHERM_MACOS_NATIVE_AUDIT_SOURCES})
        target_include_directories(otherm_macos_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../aurora/include")
        target_compile_features(otherm_macos_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(otherm_macos_native_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
        target_link_libraries(otherm_macos_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            otherm::pugixml otherm::toml11 otherm::cryptopp otherm::mbedtls)
        if(OTHERM_PLATFORM_MACOS_X86_64)
            target_link_libraries(otherm_macos_native_compile PRIVATE otherm::libco)
            # Keep this compile-only audit on the same Haswell-era x86-64-v3
            # baseline as the translated product. PPC paired FMA helpers use
            # FMA intrinsics and intentionally cannot compile for plain x86-64.
            target_compile_options(otherm_macos_native_compile PRIVATE -march=x86-64-v3)
        endif()
        set_target_properties(otherm_macos_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    add_custom_target(otherm_platform_paths_check DEPENDS otherm_platform)
    message(STATUS "Translated product targets disabled (OTHERM_BUILD_PRODUCTS=OFF)")
endif()
