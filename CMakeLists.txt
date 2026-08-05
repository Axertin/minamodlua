cmake_minimum_required(VERSION 3.19)

project(minamodlua
    VERSION 0.0.1
    DESCRIPTION "Lua bindings for MinaModAPI - write Mina the Hollower mods in Lua"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)          # template<auto> is needed by the binding layer
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The mod is loaded into the game's process, so it must export only its entry
# point: anything else is interposition surface against the host and every other
# loaded mod. MinaMod_Init carries MM_EXPORT and so survives this.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Must precede include(LuaJIT), which checks it. Forced by LuaJIT: msvcbuild.bat's
# static path emits /DEFAULTLIB:LIBCMT, so anything else is a link conflict. The
# plain form, not the $<CONFIG:Debug> generator expression - msvcbuild.bat builds
# one configuration.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MinaModApi)
include(LuaJIT)
include(MinaMod)
include(GenApiList)

# Every mod lands in its own folder here, matching the layout the game expects.
set(MML_MODS_DIR "${CMAKE_BINARY_DIR}/mods")

add_subdirectory(src/bridge)
add_subdirectory(src/host)
# Builds a host-side tool, so it has no meaning when cross-compiling: the mingw
# lane would produce a Windows .exe the build machine cannot run.
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tools/apidoc)
endif()

if(WIN32)
    set(MML_DEPLOY_DIR "$ENV{APPDATA}/Yacht Club Games/Mina the Hollower/mods")
else()
    set(MML_DEPLOY_DIR "$ENV{HOME}/.local/share/Yacht Club Games/Mina the Hollower/mods")
endif()
file(TO_CMAKE_PATH "${MML_DEPLOY_DIR}" MML_DEPLOY_DIR)

# Copies every built mod into the game's folder. Launch with -mod -mod-allow-code.
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${MML_MODS_DIR}" "${MML_DEPLOY_DIR}"
    COMMENT "Deploying mods to ${MML_DEPLOY_DIR}")
add_dependencies(deploy minamodlua)
