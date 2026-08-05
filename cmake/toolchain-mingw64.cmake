# toolchain-mingw64.cmake - cross-compile Windows x64 mods from Linux.
#
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
#   cmake --build build-win
#
# For fast iteration when the game is easier to run on Windows than the build
# host. mingw-w64 and MSVC agree on the calling conventions that matter here (a
# 2-float struct in RAX, a 16-byte struct via hidden pointer in RCX).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The mod is dropped into the game's folder on its own. Anything not statically
# linked is a DLL the user does not have, and the failure mode is LoadLibraryA
# failing with error 126 and the game silently not loading the mod.
#
# These must be the *_INIT variants. Setting CMAKE_CXX_STANDARD_LIBRARIES here
# looks like it works and does nothing: CMakeCXXInformation.cmake later creates
# that same name as a CACHE entry from the platform default, and creating a
# cache entry removes the same-named normal variable. The flags vanish silently,
# and a mod with no real C++ in it still links clean - so the bug stays hidden
# until the first std:: type shows up. CMAKE_CXX_STANDARD_LIBRARIES_INIT does
# not work either; Platform/Windows-GNU.cmake overwrites it.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++ -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
