# MinaModApi.cmake - the game's native mod API headers, via FetchContent.
#
# Header-only: MinaModAPI.h, MinaModTypes.h, MinaModEnums.h. Pinned to an exact
# commit; the upstream-drift CI job reports when that pin goes stale.
#
# Exposes: minamodlua::minamodapi (INTERFACE, system include dir only)
#          MINAMODAPI_SOURCE_DIR   (for the name extraction)

include_guard(GLOBAL)
include(FetchContent)

set(MINAMODAPI_GIT_TAG "29cede8" CACHE STRING
    "MinaModAPI commit to build against. Bump deliberately; CI diffs the result.")

# SOURCE_SUBDIR points at a path with no CMakeLists.txt so MakeAvailable
# populates but does NOT add_subdirectory - we want the headers, not its
# example-mod build.
FetchContent_Declare(
    minamodapi
    GIT_REPOSITORY https://github.com/YachtClubGames/MinaModAPI.git
    GIT_TAG ${MINAMODAPI_GIT_TAG}
    SOURCE_SUBDIR _headers_only
)
FetchContent_MakeAvailable(minamodapi)

set(MINAMODAPI_SOURCE_DIR "${minamodapi_SOURCE_DIR}" CACHE INTERNAL "")

add_library(minamodlua_minamodapi INTERFACE)
add_library(minamodlua::minamodapi ALIAS minamodlua_minamodapi)
target_include_directories(minamodlua_minamodapi SYSTEM INTERFACE "${minamodapi_SOURCE_DIR}")
