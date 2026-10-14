# Cross-compile for Windows x86_64 with MinGW-w64 (gcc).
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DTLK_STATIC_SDL=ON
# snesrecomp links the MinGW runtime statically (SNESRECOMP_STATIC_RUNTIME),
# so the executables need only system DLLs. Run them under Wine to check.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TLK_MINGW_PREFIX x86_64-w64-mingw32 CACHE STRING "MinGW-w64 tool prefix")
set(CMAKE_C_COMPILER   ${TLK_MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TLK_MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TLK_MINGW_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TLK_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
