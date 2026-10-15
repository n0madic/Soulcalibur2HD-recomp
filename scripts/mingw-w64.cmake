# CMake toolchain file: cross-compile the host for 64-bit Windows with MinGW-w64 (scripts/build_host.sh, SC2HD_TARGET=windows).
# Homebrew: brew install mingw-w64. Debian/Ubuntu: apt install g++-mingw-w64-x86-64-posix (the posix thread model, for std::thread).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
