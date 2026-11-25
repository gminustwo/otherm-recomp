# The game's build graph: the runtime objects, the translated shards (the translator owns that graph) and the
# program itself.

set(DATA_INIT_FILE "${OTHERM_RUNTIME_SOURCE_DIR}/../generated/data_sections_init.cpp")
set(DATA_INIT_BLOB_ASM "${OTHERM_RUNTIME_SOURCE_DIR}/../generated/data_sections_init_blobs.S")
if(EXISTS "${DATA_INIT_FILE}")
    list(APPEND SOURCES "${DATA_INIT_FILE}")
endif()
# Crash-report symbolization table emitted by generate-data-init. The stub
# (deliberately outside the globbed src/ tree so it is never picked up twice)
# keeps link succeeding when the generated table has not been produced yet.
set(GUEST_SYMBOL_TABLE_FILE "${OTHERM_RUNTIME_SOURCE_DIR}/../generated/guest_symbol_table.cpp")
if(EXISTS "${GUEST_SYMBOL_TABLE_FILE}")
    list(APPEND SOURCES "${GUEST_SYMBOL_TABLE_FILE}")
else()
    list(APPEND SOURCES "${OTHERM_RUNTIME_SOURCE_DIR}/cmake/guest_symbol_table_stub.cpp")
endif()
if(EXISTS "${DATA_INIT_BLOB_ASM}")
    enable_language(ASM)
    set_source_files_properties("${DATA_INIT_BLOB_ASM}" PROPERTIES LANGUAGE ASM SKIP_UNITY_BUILD_INCLUSION ON)
    list(APPEND SOURCES "${DATA_INIT_BLOB_ASM}")
endif()
list(REMOVE_DUPLICATES SOURCES)

