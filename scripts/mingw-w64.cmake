# CMake toolchain file: build the host for 64-bit Windows with MinGW-w64 (scripts/build_host.sh, SC2HD_TARGET=windows),
# cross from macOS/Linux or natively from Git Bash / MSYS on Windows.
#
# Clang is preferred: llvm-mingw (x86_64-w64-mingw32-clang), or on Windows a MinGW clang on PATH (WinLibs' LLVM build,
# MSYS2 clang64). GCC's MinGW port has only emulated thread-local storage -- a function call for every access -- and
# the SPU threads use their thread-locals constantly. GCC still builds a working host: Homebrew `brew install mingw-w64`,
# Debian/Ubuntu `apt install g++-mingw-w64-x86-64-posix` (the posix thread model, for std::thread).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(SC2HD_MINGW_CLANG NAMES x86_64-w64-mingw32-clang)
if(NOT SC2HD_MINGW_CLANG AND CMAKE_HOST_WIN32)
    find_program(SC2HD_HOST_CLANG NAMES clang)
    if(SC2HD_HOST_CLANG)
        # Only a clang that targets MinGW: the LLVM installer's clang targets MSVC and cannot link this.
        execute_process(COMMAND "${SC2HD_HOST_CLANG}" -dumpmachine OUTPUT_VARIABLE _triple OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_triple MATCHES "mingw|windows-gnu")
            set(SC2HD_MINGW_CLANG "${SC2HD_HOST_CLANG}")
        endif()
    endif()
endif()

if(SC2HD_MINGW_CLANG)
    string(REGEX REPLACE "clang(\\.exe)?$" "clang++\\1" _clangxx "${SC2HD_MINGW_CLANG}")
    set(CMAKE_C_COMPILER "${SC2HD_MINGW_CLANG}")
    set(CMAKE_CXX_COMPILER "${_clangxx}")
    # Clang's MinGW driver does not add winpthread for libstdc++ the way GCC's does; lld resolves static archives in
    # any order, so the library can go with the flags.
    set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -lpthread")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -lpthread")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld -lpthread")
else()
    set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
endif()
find_program(SC2HD_MINGW_WINDRES NAMES x86_64-w64-mingw32-windres llvm-windres windres)
set(CMAKE_RC_COMPILER "${SC2HD_MINGW_WINDRES}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
