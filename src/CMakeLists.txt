cmake_minimum_required(VERSION 3.20)
project(sc2hd_host
    VERSION 0.2.0
    LANGUAGES C CXX
    DESCRIPTION "Host of the recompiled Soulcalibur II HD (PS3): the runtime and the port, without any game code"
)

# ---------------------------------------------------------------------------
# What this builds
# ---------------------------------------------------------------------------
# One executable, sc2hd_host: the ps3recomp runtime, the toolkit's PPU boot scaffold and this port's sources
# (main.cpp, the HLE overrides), with every dependency linked in statically. It contains nothing derived from the
# game. The lifted PPU and SPU code is a separate loadable module that the user's machine builds from their own copy
# of the game (launcher/); the host loads it at start (main.cpp, load_game_module) and the module resolves its
# runtime calls against the symbols this executable exports.
#
# Because the module is compiled against this build's headers, the build also writes an SDK next to the executable
# (sdk/, see the install rules) and an ABI string that both sides check.
#
# Personal-use option: -DSC2HD_MONOLITH_DIR=<work dir of a lift> links the lifted code into the executable instead
# (see below). Such a binary contains derived game data and must not be shared.

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED PS3RECOMP_DIR)
    message(FATAL_ERROR "Pass -DPS3RECOMP_DIR=<toolkit checkout>")
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")

# The host links no LGPL code: the ATRAC3plus decoder is swapped for a stub (the game imports no cellAtrac).
set(PS3RECOMP_ATRAC_LGPL OFF CACHE BOOL "" FORCE)

if(WIN32)
    # Windows 10 API level for the runtime too (GetCurrentThreadStackLimits needs 8.0); some MinGW distributions
    # default to Windows 7.
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()
if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang on a GCC-flavoured MinGW (libstdc++, winpthreads) does not put winpthread after libstdc++ in a static
    # link the way GCC's driver does.
    string(APPEND CMAKE_CXX_STANDARD_LIBRARIES " -lpthread")
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Generated, game-independent files
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")

# The lifter's header preamble without the per-game function table.
set(HOST_HEADER "${GEN_DIR}/ppu_recomp.h")
add_custom_command(
    OUTPUT "${HOST_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" -I "${REPO_ROOT}/scripts/make_host_header.py"
            "${PS3RECOMP_DIR}" "${HOST_HEADER}"
    DEPENDS "${REPO_ROOT}/scripts/make_host_header.py" "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
    COMMENT "Generating the host's ppu_recomp.h")

# The firmware import NID -> HLE handler table (every module).
set(HLE_NID_TABLE "${GEN_DIR}/ppu_hle_nids.cpp")
add_custom_command(
    OUTPUT "${HLE_NID_TABLE}"
    COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" --out "${HLE_NID_TABLE}" --all
    DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" "${PS3RECOMP_DIR}/tools/nid_database.py"
    COMMENT "Generating the HLE NID -> handler table")

# ABI string: a hash over everything a module is compiled against. A module built for another host refuses to load.
file(GLOB_RECURSE ABI_INPUTS CONFIGURE_DEPENDS
    "${PS3RECOMP_DIR}/include/*.h"
    "${PS3RECOMP_DIR}/runtime/ppu/*.h"
    "${PS3RECOMP_DIR}/runtime/spu/*.h"
    "${PS3RECOMP_DIR}/runtime/memory/*.h"
    "${PS3RECOMP_DIR}/runtime/platform/*.h"
    "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
    "${PS3RECOMP_DIR}/tools/spu_lifter.py")
list(FILTER ABI_INPUTS EXCLUDE REGEX "/tests/")
list(SORT ABI_INPUTS)
set(ABI_TEXT "")
foreach(f IN LISTS ABI_INPUTS)
    file(SHA256 "${f}" h)
    string(APPEND ABI_TEXT "${h}\n")
endforeach()
string(SHA256 SC2HD_HOST_ABI "${ABI_TEXT}")
string(SUBSTRING "${SC2HD_HOST_ABI}" 0 16 SC2HD_HOST_ABI)
file(WRITE "${CMAKE_BINARY_DIR}/ABI" "${SC2HD_HOST_ABI}\n")
message(STATUS "Host ABI:        ${SC2HD_HOST_ABI}")