if(OTHERM_PLATFORM_MACOS)
    find_library(OTHERM_IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(OTHERM_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(OTHERM_COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
endif()

function(otherm_apply_common_compile_options target)
    target_compile_options(${target} PRIVATE -O3 -ffast-math -w -pipe)
endfunction()

function(otherm_apply_translated_compile_options target)
    target_compile_options(${target} PRIVATE
        -O2 ${OTHERM_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize -w -pipe)
endfunction()

function(otherm_configure_object_target target)
    target_include_directories(${target} PRIVATE
        "${OTHERM_RUNTIME_SOURCE_DIR}/include"
        "${OTHERM_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${OTHERM_RUNTIME_SOURCE_DIR}/.."
        "${OTHERM_RUNTIME_SOURCE_DIR}/../aurora/include")
    target_compile_definitions(${target} PRIVATE
        TARGET_PC)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endfunction()

# Translated shard TUs are the memory-hungry compiles; everything else in the build is
# comparatively small. A dedicated Ninja job pool caps how many of them run at once so the
# global parallelism can use every core for the cheap TUs without the memory ceiling being a
# guess. The pool depth is per-machine (derived from installed RAM by tools/build-otherm.ps1) and is
# deliberately not part of the canonical flag set: it changes scheduling, never output bytes.
if(OTHERM_TRANSLATED_COMPILE_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS "otherm_translated=${OTHERM_TRANSLATED_COMPILE_JOBS}")
endif()

function(otherm_bound_translated_compiles target)
    if(OTHERM_TRANSLATED_COMPILE_JOBS GREATER 0)
        set_property(TARGET ${target} PROPERTY JOB_POOL_COMPILE otherm_translated)
    endif()
endfunction()

function(otherm_configure_translated_target target)
    otherm_configure_object_target(${target})
    otherm_apply_translated_compile_options(${target})
    otherm_bound_translated_compiles(${target})
endfunction()

add_library(otherm_runtime_common OBJECT ${SOURCES})
otherm_configure_object_target(otherm_runtime_common)
target_compile_features(otherm_runtime_common PRIVATE cxx_std_20)
target_compile_definitions(otherm_runtime_common PRIVATE
    SDL_MAIN_HANDLED
    _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
target_link_libraries(otherm_runtime_common PRIVATE
    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)
target_link_libraries(otherm_runtime_common PRIVATE otherm_platform otherm::pugixml otherm::toml11 otherm::cryptopp otherm::mbedtls)
if(OTHERM_PLATFORM_WINDOWS)
    # ole32/uuid: the first-start game folder picker (game_folder.cpp).
    target_link_libraries(otherm_runtime_common PRIVATE shell32 windowsapp ole32 uuid)
elseif(OTHERM_PLATFORM_LINUX)
    target_link_libraries(otherm_runtime_common PRIVATE otherm::libco ${CMAKE_DL_LIBS})
elseif(OTHERM_PLATFORM_ANDROID)
    target_link_libraries(otherm_runtime_common PRIVATE android log otherm::nod ${CMAKE_DL_LIBS})
endif()

if(OTHERM_PLATFORM_MACOS)
    target_link_libraries(otherm_runtime_common PRIVATE "${OTHERM_COREAUDIO_FRAMEWORK}")
    if(OTHERM_PLATFORM_MACOS_X86_64)
        # libco is used by Intel macOS. Apple Silicon uses the local
        # x18-safe assembly backend and therefore does not define otherm::libco.
        target_link_libraries(otherm_runtime_common PRIVATE otherm::libco)
    endif()
endif()

if(OTHERM_CPPWINRT_INCLUDE_DIR)
    if(NOT EXISTS "${OTHERM_CPPWINRT_INCLUDE_DIR}/winrt/base.h")
        message(FATAL_ERROR
            "OTHERM_CPPWINRT_INCLUDE_DIR does not contain winrt/base.h: ${OTHERM_CPPWINRT_INCLUDE_DIR}")
    endif()
    target_include_directories(otherm_runtime_common PRIVATE "${OTHERM_CPPWINRT_INCLUDE_DIR}")
endif()

# Keep runtime unity units small and semantically related. The old generated-TU
# batch size put all 57 native runtime sources into one memory-heavy compiler job.
foreach(source IN LISTS SOURCES)
    get_filename_component(source_name "${source}" NAME_WE)
    string(REPLACE "\\" "/" source_normalized "${source}")
    if(source_normalized MATCHES "/hle/gx/")
        set(runtime_group "gx_bridge")
    elseif(source_name MATCHES "network|socket|dns|dwc|ios")
        set(runtime_group "network_ios")
    elseif(source_name MATCHES "os_|system|memory|fiber|scheduler")
        set(runtime_group "guest_system")
    elseif(source_name MATCHES "debug|trace|prof")
        set(runtime_group "diagnostics")
    else()
        string(SHA256 source_hash "${source_name}")
        string(SUBSTRING "${source_hash}" 0 4 source_hash_prefix)
        math(EXPR runtime_bucket "0x${source_hash_prefix} % 8")
        set(runtime_group "runtime_${runtime_bucket}")
    endif()
    set_source_files_properties("${source}" PROPERTIES UNITY_GROUP "${runtime_group}")
endforeach()
# PPC semantic sources are excluded from unity/PCH and configured in CMakeLists.txt.
set_target_properties(otherm_runtime_common PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
target_precompile_headers(otherm_runtime_common PRIVATE "${OTHERM_RUNTIME_SOURCE_DIR}/include/otherm_pch.h")
otherm_apply_common_compile_options(otherm_runtime_common)

# Host ISA guard. Windows and Linux x86_64 product targets use x86-64-v3, so
# this object deliberately keeps the plain baseline ISA and checks the CPU
# before any AVX2/FMA code can execute. AArch64 has no equivalent optional ISA
# floor to probe: NEON/FMA are architectural requirements.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    add_library(otherm_cpu_baseline OBJECT "${OTHERM_CPU_BASELINE_SOURCE}")
    target_compile_features(otherm_cpu_baseline PRIVATE cxx_std_17)
    set_target_properties(otherm_cpu_baseline PROPERTIES UNITY_BUILD OFF)
    target_compile_options(otherm_cpu_baseline PRIVATE -w)
endif()

if(NOT OTHERM_BASE_COMMON_SHARDS)
    message(FATAL_ERROR "Translator build graph contains no shared base shards")
endif()

add_library(otherm_base_shared STATIC ${OTHERM_BASE_COMMON_SHARDS})
otherm_configure_translated_target(otherm_base_shared)
target_precompile_headers(otherm_base_shared PRIVATE "${OTHERM_RUNTIME_SOURCE_DIR}/include/otherm_pch.h")

function(otherm_configure_product target)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:otherm_runtime_common>)
    # Startup CPU check. Must stay a separate object library so it keeps the
    # plain baseline ISA while everything around it is built for x86-64-v3.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:otherm_cpu_baseline>)
    endif()
    target_include_directories(${target} PRIVATE
        "${OTHERM_RUNTIME_SOURCE_DIR}/include"
        "${OTHERM_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${OTHERM_RUNTIME_SOURCE_DIR}/.."
        "${OTHERM_RUNTIME_SOURCE_DIR}/../aurora/include")
    target_compile_definitions(${target} PRIVATE
        SDL_MAIN_HANDLED _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION TARGET_PC)
    target_compile_features(${target} PRIVATE cxx_std_20)
    otherm_apply_common_compile_options(${target})
    # The dispatch-table and registration shards compile inside the product target itself and
    # include the same fat translated headers; bound them by the same pool.
    otherm_bound_translated_compiles(${target})
    target_link_libraries(${target} PRIVATE
        otherm_platform otherm_base_shared otherm::pugixml otherm::toml11 otherm::cryptopp otherm::mbedtls)

    target_link_libraries(${target} PRIVATE
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)
    if(OTHERM_PLATFORM_MACOS)
        target_link_libraries(${target} PRIVATE
            "${OTHERM_IOKIT_FRAMEWORK}" "${OTHERM_COREFOUNDATION_FRAMEWORK}" "${OTHERM_COREAUDIO_FRAMEWORK}")
        target_link_options(${target} PRIVATE
            "LINKER:-U,_OBJC_CLASS_$_MTLLogStateDescriptor")
    endif()
    if(EXISTS "${OTHERM_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        include("${OTHERM_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        aurora_copy_runtime_dlls(${target})
    endif()
    if(TARGET sqlite3)
        get_target_property(OTHERM_SQLITE_TARGET_TYPE sqlite3 TYPE)
    endif()
    if(TARGET sqlite3 AND
       (OTHERM_SQLITE_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
        OTHERM_SQLITE_TARGET_TYPE STREQUAL "MODULE_LIBRARY"))
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sqlite3> $<TARGET_FILE_DIR:${target}>)
    endif()

    if(OTHERM_PLATFORM_WINDOWS)
        target_link_libraries(${target} PRIVATE
            dbghelp user32 winmm ws2_32 iphlpapi secur32 crypt32 windowsapp)

        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(OTHERM_PLATFORM_LINUX OR OTHERM_PLATFORM_MACOS_X86_64)
        # otherm_runtime_common is an OBJECT library: the program only pulls in its .o
        # files via $<TARGET_OBJECTS:>, which does not propagate otherm_runtime_common's own
        # target_link_libraries (object libraries don't carry usage requirements to a consumer
        # that isn't itself linked against as a target). fiber_manager.cpp's co_* calls live in
        # those objects, so the actual executable link needs otherm::libco directly, same as it
        # needs it independently of that first `if(WIN32)` branch above.
        target_link_libraries(${target} PRIVATE otherm::libco ${CMAKE_DL_LIBS})
    elseif(OTHERM_PLATFORM_ANDROID)
        # The same object-library gap as above: the product link needs the libraries the runtime
        # objects call into. SDL's Android activity loads the product as libmain.so and runs its
        # exported SDL_main (runtime/src/main.cpp). 16 KiB pages keep it loadable on Android 15+
        # devices that use them.
        target_link_libraries(${target} PRIVATE android log otherm::nod ${CMAKE_DL_LIBS})
        target_link_options(${target} PRIVATE "LINKER:-z,max-page-size=16384" "LINKER:--no-undefined")
        set_target_properties(${target} PROPERTIES OUTPUT_NAME main)
    endif()
    if(OTHERM_PLATFORM_WINDOWS)
        foreach(runtime_dll libc++.dll libunwind.dll)
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" "--print-file-name=${runtime_dll}"
                OUTPUT_VARIABLE runtime_dll_path
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT EXISTS "${runtime_dll_path}")
                get_filename_component(otherm_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
                set(runtime_dll_path "${otherm_compiler_bin}/${runtime_dll}")
            endif()
            if(NOT EXISTS "${runtime_dll_path}")
                message(FATAL_ERROR "llvm-mingw runtime DLL not found: ${runtime_dll}")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_dll_path}" $<TARGET_FILE_DIR:${target}>)
        endforeach()
    endif()

    set(OTHERM_WII_BOOTSTRAP_SOURCE_DIR "${OTHERM_RUNTIME_SOURCE_DIR}/assets/wii")
    if(NOT EXISTS "${OTHERM_WII_BOOTSTRAP_SOURCE_DIR}/shared2/wc24")
        message(FATAL_ERROR "Missing Wii first-run bootstrap payload: ${OTHERM_WII_BOOTSTRAP_SOURCE_DIR}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${OTHERM_WII_BOOTSTRAP_SOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>/wii_bootstrap")
    # Pictures the overlay menus draw (settings_overlay.cpp / hud_theme.cpp: startup/emblem.png).
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${OTHERM_RUNTIME_SOURCE_DIR}/assets/startup" "$<TARGET_FILE_DIR:${target}>/startup")

    set(OTHERM_DSP_COEFFICIENT_ROM "${OTHERM_RUNTIME_SOURCE_DIR}/assets/dsp/dsp_coef.bin")
    if(NOT EXISTS "${OTHERM_DSP_COEFFICIENT_ROM}")
        message(FATAL_ERROR "Missing Wii DSP coefficient ROM: ${OTHERM_DSP_COEFFICIENT_ROM}")
    endif()
    file(SHA256 "${OTHERM_DSP_COEFFICIENT_ROM}" OTHERM_DSP_COEFFICIENT_ROM_SHA256)
    if(NOT OTHERM_DSP_COEFFICIENT_ROM_SHA256 STREQUAL
       "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179")
        message(FATAL_ERROR "Wii DSP coefficient ROM hash mismatch: ${OTHERM_DSP_COEFFICIENT_ROM_SHA256}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OTHERM_DSP_COEFFICIENT_ROM}" "$<TARGET_FILE_DIR:${target}>/dsp_coef.bin")

    # Aurora imports this portable recipe database into each user's writable
    # pipeline cache. Keep the upstream filename so its default resourcesPath
    # lookup works without application-specific configuration.
    # The repository keeps it zipped (project/tools/make_pipeline_seed.py: 58 MB of 4 KB SQLite pages
    # are 1.2 MB zipped); it is unpacked here at configure time, again whenever the zip changes.
    set(OTHERM_INITIAL_PIPELINE_CACHE_ZIP
        "${OTHERM_RUNTIME_SOURCE_DIR}/assets/pipeline/initial_pipeline_cache.zip")
    if(EXISTS "${OTHERM_INITIAL_PIPELINE_CACHE_ZIP}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${OTHERM_INITIAL_PIPELINE_CACHE_ZIP}")
        set(OTHERM_PIPELINE_SEED_DIR "${CMAKE_BINARY_DIR}/pipeline_seed")
        file(REMOVE_RECURSE "${OTHERM_PIPELINE_SEED_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${OTHERM_INITIAL_PIPELINE_CACHE_ZIP}" DESTINATION "${OTHERM_PIPELINE_SEED_DIR}")
        set(OTHERM_INITIAL_PIPELINE_CACHE "${OTHERM_PIPELINE_SEED_DIR}/initial_pipeline_cache.db")
    else()
        set(OTHERM_INITIAL_PIPELINE_CACHE
            "${OTHERM_RUNTIME_SOURCE_DIR}/assets/pipeline/initial_pipeline_cache.db")
    endif()
    if(NOT EXISTS "${OTHERM_INITIAL_PIPELINE_CACHE}")
        message(FATAL_ERROR "Missing transferable Aurora pipeline cache: ${OTHERM_INITIAL_PIPELINE_CACHE}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OTHERM_INITIAL_PIPELINE_CACHE}"
        "$<TARGET_FILE_DIR:${target}>/initial_pipeline_cache.db")

    # Non-Windows TLS (runtime/src/hle/net/network_ssl.cpp's mbed TLS path) needs a trusted root
    # CA bundle to verify server certificates against - Windows gets this for free from the OS via
    # Schannel, mbed TLS does not ship one itself. Not SHA256-pinned like the DSP ROM above: unlike
    # a fixed hardware ROM, this bundle is expected to be refreshed periodically as CAs rotate.
    # Windows gets its trust store from Schannel, so only the platforms that actually build the
    # mbed TLS path need the bundle beside the executable.
    if(NOT OTHERM_PLATFORM_WINDOWS)
        set(OTHERM_CA_CERTIFICATE_BUNDLE "${OTHERM_RUNTIME_SOURCE_DIR}/assets/certs/cacert.pem")
        if(NOT EXISTS "${OTHERM_CA_CERTIFICATE_BUNDLE}")
            message(FATAL_ERROR "Missing TLS root CA bundle: ${OTHERM_CA_CERTIFICATE_BUNDLE}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OTHERM_CA_CERTIFICATE_BUNDLE}" "$<TARGET_FILE_DIR:${target}>/cacert.pem")
    endif()
endfunction()

# OTHERM_ENGINE_MODULE (Windows): the engine is a DLL named OTHERM_ENGINE_MODULE_NAME exporting SDL_main, like the Android
# libmain.so, and MetroidOtherM.exe is the small loader in src/product/engine_loader.cpp that starts it.
# Elsewhere the program is MetroidOtherM itself.
option(OTHERM_ENGINE_MODULE "Build the engine as a module DLL plus the loader executable" OFF)
set(OTHERM_ENGINE_MODULE_NAME "engine_usa" CACHE STRING "Output name of the engine module DLL")
if(OTHERM_PLATFORM_ANDROID OR (OTHERM_PLATFORM_WINDOWS AND OTHERM_ENGINE_MODULE))
    add_library(OtherMRecompiled SHARED "${OTHERM_BASE_PRODUCT_SOURCE}" ${OTHERM_BASE_REGISTRATION_SOURCES})
else()
    add_executable(OtherMRecompiled "${OTHERM_BASE_PRODUCT_SOURCE}" ${OTHERM_BASE_REGISTRATION_SOURCES})
    set_target_properties(OtherMRecompiled PROPERTIES OUTPUT_NAME MetroidOtherM)
endif()
otherm_configure_product(OtherMRecompiled)
if(OTHERM_PLATFORM_WINDOWS AND OTHERM_ENGINE_MODULE)
    set_target_properties(OtherMRecompiled PROPERTIES OUTPUT_NAME "${OTHERM_ENGINE_MODULE_NAME}" PREFIX "")
    target_compile_definitions(otherm_runtime_common PRIVATE OTHERM_ENGINE_MODULE)  # main.cpp exports SDL_main instead of main
    add_executable(OtherMRecompiledLoader "${OTHERM_RUNTIME_SOURCE_DIR}/src/product/engine_loader.cpp")
    target_compile_features(OtherMRecompiledLoader PRIVATE cxx_std_20)
    set_target_properties(OtherMRecompiledLoader PROPERTIES OUTPUT_NAME MetroidOtherM WIN32_EXECUTABLE TRUE)
    target_link_libraries(OtherMRecompiledLoader PRIVATE user32)
endif()

target_precompile_headers(OtherMRecompiled PRIVATE
    "${OTHERM_RUNTIME_SOURCE_DIR}/include/otherm_pch.h")
if(OTHERM_PLATFORM_WINDOWS)
    # The app icon (tools/make-app-icon.py): Explorer, the taskbar and, through SDL's
    # default of the executable's first icon, the game window.
    enable_language(RC)
    file(TO_CMAKE_PATH "${OTHERM_RUNTIME_SOURCE_DIR}/assets/icon/otherm.ico" OTHERM_APP_ICON)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc" "1 ICON \"${OTHERM_APP_ICON}\"\n")
    target_sources(OtherMRecompiled PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
    if(TARGET OtherMRecompiledLoader)
        target_sources(OtherMRecompiledLoader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
    endif()
endif()

# Windows and Linux x86_64 share the x86-64-v3 floor that the CPU baseline
# object above checks. AArch64 builds are compiled locally for the host that
# will run them, so both Linux and Apple Silicon use the compiler's native CPU
# tuning rather than leaving target-specific performance on the table.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(OTHERM_BASELINE_ARCH_FLAG -march=x86-64-v3)
elseif(OTHERM_PLATFORM_ANDROID)
    # Cross-compiled, so -mcpu=native would describe the build machine. ARMv8.2-A (LSE atomics,
    # RCpc loads) is the floor of every Android phone since Cortex-A55/A75.
    set(OTHERM_ANDROID_ARCH "armv8.2-a" CACHE STRING "Android ARM ISA floor (armv8-a for older devices)")
    set(OTHERM_BASELINE_ARCH_FLAG "-march=${OTHERM_ANDROID_ARCH}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(OTHERM_BASELINE_ARCH_FLAG -mcpu=native)
else()
    set(OTHERM_BASELINE_ARCH_FLAG "")
endif()

set(OTHERM_ALL_BUILD_TARGETS
    otherm_runtime_common otherm_base_shared OtherMRecompiled)
foreach(target IN LISTS OTHERM_ALL_BUILD_TARGETS)
    if(TARGET ${target} AND OTHERM_BASELINE_ARCH_FLAG)
        target_compile_options(${target} PRIVATE ${OTHERM_BASELINE_ARCH_FLAG})
    endif()
endforeach()