# ---------------------------------------------------------------------------
# Optional monolith (personal use only)
# ---------------------------------------------------------------------------
# <dir>/recompiled holds ppu_recomp.h and the lifted chunks, <dir>/spu the lifted SPU images (the launcher's work
# directory of a game). The lifted code is then compiled and linked into the executable, as it was before the split:
# no module, no export tables, the lifter's own header, direct thread-local access.
set(SC2HD_MONOLITH_DIR "" CACHE PATH "Link the lifted game code in <dir>/recompiled and <dir>/spu into the executable")
set(MONOLITH_SOURCES "")
if(SC2HD_MONOLITH_DIR)
    get_filename_component(SC2HD_MONOLITH_DIR "${SC2HD_MONOLITH_DIR}" ABSOLUTE)
    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${SC2HD_MONOLITH_DIR}/recompiled/ppu_recomp_*.cpp")
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SC2HD_MONOLITH_DIR}/spu/*/spu_recomp.c")
    if(NOT RECOMP_SOURCES OR NOT SPU_LIFTED)
        message(FATAL_ERROR "No lifted sources in ${SC2HD_MONOLITH_DIR}/recompiled and /spu; run `sc2hd build` first")
    endif()
    set(SPU_SOURCES ${SPU_LIFTED} "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
    foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c sc2_overlays.c)
        if(EXISTS "${SC2HD_MONOLITH_DIR}/spu/${gen}")
            list(APPEND SPU_SOURCES "${SC2HD_MONOLITH_DIR}/spu/${gen}")
        endif()
    endforeach()
    set(MONOLITH_SOURCES ${RECOMP_SOURCES} ${SPU_SOURCES})
    # Same flags as the module build (launcher/sc2hd_launcher/module.py): the chunks are generated and huge, so no
    # warnings and no floating-point contraction; the SPU sources are small and hot, so -O2 and the musttail dispatcher.
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-ffp-contract=off")
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT COMPILE_OPTIONS "-w;-O2")
    message(STATUS "Monolith:        ${SC2HD_MONOLITH_DIR} (${CMAKE_CURRENT_SOURCE_DIR}/main.cpp skips the module)")
endif()

# ---------------------------------------------------------------------------
# The executable
# ---------------------------------------------------------------------------
# The boot scaffold is compiled here: it includes ppu_recomp.h, which in the host is the game-independent one.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Icon
# ---------------------------------------------------------------------------
# The host's own neutral icon (assets/icon/, drawn by scripts/gen_icon_art.py). A personal-use monolith may use any
# picture, e.g. the game's ICON0.PNG (scripts/build_monolith.sh does): -DSC2HD_ICON_PNG=<file>. It becomes the
# executable's icon resource on Windows and the Dock icon on macOS (app_icon.m); Linux has no window yet.
set(SC2HD_ICON_PNG "${REPO_ROOT}/assets/icon/sc2hd.png" CACHE FILEPATH "The PNG the executable's icon is made from")
set(ICON_DIR "${GEN_DIR}/icon")
add_custom_command(
    OUTPUT "${ICON_DIR}/sc2hd.ico" "${ICON_DIR}/sc2hd_icon.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${REPO_ROOT}/scripts/make_icon.py" "${SC2HD_ICON_PNG}" "${ICON_DIR}"
    DEPENDS "${REPO_ROOT}/scripts/make_icon.py" "${SC2HD_ICON_PNG}"
    COMMENT "Making the icon from ${SC2HD_ICON_PNG}")
set(ICON_SOURCES "")
if(WIN32)
    enable_language(RC)
    set(SC2HD_ICO "${ICON_DIR}/sc2hd.ico")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sc2hd.rc.in" "${GEN_DIR}/sc2hd.rc" @ONLY)
    set_source_files_properties("${GEN_DIR}/sc2hd.rc" PROPERTIES OBJECT_DEPENDS "${ICON_DIR}/sc2hd.ico")
    list(APPEND ICON_SOURCES "${ICON_DIR}/sc2hd.ico" "${GEN_DIR}/sc2hd.rc")
elseif(APPLE)
    enable_language(OBJC)
    list(APPEND ICON_SOURCES "${ICON_DIR}/sc2hd_icon.h" app_icon.m)
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    sc2hd_hle.cpp
    sail_hle.cpp
    spurs_lfqueue.cpp
    mp3_hle.c
    crypto.cpp
    install.cpp
    media/movie.cpp
    media/mp4.c
    media/h264.c
    media/h264_mb.c
    media/h264_mc.c
    media/h264_dbf.c
    media/aac.c
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${HOST_HEADER}
    ${MONOLITH_SOURCES}
    ${ICON_SOURCES}
)

if(SC2HD_MONOLITH_DIR)
    # The lifter's header, with a prototype for every lifted function, shadows the host's.
    target_include_directories(${PROJECT_NAME} PRIVATE "${SC2HD_MONOLITH_DIR}/recompiled")
    target_compile_definitions(${PROJECT_NAME} PRIVATE SC2HD_MONOLITH=1)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME sc2hd_monolith)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE
    "${GEN_DIR}"                               # the host's ppu_recomp.h
    "${ICON_DIR}"                              # sc2hd_icon.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/runtime/platform"
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(${PROJECT_NAME} PRIVATE SC2HD_HOST_ABI="${SC2HD_HOST_ABI}")
target_link_libraries(${PROJECT_NAME} PRIVATE ps3recomp_runtime)
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# The module is not linked against the host at build time; it binds to the host's symbols when it is loaded. So the
# host has to export every runtime symbol, including those no host object refers to: keep the whole runtime archive
# and export all of it.
if(SC2HD_MONOLITH_DIR)
    # Nothing binds to the executable at run time: link the runtime the usual way.
elseif(APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:ps3recomp_runtime>" "LINKER:-export_dynamic")
elseif(UNIX)
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:ps3recomp_runtime>,--no-whole-archive" "LINKER:--export-dynamic")
elseif(MINGW)
    # A PE image cannot leave symbols undefined, so the module links against an import library the host writes here
    # (it goes into the SDK, see below). Static, so the release needs no MinGW runtime DLLs next to it.
    set(HOST_IMPLIB "${CMAKE_BINARY_DIR}/libsc2hd_host.dll.a")
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:--whole-archive,$<TARGET_FILE:ps3recomp_runtime>,--no-whole-archive"
        "LINKER:--export-all-symbols" "LINKER:--out-implib,${HOST_IMPLIB}" -static)
endif()
# TODO: Windows with MSVC: the same needs a .def file or __declspec(dllexport) on the runtime.

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp user32 gdi32 winmm)
    if(NOT SC2HD_MONOLITH_DIR)
        target_sources(${PROJECT_NAME} PRIVATE win32_table_stub.c)
    endif()
    if(MINGW)
        # MSVC picks these up from the backend's #pragma comment(lib); GCC ignores the pragma.
        target_link_libraries(${PROJECT_NAME} PRIVATE d3d12 dxgi d3dcompiler dxguid)
    endif()
endif()

# ---------------------------------------------------------------------------
# SDK for the module build
# ---------------------------------------------------------------------------
# What the launcher needs to lift and compile a game against this host: the lifter tools, the headers the lifted code
# includes, and the one SPU runtime source that has to be compiled with the lift (spu_dispatch_mt.c, musttail).
set(SDK_DIR "${CMAKE_BINARY_DIR}/sdk")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SDK_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SDK_DIR}/runtime"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/include" "${SDK_DIR}/include"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/tools" "${SDK_DIR}/tools"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/ppu" "${SDK_DIR}/runtime/ppu"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/spu" "${SDK_DIR}/runtime/spu"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/memory" "${SDK_DIR}/runtime/memory"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS3RECOMP_DIR}/runtime/platform" "${SDK_DIR}/runtime/platform"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/ABI" "${SDK_DIR}/ABI"
    COMMENT "Writing the module SDK")
if(HOST_IMPLIB)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "${HOST_IMPLIB}" "${SDK_DIR}/libsc2hd_host.dll.a"
        COMMENT "Adding the host's import library to the module SDK")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# The decoders of src/media/ against ffmpeg on generated clips (needs ffmpeg with libx264, numpy and a C compiler).
option(SC2HD_TESTS "Register the media decoder tests with ctest" OFF)
if(SC2HD_TESTS)
    enable_testing()
    add_test(NAME media_decoders COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/tests/check_media.py")
endif()
